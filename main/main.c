#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "st7789.h"
#include "encoder.h"
#include "led_strip.h"
#include "effects.h"
#include "library.h"
#include "control.h"
#include "web.h"
#include "wifi.h"
#include "qr.h"

// Strip — count chips, not LEDs (24V strips usually have 6 LEDs per chip)
#define LED_PIN     13
#define LED_COUNT   10
#define LED_ORDER   LED_ORDER_BRG

#define ENC_PIN_CLK 33
#define ENC_PIN_DT  27
#define ENC_PIN_SW  14

// 200fps keeps temporal dithering fast enough not to flicker at dim levels.
// Each pixel takes 30µs on the wire, so this holds up to ~150 pixels.
#define FRAME_MS        5
#define PREVIEW_EVERY   4   // LCD preview at 50fps; it takes ~3ms to draw

#define STARTUP_MODE  0   // first patch in the library

#define HOSTNAME  "led-controller"

// WiFi runs on core 0, so the LEDs get core 1 to themselves: WiFi interrupts
// would otherwise delay the RMT refills mid-frame and corrupt the data
#define LED_CORE        1
#define LED_PRIORITY   10
#define LED_STACK    6144

// Perceptual, so 50% looks half as bright (roughly 22% PWM after gamma)
#define BRIGHTNESS_DEFAULT 50
#define BRIGHTNESS_MAX     100

// Speed is in knob steps; every SPEED_STEPS_PER_DOUBLING steps doubles it,
// so each click feels like the same-sized change at any speed
#define SPEED_STEPS_PER_DOUBLING  5
#define SPEED_STEPS_MAX          15   // ±15 steps = 1/8x to 8x

#define BUTTON_DEBOUNCE_US   30000   // the contacts must settle this long before a change counts
#define LONG_PRESS_US       800000   // holding the button this long shows the link screen
#define LINK_SHOW_US      60000000   // how long the link screen stays up on its own
#define WIFI_CHECK_US       500000
#define HEARTBEAT_US       5000000

// Screen layout (landscape 320x170, 8x8 font)
#define MARGIN_X      20
#define TITLE_Y        8
#define MODE_Y        32
#define MODE_W        (PATCH_NAME_MAX * 8)
#define KNOB_TEXT_Y   56
#define KNOB_BAR_Y    72
#define BAR_W        280
#define BAR_H          8
#define PREVIEW_Y    100
#define PREVIEW_H     24
#define HINT_Y       152
#define NETWORK_X    140   // right half of the title line
#define NETWORK_CHARS 20
#define QR_SCALE       4   // pixels per module: 37 modules × 4 fits the 170-pixel height
#define QR_QUIET       4   // light modules round the code, which scanners need
#define LINK_LINE_H   14

#define COLOR_GREY    RGB565(110, 110, 110)

static const char *TAG = "led-controller";

static rgb16_t frame[LED_COUNT];

// Modes are the library's patches in order, then the hardware tests
#define MODE_MAX  (PATCH_MAX + TEST_COUNT)

typedef struct {
    int mode;
    int brightness;                     // shared by all modes
    int speed_steps[MODE_MAX];          // each moving mode remembers its own speed
} settings_t;

// What the LED task is showing this frame. Read and written with the library locked.
typedef struct {
    const patch_set_t *set;   // NULL for a test
    int index;                // patch in set, or test
    const char *name;
    knob_role_t knob;
} showing_t;

static showing_t what_to_show(const settings_t *s)
{
    const patch_set_t *draft = library_draft();
    const patch_set_t *set = draft ? draft : library_patches();
    int index = draft ? library_draft_index() : s->mode;
    if (index < set->count) {
        const patch_t *p = &set->patches[index];
        return (showing_t){ set, index, p->name, p->knob };
    }
    test_t test = index - set->count;
    return (showing_t){ NULL, test, effects_test_name(test), effects_test_knob(test) };
}

static int mode_count(void)
{
    return library_patches()->count + TEST_COUNT;
}

// ---- Web requests ----------------------------------------------------------

typedef enum { COMMAND_SELECT, COMMAND_BRIGHTNESS } command_type_t;
typedef struct { command_type_t type; int value; } command_t;

static QueueHandle_t commands;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static control_state_t state;   // published by the LED task each frame

void control_select(int mode)
{
    command_t c = { COMMAND_SELECT, mode };
    xQueueSend(commands, &c, 0);
}

void control_set_brightness(int brightness)
{
    command_t c = { COMMAND_BRIGHTNESS, brightness };
    xQueueSend(commands, &c, 0);
}

void control_get_state(control_state_t *out)
{
    taskENTER_CRITICAL(&state_lock);
    *out = state;
    taskEXIT_CRITICAL(&state_lock);
}

static void publish_state(const settings_t *s, bool draft, float speed)
{
    taskENTER_CRITICAL(&state_lock);
    state = (control_state_t){ s->mode, draft, s->brightness, speed, LED_COUNT };
    taskEXIT_CRITICAL(&state_lock);
}

static float speed_multiplier(int steps)
{
    return powf(2.0f, (float)steps / SPEED_STEPS_PER_DOUBLING);
}

static void show_frame(uint16_t level)
{
    for (int i = 0; i < LED_COUNT; i++)
        led_strip_set_pixel16(i, (uint32_t)frame[i].r * level / LEVEL_FULL,
                                 (uint32_t)frame[i].g * level / LEVEL_FULL,
                                 (uint32_t)frame[i].b * level / LEVEL_FULL);
    led_strip_show();
}

// ---- Display --------------------------------------------------------------

#if CONFIG_LED_CONTROLLER_LCD

static const bool has_screen = true;

static void display_init(void)
{
    st7789_init();
}

static void draw_static_labels(void)
{
    st7789_fill(COLOR_BLACK);
    st7789_draw_string(MARGIN_X, TITLE_Y, "LED CONTROLLER", COLOR_CYAN, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, MODE_Y, "MODE", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, HINT_Y, "TURN: ADJUST  PRESS: NEXT  HOLD: QR", COLOR_GREY, COLOR_BLACK);
}

// The board's address, or how WiFi is getting on, at the right of the title line
static void draw_network(const char *text, uint16_t colour)
{
    char padded[NETWORK_CHARS + 1];
    snprintf(padded, sizeof(padded), "%*s", NETWORK_CHARS, text);
    st7789_draw_string(NETWORK_X, TITLE_Y, padded, colour, COLOR_BLACK);
}

static void draw_qr(const char *text)
{
    static bool modules[QR_SIZE_MAX][QR_SIZE_MAX];
    static uint16_t row[(QR_SIZE_MAX + 2 * QR_QUIET) * QR_SCALE * QR_SCALE];
    int n = qr_encode(text, modules);
    if (!n) return;
    int width = (n + 2 * QR_QUIET) * QR_SCALE;
    int x0 = ST7789_WIDTH - width - 12, y0 = (ST7789_HEIGHT - width) / 2;
    // A strip of QR_SCALE pixel rows per module row, quiet zone included
    for (int my = -QR_QUIET; my < n + QR_QUIET; my++) {
        for (int mx = -QR_QUIET; mx < n + QR_QUIET; mx++) {
            bool on = mx >= 0 && mx < n && my >= 0 && my < n && modules[my][mx];
            for (int py = 0; py < QR_SCALE; py++)
                for (int px = 0; px < QR_SCALE; px++)
                    row[py * width + (mx + QR_QUIET) * QR_SCALE + px] = on ? COLOR_BLACK : COLOR_WHITE;
        }
        st7789_draw_pixels(x0, y0 + (my + QR_QUIET) * QR_SCALE, width, QR_SCALE, row);
    }
}

// Lines of text on the left, the QR code on the right
static void draw_link_screen(const char *qr_text, const char *const *lines, int count)
{
    st7789_fill(COLOR_BLACK);
    int y = (ST7789_HEIGHT - count * LINK_LINE_H) / 2;
    for (int i = 0; i < count; i++, y += LINK_LINE_H)
        st7789_draw_string(16, y, lines[i], i < 2 ? COLOR_CYAN : i == count - 1 ? COLOR_GREY : COLOR_WHITE, COLOR_BLACK);
    draw_qr(qr_text);
}

// Padded so a shorter name overwrites a longer one. EDIT while the web editor's draft plays.
static void draw_mode(const char *name, bool draft)
{
    char padded[PATCH_NAME_MAX + 1];
    snprintf(padded, sizeof(padded), "%-*s", PATCH_NAME_MAX, name);
    st7789_draw_string(MARGIN_X, MODE_Y, draft ? "EDIT" : "MODE", draft ? COLOR_YELLOW : COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X + 96, MODE_Y, padded, COLOR_WHITE, COLOR_BLACK);
}

static void draw_bar(int filled)
{
    if (filled > 0)
        st7789_fill_rect(MARGIN_X, KNOB_BAR_Y, filled, BAR_H, COLOR_YELLOW);
    if (filled < BAR_W)
        st7789_fill_rect(MARGIN_X + filled, KNOB_BAR_Y, BAR_W - filled, BAR_H, COLOR_GREY);
}

// Shows whichever setting the knob controls in the current mode
static void draw_knob(const settings_t *s, knob_role_t knob, int slot)
{
    char text[12];
    if (knob == KNOB_SPEED) {
        int steps = s->speed_steps[slot];
        snprintf(text, sizeof(text), "%5.2fx", speed_multiplier(steps));
        st7789_draw_string(MARGIN_X, KNOB_TEXT_Y, "SPEED     ", COLOR_GREY, COLOR_BLACK);
        st7789_draw_string(MARGIN_X + 96, KNOB_TEXT_Y, text, COLOR_WHITE, COLOR_BLACK);
        draw_bar(BAR_W * (steps + SPEED_STEPS_MAX) / (2 * SPEED_STEPS_MAX));
    } else {
        snprintf(text, sizeof(text), "%5d%%", s->brightness);
        st7789_draw_string(MARGIN_X, KNOB_TEXT_Y, "BRIGHTNESS", COLOR_GREY, COLOR_BLACK);
        st7789_draw_string(MARGIN_X + 96, KNOB_TEXT_Y, text, COLOR_WHITE, COLOR_BLACK);
        draw_bar(BAR_W * s->brightness / BRIGHTNESS_MAX);
    }
}

// Effect colours before brightness, so the preview stays readable when dim
static void draw_preview(void)
{
    static uint16_t buf[BAR_W * PREVIEW_H];
    for (int x = 0; x < BAR_W; x++) {
        rgb16_t c = frame[x * LED_COUNT / BAR_W];
        uint16_t colour = RGB565(c.r >> 8, c.g >> 8, c.b >> 8);
        uint16_t swapped = (colour >> 8) | (colour << 8);
        for (int y = 0; y < PREVIEW_H; y++)
            buf[y * BAR_W + x] = swapped;
    }
    st7789_draw_pixels(MARGIN_X, PREVIEW_Y, BAR_W, PREVIEW_H, buf);
}

#else

static const bool has_screen = false;

static void display_init(void) {}
static void draw_static_labels(void) {}
static void draw_network(const char *text, uint16_t colour) {}
static void draw_link_screen(const char *qr_text, const char *const *lines, int count) {}
static void draw_mode(const char *name, bool draft) {}
static void draw_knob(const settings_t *s, knob_role_t knob, int slot) {}
static void draw_preview(void) {}

#endif

// ---- Input ----------------------------------------------------------------

typedef enum { PRESS_NONE, PRESS_SHORT, PRESS_LONG } press_t;

// A short press counts when it's let go; a long one as soon as it's been held
// long enough. A change only counts once the contacts have settled.
static press_t button_press(int64_t now)
{
    static bool raw_was, held, long_sent;
    static int64_t raw_since, down_us;
    bool raw = encoder_button_pressed();
    if (raw != raw_was) {
        raw_was = raw;
        raw_since = now;
    }
    if (raw != held && now - raw_since >= BUTTON_DEBOUNCE_US) {
        held = raw;
        if (held) {
            down_us = now;
            long_sent = false;
        } else if (!long_sent) {
            return PRESS_SHORT;
        }
    }
    if (held && !long_sent && now - down_us >= LONG_PRESS_US) {
        long_sent = true;
        return PRESS_LONG;
    }
    return PRESS_NONE;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// ---- Main -----------------------------------------------------------------

static void adjust_knob(settings_t *s, knob_role_t knob, int slot, int delta)
{
    if (knob == KNOB_SPEED) {
        int *steps = &s->speed_steps[slot];
        *steps = clamp(*steps + delta, -SPEED_STEPS_MAX, SPEED_STEPS_MAX);
        ESP_LOGI(TAG, "Speed %.2fx", speed_multiplier(*steps));
    } else {
        s->brightness = clamp(s->brightness + delta, 0, BRIGHTNESS_MAX);
        ESP_LOGI(TAG, "Brightness %d%%", s->brightness);
    }
}

// ---- Network on the screen --------------------------------------------------

// What the link screen offers: the editor's address once joined, or the setup
// hotspot to join while it's up. Empty while still joining.
typedef struct {
    char key[64];          // changes whenever there's something new to show
    char title[NETWORK_CHARS + 1];
    uint16_t title_colour;
} network_t;

static void read_network(network_t *n, wifi_status_t *s)
{
    wifi_get_status(s);
    if (s->state == WIFI_CONNECTED) {
        snprintf(n->key, sizeof(n->key), "http://%s/", s->ip);
        strlcpy(n->title, s->ip, sizeof(n->title));
        n->title_colour = COLOR_WHITE;
    } else if (s->hotspot) {
        // Phone cameras offer to join a network from a code like this. Our
        // hotspot's name has none of the characters that would need escaping.
        snprintf(n->key, sizeof(n->key), "WIFI:T:nopass;S:%s;;", s->hotspot_ssid);
        strlcpy(n->title, "WIFI SETUP", sizeof(n->title));
        n->title_colour = COLOR_YELLOW;
    } else {
        n->key[0] = 0;
        strlcpy(n->title, "JOINING WIFI", sizeof(n->title));
        n->title_colour = COLOR_GREY;
    }
}

static void show_link_screen(const network_t *n, const wifi_status_t *s)
{
    if (s->state == WIFI_CONNECTED) {
        const char *lines[] = { "SCAN TO OPEN", "THE EDITOR", "", "OR BROWSE TO", s->ip, "", "PRESS TO CLOSE" };
        draw_link_screen(n->key, lines, sizeof(lines) / sizeof(lines[0]));
    } else {
        const char *lines[] = { "SCAN TO JOIN", "THE SETUP WIFI", "", "OR JOIN", s->hotspot_ssid,
                                "AND THE SETUP", "PAGE POPS UP", "", "PRESS TO CLOSE" };
        draw_link_screen(n->key, lines, sizeof(lines) / sizeof(lines[0]));
    }
}

static void led_task(void *arg)
{
    display_init();
    encoder_init(ENC_PIN_CLK, ENC_PIN_DT, ENC_PIN_SW);
    led_strip_init(LED_PIN, LED_COUNT, LED_ORDER);

    settings_t settings = { .mode = STARTUP_MODE, .brightness = BRIGHTNESS_DEFAULT };

    // Effect time advances by real time × the mode's speed, so changing speed
    // changes the rate of motion without making the effect jump
    int64_t effect_us = 0;
    int64_t last_frame_us = esp_timer_get_time();

    // What was on last frame, to spot changes from the button or the web
    uint32_t last_version = library_version();
    bool was_draft = false;
    int last_slot = -1;
    char last_name[PATCH_NAME_MAX + 1] = "";
    const void *last_phase = NULL;
    int last_step = -1;

    uint32_t frames = 0, frames_at_heartbeat = 0;
    int64_t next_heartbeat_us = esp_timer_get_time() + HEARTBEAT_US;

    // The link screen covers the usual display: the address or hotspot as a QR
    // code. It comes up on its own when there's something new, and on a long press.
    network_t network = { .key = "" };
    wifi_status_t wifi;
    bool link_open = false, redraw = false;
    int64_t link_until = 0, next_wifi_check = 0;

    draw_static_labels();

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        int64_t now = esp_timer_get_time();
        int delta = encoder_get_delta();
        press_t press = button_press(now);

        bool open_link = false, close_link = false;
        if (now >= next_wifi_check) {
            next_wifi_check = now + WIFI_CHECK_US;
            char was[sizeof(network.key)];
            strlcpy(was, network.key, sizeof(was));
            read_network(&network, &wifi);
            if (strcmp(was, network.key) != 0) {
                if (!link_open) draw_network(network.title, network.title_colour);
                open_link = network.key[0] != 0;
                close_link = !open_link;
            }
        }
        if (press == PRESS_LONG && link_open)
            close_link = true;
        else if (press == PRESS_LONG)
            open_link = true;
        if (press == PRESS_SHORT && link_open)
            close_link = true;
        if (link_open && now >= link_until)
            close_link = true;
        if (open_link && network.key[0] && has_screen) {
            show_link_screen(&network, &wifi);
            link_open = true;
            link_until = now + LINK_SHOW_US;
        } else if (close_link && link_open) {
            link_open = false;
            draw_static_labels();
            draw_network(network.title, network.title_colour);
            redraw = true;
        }
        bool next_mode = press == PRESS_SHORT && !close_link;

        // The button leaves the editor's draft for the saved version of the
        // same patch; otherwise it moves on to the next mode
        library_lock();
        bool draft = library_draft() != NULL;
        if (next_mode && draft)
            settings.mode = library_draft_index();
        else if (next_mode)
            settings.mode = (settings.mode + 1) % mode_count();
        library_unlock();
        if (next_mode && draft)
            library_stop_draft();

        bool redraw_knob = false;
        command_t command;
        while (xQueueReceive(commands, &command, 0)) {
            if (command.type == COMMAND_BRIGHTNESS) {
                settings.brightness = clamp(command.value, 0, BRIGHTNESS_MAX);
                redraw_knob = true;
            } else {
                settings.mode = command.value;
                library_stop_draft();
            }
        }

        library_lock();
        if (library_version() != last_version || settings.mode >= mode_count()) {
            last_version = library_version();
            if (settings.mode >= mode_count())
                settings.mode = 0;
            last_slot = -1;   // the patch at this position may have changed
        }
        showing_t show = what_to_show(&settings);
        draft = library_draft() != NULL;
        // Speed is remembered per position, so a draft uses its patch's setting
        int slot = show.set ? show.index : library_patches()->count + show.index;

        // A different mode starts from its beginning; edits to the same draft
        // patch carry on from where they are, so changes can be seen smoothly
        bool changed = slot != last_slot || draft != was_draft || strcmp(show.name, last_name) != 0;
        if (changed) {
            if (slot != last_slot || draft != was_draft)
                effect_us = 0;
            ESP_LOGI(TAG, "%s %s", draft ? "Editing" : "Mode", show.name);
            redraw = true;
            strlcpy(last_name, show.name, sizeof(last_name));
            last_slot = slot;
            was_draft = draft;
            last_phase = NULL;
            last_step = -1;
        }

        if (delta != 0)
            adjust_knob(&settings, show.knob, slot, delta);
        if (redraw && !link_open) {
            draw_mode(show.name, draft);
            redraw = false;
            redraw_knob = true;
        }
        if ((delta != 0 || redraw_knob) && !link_open)
            draw_knob(&settings, show.knob, slot);

        float speed = show.knob == KNOB_SPEED ? speed_multiplier(settings.speed_steps[slot]) : 1.0f;
        effect_us += (int64_t)((now - last_frame_us) * speed);
        last_frame_us = now;
        uint32_t ms = effect_us / 1000;

        bool full_scale = false;
        if (show.set) {
            patch_render(show.set, show.index, ms, frame, LED_COUNT);
            int step = patch_playlist_step(show.set, show.index, ms);
            if (step >= 0 && step != last_step) {
                const patch_t *p = &show.set->patches[show.index];
                ESP_LOGI(TAG, "%s: %s", p->name, show.set->patches[p->playlist[step].patch].name);
            }
            last_step = step;
        } else {
            effects_render_test(show.index, ms, frame, LED_COUNT);
            const char *phase = effects_test_phase(show.index, ms);
            if (phase && phase != last_phase)
                ESP_LOGI(TAG, "%s", phase);
            last_phase = phase;
            full_scale = effects_test_full_scale(show.index);
        }
        library_unlock();

        uint16_t level = full_scale ? LEVEL_FULL : (uint32_t)settings.brightness * LEVEL_FULL / BRIGHTNESS_MAX;
        show_frame(level);
        if (++frames % PREVIEW_EVERY == 0 && !link_open)
            draw_preview();
        publish_state(&settings, draft, speed);

        if (now >= next_heartbeat_us) {
            ESP_LOGI(TAG, "Running: %s, brightness %d%%, speed %.2fx, %lu fps",
                     last_name, settings.brightness, speed,
                     (unsigned long)((frames - frames_at_heartbeat) * 1000000LL / (now - next_heartbeat_us + HEARTBEAT_US)));
            frames_at_heartbeat = frames;
            next_heartbeat_us = now + HEARTBEAT_US;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();
    library_init();
    commands = xQueueCreate(8, sizeof(command_t));
    wifi_start(HOSTNAME);
    web_start(wifi_web_server());
    // Interrupts are set up on the core that registers them, so the strip,
    // encoder and LCD are all initialised inside the task
    xTaskCreatePinnedToCore(led_task, "leds", LED_STACK, NULL, LED_PRIORITY, NULL, LED_CORE);
}
