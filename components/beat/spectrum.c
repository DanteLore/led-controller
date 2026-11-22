#include "spectrum.h"
#include "beat.h"
#include <math.h>
#include <string.h>

#define BINS       (BEAT_FFT / 2)
#define LOG_SCALE  100.0f   // log(1 + LOG_SCALE·magnitude) compresses loud and quiet alike
// Two parts of the kick do different jobs, as tapped recordings showed: its
// boom (below 110Hz) reveals the tempo, while its punch (110-600Hz) shows
// where the beat falls, because the boom band also carries bass lines, which
// in dance music often sit on the off-beat
#ifndef BASS_TOP_HZ
#define BASS_BOTTOM_HZ    0.0f
#define BASS_TOP_HZ     110.0f
#endif
#ifndef PUNCH_TOP_HZ
#define PUNCH_BOTTOM_HZ 110.0f
#define PUNCH_TOP_HZ    600.0f
#endif

typedef struct { float re, im; } complex_t;

typedef struct { int first, last; } bins_t;

static float window[BEAT_FFT], hann[BEAT_FFT];
static float last_log[BINS];
static bins_t bass_bins, punch_bins;

static void fft(complex_t *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { complex_t t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2 * (float)M_PI / len;
        complex_t step = { cosf(angle), sinf(angle) };
        for (int i = 0; i < n; i += len) {
            complex_t w = { 1, 0 };
            for (int k = 0; k < len / 2; k++) {
                complex_t a = x[i + k], b = x[i + k + len / 2];
                complex_t t = { b.re * w.re - b.im * w.im, b.re * w.im + b.im * w.re };
                x[i + k] = (complex_t){ a.re + t.re, a.im + t.im };
                x[i + k + len / 2] = (complex_t){ a.re - t.re, a.im - t.im };
                w = (complex_t){ w.re * step.re - w.im * step.im, w.re * step.im + w.im * step.re };
            }
        }
    }
}

static bool in_bins(int k, bins_t b)
{
    return k >= b.first && k <= b.last;
}

static void slide_window(const float *samples)
{
    memmove(window, window + BEAT_HOP, (BEAT_FFT - BEAT_HOP) * sizeof(float));
    memcpy(window + BEAT_FFT - BEAT_HOP, samples, BEAT_HOP * sizeof(float));
}

void spectrum_init(float sample_rate)
{
    for (int i = 0; i < BEAT_FFT; i++)
        hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (BEAT_FFT - 1));
    memset(window, 0, sizeof(window));
    memset(last_log, 0, sizeof(last_log));
    bass_bins = (bins_t){ (int)(BASS_BOTTOM_HZ * BEAT_FFT / sample_rate + 0.5f), (int)(BASS_TOP_HZ * BEAT_FFT / sample_rate) };
    if (bass_bins.first < 1) bass_bins.first = 1;
    punch_bins = (bins_t){ (int)(PUNCH_BOTTOM_HZ * BEAT_FFT / sample_rate + 0.5f), (int)(PUNCH_TOP_HZ * BEAT_FFT / sample_rate) };
}

spectrum_flux_t spectrum_process(const float *samples)
{
    slide_window(samples);
    static complex_t x[BEAT_FFT];
    for (int i = 0; i < BEAT_FFT; i++)
        x[i] = (complex_t){ window[i] * hann[i], 0 };
    fft(x, BEAT_FFT);
    spectrum_flux_t flux = { 0, 0, 0 };
    for (int k = 1; k < BINS; k++) {
        float magnitude = sqrtf(x[k].re * x[k].re + x[k].im * x[k].im);
        float level = logf(1 + LOG_SCALE * magnitude);
        float rise = level - last_log[k];
        if (rise > 0) {
            flux.all += rise;
            if (in_bins(k, bass_bins)) flux.bass += rise;
            if (in_bins(k, punch_bins)) flux.punch += rise;
        }
        last_log[k] = level;
    }
    return flux;
}
