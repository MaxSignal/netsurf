#!/bin/sh
# Builds NetSurf for the TI-Nspire (after scripts/deps.sh).
#
#   build.sh            netsurf.tns in the source tree's root
#   build.sh clean
#
# Extra arguments go to make. NSPIRE_NS_ENV as for deps.sh.
set -eu

ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
export NSPIRE_NS_ENV=${NSPIRE_NS_ENV:-$HOME/nspire/netsurf-env}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

COMMON="TARGET=framebuffer NETSURF_FB_FRONTEND=nspire NSPIRE_NS_ENV=$NSPIRE_NS_ENV
 NETSURF_USE_CURL=NO NETSURF_USE_OPENSSL=NO NETSURF_USE_DUKTAPE=YES
 NETSURF_USE_UTF8PROC=YES NETSURF_USE_NSLOG=NO NETSURF_USE_WEBP=NO
 NETSURF_USE_JPEGXL=NO NETSURF_USE_ROSPRITE=NO NETSURF_USE_HARU_PDF=NO
 NETSURF_USE_VIDEO=NO"

cd "$ROOT"
# nsgenbind (JavaScript bindings), built by deps.sh
export PATH="$NSPIRE_NS_ENV/tools/bin:$PATH"
command -v nspire-gcc >/dev/null || { echo "nspire-gcc is not on PATH (source the Ndless SDK's env)" >&2; exit 1; }
SDK=$(nspire-tools path)
E=$NSPIRE_NS_ENV/nspire
export PKG_CONFIG_PATH="$E/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$E/lib/pkgconfig:$SDK/lib/pkgconfig"
export CFLAGS="-I$E/include -O2"
export LDFLAGS="-L$E/lib -L$SDK/lib"
# NETSURF_USE_LIBICONV_PLUG=NO: GNU libiconv, linked by Makefile.nspire
# shellcheck disable=SC2086
exec make -j"$JOBS" $COMMON SUBTARGET=-nspire CC="$NSPIRE_NS_ENV/tools/nspire-cc" \
    NETSURF_USE_LIBICONV_PLUG=NO NETSURF_STRIP_BINARY=NO CCACHE= "$@"
