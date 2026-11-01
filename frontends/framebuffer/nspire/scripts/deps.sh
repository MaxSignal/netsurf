#!/bin/sh
# Builds everything NetSurf for the TI-Nspire needs besides its own source:
# the NetSurf libraries (pinned commits, libnsfb with the "nspire" surface
# patch), GNU libiconv, utf8proc, libjpeg-turbo, mbedTLS and lwIP, with the
# Ndless SDK (nspire-gcc on PATH).
#
#   deps.sh
#
# Environment:
#   NSPIRE_NS_ENV   where everything goes (default ~/nspire/netsurf-env)
#   JOBS            parallel jobs (default: number of CPUs)
#
# Results: $NSPIRE_NS_ENV/nspire/{include,lib,lib/pkgconfig}, the sources
# in $NSPIRE_NS_ENV/src, lwIP in $NSPIRE_NS_ENV/src/lwip.
set -eu

TARGET=nspire

HERE=$(cd "$(dirname "$0")/.." && pwd)          # frontends/framebuffer/nspire
ENV=${NSPIRE_NS_ENV:-$HOME/nspire/netsurf-env}
SRC=$ENV/src
PREFIX=$ENV/$TARGET
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
mkdir -p "$SRC" "$PREFIX/lib/pkgconfig" "$PREFIX/include" "$ENV/tools"

# ---------------------------------------------------------------------------
# Pinned sources
# ---------------------------------------------------------------------------
NS_GIT=https://github.com/netsurf-browser
NS_LIBS="buildsystem:e429a19d922b52e470fd8a2fd7e84a64b17175c9
libwapcaplet:c7c128d3eb3223b216c974471f82e9337fbcf4ba
libparserutils:6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca
libhubbub:6651b8cf87a4aa87bcdb2ff024a02659cd3f9402
libdom:f69781e1f062444b5af3f62d431d7d94018da53b
libcss:bc6a32fe373954a67ea30e42f08a9f4f7d1db9af
libnsgif:9abf1190d71ab3ae150ae6b29cf14cf7a3594a70
libnsbmp:3e5409942cc8a7ea7ef705b035394c6089cef4f8
libnsutils:0bd39060740b6163bd50875326654a722df97eb2
libnspsl:82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a
libsvgtiny:aea93365ea0586f598974697fe8555a003bd2e41
libnsfb:b701cdce7241c3747ccd78658a365db0983ebe24
nsgenbind:44c6736937ae17d4065d02959b82813b8f06a51e"

LIBICONV_VER=1.18
LIBICONV_URL=https://ftp.gnu.org/pub/gnu/libiconv/libiconv-$LIBICONV_VER.tar.gz
JPEG_TAG=3.1.4
JPEG_GIT=https://github.com/libjpeg-turbo/libjpeg-turbo.git
MBEDTLS_VER=3.6.7
MBEDTLS_URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VER/mbedtls-$MBEDTLS_VER.tar.bz2
UTF8PROC_TAG=v2.9.0
UTF8PROC_GIT=https://github.com/JuliaStrings/utf8proc.git
LWIP_TAG=STABLE-2_2_1_RELEASE
LWIP_GIT=https://github.com/lwip-tcpip/lwip.git

fetch_git_commit() { # dir url commit
    if [ ! -d "$1/.git" ]; then
        rm -rf "$1"
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    if [ "$(git -C "$1" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$3" ]; then
        git -C "$1" fetch -q --depth 1 origin "$3"
        git -C "$1" checkout -q -f FETCH_HEAD
        git -C "$1" clean -qfdx
    fi
}

fetch_git_tag() { # dir url tag
    [ -d "$1/.git" ] || git clone -q --depth 1 -b "$3" "$2" "$1"
}

fetch_tarball() { # dir url
    if [ ! -d "$1" ]; then
        mkdir -p "$1.tmp"
        curl -fsSL "$2" -o "$1.tmp/src.tar"
        tar xf "$1.tmp/src.tar" -C "$1.tmp" --strip-components=1
        rm "$1.tmp/src.tar"
        mv "$1.tmp" "$1"
    fi
}

echo "$NS_LIBS" | while IFS=: read -r name commit; do
    fetch_git_commit "$SRC/$name" "$NS_GIT/$name.git" "$commit"
done
fetch_tarball "$SRC/libiconv" "$LIBICONV_URL"
fetch_git_tag "$SRC/libjpeg-turbo" "$JPEG_GIT" "$JPEG_TAG"
fetch_tarball "$SRC/mbedtls" "$MBEDTLS_URL"
fetch_git_tag "$SRC/lwip" "$LWIP_GIT" "$LWIP_TAG"
fetch_git_tag "$SRC/utf8proc" "$UTF8PROC_GIT" "$UTF8PROC_TAG"

# Patches: patches/<library>-NNNN-*.patch, applied once (libnsfb: the
# TI-Nspire surface; the others: build fixes for newlib)
for p in "$HERE"/patches/*.patch; do
    lib=$(basename "$p" | sed 's/-[0-9][0-9][0-9][0-9]-.*//')
    if git -C "$SRC/$lib" apply --check "$p" 2>/dev/null; then
        git -C "$SRC/$lib" apply "$p"
    fi
done

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
command -v nspire-gcc >/dev/null || { echo "nspire-gcc is not on PATH (source the Ndless SDK's env)" >&2; exit 1; }
SDK=$(nspire-tools path)
# nspire-gcc with ARMv5T code tuned for the ARM926 instead of its
# -mcpu=arm926ej-s (v5TE): no LDRD/STRD, which need 8-byte alignment that
# neither the OS heap nor the loader promise.
WRAP=$ENV/tools/nspire-cc
cat > "$WRAP" <<EOF
#!/bin/sh
exec arm-none-eabi-gcc -marm -march=armv5t -mtune=arm926ej-s -D_TINSPIRE -fuse-ld=gold "\$@" -I "$SDK/include" -I "$SDK/include/freetype2"
EOF
chmod +x "$WRAP"
CC=$WRAP
AR=arm-none-eabi-ar
RANLIB=arm-none-eabi-ranlib
HOSTTRIPLE=arm-none-eabi
OPT="-O2 -ffunction-sections -fdata-sections -Wno-psabi -Wno-error -Wno-error=incompatible-pointer-types"
# the SDK's own zlib, libpng and expat
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SDK/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# ---------------------------------------------------------------------------
# NetSurf libraries (NetSurf's buildsystem)
# ---------------------------------------------------------------------------
make -C "$SRC/buildsystem" -s install PREFIX="$ENV/buildsystem" >/dev/null
NSSHARED=$ENV/buildsystem/share/netsurf-buildsystem

nsmake() { # lib [extra make args]
    lib=$1; shift
    echo "== $lib ($TARGET)"
    CFLAGS="$OPT -I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
    make -C "$SRC/$lib" -s -j"$JOBS" install \
        PREFIX="$PREFIX" NSSHARED="$NSSHARED" HOST="$HOSTTRIPLE" \
        CC="$CC" AR="$AR" CCACHE= VARIANT=release COMPONENT_TYPE=lib-static \
        "$@"
}

# GNU libiconv (newlib has the header only)
if [ ! -f "$PREFIX/lib/libiconv.a" ]; then
    echo "== libiconv ($TARGET)"
    rm -rf "$SRC/libiconv-build" && mkdir -p "$SRC/libiconv-build"
    (cd "$SRC/libiconv-build" &&
        CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$OPT" \
        "$SRC/libiconv/configure" -q --host=arm-none-eabi --prefix="$PREFIX" \
            --enable-static --disable-shared --disable-nls --enable-extra-encodings=no \
            >/dev/null &&
        make -s -C libcharset all >/dev/null &&
        make -s -C libcharset install-lib libdir="$SRC/libiconv-build/lib" \
            includedir="$SRC/libiconv-build/lib" >/dev/null &&
        make -s -j"$JOBS" -C lib >/dev/null &&
        make -s -C lib install >/dev/null &&
        cp include/iconv.h "$PREFIX/include/iconv.h")
fi

# nsgenbind: a build tool (JavaScript bindings), always for this computer
if [ ! -x "$ENV/tools/bin/nsgenbind" ]; then
    echo "== nsgenbind (tool)"
    make -C "$SRC/nsgenbind" -s -j"$JOBS" install PREFIX="$ENV/tools" \
        NSSHARED="$NSSHARED" CCACHE= VARIANT=release
fi

nsmake libwapcaplet
nsmake libparserutils
nsmake libhubbub
nsmake libdom WITH_EXPAT_BINDING=yes WITH_HUBBUB_BINDING=yes
nsmake libcss
nsmake libnsgif
nsmake libnsbmp
nsmake libnsutils
nsmake libnspsl
nsmake libsvgtiny
nsmake libnsfb NSFB_NSPIRE_AVAILABLE=yes NSFB_SDL_AVAILABLE=no NSFB_XCB_AVAILABLE=no \
    NSFB_VNC_AVAILABLE=no NSFB_WLD_AVAILABLE=no

# ---------------------------------------------------------------------------
# utf8proc (international domain names)
# ---------------------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libutf8proc.a" ]; then
    echo "== utf8proc ($TARGET)"
    B=$SRC/utf8proc-build-$TARGET
    rm -rf "$B" && mkdir -p "$B"
    $CC $OPT -DUTF8PROC_STATIC -c "$SRC/utf8proc/utf8proc.c" -o "$B/utf8proc.o"
    $AR rcs "$PREFIX/lib/libutf8proc.a" "$B/utf8proc.o"
    cp "$SRC/utf8proc/utf8proc.h" "$PREFIX/include/"
    cat > "$PREFIX/lib/pkgconfig/libutf8proc.pc" <<EOF
prefix=$PREFIX
Name: libutf8proc
Description: UTF-8 processing
Version: ${UTF8PROC_TAG#v}
Cflags: -I\${prefix}/include -DUTF8PROC_STATIC
Libs: -L\${prefix}/lib -lutf8proc
EOF
fi

# ---------------------------------------------------------------------------
# libjpeg-turbo (C only)
# ---------------------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libjpeg.a" ]; then
    echo "== libjpeg-turbo ($TARGET)"
    B=$SRC/libjpeg-turbo-build-$TARGET
    rm -rf "$B" && mkdir -p "$B"
    cat > "$B/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER $CC)
set(CMAKE_AR $(command -v $AR))
set(CMAKE_RANLIB $(command -v $RANLIB))
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF
    cmake -S "$SRC/libjpeg-turbo" -B "$B" -DCMAKE_TOOLCHAIN_FILE="$B/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$OPT" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF \
        -DWITH_TOOLS=OFF -DWITH_TESTS=OFF -DWITH_JAVA=OFF >/dev/null
    cmake --build "$B" -j"$JOBS" >/dev/null
    cmake --install "$B" >/dev/null
fi

# ---------------------------------------------------------------------------
# mbedTLS: TLS 1.2/1.3 client, no sockets, files or clock of its own
# ---------------------------------------------------------------------------
if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    echo "== mbedtls ($TARGET)"
    B=$SRC/mbedtls-build-$TARGET
    rm -rf "$B" && cp -r "$SRC/mbedtls" "$B"
    CFG=$B/include/mbedtls/mbedtls_config.h
    for opt in MBEDTLS_NET_C MBEDTLS_TIMING_C MBEDTLS_FS_IO MBEDTLS_PSA_ITS_FILE_C \
        MBEDTLS_PSA_CRYPTO_STORAGE_C MBEDTLS_SSL_SRV_C MBEDTLS_SSL_PROTO_DTLS \
        MBEDTLS_SSL_DTLS_ANTI_REPLAY MBEDTLS_SSL_DTLS_HELLO_VERIFY \
        MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE MBEDTLS_SSL_DTLS_CONNECTION_ID \
        MBEDTLS_SSL_DTLS_SRTP MBEDTLS_SSL_COOKIE_C MBEDTLS_AESNI_C MBEDTLS_PADLOCK_C \
        MBEDTLS_AESCE_C MBEDTLS_SELF_TEST MBEDTLS_DEBUG_C MBEDTLS_ERROR_C \
        MBEDTLS_SSL_TICKET_C MBEDTLS_SSL_CACHE_C MBEDTLS_PSA_CRYPTO_SE_C \
        MBEDTLS_LMS_C MBEDTLS_PKCS7_C MBEDTLS_X509_CSR_WRITE_C MBEDTLS_X509_CRT_WRITE_C \
        MBEDTLS_X509_CREATE_C MBEDTLS_X509_CSR_PARSE_C MBEDTLS_X509_CRL_PARSE_C \
        MBEDTLS_CAMELLIA_C MBEDTLS_ARIA_C MBEDTLS_DES_C MBEDTLS_NIST_KW_C MBEDTLS_CMAC_C \
        MBEDTLS_RIPEMD160_C MBEDTLS_DHM_C MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED \
        MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_PSK_ENABLED \
        MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED \
        MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED MBEDTLS_ECJPAKE_C \
        MBEDTLS_ECP_DP_SECP192R1_ENABLED MBEDTLS_ECP_DP_SECP224R1_ENABLED \
        MBEDTLS_ECP_DP_SECP192K1_ENABLED MBEDTLS_ECP_DP_SECP224K1_ENABLED \
        MBEDTLS_ECP_DP_SECP256K1_ENABLED MBEDTLS_ECP_DP_BP256R1_ENABLED \
        MBEDTLS_ECP_DP_BP384R1_ENABLED MBEDTLS_ECP_DP_BP512R1_ENABLED \
        MBEDTLS_ECP_DP_CURVE448_ENABLED MBEDTLS_HAVE_TIME_DATE_ALT_UNUSED; do
        python3 "$B/scripts/config.py" -f "$CFG" unset "$opt" 2>/dev/null || true
    done
    # roots are looked up per certificate (tls.c)
    python3 "$B/scripts/config.py" -f "$CFG" set MBEDTLS_X509_TRUSTED_CERTIFICATE_CALLBACK
    # entropy and the millisecond clock come from the program
    python3 "$B/scripts/config.py" -f "$CFG" set MBEDTLS_NO_PLATFORM_ENTROPY
    python3 "$B/scripts/config.py" -f "$CFG" set MBEDTLS_ENTROPY_HARDWARE_ALT
    python3 "$B/scripts/config.py" -f "$CFG" set MBEDTLS_PLATFORM_MS_TIME_ALT
    make -C "$B/library" -s -j"$JOBS" CC="$CC" AR="$AR" \
        CFLAGS="$OPT -I../include" libmbedcrypto.a libmbedx509.a libmbedtls.a >/dev/null
    cp "$B"/library/libmbed*.a "$PREFIX/lib/"
    rm -rf "$PREFIX/include/mbedtls" "$PREFIX/include/psa"
    cp -r "$B/include/mbedtls" "$B/include/psa" "$PREFIX/include/"
fi

# ---------------------------------------------------------------------------
# lwIP, with the frontend's configuration (net/lwipopts.h)
# ---------------------------------------------------------------------------
LW=$SRC/lwip/src
echo "== lwip ($TARGET)"
B=$SRC/lwip-build-$TARGET
rm -rf "$B" && mkdir -p "$B"
for s in core/init.c core/def.c core/dns.c core/inet_chksum.c core/ip.c core/mem.c \
    core/memp.c core/netif.c core/pbuf.c core/raw.c core/stats.c core/sys.c core/tcp.c \
    core/tcp_in.c core/tcp_out.c core/timeouts.c core/udp.c core/ipv4/acd.c core/ipv4/autoip.c \
    core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/igmp.c core/ipv4/ip4_frag.c \
    core/ipv4/ip4.c core/ipv4/ip4_addr.c netif/ethernet.c; do
    $CC $OPT -I"$HERE/net" -I"$LW/include" -c "$LW/$s" -o "$B/$(echo "$s" | tr / _ | sed 's/\.c$/.o/')"
done
rm -f "$PREFIX/lib/liblwip.a"
$AR rcs "$PREFIX/lib/liblwip.a" "$B"/*.o

echo "== done: $PREFIX"
