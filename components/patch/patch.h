#pragma once

// Patches: LED effects described as data rather than code. Each patch is a
// couple of oscillators, a shaper and a palette, read from JSON (see the
// "Patches" section of README.md for the format).
//
// Plain C with no ESP-IDF dependencies apart from cJSON, so it also builds on
// a Mac for testing. patch.js next to this is a line-for-line JavaScript copy that the
// web editor uses for its preview: change one, change the other, and run
// test/run.sh to check they still agree.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Perceptual levels: 0 = off, 65535 = full. The strip driver applies gamma.
typedef struct { uint16_t r, g, b; } rgb16_t;

#define LEVEL_FULL 65535

// Palette blends happen in linear light so mixes stay equal-power; this must
// match the strip driver's gamma
#define PATCH_GAMMA 2.2f

#define PATCH_MAX          16
#define PATCH_NAME_MAX     16
#define PATCH_PALETTE_MAX  12
#define PATCH_PLAYLIST_MAX  8
#define PATCH_JSON_MAX   8192   // biggest patch file accepted

typedef enum { SHAPE_SINE, SHAPE_TRIANGLE, SHAPE_SAW, SHAPE_SQUARE, SHAPE_NOISE, SHAPE_DECAY } patch_shape_t;

// Every oscillator gives 0–1. Sine, triangle and square start at 0 and peak
// half way through; saw ramps 0 → 1; noise drifts between random values;
// decay jumps to 1 and fades, like a drum hit.
//
// An oscillator runs on time, or on the beat clock from the microphone, when
// its periods are in beats rather than seconds (stored ×1000 either way). In
// the glow, beat oscillators are scaled by the beat energy, so pulses go quiet
// in a breakdown; colour oscillators keep stepping in time regardless.
typedef struct {
    patch_shape_t shape;
    bool beat;                // on the beat clock
    uint32_t period_min_ms;   // 0 = still. A range gives each LED its own period
    uint32_t period_max_ms;
    float phase;              // start point, in cycles
    float spread;             // cycles across the strip, so the wave travels
    float scatter;            // 0–1: how much each LED's start point is randomised
} patch_osc_t;

typedef struct {
    uint8_t patch;            // index into the set
    uint32_t ms;
    float speed_from, speed_to;
} patch_step_t;

typedef struct {
    char name[PATCH_NAME_MAX + 1];

    // Glow: brightness of each LED, before the brightness setting
    bool has_glow;            // false = always full
    bool has_b;
    bool multiply;            // a × b rather than blending them
    patch_osc_t a, b;
    float balance;            // share of a when blending
    float power, floor, ceiling;

    // Colour: a palette position for each LED, from its glow or oscillator c
    bool colour_by_osc;
    patch_osc_t c;
    bool steps;               // hard steps between colours, not smooth blends
    bool wrap;                // last colour blends back round to the first
    int palette_count;
    rgb16_t palette[PATCH_PALETTE_MAX];
    float palette_linear[PATCH_PALETTE_MAX][3];

    // A playlist plays other patches in turn instead
    int playlist_count;
    patch_step_t playlist[PATCH_PLAYLIST_MAX];
} patch_t;

typedef struct {
    int count;
    patch_t patches[PATCH_MAX];
} patch_set_t;

// Reads a patch file into set. On failure returns false with a message
// naming the patch and field, and leaves set in an unspecified state.
bool patch_parse(const char *json, patch_set_t *set, char *error, size_t error_size);

// Fills the frame for a point in effect time, in ms, and on the beat clock,
// in thousandths of a beat; beat_energy (0–1) is how hard the music hits now
void patch_render(const patch_set_t *set, int index, uint32_t ms, uint32_t beat_milli, float beat_energy,
                  rgb16_t *frame, int count);

// Which playlist step is playing, or -1 if the patch isn't a playlist
int patch_playlist_step(const patch_set_t *set, int index, uint32_t ms);
