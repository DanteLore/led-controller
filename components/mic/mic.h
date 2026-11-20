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
    bool holding;       // no beat for a while: the position waits at the end of a beat
    bool coasting;      // beat unclear just now (a breakdown): running on at the locked tempo
    float energy;       // 0–1: how hard the music hits now
    float belief;       // 0–1: how sure the tracker is of the beat
} mic_beat_t;

// The beat clock now, carried forward from the last analysis and a little
// ahead, so lights land on the beat rather than after it
void mic_beat(mic_beat_t *beat);

// Recording what the beat tracker hears (after the high-pass), as 16-bit
// samples at MIC_SAMPLE_RATE, for tuning on a computer. Start, read until
// done, stop. One recording at a time; mic_record_start returns false if busy.
#include <stddef.h>
#include <stdint.h>
bool mic_record_start(void);
size_t mic_record_read(int16_t *samples, size_t max, int timeout_ms);
void mic_record_stop(void);
bool mic_recording(void);
float mic_record_seconds(void);   // into the current recording, or -1 if none

// Beats tapped by a person during a recording, as the answer key for tuning.
// Times are esp_timer microseconds; they're kept as seconds from the
// recording's first sample, until the next recording starts.
#define MIC_TAPS_MAX 512
void mic_record_tap(int64_t when_us);
int mic_record_taps(float *seconds, int max);
