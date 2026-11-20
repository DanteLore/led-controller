#include "effects.h"
#include <stddef.h>

#define DIM_GRADIENT_MS    5000
#define DIM_FADE_MS       10000
#define DIM_CYCLE_MS      (DIM_GRADIENT_MS + 2 * DIM_FADE_MS)

static const char *names[TEST_COUNT] = {
    [TEST_RGB] = "RGB TEST",
    [TEST_DIM] = "DIM TEST",
};

static const char *DIM_GRADIENT = "Dim test: gradient, full at input end to off at far end";
static const char *DIM_FADE_OUT = "Dim test: whole strip fading full to off over 10s";
static const char *DIM_FADE_IN  = "Dim test: whole strip fading off to full over 10s";

// Thirds of the strip lit red, green, blue — if they don't match, fix LED_ORDER
static void render_rgb_test(rgb16_t *frame, int count)
{
    static const rgb16_t thirds[3] = {{LEVEL_FULL, 0, 0}, {0, LEVEL_FULL, 0}, {0, 0, LEVEL_FULL}};
    for (int i = 0; i < count; i++)
        frame[i] = thirds[i * 3 / count];
}

// Full white, ignoring the knob: a static gradient, then a slow fade out and in.
// With gamma and dithering, both should look even, with no visible steps.
static void render_dim_test(uint32_t ms, rgb16_t *frame, int count)
{
    uint32_t t = ms % DIM_CYCLE_MS;
    for (int i = 0; i < count; i++) {
        uint16_t level;
        if (t < DIM_GRADIENT_MS)
            level = count > 1 ? LEVEL_FULL - (uint32_t)LEVEL_FULL * i / (count - 1) : LEVEL_FULL;
        else if (t < DIM_GRADIENT_MS + DIM_FADE_MS)
            level = LEVEL_FULL - (uint64_t)LEVEL_FULL * (t - DIM_GRADIENT_MS) / DIM_FADE_MS;
        else
            level = (uint64_t)LEVEL_FULL * (t - DIM_GRADIENT_MS - DIM_FADE_MS) / DIM_FADE_MS;
        frame[i] = (rgb16_t){level, level, level};
    }
}

const char *effects_test_name(test_t test)
{
    return names[test];
}

void effects_render_test(test_t test, uint32_t ms, rgb16_t *frame, int count)
{
    if (test == TEST_RGB)
        render_rgb_test(frame, count);
    else
        render_dim_test(ms, frame, count);
}

const char *effects_test_phase(test_t test, uint32_t ms)
{
    if (test != TEST_DIM)
        return NULL;
    uint32_t t = ms % DIM_CYCLE_MS;
    return t < DIM_GRADIENT_MS               ? DIM_GRADIENT
         : t < DIM_GRADIENT_MS + DIM_FADE_MS ? DIM_FADE_OUT
         :                                     DIM_FADE_IN;
}

bool effects_test_full_scale(test_t test)
{
    return test == TEST_DIM;
}
