# led-controller — Project Context

## What this project is

A standalone controller for an addressable LED strip, built on the diymore ESP32 board with its built-in 1.9" LCD between two KY-040 rotary encoders, plus a three-position mode switch. It reuses the display and encoder components from [bt-mixer](https://github.com/DanteLore/bt-mixer).

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
│   ├── presets.json     ← built-in patch library, compiled in
│   ├── library.c/h      ← the patch library: saved in NVS, or the presets; the editor's draft
│   ├── web.c/h          ← patch editor page (editor.html) and its JSON API
│   ├── effects.c/h      ← RGB TEST and DIM TEST, kept in C
│   └── Kconfig.projbuild ← LED_CONTROLLER_LCD option
├── components/
│   ├── led_strip/       ← RMT-driven WS2811-protocol driver
│   ├── st7789/          ← LCD driver (from bt-mixer)
│   ├── encoder/         ← KY-040 quadrature ISR (from bt-mixer)
│   ├── mic/             ← INMP441 reader: I2S at 22.05kHz, 60Hz high-pass, level meter
│   ├── qr/              ← minimal QR code encoder for the LCD (versions 2–3, byte mode)
│   ├── patch/           ← patch engine: patch.c (render), patch_json.c (read), patch.js
│   │                      (JavaScript copy for the editor), test/run.sh (checks they agree)
│   └── wifi/            ← joins WiFi, setup hotspot, captive DNS, setup page (setup.html)
├── tools/editor_server.py ← runs the editor on the Mac with a pretend controller
├── HARDWARE.md          ← pinouts, strip wiring, power, parts and design decisions
├── pinout.html / .pdf   ← printable bench wiring card for the diymore LCD board
└── README.md            ← this file
```

## Hardware

See HARDWARE.md for full details.

- **LCD (internal):** GPIO2 (DC), GPIO4 (RST), GPIO15 (CS), GPIO18 (SCLK), GPIO23 (MOSI), GPIO32 (BL)
- **Left knob (brightness):** GPIO33 (CLK), GPIO27 (DT), GPIO14 (SW)
- **Right knob (speed):** GPIO19 (CLK), GPIO21 (DT), GPIO22 (SW)
- **Mode switch:** GPIO16 (A, Setup), GPIO17 (B, Edit), middle pin to GND; middle position is Play
- **Microphone:** INMP441 on I2S: GPIO26 (SCK), GPIO25 (WS), GPIO35 (SD), L/R to GND (left channel)
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

- **Left knob:** brightness; press for the previous patch. **Right knob:** speed; press for the next patch. Patches come in library order, then RGB TEST and DIM TEST, and each starts from its beginning. `STARTUP_MODE` in `main.c` picks the first.
- **Mode switch:** **Setup** (A) shows the address as a QR code, or while the setup hotspot is up a WiFi QR code that joins it. **Play** (middle) is the normal screen. **Edit** (B) is a placeholder for editing patches on the device (text only, no QR code, so it can't be mistaken for Setup). The strip keeps playing in all three

Modes are patches (see **Patches** below), edited from a phone or laptop. The presets:

| Patch | What it does |
|---|---|
| Demo | Playlist: Rainbow speeding up from 0.5x to 10x, then Pulse, then Whites, 30s each. Step changes are logged |
| Rainbow | Scrolling rainbow, one cycle per 5s at 1x |
| Pulse | Warm white easing full → dark → full, 6s at 1x |
| Embers | Each LED glows and fades on its own, deep orange when dim to gold when bright |
| Candle | Fast flicker over a slow sway, orange to yellow (new, not yet seen on the strip) |
| Ocean | Blues and teals with waves travelling both ways (new, not yet seen on the strip) |
| Warm | Steady warm white |
| Whites | Whole strip easing warm white → cool white → warm over 30s |
| Pattern | Ten distinct hues stepping along the strip every 0.5s while the whole strip eases in and out over 4s |

Always after the patches, in C:

| Mode | What it does |
|---|---|
| RGB TEST | Red, green, blue thirds from the input end, to check `LED_ORDER` |
| DIM TEST | Full white at full scale (ignores brightness): 5s static gradient, then 10s fade out and 10s fade in. Each phase is logged |

- **Brightness** is perceptual 0–100% (default 50%, roughly 22% PWM), shared by all modes
- **Speed** is per mode, so each mode remembers its own. Knob steps are logarithmic: every 5 clicks doubles or halves it, from 1/8x to 8x, so each click feels the same size at any speed. Changing speed changes the rate of motion without the effect jumping, because effect time is accumulated as real time × speed
- Serial log on every change plus a heartbeat every 5s with mode, brightness, speed and measured frame rate
- LCD (Play) shows the patch, brightness and speed (each with a bar), a live preview of the strip, and a MIC level bar. The title line shows the board's address once joined, or WIFI SETUP while the hotspot is up
- **QR codes:** only in Setup, so Play's screen is never covered
- Settings are compile-time `#define`s at the top of `main.c`: `LED_PIN`, `LED_COUNT` (chips, not LEDs), `LED_ORDER`, `HOSTNAME`

### Microphone

The mic task runs on core 0 with WiFi, reading 256-sample blocks (about 12ms) at 22.05kHz through a 60Hz Butterworth high-pass, which removes the INMP441's slow offset drift and rumble. It logs both I2S slots' levels once a second; the mic answers in the first slot. Measured 2026-11-20 on the bench: a silent room reads about −70dBFS (the bottom of the MIC bar), speech −40dBFS to −30dBFS. The bench PSU's mains hum shows as small peaks at 100Hz and its multiples. Lit and dark strip measured the same, so LED current doesn't get into the mic wiring. One mic was killed by heat while moving its header; solder quickly, chip side, first time.

### Patch editor

Browse to the board's address (logged on serial once it joins WiFi). The page has two tabs, Patches and Playlists, each showing its cards as live previews; click one to edit it, ⧉ clones it. Playlists are built by dragging patch chips into the step list (or tapping a chip to add it at the end) and dragging ⠿ to reorder; this works with a finger too. It has a big preview with a history view (each LED a column, newest at the top) and a brightness graph, then the patch's settings. "Show edits on the strip as I make them" plays the unsaved version on the strip (the screen says EDIT; the button goes back to the saved one). **Save to device** keeps it in flash; Download and Upload move libraries around as JSON files; Reset to presets loads the built-in ones (saving makes it stick). The page also sets brightness and plays any saved patch. It works on a phone (single column).

To work on the pages without the board: `tools/editor_server.py`, then open `http://localhost:8000/` (the WiFi page at `/wifi` shows a board that has just joined). It serves the same files with a pretend controller.

### WiFi

- **First boot, or after moving house:** with no saved network, the board opens an open hotspot called `LED-Setup-XXXX` (last digits of its MAC). Join it from a phone and the setup page pops up as a captive portal, like hotel WiFi (Android shows a "Sign in to Wi-Fi network" notification); if it doesn't, browse to `http://4.3.2.1/wifi`. On Android, turn mobile data off first: with it on, the phone sends traffic for a no-internet network over mobile data and never reaches the board
- The page scans for networks (2.4GHz only: the ESP32 has no 5GHz radio), you tap one, enter the password and press Join. The page shows progress and, once joined, the board's new address in large type with a Copy button and the steps to get back to it (switch WiFi back, open the address or scan the QR code on the LCD), and the Join button becomes Change network. The hotspot stays up for a minute so the phone can read it, then closes
- The network is saved in flash by the WiFi driver, so later boots join straight away. The board's address is logged on serial, and it asks the router for the hostname `led-controller`
- **Can't join the saved network** (5 attempts, about 15s): the hotspot comes back, with the reason (not found, wrong password) on the page. If no phone uses it for 5 minutes, it closes and the saved network is tried again, so a router outage or power cut sorts itself out
- **Dropped after joining:** retried every 3s indefinitely, without the hotspot
- **Changing network** from home: the setup page is also at `http://<board address>/wifi` (the root redirects there until there's a control page)
- Security: the hotspot is open, and anyone on the home network can reach the setup page. Fine for a lamp; revisit before it controls anything that matters
- Tested on the DevKit with an Android phone (2026-11-19): popup, scan, join, hotspot closing after a minute, and rejoining on reboot all work. Not yet tried from an iPhone

## Patches

Older files may give a patch a `"knob"`; it's ignored now that each knob has one job.

A patch file is JSON: `{"version": 1, "patches": [...]}`, up to 16 patches and 8KB. `main/presets.json` is the reference. Each LED's brightness ("glow") comes from one or two oscillators, shaped; its colour is a palette position, picked by the glow or by a colour oscillator. Anything left out takes the default.

```json
{ "name": "Embers",                       // 1–16 characters, shown on the LCD
  "glow": {                               // leave out for always full
    "a": { "shape": "sine", "period": [3, 9], "scatter": 1 },
    "b": { "shape": "sine", "period": [3, 9], "scatter": 1 },
    "mix": "blend", "balance": 0.6,       // balance = share of a; or "mix": "multiply"
    "power": 2, "floor": 0.15, "ceiling": 1
  },
  "colour": {
    "palette": ["#FF3A00", "#FF940C"],    // 1–12 colours
    "blend": "smooth",                    // or "steps"
    "wrap": false,                        // last colour blends back to the first
    "source": "glow"                      // or "osc", with "osc": { ... } for the colour
  } }
```

- **Oscillator:** `shape` sine | triangle | saw | square | noise (default sine), `period` seconds (default 1; 0 holds still; `[shortest, longest]` gives each LED its own), `phase` start point in cycles, `spread` cycles across the strip (the wave travels; minus goes the other way), `scatter` 0–1 random start point per LED. All give 0–1: sine, triangle and square start at 0 and peak half way; saw ramps up; noise eases between random values once per period
- **Glow:** a, or a and b blended or multiplied, raised to `power` (above 1 lingers dim), then scaled to `floor`…`ceiling`. That is the LED's brightness
- **Colour:** with `"source": "glow"` the shaped glow (before floor and ceiling) picks the palette position, so dim LEDs get the first colour. Smooth blends happen in linear light so mixes stay equal-power
- **Playlist:** `{"name": "Demo", "playlist": [{"patch": "Rainbow", "secs": 30, "speed": [0.5, 10]}, ...]}` plays other patches in turn; `speed` is a number or `[start, end]` for a ramp. Playlists can't play playlists
- "Random" values come from hashing the LED's position, so they're fixed per LED and nothing is stored: the whole patch is a function of time, like the old effects
- The engine is C in `components/patch/` (no ESP-IDF calls apart from cJSON) and `patch.js` is a line-for-line JavaScript copy for the editor's preview. After changing either, run `components/patch/test/run.sh`: it builds the C on the Mac, renders the presets with both and checks they agree, and checks both reject the files in `test/bad/` with the same message
- Effect time is a 32-bit ms count, so a patch jumps once every 49 days (6 days at 8x)

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
- Single loop at 200fps in the LED task (`vTaskDelayUntil`, 5ms): read knobs and switch → render patch → apply brightness → push to strip; LCD preview every 4th frame
- Patches are pure functions of effect time (ms), so motion speed doesn't depend on frame rate, and speed control lives entirely in the main loop
- Patches render 16-bit perceptual RGB into `frame[]`; the LCD preview reads the same buffer before brightness is applied
- The library is read once into structs (`patch_set_t`); the render loop never touches JSON. The web server parses into a spare copy and swaps it in under the library's mutex, which the LED task holds while it renders. Mode changes and brightness from the page go through a queue to the LED task, which owns the settings

## What to work on next

1. **WiFi:** possibly MQTT for Home Assistant (ESP-IDF has a built-in MQTT client). A `led-controller.local` name would need mDNS, which is a managed component in ESP-IDF 5.x
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
