#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "st7789.h"
#include "encoder.h"
#include "led_strip.h"
#include "effects.h"
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

#define STARTUP_MODE  MODE_DEMO

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

typedef struct {
    led_mode_t mode;
    int brightness;                     // shared by all modes
    int speed_steps[MODE_COUNT];        // each moving mode remembers its own speed
} settings_t;

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

static void draw_mode(led_mode_t mode)
{
    st7789_draw_string(MARGIN_X + 96, MODE_Y, effects_mode_name(mode), COLOR_WHITE, COLOR_BLACK);
}

static void draw_bar(int filled)
{
    if (filled > 0)
        st7789_fill_rect(MARGIN_X, KNOB_BAR_Y, filled, BAR_H, COLOR_YELLOW);
    if (filled < BAR_W)
        st7789_fill_rect(MARGIN_X + filled, KNOB_BAR_Y, BAR_W - filled, BAR_H, COLOR_GREY);
}

// Shows whichever setting the knob controls in the current mode
static void draw_knob(const settings_t *s)
{
    char text[12];
    if (effects_knob_role(s->mode) == KNOB_SPEED) {
        int steps = s->speed_steps[s->mode];
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
static void draw_mode(led_mode_t mode) {}
static void draw_knob(const settings_t *s) {}
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

static void adjust_knob(settings_t *s, int delta)
{
    if (effects_knob_role(s->mode) == KNOB_SPEED) {
        int *steps = &s->speed_steps[s->mode];
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

    const char *last_phase = NULL;
    uint32_t frames = 0, frames_at_heartbeat = 0;
    int64_t next_heartbeat_us = esp_timer_get_time() + HEARTBEAT_US;

    ESP_LOGI(TAG, "Mode %s, brightness %d%%", effects_mode_name(settings.mode), settings.brightness);

    draw_static_labels();
    draw_mode(settings.mode);
    draw_knob(&settings);

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        int delta = encoder_get_delta();
        if (delta != 0) {
            adjust_knob(&settings, delta);
            draw_knob(&settings);
        }

        if (button_edge()) {
            settings.mode = (settings.mode + 1) % MODE_COUNT;
            effect_us = 0;   // every mode starts from its beginning
            last_phase = NULL;
            draw_mode(settings.mode);
            draw_knob(&settings);
            ESP_LOGI(TAG, "Mode %s", effects_mode_name(settings.mode));
        }

        int64_t now = esp_timer_get_time();
        float speed = effects_knob_role(settings.mode) == KNOB_SPEED
                    ? speed_multiplier(settings.speed_steps[settings.mode]) : 1.0f;
        effect_us += (int64_t)((now - last_frame_us) * speed);
        last_frame_us = now;
        uint32_t ms = effect_us / 1000;

        const char *phase = effects_phase(settings.mode, ms);
        if (phase && phase != last_phase)
            ESP_LOGI(TAG, "%s", phase);
        last_phase = phase;

        effects_render(settings.mode, ms, frame, LED_COUNT);
        uint16_t level = effects_full_scale(settings.mode)
                       ? LEVEL_FULL : (uint32_t)settings.brightness * LEVEL_FULL / BRIGHTNESS_MAX;
        show_frame(level);
        if (++frames % PREVIEW_EVERY == 0)
            draw_preview();

        if (now >= next_heartbeat_us) {
            ESP_LOGI(TAG, "Running: %s, brightness %d%%, speed %.2fx, %lu fps",
                     effects_mode_name(settings.mode), settings.brightness, speed,
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
    wifi_start(HOSTNAME);
    // Interrupts are set up on the core that registers them, so the strip,
    // encoder and LCD are all initialised inside the task
    xTaskCreatePinnedToCore(led_task, "leds", LED_STACK, NULL, LED_PRIORITY, NULL, LED_CORE);
}
