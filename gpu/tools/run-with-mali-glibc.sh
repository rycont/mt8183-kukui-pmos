#!/bin/sh
# Run a glibc program against the Mali stack. The blob wants a libc++ new enough
# to have std::__1::__hash_memory (LLVM 20+), which neither the host nor the
# freedesktop runtime ships, so both it and libc++ live under $PREFIX.
set -e
PREFIX="${MALI_GLIBC_PREFIX:-/opt/mali/glibc}"
exec env \
	LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
	VK_DRIVER_FILES="$PREFIX/share/vulkan/icd.d/mali.json" \
	VK_ADD_LAYER_PATH="$PREFIX/share/vulkan/implicit_layer.d" \
	"$@"
