#pragma once

// INMP441 I2S MEMS microphone. Reads in a background task, keeps a sound
// level for meters and runs the beat tracker (components/beat). Logs both I2S
// channels' levels, and the tempo, once a second.

#include "beat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_SAMPLE_RATE 22050

// Starts the reader task on the given core
void mic_start(int pin_sck, int pin_ws, int pin_sd, int core);

// Sound level for a meter: 0 = quiet room (about -70dBFS), 1 = very loud (-10dBFS).
// Rises at once and falls back gently, like a VU meter.
float mic_level(void);

// The beat clock now, carried forward from the last analysis and a little
// ahead, so lights land on the beat rather than after it
void mic_beat(beat_state_t *beat);

// Recording what the beat tracker hears (after the high-pass), as 16-bit
// samples at MIC_SAMPLE_RATE, for tuning on a computer. Start, read until
// done, stop. One recording at a time; mic_record_start returns false if busy.
bool mic_record_start(void);
size_t mic_record_read(int16_t *samples, size_t max, int timeout_ms);
void mic_record_stop(void);
