#!/bin/sh
# Zed on the Mali stack. The compositor is a host (musl) program while Zed lives
# in a glibc runtime, so each half loads its own copy of the driver and they
# share only the Wayland socket.
#
# VK_ADD_LAYER_PATH does not reach implicit layers -- the WSI layer is one --
# so the layer has to be found through XDG_DATA_DIRS.
PREFIX="${MALI_GLIBC_PREFIX:-/opt/mali/glibc}"
exec flatpak run --filesystem=host --device=all --socket=wayland \
	--env=LD_LIBRARY_PATH="$PREFIX/lib" \
	--env=VK_DRIVER_FILES="$PREFIX/share/vulkan/icd.d/mali.json" \
	--env=XDG_DATA_DIRS="$PREFIX/share:/app/share:/usr/share" \
	--command=/home/user/zed-patched dev.zed.Zed "$@"
