#pragma once

// KY-040 rotary encoders, quadrature state machine, ISR-driven. Up to
// ENCODER_MAX of them, each with its own pins.

#define ENCODER_MAX 2

typedef struct encoder encoder_t;

// Sets up the pins (with pull-ups) and interrupts for one encoder
encoder_t *encoder_init(int pin_clk, int pin_dt, int pin_sw);

// Returns accumulated rotation delta since last call, then resets to zero.
// Positive = clockwise, negative = counter-clockwise.
int encoder_get_delta(encoder_t *e);

// Returns 1 if the push button is currently held down, 0 otherwise.
int encoder_button_pressed(encoder_t *e);
