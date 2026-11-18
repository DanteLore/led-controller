#include "effects.h"
#include "led_strip.h"
#include <math.h>
#include <stddef.h>

#define RAINBOW_CYCLE_MS   5000

#define PATTERN_COLOURS      10
#define PATTERN_STEP_MS     500
#define PATTERN_FADE_MS    4000

#define DEMO_SEGMENT_MS   30000
#define DEMO_RAINBOW_SLOW  0.1f   // rainbow cycles per second at the start of the segment
#define DEMO_RAINBOW_FAST  2.0f   // ... and at the end
#define WARM_PULSE_MS      6000   // full → dark → full

#define DIM_GRADIENT_MS    5000
#define DIM_FADE_MS       10000
#define DIM_CYCLE_MS      (DIM_GRADIENT_MS + 2 * DIM_FADE_MS)

#define TWO_PI 6.2831853f

// RGB approximations of white; the strip has no dedicated white LED
static const rgb16_t WARM_WHITE = {65535, 33410, 10280};
static const rgb16_t COOL_WHITE = {43690, 51400, 65535};
static const rgb16_t WHITE      = {65535, 65535, 65535};

static const char *mode_names[MODE_COUNT] = {
    [MODE_DEMO]       = "DEMO    ",
    [MODE_RAINBOW]    = "RAINBOW ",
    [MODE_PULSE]      = "PULSE   ",
    [MODE_WARM_WHITE] = "WARM    ",
    [MODE_PATTERN]    = "PATTERN ",
    [MODE_RGB_TEST]   = "RGB TEST",
    [MODE_DIM_TEST]   = "DIM TEST",
};

static const knob_role_t knob_roles[MODE_COUNT] = {
    [MODE_DEMO]       = KNOB_BRIGHTNESS,   // has its own speed ramp
    [MODE_RAINBOW]    = KNOB_SPEED,
    [MODE_PULSE]      = KNOB_SPEED,
    [MODE_WARM_WHITE] = KNOB_BRIGHTNESS,
    [MODE_PATTERN]    = KNOB_SPEED,
    [MODE_RGB_TEST]   = KNOB_BRIGHTNESS,
    [MODE_DIM_TEST]   = KNOB_SPEED,
};

typedef enum { DEMO_RAINBOW, DEMO_WARM_PULSE, DEMO_WHITE_SHIFT, DEMO_SEGMENT_COUNT } demo_segment_t;

static const char *demo_segment_names[DEMO_SEGMENT_COUNT] = {
    [DEMO_RAINBOW]      = "Demo: rainbow, slow to fast",
    [DEMO_WARM_PULSE]   = "Demo: warm white pulsing bright to dark",
    [DEMO_WHITE_SHIFT] = "Demo: white, warm to cool and back",
};

static const char *DIM_GRADIENT = "Dim test: gradient, full at input end to off at far end";
static const char *DIM_FADE_OUT = "Dim test: whole strip fading full to off over 10s";
static const char *DIM_FADE_IN  = "Dim test: whole strip fading off to full over 10s";

// ---- Helpers ----------------------------------------------------------------

static uint16_t linear_to_perceptual(float linear)
{
    return powf(linear, 1.0f / LED_STRIP_GAMMA) * LEVEL_FULL;
}

// Equal-power hue wheel. A plain wheel has two channels full at yellow, cyan
// and magenta but only one at red, green and blue, so a moving rainbow seems
// to pulse brighter and dimmer. Here the channels always add up to the same
// light output (worked out in linear light, before gamma), so only the colour
// changes. Peak brightness drops a little in exchange.
static rgb16_t hue_to_rgb(uint16_t hue)
{
    // Three crossfades: red → green → blue → red
    uint32_t pos = (uint32_t)hue * 3;
    float rise = (pos & 0xFFFF) / 65536.0f;
    uint16_t up = linear_to_perceptual(rise);
    uint16_t down = linear_to_perceptual(1.0f - rise);
    switch (pos >> 16) {
        case 0:  return (rgb16_t){down, up,   0};
        case 1:  return (rgb16_t){0,    down, up};
        default: return (rgb16_t){up,   0,    down};
    }
}

static uint16_t scale_level(uint16_t v, uint16_t level)
{
    return (uint32_t)v * level / LEVEL_FULL;
}

static rgb16_t scale_rgb(rgb16_t c, uint16_t level)
{
    return (rgb16_t){scale_level(c.r, level), scale_level(c.g, level), scale_level(c.b, level)};
}

static uint16_t mix_level(uint16_t a, uint16_t b, uint16_t amount)
{
    return a + ((int32_t)b - a) * amount / LEVEL_FULL;
}

static rgb16_t mix_rgb(rgb16_t a, rgb16_t b, uint16_t amount)
{
    return (rgb16_t){mix_level(a.r, b.r, amount), mix_level(a.g, b.g, amount), mix_level(a.b, b.b, amount)};
}

// 0 → full → 0 as a cosine, so it eases in and out with no sharp turn at either end
static uint16_t ease_wave(uint32_t ms, uint32_t period_ms)
{
    float phase = (float)(ms % period_ms) / period_ms;
    return (1.0f - cosf(TWO_PI * phase)) * 0.5f * LEVEL_FULL;
}

// Position along a 0–65535 cycle, for things that wrap around the strip
static uint16_t cycle_position(uint32_t ms, uint32_t period_ms)
{
    return (uint64_t)(ms % period_ms) * 65536 / period_ms;
}

// ---- Effects ----------------------------------------------------------------

// Thirds of the strip lit red, green, blue — if they don't match, fix LED_ORDER
static void render_rgb_test(rgb16_t *frame, int count)
{
    static const rgb16_t thirds[3] = {{LEVEL_FULL, 0, 0}, {0, LEVEL_FULL, 0}, {0, 0, LEVEL_FULL}};
    for (int i = 0; i < count; i++)
        frame[i] = thirds[i * 3 / count];
}

// Ten distinct hues stepping along the strip while the whole strip fades
// in and out — exercises every colour channel, the data chain and dimming
static void render_pattern(uint32_t ms, rgb16_t *frame, int count)
{
    int shift = ms / PATTERN_STEP_MS;
    uint16_t level = ease_wave(ms, PATTERN_FADE_MS);
    for (int i = 0; i < count; i++) {
        uint16_t hue = (i + shift) % PATTERN_COLOURS * 65536 / PATTERN_COLOURS;
        frame[i] = scale_rgb(hue_to_rgb(hue), level);
    }
}

static void render_rainbow_at(uint16_t offset, rgb16_t *frame, int count)
{
    for (int i = 0; i < count; i++)
        frame[i] = hue_to_rgb(offset + i * 65536 / count);
}

// Speed ramps linearly, so position is the integral of speed: v0·t + (v1 − v0)·t²/2T.
// Driving position this way keeps the motion continuous as it speeds up.
static void render_rainbow_accelerating(uint32_t segment_ms, rgb16_t *frame, int count)
{
    float t = segment_ms / 1000.0f;
    float duration = DEMO_SEGMENT_MS / 1000.0f;
    float cycles = DEMO_RAINBOW_SLOW * t + (DEMO_RAINBOW_FAST - DEMO_RAINBOW_SLOW) * t * t / (2 * duration);
    render_rainbow_at((cycles - floorf(cycles)) * 65535, frame, count);
}

static void render_solid(rgb16_t c, rgb16_t *frame, int count)
{
    for (int i = 0; i < count; i++)
        frame[i] = c;
}

// Starts at full, eases down to off and back up
static void render_warm_pulse(uint32_t ms, rgb16_t *frame, int count)
{
    render_solid(scale_rgb(WARM_WHITE, LEVEL_FULL - ease_wave(ms, WARM_PULSE_MS)), frame, count);
}

// The whole strip eases from warm to cool and back over the segment
static void render_white_shift(uint32_t segment_ms, rgb16_t *frame, int count)
{
    render_solid(mix_rgb(WARM_WHITE, COOL_WHITE, ease_wave(segment_ms, DEMO_SEGMENT_MS)), frame, count);
}

static demo_segment_t demo_segment(uint32_t ms)
{
    return ms / DEMO_SEGMENT_MS % DEMO_SEGMENT_COUNT;
}

static void render_demo(uint32_t ms, rgb16_t *frame, int count)
{
    uint32_t segment_ms = ms % DEMO_SEGMENT_MS;
    switch (demo_segment(ms)) {
        case DEMO_RAINBOW:    render_rainbow_accelerating(segment_ms, frame, count); break;
        case DEMO_WARM_PULSE: render_warm_pulse(segment_ms, frame, count);           break;
        default:              render_white_shift(segment_ms, frame, count);          break;
    }
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
        frame[i] = scale_rgb(WHITE, level);
    }
}

// ---- Public -----------------------------------------------------------------

const char *effects_mode_name(led_mode_t mode)
{
    return mode_names[mode];
}

void effects_render(led_mode_t mode, uint32_t ms, rgb16_t *frame, int count)
{
    switch (mode) {
        case MODE_DEMO:       render_demo(ms, frame, count);                                     break;
        case MODE_RAINBOW:    render_rainbow_at(cycle_position(ms, RAINBOW_CYCLE_MS), frame, count); break;
        case MODE_PULSE:      render_warm_pulse(ms, frame, count);                               break;
        case MODE_WARM_WHITE: render_solid(WARM_WHITE, frame, count);                            break;
        case MODE_PATTERN:    render_pattern(ms, frame, count);                                  break;
        case MODE_RGB_TEST:   render_rgb_test(frame, count);                                     break;
        default:              render_dim_test(ms, frame, count);                                 break;
    }
}

const char *effects_phase(led_mode_t mode, uint32_t ms)
{
    if (mode == MODE_DEMO)
        return demo_segment_names[demo_segment(ms)];
    if (mode == MODE_DIM_TEST) {
        uint32_t t = ms % DIM_CYCLE_MS;
        return t < DIM_GRADIENT_MS               ? DIM_GRADIENT
             : t < DIM_GRADIENT_MS + DIM_FADE_MS ? DIM_FADE_OUT
             :                                     DIM_FADE_IN;
    }
    return NULL;
}

int effects_full_scale(led_mode_t mode)
{
    return mode == MODE_DIM_TEST;
}

knob_role_t effects_knob_role(led_mode_t mode)
{
    return knob_roles[mode];
}
