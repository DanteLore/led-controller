#pragma once

#include <stdint.h>

// Perceptual level → PWM: pwm = level^GAMMA. Exposed so effects can work in
// linear light where they need to (e.g. keeping colour mixes equal-power).
#define LED_STRIP_GAMMA 2.2f

// WS2811-protocol pixel strips (SM16703, WS2811, WS2812 etc.), driven by RMT.
// Colour order is the order bytes go down the wire; it varies by strip maker.
typedef enum {
    LED_ORDER_RGB,
    LED_ORDER_RBG,
    LED_ORDER_GRB,
    LED_ORDER_GBR,
    LED_ORDER_BRG,
    LED_ORDER_BGR,
} led_order_t;

void led_strip_init(int pin, int num_pixels, led_order_t order);
void led_strip_set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);

// 16-bit perceptual levels (0 = off, 65535 = full). Gamma-corrects so equal
// steps look equal, then dithers down to the strip's 8 bits: a level between
// two PWM steps alternates between them in a 4-frame pattern shared by all
// pixels. The dimmest few steps are rounded instead, to avoid visible flicker.
// Call for every pixel on every frame; dithering relies on a steady frame rate.
void led_strip_set_pixel16(int index, uint16_t r, uint16_t g, uint16_t b);
void led_strip_clear(void);

// Blocks until the frame has gone out on the wire.
void led_strip_show(void);
