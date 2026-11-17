# Hardware Reference

> **Reference document.** The board, LCD and encoder sections come from bt-mixer, where they were confirmed working.

---

## Overview

| Part | Detail |
|------|--------|
| Controller | diymore ESP32-WROOM-32 with 1.9" ST7789V2 LCD |
| Input | KY-040 rotary encoder with push button |
| Strip | 24V addressable strip offcut, Sunmoon **SM16703SP3** driver ICs (WS2811 protocol, 800kHz) |
| Level shifter | **SN74AHCT125N** (DIP-14), 3.3V → 5V on the data line |
| Power — strip | 24V PSU, direct to the strip |
| Power — ESP32 | USB during development; **LM2596** adjustable buck (24V → 5V) when running standalone |

---

## Parts List

Status as of 2026-11-17: level shifters, capacitors and buck converters on order. Firmware is on hold until they arrive.

| Item | Part | Qty | Status | Purpose |
|------|------|-----|--------|---------|
| Controller | diymore ESP32 + 1.9" LCD | 1 | Have (from bt-mixer) | |
| Encoder | KY-040 | 1 | Have (from bt-mixer) | Brightness / mode |
| Strip | 24V SM16703SP3 offcut | 1 | Have | Pads: 24V, DIN, GND |
| Level shifter | SN74AHCT125N, DIP-14 (PENGLIN 10-pack) | 1 | Ordered | Data line 3.3V → 5V |
| Decoupling cap | 0.1µF ceramic | 1 | Ordered | Across 74AHCT125 pins 14 and 7 |
| Buck converter | LM2596 adjustable module (5-pack), 3–40V in | 1 | Ordered | 24V → 5V for ESP32 VIN |
| Series resistor | 330Ω | 1 | Have | Data line, at the shifter output |
| Bulk cap | 1000µF electrolytic, **35V+** | 1 | To get | Across 24V/GND at strip input |
| Fuse | Inline holder + 3–5A fuse | 1 | To get | 24V feed |
| PSU | 24V, sized from strip W/m × length + 30% | 1 | To get / borrow | Strip power |
| Nice to have | DC barrel → screw terminal, 14-pin DIP socket, stripboard, 3-pin JST SM pair, heat-shrink | | To get | Assembly |

---

## Design Decisions

### Level shifter — SN74AHCT125N

The SM16703 regulates its own supply down to about 5V even on a 24V strip, and WS2811-style inputs usually need around 0.7 × VDD (about 3.5V) to read a logic high. The ESP32's 3.3V output sits just below that. It often works on a short data wire, but is marginal and can glitch with longer wires, noise or temperature. The AHCT family has TTL input thresholds (high ≥ 2.0V) and outputs a full 5V, so it fixes this for pennies. The N suffix is the through-hole DIP-14 package.

Testing direct 3.3V first is safe, provided the strip's 24V pad goes nowhere near the ESP32: GPIO13 → 330Ω → DIN, short wire, common ground.

| 74AHCT125 pin | Name | Connect to |
|---|---|---|
| 14 | VCC | 5V (board VIN) |
| 7 | GND | GND |
| 1 | 1OE | GND (enables channel 1) |
| 2 | 1A | ESP32 GPIO13 |
| 3 | 1Y | 330Ω → strip DIN |
| 5, 9, 12 | 2A, 3A, 4A | GND (unused inputs must not float) |
| 4, 10, 13 | 2OE, 3OE, 4OE | 5V (disables unused outputs) |

Fit the 0.1µF capacitor across pins 14 and 7, as close to the chip as possible. Generic-brand chips: if 1Y doesn't reach about 5V with GPIO13 high, try another from the pack.

### Buck converter — LM2596, not MP1584

| | MP1584 (DollaTek, used in bunny-cam) | LM2596 (chosen) |
|---|---|---|
| Max input | 28V | **40V** |
| Headroom on 24V | ~4V | ~16V |
| Rated / realistic continuous current | 3A / ~1.5–2A | 3A / ~2A |
| Efficiency, 24V → 5V | ~85–90% | ~75–80% |
| Switching frequency | 1.5MHz | 150kHz |
| Size | Tiny | ~43 × 21mm |

The MP1584 would probably work, but a 24V PSU reading 25V or a switch-on spike leaves almost no margin, and these boards can fail short, putting the input voltage on the output. The LM2596 is less efficient but has a comfortable input rating. Current isn't a deciding factor: the ESP32, LCD and shifter draw about 100–250mA (Wi-Fi peaks ~500mA), which is about 0.13A from the 24V side at worst. Keep the MP1584s for 12V projects.

Before first use:
1. Connect 24V to IN+/IN− with nothing on the output.
2. Turn the multi-turn trim pot until OUT+/OUT− reads **5.0V** on a meter. It may take 10+ turns before it moves.
3. Then wire OUT+ → board VIN and OUT− → GND.

**Don't power from USB and VIN at the same time.** Unplug the buck before plugging in USB to flash.

### Power routing

The strip takes 24V straight from the PSU; it never goes through the buck converter. The 1000µF/35V capacitor at the strip input absorbs switch-on spikes, and the inline fuse protects against a short on the 24V side. All grounds (PSU, strip, buck, ESP32, shifter) are common.

### Data pin — GPIO13

Free on this board, not a strapping pin, and not shared with the LCD's SPI pins (GPIO18/23).

---

## LED Strip — SM16703SP3, 24V

- WS2811-compatible single-wire protocol; each chip takes the first 24 bits and passes the rest on, so there is no fixed addressing and a cut strip works from whichever end receives data.
- On 24V strips each chip usually drives a group of 6 LEDs. `LED_COUNT` in `main.c` is the number of **chips**, not LEDs.
- Colour order varies by strip maker. Use the RGB TEST mode: the strip should show red, green, blue thirds in that order. If not, change `LED_ORDER`.
- Data flows in the direction of the arrows printed on the strip — connect to the end the arrows point away from.
- Solder to the three pads at a cut line (24V, DIN, GND). A segment cut off-line may be dead; leave it out of `LED_COUNT`.

### Wiring

```
24V PSU (+) ──[fuse 3–5A]──┬──────────────────────── strip 24V
                           ├── 1000µF/35V ──┐
                           └── LM2596 IN+   │
24V PSU (–) ───────────────┬────────────────┴─────── strip GND
                           └── LM2596 IN−
LM2596 OUT+ (5.0V) ──┬── ESP32 VIN
                     └── 74AHCT125 pin 14 (VCC)
LM2596 OUT− ─────────┬── ESP32 GND ── 74AHCT125 pin 7 (GND)
                     └── (joined to strip GND via the PSU −)

ESP32 GPIO13 ── 74AHCT125 pin 2 (1A)
74AHCT125 pin 3 (1Y) ──[330Ω]── strip DIN
```

> **Safety:** a wiring mistake here puts 24V on an ESP32 pin and kills it instantly. Check the buck output with a meter before connecting the board, and keep strip 24V well away from DIN.

### Timing (RMT, 10MHz resolution)

| Symbol | Ticks | Time |
|--------|-------|------|
| T0H / T0L | 3 / 9 | 0.3µs / 0.9µs |
| T1H / T1L | 9 / 3 | 0.9µs / 0.3µs |
| Reset | — | Provided by the 20ms frame gap |

---

## Attached Peripherals

### RUNCCI-YUN KY-040 Rotary Encoder

Incremental rotary encoder with push button. Has onboard pull-up resistors on all signal lines — no internal pull-ups required in firmware.

| KY-040 Pin | Signal | GPIO | Board Label (Left Header) |
|-----------|--------|------|--------------------------|
| VCC | Power | 3.3V | Right header, bottom pin |
| GND | Ground | GND | Left header, bottom pin |
| CLK | Encoder A | GPIO33 | CH8 (ADC5) |
| DT | Encoder B | GPIO27 | CH7 (ADC17) |
| SW | Push button (active-low) | GPIO14 | CH6 (ADC16) |

**How it works:** CLK and DT produce quadrature signals 90° out of phase. Interrupt on CLK; read DT to determine direction — states differ = clockwise, states match = counter-clockwise. SW goes LOW when the knob is pressed.

---

## ESP32 Board — diymore ESP32-WROOM-32 with 1.9" LCD Display

---

### Board overview

The diymore ESP32 board integrates the ESP32-WROOM-32 module with a 1.9-inch ST7789V2 LCD screen, combining full ESP32 functionality with an onboard colour display. The LCD driver code is based on Waveshare's standard LCD library (confirmed from product documentation showing `LCD_Driver.cpp`, `WIFI_Driver.c` etc., authored by the Waveshare team, dated 2023-11-22). Suitable for beginners and professionals building IoT projects.

---

## Hardware Specifications

| Parameter | Value |
|-----------|-------|
| Module | ESP32-WROOM-32 |
| USB Interface | Type-C |
| Working Voltage | USB 5V |
| USB-Serial Chip | CH340 |
| LCD Size | 1.9 inches |
| LCD Resolution | 170 × 320 pixels |
| LCD Driver IC | ST7789V2 |
| LCD Colour Depth | 262K colours (RGB565) |
| LCD Interface | 4-wire SPI (write-only, no MISO) |
| SPI Flash | 32Mbit default |
| Serial Speed | 115200 bps |
| WiFi | 2.4GHz, 802.11 b/g/n, 2412–2484 MHz |
| Bluetooth | 4.2 BR/EDR and BLE |
| Antenna | Onboard PCB, 2dBi gain |

---

## Supported Interfaces

UART, SPI, SDIO, I2C, PWM, I2S, IR, ADC, DAC

---

## Pinout — Header Pins

### Left Side (top to bottom)

| Outer Label | Inner Label | GPIO |
|-------------|-------------|------|
| — | EN | RESET |
| — | ADC0 | GPIO36 |
| — | ADC3 | GPIO39 |
| — | ADC6 | GPIO34 |
| — | ADC7 | GPIO35 |
| CH9 | ADC4 | GPIO32 |
| CH8 | ADC5 | GPIO33 |
| DAC1 | ADC18 | GPIO25 |
| DAC2 | ADC19 | GPIO26 |
| CH7 | ADC17 | GPIO27 |
| CH6 | ADC16 | GPIO14 |
| CH5 | ADC15 | GPIO12 |
| CH4 | ADC14 | GPIO13 |
| — | — | GND |
| — | — | VIN (5V) |

### Right Side (top to bottom)

| GPIO | Function | Outer Label |
|------|----------|-------------|
| GPIO23 | MOSI (SPI) | — |
| GPIO22 | I2C SCL | — |
| GPIO1 | UART0 TX | — |
| GPIO3 | UART0 RX | — |
| GPIO21 | I2C SDA | — |
| GPIO19 | MISO (SPI) | — |
| GPIO18 | SCK (SPI) | — |
| GPIO5 | SS/CS (SPI) | — |
| GPIO17 | UART2 TX | — |
| GPIO16 | UART2 RX | — |
| GPIO4 | ADC10 | CH0 |
| GPIO2 | ADC12 | CH2 |
| GPIO15 | ADC13 | CH3 |
| — | GND | — |
| — | 3.3V | — |

### Onboard Buttons

| Button | Function |
|--------|----------|
| BOOT | Hold during power-on/reset to enter firmware download mode |
| EN | Hardware reset |

---

## Internal LCD Wiring — Confirmed ✅

The onboard ST7789V2 display is wired to the ESP32 internally via dedicated GPIOs. These are **separate from and independent of the broken-out SPI header pins**. Confirmed from the ideaspark board (identical hardware design) official documentation, and cross-referenced against multiple working community configurations.

| LCD Signal | GPIO | Notes |
|-----------|------|-------|
| MOSI | GPIO23 | Shared with header — do not use header GPIO23 for other SPI devices simultaneously |
| SCLK | GPIO18 | Shared with header |
| CS | GPIO15 | Strapping pin — dedicated to LCD |
| DC (Data/Command) | GPIO2 | Strapping pin — see WiFi caveat below |
| RST | GPIO4 | Dedicated to LCD reset |
| BL (Backlight) | GPIO32 | PWM-capable — can be used to dim the display |

> **Strapping pin note:** GPIO2 and GPIO15 are ESP32 strapping pins. The board manages their boot states correctly in normal operation. Do not add strong external pull-resistors to these pins.

> **WiFi + GPIO2 caveat:** GPIO2 is on ADC2, which shares resources with the WiFi radio. The DC signal is output-only so display operation is unaffected, but do not attempt to use GPIO2 for analogue reads when WiFi is active.

---

## Backlight Dimming (PWM — ESP-IDF)

GPIO32 is PWM-capable via the LEDC peripheral:

```c
#include "driver/ledc.h"

ledc_timer_config_t timer = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .timer_num       = LEDC_TIMER_0,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .freq_hz         = 5000,
    .clk_cfg         = LEDC_AUTO_CLK,
};
ledc_timer_config(&timer);

ledc_channel_config_t ch = {
    .gpio_num   = 32,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel    = LEDC_CHANNEL_0,
    .timer_sel  = LEDC_TIMER_0,
    .duty       = 200,   // 0 = off, 255 = full brightness
    .hpoint     = 0,
};
ledc_channel_config(&ch);
```

---

## GPIO Usage Guide

### Pins used internally by the LCD — do not reuse

| GPIO | LCD Function |
|------|-------------|
| GPIO2 | DC (Data/Command) |
| GPIO4 | RST |
| GPIO15 | CS |
| GPIO18 | SCLK — shared with header |
| GPIO23 | MOSI — shared with header |
| GPIO32 | Backlight |

### Free to use for your project

| GPIO | Notes |
|------|-------|
| GPIO19 | Free, broken out on header |
| GPIO21 | Free, broken out on header |
| GPIO22 | Free, broken out on header |
| GPIO26 | Also DAC2 |
| GPIO34, 35, 36, 39 | **Input only** — no pull-up/down, no output |

### In use by peripherals

| GPIO | Peripheral | Signal |
|------|-----------|--------|
| GPIO14 | KY-040 rotary encoder | SW (push button) |
| GPIO27 | KY-040 rotary encoder | DT (encoder B) |
| GPIO33 | KY-040 rotary encoder | CLK (encoder A) |
| GPIO13 | LED strip | Data out (RMT) → level shifter → strip DIN |

### Strapping pins — handle with care

| GPIO | State at boot | In normal use |
|------|---------------|---------------|
| GPIO0 | LOW = flash download mode | Fine after boot |
| GPIO2 | Must be LOW or floating | Used for LCD DC |
| GPIO5 | Must be HIGH | Fine after boot |
| GPIO12 | Controls flash voltage — leave unconnected | Avoid |
| GPIO15 | Must be HIGH | Used for LCD CS |

### ADC2 pins — avoid for analogue when WiFi is active

GPIO2, 4, 12, 13, 14, 15, 25, 26, 27 are all ADC2 channels. ADC2 is unreliable for analogue reads while WiFi is running. Use **ADC1 pins** (GPIO32–39) for any analogue sensing in WiFi projects.

---

## ESP-IDF Components

All built into ESP-IDF — no external dependencies needed for display or core peripherals:

| Component | Header | Purpose |
|-----------|--------|---------|
| SPI Master | `driver/spi_master.h` | SPI bus and device management |
| GPIO | `driver/gpio.h` | Pin control |
| LEDC | `driver/ledc.h` | PWM for backlight dimming |
| WiFi | `esp_wifi.h` | 2.4GHz WiFi |
| HTTP Server | `esp_http_server.h` | Embedded web server |
| MQTT | `mqtt_client.h` | MQTT (via `idf_component.yml`) |

---

## Development Environment Setup

### ESP-IDF

See README.md for toolchain and `flash.sh` usage.

> **Note:** ESP-IDF defines `SPI_HOST` as `SPI1_HOST`. Use `SPI2_HOST` directly for the LCD. See Known Issues.

### Firmware Download Mode

If upload fails: hold **BOOT**, press **EN** to reset, release **BOOT** once the upload starts. Alternatively, hold BOOT while plugging in the USB cable.

---

## Official Sample Code

The official diymore demo sketch is called **`WIFI_AP_LCD`** and consists of these files (all based on Waveshare's standard library structure):

| File | Purpose |
|------|---------|
| `LCD_Driver.cpp` / `.h` | ST7789 SPI driver |
| `GUI_Paint.cpp` / `.h` | Drawing primitives (lines, rectangles, text, images) |
| `WIFI_Driver.cpp` / `.h` | WiFi access point setup |
| `font12.cpp` etc. | Bitmap font data |

After flashing, the board creates a WiFi access point (`HW-656 Server` at `192.168.4.1`). Connect to it and browse to that address to control display output (monochrome or image mode).

---

## Known Issues & Caveats

| Issue | Detail |
|-------|--------|
| **GPIO2 / WiFi** | GPIO2 (DC pin) is ADC2; ADC2 shares resources with WiFi. No display issue, but don't use GPIO2 for ADC reads when WiFi is active. |
| **Strapping pins** | GPIO2 and GPIO15 are boot strapping pins used by the LCD. Do not add external pull-ups/downs. |
| **GPIO12** | Controls ESP32 flash voltage. Never pull HIGH at boot — can prevent startup. Avoid using it. |
| **Upload failures** | Drop upload speed from 921600 to 460800 if experiencing timeouts. |
| **USB-C to USB-C** | Some users report USB-C to USB-C cables not powering the board — use USB-A to USB-C. |
| **ADC2 with WiFi** | All ADC2 pins (GPIO2, 4, 12–15, 25–27) are unreliable for analogue reads when WiFi is active. |
| **SPI bus sharing** | GPIO18 (SCLK) and GPIO23 (MOSI) are shared between the LCD and the external SPI header. Adding another SPI device on these pins will work electrically, but requires careful CS management. |
| **Board type** | Must use "ESP32 Wrover Module" not generic esp32dev — wrong partition/memory layout otherwise. |
| **ST7789V2 column offset** | The frame buffer is 240 columns wide but the panel is only 170. The visible area starts at column 35. Any SPI driver must add a column offset of 35 to all CASET commands, otherwise drawing is misaligned and old GRAM content shows at the edges. Row offset is 0. |
| **SPI clock limit (ESP-IDF)** | ESP-IDF rejects clock speeds above ~26.6 MHz for SPI devices on non-IOMUX pins in full-duplex mode. Use 26 MHz. Also set `SPI_DEVICE_HALFDUPLEX` since the display is write-only — this removes the frequency restriction entirely. |
| **SPI_HOST name conflict** | ESP-IDF defines `SPI_HOST` as `SPI1_HOST` in `hal/spi_types.h`. Do not redefine it. Use `SPI2_HOST` directly or name your macro something else (e.g. `LCD_SPI_HOST`). |

---

## Typical Applications

- WiFi-connected IoT sensors with live display readout
- Smart home status dashboards
- Mesh networking nodes with status screen
- BLE peripheral devices
- WiFi access point with web configuration UI
- Data logging with on-device display
- General Arduino-compatible prototyping

---

## Resources

| Resource | Link |
|----------|------|
| diymore official store | https://www.diymore.cc |
| Amazon UK listing | https://www.amazon.co.uk/dp/B0DK399Y9Q |
| ESP32 Arduino Core docs | https://docs.espressif.com/projects/arduino-esp32/en/latest/ |
| ESP-IDF GPIO reference | https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/gpio.html |
| TFT_eSPI library | https://github.com/Bodmer/TFT_eSPI |
| LovyanGFX library | https://github.com/lovyan03/LovyanGFX |
| LVGL | https://lvgl.io |
| Waveshare 1.9" LCD wiki | https://www.waveshare.com/wiki/1.9inch_LCD_Module |
| ST7789 170×320 TFT_eSPI config | https://github.com/mboehmerm/IPS-Display-ST7789-170x320 |
| ideaspark example code (same HW) | https://github.com/jwkunz/ideaspark_esp32_114LCD_example |
| Board manager JSON URL | https://dl.espressif.com/dl/package_esp32_index.json |