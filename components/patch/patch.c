// Rendering. patch.js mirrors this line for line; keep them in step.

#include "patch.h"
#include <math.h>

#define SINE_STEPS   1024
#define GAMMA_STEPS  4096

// Lookup tables keep the per-LED cost down: cosf and powf are slow on the ESP32
static float sine_table[SINE_STEPS + 1];          // (1 − cos 2πp) / 2
static uint16_t perceptual_table[GAMMA_STEPS + 1]; // linear light → perceptual level
static bool tables_ready;

static void make_tables(void)
{
    for (int i = 0; i <= SINE_STEPS; i++)
        sine_table[i] = (1.0f - cosf(6.2831853f * i / SINE_STEPS)) * 0.5f;
    for (int i = 0; i <= GAMMA_STEPS; i++)
        perceptual_table[i] = (uint16_t)(powf((float)i / GAMMA_STEPS, 1.0f / PATCH_GAMMA) * LEVEL_FULL + 0.5f);
    tables_ready = true;
}

static float lookup_sine(float p)
{
    float x = p * SINE_STEPS;
    int i = (int)x;
    return sine_table[i] + (sine_table[i + 1] - sine_table[i]) * (x - i);
}

static float to_perceptual(float linear)
{
    if (linear <= 0) return 0;
    if (linear >= 1) return LEVEL_FULL;
    float x = linear * GAMMA_STEPS;
    int i = (int)x;
    return perceptual_table[i] + (perceptual_table[i + 1] - perceptual_table[i]) * (x - i);
}

// Cheap integer hash, so each LED gets its own fixed "random" numbers
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d;
    x ^= x >> 15; x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static float smooth(float f)
{
    return f * f * (3 - 2 * f);
}

// slot keeps a, b and c apart, so the same LED gets different random numbers in each
static float osc_value(const patch_osc_t *o, int slot, int led, int count, uint32_t ms)
{
    uint32_t h = hash32((uint32_t)led * 3 + slot + 1);
    uint32_t period = o->period_min_ms
                    + (uint32_t)(((uint64_t)(o->period_max_ms - o->period_min_ms) * (h & 0xFFFF)) >> 16);
    float start = o->phase + o->spread * led / count + o->scatter * (h >> 16) / 65536.0f;

    uint32_t cycles = 0;
    float frac = start;
    if (period) {
        cycles = ms / period;
        frac += (float)(ms % period) / period;
    }
    float whole = floorf(frac);
    frac -= whole;
    if (frac >= 1) frac = 0;   // rounding can land exactly on 1

    switch (o->shape) {
        case SHAPE_SINE:     return lookup_sine(frac);
        case SHAPE_TRIANGLE: return frac < 0.5f ? 2 * frac : 2 - 2 * frac;
        case SHAPE_SAW:      return frac;
        case SHAPE_SQUARE:   return frac < 0.5f ? 0 : 1;
        default: {
            // A random value at each whole cycle, eased from one to the next
            uint32_t k = cycles + (uint32_t)(int32_t)whole;
            float from = hash32(h ^ hash32(k)) / 4294967296.0f;
            float to = hash32(h ^ hash32(k + 1)) / 4294967296.0f;
            return from + (to - from) * smooth(frac);
        }
    }
}

static rgb16_t palette_colour(const patch_t *p, float pos)
{
    int n = p->palette_count;
    if (n == 1) return p->palette[0];
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    if (p->steps) {
        int i = (int)(pos * n);
        return p->palette[i < n ? i : n - 1];
    }

    float x = pos * (p->wrap ? n : n - 1);
    int i = (int)x;
    float f = x - i;
    int j = i + 1;
    if (p->wrap) {
        i %= n;
        j %= n;
    } else if (i >= n - 1) {
        return p->palette[n - 1];
    }
    const float *a = p->palette_linear[i], *b = p->palette_linear[j];
    return (rgb16_t){
        (uint16_t)(to_perceptual(a[0] + (b[0] - a[0]) * f) + 0.5f),
        (uint16_t)(to_perceptual(a[1] + (b[1] - a[1]) * f) + 0.5f),
        (uint16_t)(to_perceptual(a[2] + (b[2] - a[2]) * f) + 0.5f),
    };
}

static void render_patch(const patch_t *p, uint32_t ms, rgb16_t *frame, int count)
{
    for (int led = 0; led < count; led++) {
        float glow = 1, level = 1;
        if (p->has_glow) {
            float mix = osc_value(&p->a, 0, led, count, ms);
            if (p->has_b) {
                float b = osc_value(&p->b, 1, led, count, ms);
                mix = p->multiply ? mix * b : p->balance * mix + (1 - p->balance) * b;
            }
            glow = p->power == 1 ? mix : p->power == 2 ? mix * mix : powf(mix, p->power);
            level = p->floor + (p->ceiling - p->floor) * glow;
        }
        float pos = p->colour_by_osc ? osc_value(&p->c, 2, led, count, ms) : glow;
        rgb16_t c = palette_colour(p, pos);
        frame[led] = (rgb16_t){
            (uint16_t)(c.r * level + 0.5f),
            (uint16_t)(c.g * level + 0.5f),
            (uint16_t)(c.b * level + 0.5f),
        };
    }
}

// Speed ramps linearly through a step, so its time is the integral of speed:
// v0·t + (v1 − v0)·t²/2T. That keeps motion continuous as it speeds up.
static uint32_t step_time(const patch_step_t *s, uint32_t t)
{
    return (uint32_t)(s->speed_from * t + (s->speed_to - s->speed_from) * (float)t * t / (2.0f * s->ms));
}

static int find_step(const patch_t *p, uint32_t ms, uint32_t *step_ms)
{
    uint32_t total = 0;
    for (int i = 0; i < p->playlist_count; i++)
        total += p->playlist[i].ms;
    uint32_t t = ms % total;
    int i = 0;
    while (t >= p->playlist[i].ms) {
        t -= p->playlist[i].ms;
        i++;
    }
    *step_ms = t;
    return i;
}

void patch_render(const patch_set_t *set, int index, uint32_t ms, rgb16_t *frame, int count)
{
    if (!tables_ready) make_tables();
    const patch_t *p = &set->patches[index];
    if (p->playlist_count) {
        uint32_t t;
        const patch_step_t *s = &p->playlist[find_step(p, ms, &t)];
        render_patch(&set->patches[s->patch], step_time(s, t), frame, count);
    } else {
        render_patch(p, ms, frame, count);
    }
}

int patch_playlist_step(const patch_set_t *set, int index, uint32_t ms)
{
    const patch_t *p = &set->patches[index];
    uint32_t t;
    return p->playlist_count ? find_step(p, ms, &t) : -1;
}
