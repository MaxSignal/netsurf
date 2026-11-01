#!/bin/sh
# Installs the prebuilt Ndless SDK (arm-none-eabi GCC 14.2, newlib, libndls,
# genzehn, make-prg; https://github.com/MaxSignal/build-toolchain) into
# $NSPIRE_HOME/prebuilt/ndless-sdk and writes $NSPIRE_HOME/env.sh, which puts
# it on PATH. genzehn is rebuilt against this computer's boost when the
# prebuilt one cannot run (it links boost 1.74).
#
#   sdk.sh && . ~/nspire/env.sh
set -eu

NSPIRE_HOME=${NSPIRE_HOME:-$HOME/nspire}
SDK=$NSPIRE_HOME/prebuilt/ndless-sdk
URL=https://github.com/MaxSignal/build-toolchain/releases/download/latest/ndless-sdk.tar.gz

mkdir -p "$NSPIRE_HOME/prebuilt"
if [ ! -x "$SDK/bin/nspire-gcc" ]; then
    echo "== downloading the Ndless SDK"
    curl -fsSL "$URL" -o "$NSPIRE_HOME/prebuilt/ndless-sdk.tar.gz"
    tar xzf "$NSPIRE_HOME/prebuilt/ndless-sdk.tar.gz" -C "$NSPIRE_HOME/prebuilt"
    rm "$NSPIRE_HOME/prebuilt/ndless-sdk.tar.gz"
fi

if ! "$SDK/bin/genzehn" --help >/dev/null 2>&1; then
    echo "== rebuilding genzehn (needs libboost-program-options-dev)"
    [ -f "$SDK/bin/genzehn.orig" ] || mv "$SDK/bin/genzehn" "$SDK/bin/genzehn.orig"
    g++ -O2 -std=c++11 -I "$SDK/tools/genzehn/elfio-3.2" "$SDK/tools/genzehn/genzehn.cpp" \
        -o "$SDK/bin/genzehn" -Wl,-Bstatic -lboost_program_options -lz -Wl,-Bdynamic
fi

cat > "$NSPIRE_HOME/env.sh" <<EOT
# Source this to use the Ndless SDK.
export NDLESS_SDK="$SDK"
export PATH="\$NDLESS_SDK/toolchain/install/bin:\$NDLESS_SDK/bin:\$PATH"
EOT
echo "== Ndless SDK ready: . $NSPIRE_HOME/env.sh"
