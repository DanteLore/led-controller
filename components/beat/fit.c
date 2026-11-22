#include "fit.h"
#include <math.h>

#define BPM_MIN        70.0f
#define BPM_MAX       180.0f
#define BPM_PREFERRED 120.0f
#define PRIOR_OCTAVES   1.0f   // width of the preference around BPM_PREFERRED
#define BASS_WEIGHT     3.0f
#define PUNCH_WEIGHT    3.0f
#define PHASE_BEATS     6      // past beats the phase is fitted against
#define PHASE_DECAY     0.8f   // weight of each older beat

// Scratch space, newest first
static float signal[ONSET_HISTORY];
static float r[ONSET_HISTORY], comb[ONSET_HISTORY], score[ONSET_HISTORY];

// Each band scaled by its own average, so one can be weighted fairly against the other
static void weigh_bands(const band_t *all, const band_t *other, float other_weight)
{
    float all_scale = band_total(all), other_scale = band_total(other);
    all_scale = all_scale > 0 ? ONSET_HISTORY / all_scale : 0;
    other_scale = other_scale > 0 ? other_weight * ONSET_HISTORY / other_scale : 0;
    for (int i = 0; i < ONSET_HISTORY; i++)
        signal[i] = band_at(all, i) * all_scale + band_at(other, i) * other_scale;
}

// Without the average taken out, any steady noise would look rhythmic
static void remove_mean(void)
{
    float mean = 0;
    for (int i = 0; i < ONSET_HISTORY; i++)
        mean += signal[i];
    mean /= ONSET_HISTORY;
    for (int i = 0; i < ONSET_HISTORY; i++)
        signal[i] -= mean;
}

static float autocorrelation(int lag)
{
    float sum = 0;
    for (int i = 0; i + lag < ONSET_HISTORY; i++)
        sum += signal[i] * signal[i + lag];
    return sum / (ONSET_HISTORY - lag);
}

static float r_or_lag(int multiple, int lag)
{
    return r[multiple < ONSET_HISTORY ? multiple : lag];
}

// Parabolic interpolation for a period between whole frames, on the comb
// without the prior: the prior picks the peak, but tilted towards 120 BPM
// it would also pull the peak's position, reading fast songs slow
static float refine_peak(int best, int lag_min, int lag_max)
{
    if (best <= lag_min || best >= lag_max)
        return best;
    float a = comb[best - 1], b = comb[best], c = comb[best + 1];
    float d = a - 2 * b + c;
    return d < 0 ? best + 0.5f * (a - c) / d : best;
}

// The kick is usually the clearest marker of the beat, so the boom is weighted up
period_fit_t fit_period(const band_t *all, const band_t *bass, float frame_rate, float current_period)
{
    int lag_min = (int)floorf(60 * frame_rate / BPM_MAX);
    int lag_max = (int)ceilf(60 * frame_rate / BPM_MIN);
    weigh_bands(all, bass, BASS_WEIGHT);
    remove_mean();
    int lag_top = lag_max * 3 + 2;
    if (lag_top >= ONSET_HISTORY) lag_top = ONSET_HISTORY - 1;
    for (int lag = 0; lag <= lag_top; lag++)
        r[lag] = autocorrelation(lag);
    if (r[0] <= 0)
        return (period_fit_t){ current_period, 0, 0 };

    // A comb over the beat and its multiples favours the beat itself over a
    // half or double tempo, and the prior settles what's left
    float preferred_lag = 60 * frame_rate / BPM_PREFERRED;
    int best = lag_min;
    float best_score = -1;
    for (int lag = lag_min; lag <= lag_max; lag++) {
        comb[lag] = r[lag] + 0.5f * r_or_lag(2 * lag, lag) + 0.25f * r_or_lag(3 * lag, lag);
        float octaves = log2f(lag / preferred_lag) / PRIOR_OCTAVES;
        score[lag] = comb[lag] * expf(-0.5f * octaves * octaves);
        if (score[lag] > best_score) {
            best_score = score[lag];
            best = lag;
        }
    }
    int current = (int)(current_period + 0.5f);
    float current_fit = current >= lag_min && current <= lag_max && best_score > 0 ? score[current] / best_score : 0;
    return (period_fit_t){ refine_peak(best, lag_min, lag_max), r[best] / r[0], current_fit };
}

// Recent beats count most: each older one is weighted down by PHASE_DECAY
static float score_offset(int offset, float period)
{
    float score = 0, weight = 1;
    for (int k = 0; k < PHASE_BEATS; k++, weight *= PHASE_DECAY) {
        float ago = offset + k * period;
        int i = (int)ago;
        if (i + 1 >= ONSET_HISTORY) break;
        float f = ago - i;
        score += weight * (signal[i] * (1 - f) + signal[i + 1] * f);
    }
    return score;
}

float fit_phase(const band_t *all, const band_t *punch, float period, float *clarity)
{
    weigh_bands(all, punch, PUNCH_WEIGHT);
    int span = (int)period;
    float best_score = -1, best = 0, total = 0;
    for (int offset = 0; offset < span; offset++) {
        float score = score_offset(offset, period);
        total += score;
        if (score > best_score) {
            best_score = score;
            best = offset;
        }
    }
    *clarity = total > 0 ? best_score * span / total : 0;
    return best;
}
