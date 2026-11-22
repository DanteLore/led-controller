#pragma once

// Fitting a beat grid to the onsets: the period that repeats best, and where
// the beats fall. Pure functions of the onset history. Private to the beat
// component.

#include "band.h"

typedef struct {
    float period;        // frames per beat
    float clarity;       // 0–1: how strongly the onsets repeat at that period
    float current_fit;   // how well the current period still fits, against the best
} period_fit_t;

// Autocorrelation of the onsets, the boom weighted up, between 70 and 180 BPM.
// Returns current_period with clarity 0 if the onsets don't vary at all.
period_fit_t fit_period(const band_t *all, const band_t *bass, float frame_rate, float current_period);

// Frames since the last beat, judged by where onsets have been landing, the
// punch weighted up. *clarity is how far the best fit stands out from the average.
float fit_phase(const band_t *all, const band_t *punch, float period, float *clarity);
