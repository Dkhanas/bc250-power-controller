#!/usr/bin/env bash
# Build the firmware with the pinned library versions from deps.env.
#   bash firmware/build.sh   -> firmware/build/bc250_power.uf2     (without Home Assistant)
#                               firmware/build/bc250_power_ha.uf2  (with Home Assistant / MQTT)
# Needs: git, cmake, ninja, gcc-arm-none-eabi (+ newlib). Libraries are downloaded once into firmware/.deps/.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
. <(sed 's/#.*//' "$here/deps.env")
deps="${DEPS_DIR:-$here/.deps}"
mkdir -p "$deps"

# Pico SDK (tag) with the submodules we use
if [ ! -d "$deps/pico-sdk/.git" ]; then
    git clone --depth 1 -b "$PICO_SDK_TAG" https://github.com/raspberrypi/pico-sdk.git "$deps/pico-sdk"
    git -C "$deps/pico-sdk" submodule update --init --depth 1 lib/tinyusb lib/cyw43-driver lib/lwip lib/mbedtls
fi
sdk_tag="$(git -C "$deps/pico-sdk" describe --tags --exact-match 2>/dev/null || echo '?')"
[ "$sdk_tag" = "$PICO_SDK_TAG" ] || { echo "pico-sdk in $deps is $sdk_tag, want $PICO_SDK_TAG - delete $deps and retry"; exit 1; }

# Bluepad32 (exact commit) + its BTstack
if [ ! -d "$deps/bluepad32/.git" ]; then
    git clone https://github.com/ricardoquesada/bluepad32.git "$deps/bluepad32"
fi
if [ "$(git -C "$deps/bluepad32" rev-parse HEAD)" != "$BLUEPAD32_COMMIT" ]; then
    git -C "$deps/bluepad32" fetch -q origin "$BLUEPAD32_COMMIT" || git -C "$deps/bluepad32" fetch -q origin
    git -C "$deps/bluepad32" checkout -q "$BLUEPAD32_COMMIT"
fi
git -C "$deps/bluepad32" submodule update --init external/btstack
bt="$(git -C "$deps/bluepad32/external/btstack" rev-parse HEAD)"
[ "$bt" = "$BTSTACK_COMMIT" ] || { echo "BTstack is $bt, want $BTSTACK_COMMIT"; exit 1; }

export PICO_SDK_PATH="$deps/pico-sdk"
cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBLUEPAD32_ROOT="$deps/bluepad32"
ninja -C "$here/build"
echo
echo "Firmware: $here/build/bc250_power.uf2      (without Home Assistant)"
echo "          $here/build/bc250_power_ha.uf2   (with Home Assistant)"
