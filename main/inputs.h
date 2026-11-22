#pragma once

// The front panel: two knobs with push buttons, and the three-position switch

#include <stdint.h>

typedef enum { PANEL_PLAY, PANEL_SETUP, PANEL_EDIT } panel_t;

typedef struct {
    int brightness_delta;   // left knob turns
    int speed_delta;        // right knob turns
    int step;               // +1 right button pressed (next patch), -1 left (previous)
    panel_t panel;          // where the switch is
} inputs_t;

// Interrupts are set up on the core that calls this
void inputs_init(void);

// What's changed since the last call. Buttons and switch are debounced.
inputs_t inputs_read(int64_t now_us);
