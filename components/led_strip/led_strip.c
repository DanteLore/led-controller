#include "led_strip.h"
#include "driver/rmt_tx.h"
#include "esp_rom_sys.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 10MHz tick = 0.1µs. WS2811 fast mode (800kHz): 1.2µs per bit.
#define RMT_RESOLUTION_HZ 10000000
#define T0H 3
#define T0L 9
#define T1H 9
#define T1L 3

// Latch gap after each frame. Datasheets for clones vary from 50µs to 280µs.
#define RESET_US 300

// Ordered temporal dithering over a 4-frame cycle: each pixel steps between two
// adjacent PWM values in a fixed pattern shared by every pixel, so pixels at
// the same level always match. At 200fps the slowest pattern repeats at 50Hz.
static const uint8_t dither_thresholds[4] = {32, 160, 96, 224};

// Below this PWM step, alternating with the next step is a 33–100% change in
// brightness and shows as flicker, so those levels are rounded instead
#define DITHER_MIN_STEP 3

// Byte positions of R, G and B within each 3-byte pixel, per colour order
static const uint8_t order_offsets[][3] = {
    [LED_ORDER_RGB] = {0, 1, 2},
    [LED_ORDER_RBG] = {0, 2, 1},
    [LED_ORDER_GRB] = {1, 0, 2},
    [LED_ORDER_GBR] = {2, 0, 1},
    [LED_ORDER_BRG] = {1, 2, 0},
    [LED_ORDER_BGR] = {2, 1, 0},
};

static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static uint8_t *pixels;
static uint32_t frame_number;
static int pixel_count;
static const uint8_t *offsets;

void led_strip_init(int pin, int num_pixels, led_order_t order)
{
    pixel_count = num_pixels;
    pixels = calloc(num_pixels, 3);
    offsets = order_offsets[order];

    rmt_tx_channel_config_t channel_config = {
        .gpio_num          = pin,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 128,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &channel));

    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = { .level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L },
        .bit1 = { .level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));
}

void led_strip_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *p = &pixels[index * 3];
    p[offsets[0]] = r;
    p[offsets[1]] = g;
    p[offsets[2]] = b;
}

// Gamma-corrected target in 8.8 fixed point: the integer part is the PWM
// step, the fraction decides how often this frame rounds up
static uint8_t dither_channel(uint16_t level)
{
    float linear = powf(level / 65535.0f, LED_STRIP_GAMMA);
    uint32_t target = (uint32_t)(linear * 255.0f * 256.0f + 0.5f);
    uint8_t step = target >> 8;
    uint8_t fraction = target & 0xFF;
    if (step < DITHER_MIN_STEP)
        return step + (fraction >= 128);
    return step + (fraction > dither_thresholds[frame_number % 4]);
}

void led_strip_set_pixel16(int index, uint16_t r, uint16_t g, uint16_t b)
{
    led_strip_set_pixel(index, dither_channel(r), dither_channel(g), dither_channel(b));
}

void led_strip_clear(void)
{
    memset(pixels, 0, pixel_count * 3);
}

// The line idles low after each frame. The explicit reset gap means a caller
// that runs late can never start the next frame before the strip has latched.
void led_strip_show(void)
{
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(channel, encoder, pixels, pixel_count * 3, &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, -1));
    esp_rom_delay_us(RESET_US);
    frame_number++;
}
