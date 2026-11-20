#include "beat.h"
#include <math.h>
#include <string.h>

#define BINS         (BEAT_FFT / 2)
#define HISTORY      512     // onset frames kept: about 6s at 86 frames/s
#define THRESHOLD_N  16      // frames in the moving average subtracted from the flux
#define UPDATE_EVERY 22      // frames between tempo and phase updates: about 0.25s
#define BPM_MIN      70.0f
#define BPM_MAX     180.0f
#define BPM_PREFERRED 120.0f
#define PRIOR_OCTAVES 1.0f   // width of the preference around BPM_PREFERRED
#define PHASE_BEATS   6      // past beats the phase is fitted against
#define PHASE_DECAY   0.8f   // weight of each older beat
#define PHASE_GAIN    0.3f   // how much of the phase error each update corrects
#define TEMPO_SMOOTH  0.25f  // how far each agreeing estimate moves the tempo
#define TEMPO_SWITCH  4      // disagreeing estimates in a row before jumping tempo
#define CONFIDENCE_START 0.25f   // a beat is believed once confidence reaches this...
#define CONFIDENCE_STOP  0.15f   // ...and dropped below this. Noise scores about 0.1, music 0.3 up
#define QUIET_RMS     0.0018f // below about -55dBFS is silence, whatever the onsets look like
#define LOG_SCALE   100.0f   // log(1 + LOG_SCALE·magnitude) compresses loud and quiet alike
#define BASS_TOP_HZ  260.0f  // kick drums live below this
#define BASS_WEIGHT  2.0f    // for phase: the kick marks the beat; hi-hats often sit between

typedef struct { float re, im; } complex_t;

static float rate, frame_rate;
static float window[BEAT_FFT], hann[BEAT_FFT];
static float last_log[BINS];
static float onsets[HISTORY];      // ring, newest at onset_head - 1
static float bass_onsets[HISTORY]; // the same from the bass alone
static float bass_recent[THRESHOLD_N];
static int bass_bins;
static int onset_head;
static float flux_recent[THRESHOLD_N];
static int frames;

static float period;               // frames per beat
static int disagreements;
static float pending_period;
static double position;
static float correction;           // phase still to be corrected, spread over the next update period
static float confidence;
static bool holding;
static float latest_onset;
static float loudness;             // smoothed RMS of the audio

// ---- FFT ----------------------------------------------------------------------

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

// ---- Onsets ---------------------------------------------------------------------

static int ring_index(int ago)   // 0 = newest
{
    return ((onset_head - 1 - ago) % HISTORY + HISTORY) % HISTORY;
}

static float onset_at(int ago)
{
    return onsets[ring_index(ago)];
}

// Full-spectrum flux, plus the bass part of it in *bass
static float spectral_flux(float *bass)
{
    static complex_t x[BEAT_FFT];
    for (int i = 0; i < BEAT_FFT; i++)
        x[i] = (complex_t){ window[i] * hann[i], 0 };
    fft(x, BEAT_FFT);
    float flux = 0;
    *bass = 0;
    for (int k = 1; k < BINS; k++) {
        float magnitude = sqrtf(x[k].re * x[k].re + x[k].im * x[k].im);
        float level = logf(1 + LOG_SCALE * magnitude);
        float rise = level - last_log[k];
        if (rise > 0) {
            flux += rise;
            if (k <= bass_bins) *bass += rise;
        }
        last_log[k] = level;
    }
    return flux;
}

// Flux minus its recent average, so a steady loud passage doesn't count as onsets
static float onset_strength(float flux, float *recent)
{
    float mean = 0;
    for (int i = 0; i < THRESHOLD_N; i++) mean += recent[i];
    mean /= THRESHOLD_N;
    recent[frames % THRESHOLD_N] = flux;
    float o = flux - mean;
    return o > 0 ? o : 0;
}

// ---- Tempo ----------------------------------------------------------------------

static float history[HISTORY];   // onsets unrolled, newest first, for the tempo maths

static float autocorrelation(int lag)
{
    float sum = 0;
    for (int i = 0; i + lag < HISTORY; i++)
        sum += history[i] * history[i + lag];
    return sum / (HISTORY - lag);
}

// Best beat period in frames, and how clear it is (0–1)
static float estimate_period(float *clarity)
{
    int lag_min = (int)floorf(60 * frame_rate / BPM_MAX);
    int lag_max = (int)ceilf(60 * frame_rate / BPM_MIN);
    // Without the average taken out, any steady noise would look rhythmic
    float mean = 0;
    for (int i = 0; i < HISTORY; i++)
        mean += onset_at(i);
    mean /= HISTORY;
    for (int i = 0; i < HISTORY; i++)
        history[i] = onset_at(i) - mean;
    static float r[HISTORY];
    int lag_top = lag_max * 3 + 2;
    if (lag_top >= HISTORY) lag_top = HISTORY - 1;
    for (int lag = 0; lag <= lag_top; lag++)
        r[lag] = autocorrelation(lag);
    if (r[0] <= 0) {
        *clarity = 0;
        return period;
    }

    // A comb over the beat and its multiples favours the beat itself over a
    // half or double tempo, and the prior settles what's left
    float preferred_lag = 60 * frame_rate / BPM_PREFERRED;
    int best = lag_min;
    float best_score = -1;
    static float score[HISTORY];
    for (int lag = lag_min; lag <= lag_max; lag++) {
        float comb = r[lag] + 0.5f * r[2 * lag < HISTORY ? 2 * lag : lag] + 0.25f * r[3 * lag < HISTORY ? 3 * lag : lag];
        float octaves = log2f(lag / preferred_lag) / PRIOR_OCTAVES;
        score[lag] = comb * expf(-0.5f * octaves * octaves);

        if (score[lag] > best_score) {
            best_score = score[lag];
            best = lag;
        }
    }
    *clarity = r[best] / r[0];

    // Parabolic interpolation for a period between whole frames
    float refined = best;
    if (best > lag_min && best < lag_max) {
        float a = score[best - 1], b = score[best], c = score[best + 1];
        float d = a - 2 * b + c;
        if (d < 0) refined = best + 0.5f * (a - c) / d;
    }
    return refined;
}

static void update_tempo(float estimate)
{
    if (period == 0) {
        period = estimate;
        return;
    }
    float ratio = estimate / period;
    if (ratio > 0.95f && ratio < 1.05f) {
        period += (estimate - period) * TEMPO_SMOOTH;
        disagreements = 0;
        return;
    }
    // A different tempo has to be seen a few times in a row before it's believed
    if (disagreements && fabsf(estimate / pending_period - 1) < 0.05f)
        disagreements++;
    else
        disagreements = 1;
    pending_period = estimate;
    if (disagreements >= TEMPO_SWITCH) {
        period = estimate;
        disagreements = 0;
    }
}

// ---- Phase ----------------------------------------------------------------------

// Frames since the last beat, judged by where onsets have been landing. Each
// signal is scaled by its own average so the bass can be weighted fairly.
static float phase_signal[HISTORY];   // newest first

static float frames_since_beat(void)
{
    float all = 0, bass = 0;
    for (int i = 0; i < HISTORY; i++) {
        all += onsets[i];
        bass += bass_onsets[i];
    }
    all = all > 0 ? HISTORY / all : 0;
    bass = bass > 0 ? BASS_WEIGHT * HISTORY / bass : 0;
    for (int i = 0; i < HISTORY; i++)
        phase_signal[i] = onsets[ring_index(i)] * all + bass_onsets[ring_index(i)] * bass;

    int span = (int)period;
    float best_score = -1, best = 0;
    for (int offset = 0; offset < span; offset++) {
        float score = 0, weight = 1;
        for (int k = 0; k < PHASE_BEATS; k++, weight *= PHASE_DECAY) {
            float ago = offset + k * period;
            int i = (int)ago;
            if (i + 1 >= HISTORY) break;
            float f = ago - i;
            score += weight * (phase_signal[i] * (1 - f) + phase_signal[i + 1] * f);
        }
        if (score > best_score) {
            best_score = score;
            best = offset;
        }
    }
    return best;
}

static void update_phase(void)
{
    float target = frames_since_beat() / period;            // where the beat clock should be within a beat
    float phase = (float)(position - floor(position));
    float error = target - phase;
    if (error > 0.5f) error -= 1;
    if (error < -0.5f) error += 1;
    // Big errors (a new song, a missed beat) are put right at once, but only
    // forwards; small ones are eased out by running a touch fast or slow
    if (error > 0.35f) {
        position += error;
        correction = 0;
    } else {
        correction = error * PHASE_GAIN;
    }
}

// ---- Public ---------------------------------------------------------------------

void beat_init(float sample_rate)
{
    rate = sample_rate;
    frame_rate = rate / BEAT_HOP;
    for (int i = 0; i < BEAT_FFT; i++)
        hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (BEAT_FFT - 1));
    memset(window, 0, sizeof(window));
    memset(last_log, 0, sizeof(last_log));
    memset(onsets, 0, sizeof(onsets));
    memset(flux_recent, 0, sizeof(flux_recent));
    memset(bass_onsets, 0, sizeof(bass_onsets));
    memset(bass_recent, 0, sizeof(bass_recent));
    bass_bins = (int)(BASS_TOP_HZ * BEAT_FFT / rate);
    onset_head = frames = disagreements = 0;
    period = 60 * frame_rate / BPM_PREFERRED;
    position = confidence = latest_onset = loudness = correction = 0;
    holding = true;
}

void beat_process(const float *samples)
{
    memmove(window, window + BEAT_HOP, (BEAT_FFT - BEAT_HOP) * sizeof(float));
    memcpy(window + BEAT_FFT - BEAT_HOP, samples, BEAT_HOP * sizeof(float));

    float energy = 0;
    for (int i = 0; i < BEAT_HOP; i++) energy += samples[i] * samples[i];
    loudness += (sqrtf(energy / BEAT_HOP) - loudness) * 0.02f;   // about half a second

    float bass;
    float flux = spectral_flux(&bass);
    latest_onset = onset_strength(flux, flux_recent);
    onsets[onset_head] = latest_onset;
    bass_onsets[onset_head] = onset_strength(bass, bass_recent);
    onset_head = (onset_head + 1) % HISTORY;
    frames++;

    if (frames % UPDATE_EVERY == 0 && frames > HISTORY / 2) {
        float clarity;
        float estimate = estimate_period(&clarity);
        confidence += (clarity - confidence) * 0.3f;
        if (clarity >= CONFIDENCE_STOP)
            update_tempo(estimate);
        holding = confidence < (holding ? CONFIDENCE_START : CONFIDENCE_STOP) || loudness < QUIET_RMS;
        if (!holding)
            update_phase();
    }

    // The clock runs on between updates; while holding it waits just short of
    // the next beat, so nothing pulses until the music has a beat again
    // The clock never runs backwards, so a beat can't be counted twice
    float step = 1.0f / period + correction / UPDATE_EVERY;
    float slowest = 0.25f / period;
    double next = position + (step > slowest ? step : slowest);
    if (holding && floor(next) != floor(position))
        next = floor(next) - 1e-6;
    position = next;
}

void beat_get(beat_state_t *state)
{
    *state = (beat_state_t){
        .position = position,
        .bpm = 60 * frame_rate / period,
        .confidence = confidence,
        .holding = holding,
        .onset = latest_onset,
    };
}

float beat_frame_rate(void)
{
    return frame_rate;
}
