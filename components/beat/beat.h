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
//
// Two things come out, like rhythm and dancing: the beat clock, which keeps
// time through everything, and the energy, how hard the music is hitting right
// now. Lights can pulse with the energy and keep time with the clock, so they
// go quiet in a breakdown and come back on the beat for the drop.
//
// Songs rarely change tempo, so once locked it's sticky: a bang on the table
// or a busy bar can only nudge it. Through a quiet breakdown it coasts at the
// locked tempo and phase, ready for the drop, and only stops after a long
// spell without a beat, or a short one of near silence.

#include <stdbool.h>

#define BEAT_HOP  256   // samples per call to beat_process
#define BEAT_FFT  512

typedef struct {
    double position;    // beats since starting; the fraction is the phase within a beat
    float bpm;
    float confidence;   // 0–1: how clearly the music has a beat
    bool holding;       // no beat for a while: the clock waits at the end of a beat
    bool coasting;      // beat unclear just now, so running on at the locked tempo
    float energy;       // 0–1: how hard the music hits now, against its own kicks while the beat
                        // was clear. Jumps up on a hit, falls away over about half a second
    float belief;       // 0–1: how sure it is of the beat, earned from hits landing where expected
    bool breakdown;     // the music has gone quiet and the beat has faded: keep time, don't move
    float pulse;        // 0–1: how hard the lights should pulse on the beat now: the energy, bolder
                        // with belief, and nothing in a breakdown
    float onset;        // latest onset strength, for meters
} beat_state_t;

void beat_init(float sample_rate);

// Feeds BEAT_HOP new samples, scaled so full scale is ±1
void beat_process(const float *samples);

// State as of the end of the last hop
void beat_get(beat_state_t *state);

// Hops per second, for turning positions into time
float beat_frame_rate(void);
