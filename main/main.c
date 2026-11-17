#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "st7789.h"
#include "encoder.h"
#include "led_strip.h"

// Strip — count chips, not LEDs (24V strips usually have 6 LEDs per chip)
#define LED_PIN     13
#define LED_COUNT   30
#define LED_ORDER   LED_ORDER_BRG

#define ENC_PIN_CLK 33
#define ENC_PIN_DT  27
#define ENC_PIN_SW  14

#define FRAME_INTERVAL_US  20000   // 50Hz

#define BRIGHTNESS_DEFAULT 25
#define BRIGHTNESS_MAX     100

// Screen layout (landscape 320x170, 8x8 font)
#define MARGIN_X      20
#define TITLE_Y        8
#define MODE_Y        32
#define BRIGHT_TEXT_Y 56
#define BRIGHT_BAR_Y  72
#define BAR_W        280
#define BAR_H          8
#define PREVIEW_Y    100
#define PREVIEW_H     24
#define HINT_Y       152

#define COLOR_GREY    RGB565(110, 110, 110)

typedef enum { MODE_RAINBOW, MODE_RGB_TEST, MODE_COUNT } led_mode_t;

static const char *mode_names[MODE_COUNT] = {
    [MODE_RAINBOW]  = "RAINBOW ",
    [MODE_RGB_TEST] = "RGB TEST",
};

typedef struct { uint8_t r, g, b; } rgb_t;

static rgb_t frame[LED_COUNT];

// ---- Effects --------------------------------------------------------------

static rgb_t hue_to_rgb(uint8_t hue)
{
    uint8_t sector = hue / 43;
    uint8_t rise   = (hue - sector * 43) * 6;
    uint8_t fall   = 255 - rise;
    switch (sector) {
        case 0:  return (rgb_t){255,  rise, 0};
        case 1:  return (rgb_t){fall, 255,  0};
        case 2:  return (rgb_t){0,    255,  rise};
        case 3:  return (rgb_t){0,    fall, 255};
        case 4:  return (rgb_t){rise, 0,    255};
        default: return (rgb_t){255,  0,    fall};
    }
}

static void render_rainbow(uint32_t tick)
{
    for (int i = 0; i < LED_COUNT; i++)
        frame[i] = hue_to_rgb(tick + i * 256 / LED_COUNT);
}

// Thirds of the strip lit red, green, blue — if they don't match, fix LED_ORDER
static void render_rgb_test(void)
{
    static const rgb_t thirds[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    for (int i = 0; i < LED_COUNT; i++)
        frame[i] = thirds[i * 3 / LED_COUNT];
}

static void render(led_mode_t mode, uint32_t tick)
{
    if (mode == MODE_RAINBOW) render_rainbow(tick);
    else                      render_rgb_test();
}

static void show_frame(int brightness)
{
    int scale = brightness * 255 / BRIGHTNESS_MAX;
    for (int i = 0; i < LED_COUNT; i++)
        led_strip_set_pixel(i, frame[i].r * scale / 255,
                               frame[i].g * scale / 255,
                               frame[i].b * scale / 255);
    led_strip_show();
}

// ---- Display --------------------------------------------------------------

static void draw_static_labels(void)
{
    st7789_fill(COLOR_BLACK);
    st7789_draw_string(MARGIN_X, TITLE_Y, "LED CONTROLLER", COLOR_CYAN, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, MODE_Y, "MODE", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, BRIGHT_TEXT_Y, "BRIGHTNESS", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, HINT_Y, "TURN: BRIGHTNESS   PRESS: MODE", COLOR_GREY, COLOR_BLACK);
}

static void draw_mode(led_mode_t mode)
{
    st7789_draw_string(MARGIN_X + 96, MODE_Y, mode_names[mode], COLOR_WHITE, COLOR_BLACK);
}

static void draw_brightness(int brightness)
{
    char text[8];
    snprintf(text, sizeof(text), "%3d%%", brightness);
    st7789_draw_string(MARGIN_X + 96, BRIGHT_TEXT_Y, text, COLOR_WHITE, COLOR_BLACK);

    int filled = BAR_W * brightness / BRIGHTNESS_MAX;
    if (filled > 0)
        st7789_fill_rect(MARGIN_X, BRIGHT_BAR_Y, filled, BAR_H, COLOR_YELLOW);
    if (filled < BAR_W)
        st7789_fill_rect(MARGIN_X + filled, BRIGHT_BAR_Y, BAR_W - filled, BAR_H, COLOR_GREY);
}

// Full-brightness colours so the preview stays readable at low brightness
static void draw_preview(void)
{
    static uint16_t buf[BAR_W * PREVIEW_H];
    for (int x = 0; x < BAR_W; x++) {
        rgb_t c = frame[x * LED_COUNT / BAR_W];
        uint16_t colour = RGB565(c.r, c.g, c.b);
        uint16_t swapped = (colour >> 8) | (colour << 8);
        for (int y = 0; y < PREVIEW_H; y++)
            buf[y * BAR_W + x] = swapped;
    }
    st7789_draw_pixels(MARGIN_X, PREVIEW_Y, BAR_W, PREVIEW_H, buf);
}

// ---- Input ----------------------------------------------------------------

static int button_edge(void)
{
    static int was_pressed = 0;
    int pressed = encoder_button_pressed();
    int edge = pressed && !was_pressed;
    was_pressed = pressed;
    return edge;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// ---- Main -----------------------------------------------------------------

void app_main(void)
{
    st7789_init();
    encoder_init(ENC_PIN_CLK, ENC_PIN_DT, ENC_PIN_SW);
    led_strip_init(LED_PIN, LED_COUNT, LED_ORDER);

    led_mode_t mode = MODE_RAINBOW;
    int brightness = BRIGHTNESS_DEFAULT;
    uint32_t tick = 0;

    draw_static_labels();
    draw_mode(mode);
    draw_brightness(brightness);

    int64_t next_frame_us = esp_timer_get_time();
    while (1) {
        int delta = encoder_get_delta();
        if (delta != 0) {
            brightness = clamp(brightness + delta, 0, BRIGHTNESS_MAX);
            draw_brightness(brightness);
        }

        if (button_edge()) {
            mode = (mode + 1) % MODE_COUNT;
            draw_mode(mode);
        }

        render(mode, tick++);
        show_frame(brightness);
        draw_preview();

        next_frame_us += FRAME_INTERVAL_US;
        int64_t wait_us = next_frame_us - esp_timer_get_time();
        if (wait_us > 0)
            vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        else
            next_frame_us = esp_timer_get_time();
    }
}
