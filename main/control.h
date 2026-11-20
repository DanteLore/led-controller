#pragma once

// What the web server can ask of the LED task, which owns the settings

#include <stdbool.h>

typedef struct {
    int mode;           // patch index, then the tests after the last patch
    bool draft;         // playing the web editor's draft instead
    int brightness;     // 0–100
    float speed;        // multiplier from the knob, for the mode playing
    int leds;
    float bpm;          // from the beat tracker
    float beat_confidence;
    bool beat_holding;  // no clear beat right now
    float beat_energy;  // how hard the music hits, 0–1
    bool record_wanted; // tapping started in Edit: a computer watching should record now
    float beat_belief;  // how sure the tracker is of the beat, 0–1
    bool breakdown;     // a quiet passage: the beat's kept but the lights don't pulse
} control_state_t;

void control_select(int mode);
void control_set_brightness(int brightness);
void control_get_state(control_state_t *state);
