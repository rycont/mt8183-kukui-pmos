/*
 * Give Mali's EGL the one window system KWin asks for.
 *
 * The blob implements EGL fully, but bound to ARM's "dummy" window system: its
 * native display is EGL_DEFAULT_DISPLAY and its native window is a pair of
 * shorts. Compositors ask instead for the GBM platform, and there is no way to
 * add a platform to EGL from outside -- EGL has no loader and no layers, so
 * eglGetPlatformDisplay must live in the driver.
 *
 * It turns out nothing else is missing. KWin never creates an EGL window
 * surface: it allocates plain gbm_bos, imports them through
 * EGL_EXT_image_dma_buf_import, renders into framebuffer objects and page-flips
 * them itself -- every piece of which the blob already supports. So the whole
 * gap is the display handle, and the gbm_device that would name it is
 * information the blob does not want: it reaches the GPU through /dev/mali0,
 * not through DRM.
 *
 * Hence: answer the GBM platform with the default display, and forward
 * everything else. Preload this ahead of a libEGL that resolves to libmali.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef void *EGLDisplay;
typedef unsigned int EGLenum;
typedef int EGLint;
typedef intptr_t EGLAttrib;

#define EGL_PLATFORM_GBM_KHR 0x31D7
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_EXTENSIONS 0x3055
#define GBM_PLATFORM_EXTENSIONS \
	" EGL_KHR_platform_gbm EGL_MESA_platform_gbm EGL_EXT_device_query EGL_EXT_device_base"
#define EGL_DEVICE_EXT 0x322C
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#define EGL_TRUE 1
#define EGL_FALSE 0

typedef void *EGLDeviceEXT;
typedef unsigned int EGLBoolean;

static void *next(const char *name)
{
	static void *mali;
	void *sym;

	/* Resolve out of the driver by name rather than by search order: this
	   object stands in for libEGL.so.1 and is reached through dlopen, where
	   RTLD_NEXT has no useful meaning. The driver is normally already
	   loaded, so this hands back the existing handle. */
	if (!mali)
		mali = dlopen("libmali.so.0", RTLD_LAZY | RTLD_GLOBAL);
	if (mali && (sym = dlsym(mali, name)))
		return sym;
	return dlsym(RTLD_NEXT, name);
}

static EGLDisplay default_display(void)
{
	EGLDisplay (*get_display)(void *) = next("eglGetDisplay");
	return get_display ? get_display(NULL) : EGL_NO_DISPLAY;
}

EGLDisplay eglGetPlatformDisplayEXT(EGLenum platform, void *native, const EGLint *attribs)
{
	EGLDisplay (*real)(EGLenum, void *, const EGLint *);

	if (platform == EGL_PLATFORM_GBM_KHR)
		return default_display();

	real = next("eglGetPlatformDisplayEXT");
	return real ? real(platform, native, attribs) : EGL_NO_DISPLAY;
}

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native, const EGLAttrib *attribs)
{
	EGLDisplay (*real)(EGLenum, void *, const EGLAttrib *);

	if (platform == EGL_PLATFORM_GBM_KHR)
		return default_display();

	real = next("eglGetPlatformDisplay");
	return real ? real(platform, native, attribs) : EGL_NO_DISPLAY;
}

/* Clients check for the platform before they use it, so the claim has to be
   made in the client extension string as well as in the entry point. */
const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
	static char advertised[1024];
	const char *(*real)(EGLDisplay, EGLint) = next("eglQueryString");
	const char *value = real ? real(dpy, name) : NULL;

	if (dpy != EGL_NO_DISPLAY || name != EGL_EXTENSIONS || !value)
		return value;
	if (!advertised[0]) {
		size_t n = strlen(value);
		if (n + sizeof(GBM_PLATFORM_EXTENSIONS) < sizeof(advertised)) {
			memcpy(advertised, value, n);
			memcpy(advertised + n, GBM_PLATFORM_EXTENSIONS,
			       sizeof(GBM_PLATFORM_EXTENSIONS));
		} else {
			return value;
		}
	}
	return advertised;
}

/* Compositors want to know which DRM node the GL renders on, so that buffers
   can be allocated on the right device. This GPU is reached through /dev/mali0
   and has no node of its own, so name the display controller: that is where the
   buffers have to come from anyway, and it is the answer the caller can act on.
   The device handle is a sentinel -- there is nothing behind it to describe. */
static char device_sentinel;

static const char *drm_node(void)
{
	const char *override = getenv("MALI_EGL_DRM_NODE");
	return override ? override : "/dev/dri/card0";
}

EGLBoolean eglQueryDisplayAttribEXT(EGLDisplay dpy, EGLint attribute, EGLAttrib *value)
{
	EGLBoolean (*real)(EGLDisplay, EGLint, EGLAttrib *);

	if (attribute == EGL_DEVICE_EXT) {
		*value = (EGLAttrib)&device_sentinel;
		return EGL_TRUE;
	}
	real = next("eglQueryDisplayAttribEXT");
	return real ? real(dpy, attribute, value) : EGL_FALSE;
}

const char *eglQueryDeviceStringEXT(EGLDeviceEXT device, EGLint name)
{
	const char *(*real)(EGLDeviceEXT, EGLint);

	if (device == &device_sentinel) {
		switch (name) {
		case EGL_EXTENSIONS:
			return "EGL_EXT_device_drm EGL_EXT_device_drm_render_node";
		case EGL_DRM_DEVICE_FILE_EXT:
		case EGL_DRM_RENDER_NODE_FILE_EXT:
			return drm_node();
		default:
			return NULL;
		}
	}
	real = next("eglQueryDeviceStringEXT");
	return real ? real(device, name) : NULL;
}

/* Callers that resolve entry points through EGL rather than the linker have to
   be handed our versions too. */
void *eglGetProcAddress(const char *name)
{
	void *(*real)(const char *);

	if (name) {
		if (__builtin_strcmp(name, "eglGetPlatformDisplayEXT") == 0)
			return (void *)eglGetPlatformDisplayEXT;
		if (__builtin_strcmp(name, "eglGetPlatformDisplay") == 0)
			return (void *)eglGetPlatformDisplay;
		if (__builtin_strcmp(name, "eglQueryDisplayAttribEXT") == 0)
			return (void *)eglQueryDisplayAttribEXT;
		if (__builtin_strcmp(name, "eglQueryDeviceStringEXT") == 0)
			return (void *)eglQueryDeviceStringEXT;
	}
	real = next("eglGetProcAddress");
	return real ? real(name) : NULL;
}
