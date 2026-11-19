#!/usr/bin/env bash
# Builds the patch engine on this Mac, renders the presets with both the C and
# JavaScript copies, and checks they agree. Also checks that both reject each
# file in bad/ with the same message. Needs cc, node and python3.
set -e
cd "$(dirname "$0")"
IDF_PATH="${IDF_PATH:-$HOME/esp/v5.5.3/esp-idf}"
CJSON="$IDF_PATH/components/json/cJSON"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -O2 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -I.. -I"$CJSON" \
   ../patch.c ../patch_json.c "$CJSON/cJSON.c" render.c -lm -o "$OUT/render"

PRESETS=../../../main/presets.json
failed=0
for leds in 10 37 150; do
    "$OUT/render" "$PRESETS" $leds > "$OUT/c.txt"
    node render.js "$PRESETS" $leds > "$OUT/js.txt"
    echo -n "presets, $leds LEDs: "
    python3 compare.py "$OUT/c.txt" "$OUT/js.txt" || failed=1
done

for bad in bad/*.json; do
    c=$("$OUT/render" "$bad" 10 || true)
    js=$(node render.js "$bad" 10 || true)
    if [ "$c" = "$js" ]; then
        echo "$(basename "$bad"): both say: ${c#error }"
    else
        echo "$(basename "$bad"): DIFFERENT"; echo "  C:  $c"; echo "  JS: $js"; failed=1
    fi
done
exit $failed
