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

/* MALI_PRESENT_DEBUG reports how evenly a swapchain is actually presenting.
   Frames leaving the client at a steady 16.7 ms while the motion still stutters
   would put the fault downstream of here; an uneven trace would put it in the
   client's own loop. */
#define TRACE_WINDOW 120

static int cmp_long(const void *a, const void *b)
{
	long x = *(const long *)a, y = *(const long *)b;
	return (x > y) - (x < y);
}

static long wait_total, wait_max;
static unsigned wait_n;

static void note_wait(long us)
{
	wait_total += us;
	wait_n++;
	if (us > wait_max)
		wait_max = us;
}

static void trace_present(VkSwapchainKHR key, long delta_us)
{
	static struct {
		VkSwapchainKHR swapchain;
		long d[TRACE_WINDOW];
		unsigned n;
	} t[PACED_SWAPCHAINS];
	long sorted[TRACE_WINDOW];
	unsigned i, slot = PACED_SWAPCHAINS, early = 0, late = 0;

	for (i = 0; i < PACED_SWAPCHAINS; i++) {
		if (t[i].swapchain == key)
			break;
		if (!t[i].swapchain && slot == PACED_SWAPCHAINS)
			slot = i;
	}
	if (i == PACED_SWAPCHAINS) {
		if (slot == PACED_SWAPCHAINS)
			return;
		i = slot;
		t[i].swapchain = key;
	}
	t[i].d[t[i].n++] = delta_us;
	if (t[i].n < TRACE_WINDOW)
		return;

	memcpy(sorted, t[i].d, sizeof(sorted));
	qsort(sorted, TRACE_WINDOW, sizeof(long), cmp_long);
	for (unsigned k = 0; k < TRACE_WINDOW; k++) {
		if (t[i].d[k] < 12000)
			early++;
		else if (t[i].d[k] > 22000)
			late++;
	}
	fprintf(stderr, "[mali] present %p: p50 %ld p95 %ld max %ld us, "
		"%u under 12ms, %u over 22ms of %d; idle mean %ld max %ld us\n",
		(void *)key, sorted[TRACE_WINDOW / 2],
		sorted[TRACE_WINDOW * 95 / 100], sorted[TRACE_WINDOW - 1],
		early, late, TRACE_WINDOW,
		wait_n ? wait_total / wait_n : 0, wait_max);
	t[i].n = 0;
	wait_total = wait_max = 0;
	wait_n = 0;
}

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

	if (info->swapchainCount == 0)
		return;
	if (interval <= 0 && !getenv("MALI_PRESENT_DEBUG"))
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
		if (getenv("MALI_PRESENT_DEBUG"))
			trace_present(key, waited);
		if (interval > 0 && waited < interval) {
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
	/* MALI_PRESENT_NO_WAIT drops the idle wait, which is what serialises the
	   client: with it, nothing overlaps a frame's rendering with the next
	   frame's recording. It is here because the compositor gets no fence. */
	if (!getenv("MALI_PRESENT_NO_WAIT")) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		data->wait_idle(queue);
		clock_gettime(CLOCK_MONOTONIC, &b);
		note_wait((b.tv_sec - a.tv_sec) * 1000000 +
			  (b.tv_nsec - a.tv_nsec) / 1000);
	}
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
	if (want)
		deeper.minImageCount = (uint32_t)atoi(want);
	/* Which present mode the client asked for decides whether the WSI layer
	   paces on the compositor's frame callbacks at all. MALI_PRESENT_MODE
	   overrides it: 0 immediate, 1 mailbox, 2 fifo, 3 fifo relaxed. */
	const char *mode = getenv("MALI_PRESENT_MODE");
	if (mode)
		deeper.presentMode = (VkPresentModeKHR)atoi(mode);
	struct timespec a, b;
	VkResult r;

	clock_gettime(CLOCK_MONOTONIC, &a);
	r = data->create_swapchain(device, &deeper, alloc, swapchain);
	clock_gettime(CLOCK_MONOTONIC, &b);
	if (getenv("MALI_PRESENT_DEBUG"))
		fprintf(stderr, "[mali] swapchain %p %ux%u images>=%u mode %d%s "
			"created in %ld us\n", (void *)*swapchain,
			deeper.imageExtent.width, deeper.imageExtent.height,
			deeper.minImageCount, (int)deeper.presentMode,
			mode ? " (forced)" : "",
			(b.tv_sec - a.tv_sec) * 1000000 +
			(b.tv_nsec - a.tv_nsec) / 1000);
	return r;
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
