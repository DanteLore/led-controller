#pragma once

// Beat tracking for any music, from a microphone. Plain C with no ESP-IDF
// dependencies, so it also builds on a Mac (see test/run.sh).
//
//  1. Onsets: every hop, a 512-point FFT of the latest audio. Spectral flux,
//     the sum of rises in log-magnitude across all frequencies, spikes when a
//     drum hits or a note starts.
//  2. Tempo: autocorrelation of the last ~6 seconds of onsets finds the
//     repeating period, with a gentle preference for around 120 BPM so it
//     picks the beat rather than the bar or the hi-hats.
//  3. Phase: the beat position that lines up best with recent onsets steers
//     a free-running beat clock, like a phase-locked loop. The clock runs on
//     between onsets, so lights can land on the beat rather than after it.

#include <stdbool.h>

#define BEAT_HOP  256   // samples per call to beat_process
#define BEAT_FFT  512

typedef struct {
    double position;    // beats since starting; the fraction is the phase within a beat
    float bpm;
    float confidence;   // 0–1: how clearly the music has a beat
    bool holding;       // quiet or no clear beat: the clock waits at the end of a beat
    float onset;        // latest onset strength, for meters
} beat_state_t;

void beat_init(float sample_rate);

// Feeds BEAT_HOP new samples, scaled so full scale is ±1
void beat_process(const float *samples);

// State as of the end of the last hop
void beat_get(beat_state_t *state);

// Hops per second, for turning positions into time
float beat_frame_rate(void);
