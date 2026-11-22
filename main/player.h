#pragma once

// What's playing: the mode (a library patch, then the hardware tests), the
// web editor's draft when there is one, brightness, each mode's speed, and the
// effect clock. Runs on the LED task only.

#include "beat.h"
#include "patch.h"
#include <stdbool.h>
#include <stdint.h>

#define BRIGHTNESS_MAX 100

typedef struct {
    int mode;
    bool draft;               // playing the web editor's draft instead
    const char *name;
    int brightness;           // 0–100, perceptual
    float speed;              // multiplier for the mode playing
    float speed_position;     // 0–1 along the knob's range
    uint16_t level;           // what to scale the frame by, 0–LEVEL_FULL
} player_status_t;

void player_init(void);

// A knob press: steps through the modes, or leaves the draft for the saved
// version of the same patch
void player_step(int step);

// From the web
void player_select(int mode);
void player_set_brightness(int brightness);

// Picks up any change to the library or draft, applies the knobs and renders
// the frame
void player_frame(int64_t now_us, int brightness_delta, int speed_delta, const beat_state_t *beat,
                  rgb16_t *frame, int count);

player_status_t player_status(void);
