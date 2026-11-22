#pragma once

// Between the web server and the LED task, which owns the settings: the web
// asks for changes, and the LED task publishes what's playing and how fast it runs

#include "beat.h"
#include <stdbool.h>

typedef struct {
    int mode;           // patch index, then the tests after the last patch
    bool draft;         // playing the web editor's draft instead
    int brightness;     // 0–100
    float speed;        // multiplier from the knob, for the mode playing
    int leds;
    beat_state_t beat;  // from the beat tracker
} control_state_t;

// Where the LED task's time goes, averaged over the last heartbeat: microseconds
// per frame for each stage
typedef struct {
    float fps;
    float render_us;    // patch engine
    float strip_us;     // dithering and sending the frame to the strip
    float screen_us;    // LCD drawing
    float frame_us;     // the whole loop body
} frame_timing_t;

// ---- For the web server
void control_select(int mode);
void control_set_brightness(int brightness);
void control_get_state(control_state_t *state);
void control_get_timing(frame_timing_t *timing);

// ---- For the LED task
typedef enum { COMMAND_SELECT, COMMAND_BRIGHTNESS } command_type_t;
typedef struct { command_type_t type; int value; } command_t;

void control_init(void);
bool control_next_command(command_t *command);   // false once there are none waiting
void control_publish_state(const control_state_t *state);
void control_publish_timing(const frame_timing_t *timing);
