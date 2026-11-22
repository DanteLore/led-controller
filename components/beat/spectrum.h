#pragma once

// The onset detector's front end: a sliding FFT window over the audio, and
// spectral flux (the sum of rises in log-magnitude) across all frequencies and
// in the kick's two bands. Private to the beat component.

typedef struct {
    float all;     // the whole spectrum
    float bass;    // the kick's boom, below 110Hz: reveals the tempo
    float punch;   // the kick's punch, 110-600Hz: shows where the beat falls
} spectrum_flux_t;

void spectrum_init(float sample_rate);

// Slides BEAT_HOP new samples into the window and measures the flux
spectrum_flux_t spectrum_process(const float *samples);
