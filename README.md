# led-controller — Project Context

## What this project is

A standalone controller for an addressable LED strip, built on the diymore ESP32 board with its built-in 1.9" LCD and a KY-040 rotary encoder. It reuses the display and encoder components from [bt-mixer](https://github.com/DanteLore/bt-mixer).

The strip is a 24V offcut using Sunmoon SM16703SP3 driver ICs (WS2811 protocol).

## Repository layout

```
led-controller/
├── CMakeLists.txt
├── flash.sh             ← build/flash wrapper (macOS/Linux)
├── sdkconfig.defaults
├── main/
│   └── main.c           ← effects, UI, main loop
├── components/
│   ├── led_strip/       ← RMT-driven WS2811-protocol driver
│   ├── st7789/          ← LCD driver (from bt-mixer)
│   └── encoder/         ← KY-040 quadrature ISR (from bt-mixer)
├── HARDWARE.md          ← pinouts, strip wiring, power
└── README.md            ← this file
```

## Hardware

See HARDWARE.md for full details.

- **LCD (internal):** GPIO2 (DC), GPIO4 (RST), GPIO15 (CS), GPIO18 (SCLK), GPIO23 (MOSI), GPIO32 (BL)
- **Rotary encoder:** GPIO33 (CLK), GPIO27 (DT), GPIO14 (SW)
- **LED data:** GPIO13 → SN74AHCT125N (3.3V → 5V) → 330Ω → strip DIN
- **Power:** 24V PSU straight to the strip; ESP32 from USB, or an LM2596 buck set to 5.0V into VIN (never both)
- Parts list, and the reasoning behind each choice, is in HARDWARE.md

## Toolchain

- **ESP-IDF:** v5.5.3
- **Install:** `~/esp/v5.5.3/esp-idf` (override with `IDF_PATH`); prerequisites `brew install cmake ninja dfu-util`
- **Build:** `./flash.sh build`
- **Flash:** `./flash.sh -p /dev/cu.usbserial-XXXX build flash monitor` (`ls /dev/cu.*` to find the port; the board uses a CH340)
- macOS/Linux only — no Windows scripts
- Language: **C only, not Arduino**
- No managed components — everything is in `components/`

## Current state — milestone 1

**On hold (2026-11-17)** until the level shifters, capacitors and LM2596 buck converters arrive. Firmware builds cleanly but hasn't been flashed to the board yet.


- Rainbow effect at 50Hz, brightness on the knob (0–100%, default 25%)
- Button toggles RAINBOW / RGB TEST. RGB TEST lights the strip in red, green, blue thirds — use it to check `LED_ORDER`
- LCD shows mode, brightness bar and a live preview of the strip colours
- Settings are compile-time `#define`s at the top of `main.c`: `LED_PIN`, `LED_COUNT` (chips, not LEDs), `LED_ORDER`

## Key architectural decisions

### LED driver
- RMT TX channel with the bytes encoder; 10MHz resolution, 800kHz bit timing
- Pixel buffer is stored in wire order; colour order is mapped once in `led_strip_set_pixel`
- `led_strip_show()` blocks until the frame is sent (~0.9ms for 30 pixels); the 20ms frame gap provides the latch/reset time
- Brightness is applied when copying the effect frame to the strip, so effects always render at full scale

### Main loop
- Single loop at 50Hz: read encoder → render effect → push to strip → draw preview
- Effects render into a shared `frame[]` of RGB values; the LCD preview reads the same buffer at full brightness

## What to work on next

1. Confirm `LED_COUNT` and `LED_ORDER` on the real strip
2. More effects: solid colour, chase, breathe, fire, twinkle
3. Click-to-cycle settings UI: effect → brightness → speed → colour
4. Persist settings to NVS, saved 2s after the knob stops (bt-mixer pattern)
5. WiFi: web page and/or MQTT for Home Assistant

## Coding style

- C only, no Arduino
- No defensive guard code unless at system boundaries
- No doc comments on non-library functions
- Short functions, self-documenting names
- Comments only for non-obvious logic
- British English in comments, US English in identifiers
- No premature abstractions
