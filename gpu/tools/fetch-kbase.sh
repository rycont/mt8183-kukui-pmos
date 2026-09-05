#!/bin/sh
# Fetch the ARM Mali kbase driver from the ChromeOS kernel tree.
#
# ChromeOS is the only source that carries the MediaTek platform glue
# (platform/mediatek/mt8183*), which the driver needs to power the GPU on this
# SoC. chromeos-6.12 is the newest branch that still ships kbase; the release is
# r44p1 and its uAPI major is 11, matching the g13p0 userspace blobs.
set -e
DEST="${1:-kbase-src}"
URL="https://chromium.googlesource.com/chromiumos/third_party/kernel/+archive/refs/heads/chromeos-6.12/drivers/gpu/arm/mali.tar.gz"

mkdir -p "$DEST"
curl -L "$URL" | tar xz -C "$DEST"
echo "Fetched r44p1 kbase into $DEST"
echo "Now apply ../patches/kbase-6.12-to-6.18.diff"
