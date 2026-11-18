#!/usr/bin/env bash
# Build and flash the led-controller project using ESP-IDF (macOS/Linux).
# Usage: ./flash.sh build
#        ./flash.sh flash monitor
# Override per run: BOARD=lcd ./flash.sh ...   PORT=/dev/cu.xxx ./flash.sh ...

set -e

# Board currently on the bench: devkit (HW-463C, no LCD) or lcd (diymore with 1.9" LCD)
BOARD="${BOARD:-devkit}"

# First USB serial adapter found (CP2102 on the DevKit, CH340 on the LCD board)
PORT="${PORT:-$(ls /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART /dev/cu.wchusbserial* 2>/dev/null | head -1)}"

export IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5.3/esp-idf}"
source "$IDF_PATH/export.sh" > /dev/null
cd "$(dirname "$0")"

port_args=()
if [ -n "$PORT" ]; then
    port_args=(-p "$PORT")
fi
echo "Board: $BOARD  Port: ${PORT:-none found}"

# Each board gets its own build dir, since sdkconfig only picks up defaults once
case "$BOARD" in
    lcd)    idf.py "${port_args[@]}" "$@" ;;
    devkit) idf.py -B build-devkit -D SDKCONFIG=build-devkit/sdkconfig \
                   -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.devkit" \
                   "${port_args[@]}" "$@" ;;
    *)      echo "Unknown BOARD '$BOARD' (lcd or devkit)"; exit 1 ;;
esac
