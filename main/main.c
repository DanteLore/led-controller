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

#define BUTTON_LOCKOUT_US  200000
#define HEARTBEAT_US      5000000

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

static void display_init(void)
{
    st7789_init();
}

static void draw_static_labels(void)
{
    st7789_fill(COLOR_BLACK);
    st7789_draw_string(MARGIN_X, TITLE_Y, "LED CONTROLLER", COLOR_CYAN, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, MODE_Y, "MODE", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, HINT_Y, "TURN: ADJUST    PRESS: NEXT MODE", COLOR_GREY, COLOR_BLACK);
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

static void display_init(void) {}
static void draw_static_labels(void) {}
static void draw_mode(const char *name, bool draft) {}
static void draw_knob(const settings_t *s, knob_role_t knob, int slot) {}
static void draw_preview(void) {}

#endif

// ---- Input ----------------------------------------------------------------

// Lockout swallows contact bounce that outlasts a few frames
static int button_edge(void)
{
    static int was_pressed = 0;
    static int64_t last_edge_us = 0;
    int pressed = encoder_button_pressed();
    int64_t now = esp_timer_get_time();
    int edge = pressed && !was_pressed && now - last_edge_us > BUTTON_LOCKOUT_US;
    if (edge) last_edge_us = now;
    was_pressed = pressed;
    return edge;
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

    draw_static_labels();

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        int delta = encoder_get_delta();
        bool next_mode = button_edge();

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
            draw_mode(show.name, draft);
            draw_knob(&settings, show.knob, slot);
            strlcpy(last_name, show.name, sizeof(last_name));
            last_slot = slot;
            was_draft = draft;
            last_phase = NULL;
            last_step = -1;
        }

        if (delta != 0)
            adjust_knob(&settings, show.knob, slot, delta);
        if (delta != 0 || redraw_knob)
            draw_knob(&settings, show.knob, slot);

        int64_t now = esp_timer_get_time();
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
        if (++frames % PREVIEW_EVERY == 0)
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
