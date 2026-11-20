#pragma once

// INMP441 I2S MEMS microphone. Reads in a background task, keeps a sound
// level for meters and runs the beat tracker (components/beat). Logs both I2S
// channels' levels, and the tempo, once a second.

#include <stdbool.h>

#define MIC_SAMPLE_RATE 22050

// Starts the reader task on the given core
void mic_start(int pin_sck, int pin_ws, int pin_sd, int core);

// Sound level for a meter: 0 = quiet room (about -70dBFS), 1 = very loud (-10dBFS).
// Rises at once and falls back gently, like a VU meter.
float mic_level(void);

typedef struct {
    double position;    // beats; the fraction is the phase within the current beat
    float bpm;
    float confidence;   // 0–1
    bool holding;       // no clear beat: the position waits at the end of a beat
} mic_beat_t;

// The beat clock now, carried forward from the last analysis and a little
// ahead, so lights land on the beat rather than after it
void mic_beat(mic_beat_t *beat);
