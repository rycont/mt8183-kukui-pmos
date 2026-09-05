#!/bin/sh
# Run a program against the proprietary Mali userspace on musl.
#
# Three things are needed beyond the blob itself:
#
#   gcompat      musl's glibc ABI layer, kept out of the way in /opt so it does
#                not collide with a real glibc installed for something else
#   the shim     glibc symbols gcompat lacks: _FORTIFY_SOURCE wrappers, the
#                C23 strtol family, and the *64 large-file aliases
#   LD_PRELOAD   the blob uses initial-exec TLS, which musl only allocates for
#                libraries present at startup -- dlopen() of it fails
#
# The blob also refuses to load under glibc: it carries DT_RELR relocations
# without the GLIBC_ABI_DT_RELR dependency glibc demands. musl has no such check.
set -e
PREFIX="${MALI_PREFIX:-/opt/mali}"
exec env \
	LD_LIBRARY_PATH="/opt/gcompat/lib:$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
	LD_PRELOAD="$PREFIX/lib/mali-shim.so $PREFIX/lib/libmali.so${LD_PRELOAD:+ $LD_PRELOAD}" \
	"$@"
