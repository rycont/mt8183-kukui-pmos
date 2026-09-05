/*
 * Wait for rendering to finish before a frame is handed to the compositor.
 *
 * Nothing in this stack tells the compositor when a frame is ready. kbase is
 * not a DRM driver, so its buffers carry no implicit fence; ARM's WSI layer
 * offers zwp_linux_explicit_synchronization_v1, which KWin dropped in Plasma 6;
 * and KWin cannot offer linux-drm-syncobj-v1 because there is no DRM render
 * node behind this GPU. With all three missing, the compositor samples buffers
 * the GPU is still writing -- the previous frame shows through, and state
 * changes that fall in one frame are never displayed at all.
 *
 * Idling the queue before present is the blunt instrument that closes the gap:
 * the buffer is complete before anyone else can look at it. It costs the
 * overlap between drawing and presenting, which is a fair price for frames that
 * are correct. Set MALI_PRESENT_SYNC=0 to take the hook out of the way.
 */
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define LAYER_NAME "VK_LAYER_mali_present_sync"

/* Dispatchable handles start with a pointer to the loader's dispatch table;
   the loader uses it as the identity of the device a queue belongs to. */
static void *dispatch_key(void *handle)
{
	return *(void **)handle;
}

struct device_data {
	void *key;
	PFN_vkCreateSwapchainKHR create_swapchain;
	PFN_vkQueuePresentKHR present;
	PFN_vkQueueWaitIdle wait_idle;
	PFN_vkGetDeviceProcAddr get_proc;
	PFN_vkDestroyDevice destroy;
	struct device_data *next;
};

static struct device_data *devices;
static int announced;

/* Qt arms a frame callback of its own and the WSI layer arms another, so the
   two throttle the client independently and two frames can land inside one
   refresh. Qt's animation driver advances by a fixed vsync interval per frame
   it renders, so a doubled frame moves animation twice as far as the display
   did -- motion runs ahead and then appears to fall back. Hold each present
   until a refresh has actually passed. MALI_PRESENT_PACE_US sets the interval;
   0 turns the pacing off. */
static long pace_interval_us(void)
{
	static long us = -1;
	const char *set;

	if (us < 0) {
		set = getenv("MALI_PRESENT_PACE_US");
		us = set ? atol(set) : 0;
	}
	return us;
}

/* Paced per swapchain: a window that is presenting must not have its turn
   consumed by another one. */
#define PACED_SWAPCHAINS 16

static void pace(const VkPresentInfoKHR *info)
{
	static struct {
		VkSwapchainKHR swapchain;
		struct timespec last;
	} seen[PACED_SWAPCHAINS];
	long interval = pace_interval_us(), waited;
	struct timespec now;
	VkSwapchainKHR key;
	unsigned i, slot;

	if (interval <= 0 || info->swapchainCount == 0)
		return;
	key = info->pSwapchains[0];
	for (i = 0, slot = PACED_SWAPCHAINS; i < PACED_SWAPCHAINS; i++) {
		if (seen[i].swapchain == key)
			break;
		if (!seen[i].swapchain && slot == PACED_SWAPCHAINS)
			slot = i;
	}
	if (i == PACED_SWAPCHAINS) {
		if (slot == PACED_SWAPCHAINS)
			return;
		i = slot;
		seen[i].swapchain = key;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (seen[i].last.tv_sec) {
		waited = (now.tv_sec - seen[i].last.tv_sec) * 1000000 +
			 (now.tv_nsec - seen[i].last.tv_nsec) / 1000;
		if (waited < interval) {
			struct timespec rest = {
				.tv_sec = 0,
				.tv_nsec = (interval - waited) * 1000,
			};
			nanosleep(&rest, NULL);
			clock_gettime(CLOCK_MONOTONIC, &now);
		}
	}
	seen[i].last = now;
}


static struct device_data *device_for(void *handle)
{
	void *key = dispatch_key(handle);
	for (struct device_data *d = devices; d; d = d->next)
		if (d->key == key)
			return d;
	return NULL;
}

static VkLayerInstanceCreateInfo *instance_chain(const VkInstanceCreateInfo *info)
{
	VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)info->pNext;

	while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
			  chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerInstanceCreateInfo *)chain->pNext;
	return chain;
}

static VkLayerDeviceCreateInfo *device_chain(const VkDeviceCreateInfo *info)
{
	VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)info->pNext;

	while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
			  chain->function == VK_LAYER_LINK_INFO))
		chain = (VkLayerDeviceCreateInfo *)chain->pNext;
	return chain;
}

static PFN_vkGetInstanceProcAddr next_instance_proc;

static VKAPI_ATTR VkResult VKAPI_CALL
create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *alloc,
		VkInstance *instance)
{
	VkLayerInstanceCreateInfo *chain = instance_chain(info);

	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	next_instance_proc = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	PFN_vkCreateInstance next = (PFN_vkCreateInstance)
		next_instance_proc(NULL, "vkCreateInstance");
	return next(info, alloc, instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL
create_device(VkPhysicalDevice phys, const VkDeviceCreateInfo *info,
	      const VkAllocationCallbacks *alloc, VkDevice *device)
{
	VkLayerDeviceCreateInfo *chain = device_chain(info);

	if (!chain)
		return VK_ERROR_INITIALIZATION_FAILED;
	PFN_vkGetInstanceProcAddr next_instance = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr next_device = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

	PFN_vkCreateDevice next = (PFN_vkCreateDevice)
		next_instance(NULL, "vkCreateDevice");
	VkResult result = next(phys, info, alloc, device);
	if (result != VK_SUCCESS)
		return result;

	struct device_data *data = calloc(1, sizeof(*data));
	if (!data)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	data->key = dispatch_key(*device);
	data->get_proc = next_device;
	data->create_swapchain = (PFN_vkCreateSwapchainKHR)
		next_device(*device, "vkCreateSwapchainKHR");
	data->present = (PFN_vkQueuePresentKHR)next_device(*device, "vkQueuePresentKHR");
	data->wait_idle = (PFN_vkQueueWaitIdle)next_device(*device, "vkQueueWaitIdle");
	data->destroy = (PFN_vkDestroyDevice)next_device(*device, "vkDestroyDevice");
	data->next = devices;
	devices = data;
	return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
destroy_device(VkDevice device, const VkAllocationCallbacks *alloc)
{
	struct device_data *data = device_for(device), **link = &devices;

	if (!data)
		return;
	while (*link != data)
		link = &(*link)->next;
	*link = data->next;
	data->destroy(device, alloc);
	free(data);
}

static VKAPI_ATTR VkResult VKAPI_CALL
queue_present(VkQueue queue, const VkPresentInfoKHR *info)
{
	struct device_data *data = device_for(queue);

	if (!data)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!announced) {
		announced = 1;
		fprintf(stderr, LAYER_NAME ": idling the queue before each present\n");
	}
	data->wait_idle(queue);
	pace(info);
	return data->present(queue, info);
}

/* Without a release fence the swapchain has no way to know when the compositor
   has finished with an image, so it comes back around to one that is still on
   screen and draws the next frame into it -- the coming frame flashes early,
   then the intended one follows. More images buy time before that wrap-around.
   The surface here allows up to six. */
static VKAPI_ATTR VkResult VKAPI_CALL
create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
		 const VkAllocationCallbacks *alloc, VkSwapchainKHR *swapchain)
{
	struct device_data *data = device_for(device);
	VkSwapchainCreateInfoKHR deeper;

	if (!data)
		return VK_ERROR_INITIALIZATION_FAILED;
	deeper = *info;
	/* Off by default: more images did not help the frame ordering, and each
	   one is a full-screen allocation that a newly created window has to
	   wait for. Set MALI_SWAPCHAIN_IMAGES to try a different count. */
	const char *want = getenv("MALI_SWAPCHAIN_IMAGES");
	if (want) {
		uint32_t n = (uint32_t)atoi(want);
		if (n > deeper.minImageCount)
			deeper.minImageCount = n;
	}
	return data->create_swapchain(device, &deeper, alloc, swapchain);
}

#define INTERCEPT(name, fn) \
	if (strcmp(pName, name) == 0) return (PFN_vkVoidFunction)fn

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
get_device_proc(VkDevice device, const char *pName)
{
	struct device_data *data;

	INTERCEPT("vkGetDeviceProcAddr", get_device_proc);
	INTERCEPT("vkDestroyDevice", destroy_device);
	INTERCEPT("vkCreateSwapchainKHR", create_swapchain);
	if (strcmp(pName, "vkQueuePresentKHR") == 0) {
		const char *off = getenv("MALI_PRESENT_SYNC");
		if (!off || strcmp(off, "0") != 0)
			return (PFN_vkVoidFunction)queue_present;
	}
	data = device ? device_for(device) : NULL;
	return data ? data->get_proc(device, pName) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
get_instance_proc(VkInstance instance, const char *pName)
{
	INTERCEPT("vkGetInstanceProcAddr", get_instance_proc);
	INTERCEPT("vkCreateInstance", create_instance);
	INTERCEPT("vkCreateDevice", create_device);
	INTERCEPT("vkGetDeviceProcAddr", get_device_proc);
	INTERCEPT("vkQueuePresentKHR", queue_present);
	INTERCEPT("vkCreateSwapchainKHR", create_swapchain);
	INTERCEPT("vkDestroyDevice", destroy_device);
	return next_instance_proc ? next_instance_proc(instance, pName) : NULL;
}

__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
	if (iface->loaderLayerInterfaceVersion < 2)
		return VK_ERROR_INITIALIZATION_FAILED;
	iface->loaderLayerInterfaceVersion = 2;
	iface->pfnGetInstanceProcAddr = get_instance_proc;
	iface->pfnGetDeviceProcAddr = get_device_proc;
	iface->pfnGetPhysicalDeviceProcAddr = NULL;
	return VK_SUCCESS;
}
