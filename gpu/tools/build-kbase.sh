#!/bin/sh
# Build the Mali kbase module against a prepared kernel tree.
#
# The pmOS kernel is built with clang, so the module must be too. kbase reads
# its CONFIG_MALI_* options from the C preprocessor rather than the kernel
# .config, hence the -D flags.
#
#   usage: build-kbase.sh <kernel-tree> <kbase-src>
set -e
K="${1:?kernel tree}"
M="${2:?kbase source}"

DEFS="-DCONFIG_MALI=1 -DCONFIG_MALI_REAL_HW=1 -DCONFIG_MALI_DEVFREQ=1 \
-DCONFIG_MALI_PLATFORM_NAME=\\\"mediatek\\\" -DCONFIG_MALI_GATOR_SUPPORT=1 \
-DCONFIG_PAGE_MIGRATION_SUPPORT=0"

# The kernel needs DEVFREQ_THERMAL; kbase refuses to build without it.
make -C "$K" ARCH=arm64 LLVM=1 M="$(realpath "$M")" -j"$(nproc)" modules \
	CONFIG_MALI=m CONFIG_MALI_REAL_HW=y CONFIG_MALI_DEVFREQ=y \
	CONFIG_MALI_PLATFORM_NAME=mediatek CONFIG_MALI_CSF_SUPPORT=n \
	KCFLAGS="$DEFS" KBUILD_MODPOST_WARN=1
