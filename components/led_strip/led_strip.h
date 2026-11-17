#pragma once

#include <stdint.h>

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
void led_strip_clear(void);

// Blocks until the frame has gone out on the wire.
void led_strip_show(void);
