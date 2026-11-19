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
├── sdkconfig.devkit     ← extra defaults for the DevKit V1 (LCD off)
├── main/
│   ├── main.c           ← UI, main loop, output
│   ├── effects.c/h      ← all effects, time-based, 16-bit perceptual colour
│   └── Kconfig.projbuild ← LED_CONTROLLER_LCD option
├── components/
│   ├── led_strip/       ← RMT-driven WS2811-protocol driver
│   ├── st7789/          ← LCD driver (from bt-mixer)
│   ├── encoder/         ← KY-040 quadrature ISR (from bt-mixer)
│   └── wifi/            ← joins WiFi, setup hotspot, captive DNS, setup page (setup.html)
├── HARDWARE.md          ← pinouts, strip wiring, power, parts and design decisions
├── pinout.html / .pdf   ← printable bench wiring card for the diymore LCD board
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
- **Flash:** `./flash.sh flash monitor` (builds first if needed; Ctrl+] exits the monitor)
- `BOARD` defaults to `devkit` (HW-463C, no LCD, builds in `build-devkit/`); `BOARD=lcd` for the diymore LCD board. Change the default at the top of `flash.sh` when the bench board changes
- `PORT` is auto-detected from the first `/dev/cu.usbserial-*`, `cu.SLAB_USBtoUART` or `cu.wchusbserial*`; override with `PORT=/dev/cu.xxx ./flash.sh ...`
- macOS/Linux only — no Windows scripts
- Language: **C only, not Arduino**
- Few external libraries: we write small pieces ourselves in `components/`; a light library is fine where it clearly helps. ESP-IDF's own components (WiFi, web server, NVS) are used freely. No managed components so far

## Current state

Developing on an HW-463C ESP32 DevKit V1 with a 10-LED test strip (2026-11-18). Direct 3.3V data (D13 → 330Ω → DIN) works: RGB TEST shows red, green, blue thirds, and everything runs at a steady 200fps. With the 74AHCT125 in circuit the first pixel flickered and the rest froze, so the shifter has been left out for now; direct 3.3V is the working setup. The KY-040 encoder is connected (CLK D33, DT D27, SW D14, + 3V3). `LED_ORDER` (BRG) is confirmed correct on this strip. The diymore LCD board is waiting on a replacement screen; the `lcd` build compiles but hasn't run on hardware.

- **Button:** next mode (each starts from its beginning). **Knob:** speed in moving modes, brightness in the rest. `STARTUP_MODE` in `main.c` picks the first mode.

| Mode | Knob | What it does |
|---|---|---|
| DEMO | Brightness | 90s loop of three 30s segments: rainbow accelerating from 0.1 to 2 cycles/s → warm white pulsing full → dark → full every 6s → whole strip easing warm white → cool white → warm. Segment changes are logged |
| RAINBOW | Speed | Scrolling rainbow, one cycle per 5s at 1x |
| PULSE | Speed | Warm white easing full → dark → full, 6s at 1x |
| WARM | Brightness | Steady warm white |
| PATTERN | Speed | Ten distinct hues stepping along the strip every 0.5s while the whole strip eases in and out over 4s |
| RGB TEST | Brightness | Red, green, blue thirds from the input end, to check `LED_ORDER` |
| DIM TEST | Speed | Full white at full scale (ignores brightness): 5s static gradient, then 10s fade out and 10s fade in. Each phase is logged |

- **Brightness** is perceptual 0–100% (default 50%, roughly 22% PWM), shared by all modes
- **Speed** is per mode, so each moving mode remembers its own. Knob steps are logarithmic: every 5 clicks doubles or halves it, from 1/8x to 8x, so each click feels the same size at any speed. Changing speed changes the rate of motion without the effect jumping, because effect time is accumulated as real time × speed
- Serial log on every change plus a heartbeat every 5s with mode, brightness, speed and measured frame rate
- LCD shows the mode, whichever setting the knob controls (with a bar), and a live preview of the strip
- Settings are compile-time `#define`s at the top of `main.c`: `LED_PIN`, `LED_COUNT` (chips, not LEDs), `LED_ORDER`, `HOSTNAME`

### WiFi

- **First boot, or after moving house:** with no saved network, the board opens an open hotspot called `LED-Setup-XXXX` (last digits of its MAC). Join it from a phone and the setup page pops up as a captive portal, like hotel WiFi (Android shows a "Sign in to Wi-Fi network" notification); if it doesn't, browse to `http://4.3.2.1/wifi`. On Android, turn mobile data off first: with it on, the phone sends traffic for a no-internet network over mobile data and never reaches the board
- The page scans for networks (2.4GHz only: the ESP32 has no 5GHz radio), you tap one, enter the password and press Join. The page shows progress and, once joined, the board's new address, and the Join button becomes Change network. The hotspot stays up for a minute so the phone can read it, then closes
- The network is saved in flash by the WiFi driver, so later boots join straight away. The board's address is logged on serial, and it asks the router for the hostname `led-controller`
- **Can't join the saved network** (5 attempts, about 15s): the hotspot comes back, with the reason (not found, wrong password) on the page. If no phone uses it for 5 minutes, it closes and the saved network is tried again, so a router outage or power cut sorts itself out
- **Dropped after joining:** retried every 3s indefinitely, without the hotspot
- **Changing network** from home: the setup page is also at `http://<board address>/wifi` (the root redirects there until there's a control page)
- Security: the hotspot is open, and anyone on the home network can reach the setup page. Fine for a lamp; revisit before it controls anything that matters
- Tested on the DevKit with an Android phone (2026-11-19): popup, scan, join, hotspot closing after a minute, and rejoining on reboot all work. Not yet tried from an iPhone

## Key architectural decisions

### LED driver
- RMT TX channel with the bytes encoder; 10MHz resolution, 800kHz bit timing
- Pixel buffer is stored in wire order; colour order is mapped once in `led_strip_set_pixel`
- `led_strip_show()` blocks until the frame is sent (30µs per pixel) and then holds the 300µs latch gap
- Brightness is applied when copying the effect frame to the strip, so effects always render at full scale
- **Smooth dimming:** `led_strip_set_pixel16` takes 16-bit perceptual levels and applies gamma 2.2 so equal steps look equal. It then dithers to the strip's 8 bits: a level between two PWM steps alternates between them in a 4-frame ordered pattern (thresholds 32/160/96/224), giving four sub-steps per PWM step. At 200fps the slowest pattern repeats at 50Hz
  - The pattern is shared by all pixels (driven by a frame counter, not per-pixel error), so pixels at the same level always match. An earlier error-diffusion version left each pixel's carried error different after the gradient, so a uniform fade showed pixels out of sync
  - Below PWM step 3 (`DITHER_MIN_STEP`), alternating would be a 33–100% brightness change and shows as flicker, so those levels are rounded instead. The very bottom of a fade therefore steps through 1, 2, 3 out of 255 — the strip's 8-bit PWM can't go finer without flicker
- **Equal-power rainbow:** the hue wheel crossfades red → green → blue → red with the channels' linear light output always summing to one (computed before gamma, using `LED_STRIP_GAMMA`). A plain wheel lights two channels fully at yellow, cyan and magenta but only one at the primaries, so a moving rainbow appeared to pulse brighter and dimmer. Peak brightness of mixed hues is a little lower in exchange
- A fixed 300µs latch gap after every frame, so a late frame can never run into the next
- **Low-end floor:** the strip's lowest non-zero level (1/255) is still a visible glow, so a fade to black always ends with a visible switch-off; with gamma 2.2, roughly the bottom 8% of perceptual brightness rounds to off. Options noted for later: pulse modes ease to the minimum glow instead of off, or fade faster through the bottom of the range

### WiFi
- `components/wifi` uses only ESP-IDF: `esp_wifi`, `esp_netif`, and the built-in `esp_http_server`. The captive DNS server (~120 lines, `captive_dns.c`) is our own. It answers A queries with the hotspot's address, other types (AAAA) with an empty reply so phones don't hang, and impossible names (`*google.com`, `.onion`) and reverse lookups with "no such name": some Android phones look those up to detect spoofed DNS
- Phones detect the portal by probing a known URL (`captive.apple.com/hotspot-detect.html`, `connectivitycheck.gstatic.com/generate_204`); while the hotspot is up, any unknown URL gets a 302 to the setup page
- **The hotspot is at 4.3.2.1, not the softAP default 192.168.4.1.** Android 11+ treats a probe that resolves to a private address as "no internet" and silently skips the sign-in prompt; it fetched the page in the background but showed nothing. A public-looking address fixes it (WLED uses 4.3.2.1 for the same reason); the hotspot has no internet, so the address's real owner is never reached
- No DHCP option 114 (RFC 8910 captive portal URI): Android expects it to point at a JSON API, not a page, and it was a suspect while debugging
- WiFi power saving is off (`WIFI_PS_NONE`): with it on, replies took 80–170ms and the odd request failed; on mains power the saving isn't worth it
- The hotspot runs as AP+STA so the page can scan while it's up. All state changes take one mutex, since the event loop, timers and web server each run on their own task
- The setup page is a single static `setup.html` embedded in the firmware (`EMBED_FILES`). It talks to `/wifi/status`, `/wifi/scan` and `/wifi/save` (form-encoded POST), so the C side never builds HTML
- `wifi_web_server()` hands out the server so the control page can register on it later

### Cores
- WiFi runs on core 0. The LED loop is a task pinned to core 1 at priority 10, created from `app_main`, and the strip, encoder and LCD are initialised inside it so their interrupts are on core 1 too. On a shared core, WiFi interrupts can delay the RMT's mid-frame buffer refills and corrupt the data
- `CONFIG_RMT_TX_ISR_CACHE_SAFE` keeps the RMT interrupt running while flash is written (saving WiFi settings), so a frame can't glitch then
- `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`: the app is ~840KB with WiFi, past the default 1MB partition's comfort zone; the large table gives 1.5MB

### Main loop
- Single loop at 200fps in the LED task (`vTaskDelayUntil`, 5ms): read encoder → render effect → apply brightness → push to strip; LCD preview every 4th frame
- Effects are pure functions of effect time (ms), so motion speed doesn't depend on frame rate, and speed control lives entirely in the main loop
- Effects render 16-bit perceptual RGB into `frame[]`; the LCD preview reads the same buffer before brightness is applied

## What to work on next

1. **WiFi:** test the setup flow on hardware, then a control web page (mode, brightness, speed) on `wifi_web_server()`, then possibly MQTT for Home Assistant (ESP-IDF has a built-in MQTT client). A `led-controller.local` name would need mDNS, which is a managed component in ESP-IDF 5.x
2. When the replacement screen arrives: move to the diymore LCD board (`BOARD=lcd`) and do more on light control — on-screen UI, more effects (solid colour, chase, fire, twinkle), and the low-end floor options above
3. Persist settings (mode, brightness, per-mode speed) to NVS, saved 2s after the knob stops (bt-mixer pattern)
4. Set `LED_COUNT` for the full offcut once it's in use; it's 10 for the bench test piece
5. Level shifter: revisit only if a longer data wire or the final install needs it (see HARDWARE.md)

## Coding style

- C only, no Arduino
- No defensive guard code unless at system boundaries
- No doc comments on non-library functions
- Short functions, self-documenting names
- Comments only for non-obvious logic
- British English in comments, US English in identifiers
- No premature abstractions
