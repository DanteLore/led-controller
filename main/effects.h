#pragma once

#include <stdint.h>

// Perceptual levels: 0 = off, 65535 = full. The strip driver applies gamma.
typedef struct { uint16_t r, g, b; } rgb16_t;

#define LEVEL_FULL 65535

// Order here is the order the button cycles through
typedef enum {
    MODE_DEMO,
    MODE_RAINBOW,
    MODE_PULSE,
    MODE_WARM_WHITE,
    MODE_PATTERN,
    MODE_RGB_TEST,
    MODE_DIM_TEST,
    MODE_COUNT,
} led_mode_t;

// What turning the knob does in each mode
typedef enum { KNOB_BRIGHTNESS, KNOB_SPEED } knob_role_t;

// Padded to 8 characters so the LCD overwrites the previous name cleanly
const char *effects_mode_name(led_mode_t mode);

// Fills the frame for a point in effect time, in ms. Effect time runs at the
// knob's speed setting, so effects never need to know about speed themselves.
void effects_render(led_mode_t mode, uint32_t ms, rgb16_t *frame, int count);

// Describes the current phase of a sequenced mode (DEMO, DIM TEST), or NULL.
// Returns the same pointer for the whole phase, so callers can log changes.
const char *effects_phase(led_mode_t mode, uint32_t ms);

// Whether the mode ignores the brightness setting
int effects_full_scale(led_mode_t mode);

// Moving effects take speed on the knob; static or sequenced ones take brightness
knob_role_t effects_knob_role(led_mode_t mode);
