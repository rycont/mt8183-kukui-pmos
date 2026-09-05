#!/bin/sh
# Build the mali-vendor-krane apk on the device.
#
# Everything the package needs is assembled here rather than at install time:
# the driver is pulled out of a ChromeOS recovery image, the WSI layer and
# libc++ are downloaded, and the shims are compiled against both libcs. The
# resulting apk installs without touching the network.
#
# That apk contains ARM's driver, which is under their EULA. Keep it to
# yourself.
set -eu
cd "$(dirname "$0")"

[ "$(id -u)" -ne 0 ] || { echo "run this as your normal user, not root" >&2; exit 1; }

command -v abuild >/dev/null || sudo apk add alpine-sdk
sudo apk add --quiet curl gcompat libarchive-tools libc++ mesa-gbm python3 \
	vulkan-headers vulkan-loader
[ -n "$(ls ~/.abuild/*.rsa 2>/dev/null || true)" ] || abuild-keygen -a -i -n

# The tools call each other by their installed names.
mkdir -p .bin
ln -sf ../tools/mali-extract-root.py .bin/mali-extract-root
ln -sf ../tools/declare-dt-relr.py   .bin/mali-declare-dt-relr
ln -sf ../tools/mali-build-shims     .bin/mali-build-shims
PATH="$PWD/.bin:$PATH"
export PATH

rm -rf stage
./tools/mali-vendor-setup --destdir "$PWD/stage" "$@"

abuild checksum
abuild -r

echo
echo "built:"
find "${REPODEST:-$HOME/packages}" -name 'mali-vendor-krane-*.apk'
