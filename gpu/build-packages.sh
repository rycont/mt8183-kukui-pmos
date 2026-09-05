#!/bin/sh
# Build the mali-vendor-krane apk on the device. Alpine's abuild wants a
# signing key and a non-root user, so make both if they are missing.
set -eu
cd "$(dirname "$0")"

[ "$(id -u)" -ne 0 ] || { echo "run this as your normal user, not root" >&2; exit 1; }

command -v abuild >/dev/null || sudo apk add alpine-sdk
[ -n "$(ls ~/.abuild/*.rsa 2>/dev/null || true)" ] || abuild-keygen -a -i -n

abuild checksum
abuild -r

echo
echo "built:"
find "${REPODEST:-$HOME/packages}" -name 'mali-vendor-krane-*.apk'
