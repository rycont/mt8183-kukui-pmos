#!/bin/sh
# A compositor on the Mali stack, with a program inside it.
#
# KWin cannot be used here: the ChromeOS blob has no EGL platform at all, and
# KWin renders through EGL. wlroots has a Vulkan renderer, which the blob does
# serve -- so cage runs on Vulkan and the client presents through ARM's WSI
# layer. Both compositor and client end up on the vendor driver.
#
# The compositor is a host (musl) program and the client may not be, so the two
# halves use their own copies of the stack; only the Wayland socket is shared.
set -e

: "${MALI_PREFIX:=/opt/mali}"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

# The swapchain allocator wants a dma-heap named "system", but the kernel is
# built with only the CMA heap. The heaps take the same ioctls.
if [ ! -e /dev/dma_heap/system ] && [ -e /dev/dma_heap/default_cma_region ]; then
	echo "run-cage-mali: /dev/dma_heap/system is missing; see the README" >&2
	exit 1
fi

exec env \
	LIBSEAT_BACKEND="${LIBSEAT_BACKEND:-seatd}" \
	WLR_BACKENDS=drm \
	WLR_RENDERER=vulkan \
	WLR_VK_PHYSICAL_DEVICE=Mali \
	VK_DRIVER_FILES="$MALI_PREFIX/share/vulkan/icd.d/mali.json" \
	XDG_DATA_DIRS="$MALI_PREFIX/share:${XDG_DATA_DIRS:-/usr/share}" \
	LD_LIBRARY_PATH="/opt/gcompat/lib:$MALI_PREFIX/lib" \
	LD_PRELOAD="$MALI_PREFIX/lib/mali-shim.so $MALI_PREFIX/lib/libmali.so" \
	cage -- "$@"
