#!/bin/bash
# Renders panel/splash.html to main/splash.rgb565, the LCD's boot splash.
# Needs Chrome (for the panel's Futura fonts, so run it on the Mac); the output is committed.
set -euo pipefail
cd "$(dirname "$0")/.."
png=$(mktemp -t splash).png
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless --hide-scrollbars \
    --force-device-scale-factor=1 --window-size=320,170 --screenshot="$png" "file://$PWD/panel/splash.html" 2>/dev/null
python3 tools/png_to_rgb565.py "$png" main/splash.rgb565
cp "$png" panel/splash.png
rm -f "$png"
