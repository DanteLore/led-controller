#!/usr/bin/env bash
# Build and flash the led-controller project using ESP-IDF (macOS/Linux).
# Usage: ./flash.sh build
#        ./flash.sh -p /dev/cu.usbserial-110 build flash monitor

set -e
export IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5.3/esp-idf}"
source "$IDF_PATH/export.sh" > /dev/null
cd "$(dirname "$0")"
idf.py "$@"
