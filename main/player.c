#include "player.h"
#include "library.h"
#include "test_patterns.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

// Perceptual, so 50% looks half as bright (roughly 22% PWM after gamma)
#define BRIGHTNESS_DEFAULT 50

// Speed is in knob steps; every SPEED_STEPS_PER_DOUBLING steps doubles it,
// so each click feels like the same-sized change at any speed
#define SPEED_STEPS_PER_DOUBLING  5
#define SPEED_STEPS_MAX          15   // ±15 steps = 1/8x to 8x

// Modes are the library's patches in order, then the hardware tests
#define MODE_MAX  (PATCH_MAX + TEST_COUNT)

static const char *TAG = "player";

static int mode;
static int brightness = BRIGHTNESS_DEFAULT;
static int speed_steps[MODE_MAX];   // each mode remembers its own speed

// What's showing this frame. The set is only valid with the library locked.
static const patch_set_t *showing_set;   // NULL for a test pattern
static int showing_index;                // patch in the set, or test pattern
static bool draft;
static int slot;                    // mode, or the draft's patch: what the speed is remembered by
static char name[PATCH_NAME_MAX + 1];
static bool full_scale;             // a test pattern that ignores brightness

// Effect time advances by real time × the mode's speed, so changing speed
// changes the rate of motion without making the effect jump
static int64_t effect_us, last_frame_us;

// What was on last frame, to spot changes from the knobs, switch or web
static uint32_t last_version;
static int last_slot = -1;
static bool was_draft;
static const char *last_phase;
static int last_step = -1;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float speed_multiplier(int steps)
{
    return powf(2.0f, (float)steps / SPEED_STEPS_PER_DOUBLING);
}

static int mode_count(void)
{
    return library_patches()->count + TEST_COUNT;
}

void player_init(void)
{
    library_lock();
    mode = library_patches()->startup;
    last_version = library_version();
    library_unlock();
    last_frame_us = esp_timer_get_time();
}

void player_step(int step)
{
    if (!step) return;
    library_lock();
    bool leaving_draft = library_draft() != NULL;
    mode = leaving_draft ? library_draft_index() : (mode + step + mode_count()) % mode_count();
    library_unlock();
    if (leaving_draft)
        library_stop_draft();
}

void player_select(int new_mode)
{
    mode = new_mode;
    library_stop_draft();
}

void player_set_brightness(int value)
{
    brightness = clamp(value, 0, BRIGHTNESS_MAX);
}

// ---- Each frame, with the library locked

// A save may have changed or removed the patch at this position
static void follow_library(void)
{
    if (library_version() == last_version && mode < mode_count())
        return;
    last_version = library_version();
    if (mode >= mode_count())
        mode = 0;
    last_slot = -1;
}

// The draft if one's playing, or the mode: a patch, or a test pattern after them
static void choose_what_to_show(void)
{
    const patch_set_t *draft_set = library_draft();
    draft = draft_set != NULL;
    showing_set = draft ? draft_set : library_patches();
    showing_index = draft ? library_draft_index() : mode;
    slot = showing_index;
    if (showing_index >= showing_set->count) {
        showing_index -= showing_set->count;
        showing_set = NULL;
    }
}

static const char *showing_name(void)
{
    return showing_set ? showing_set->patches[showing_index].name : test_pattern_name(showing_index);
}

// A different mode starts from its beginning; edits to the same draft
// patch carry on from where they are, so changes can be seen smoothly
static void notice_changes(void)
{
    bool moved = slot != last_slot || draft != was_draft;
    if (!moved && strcmp(showing_name(), name) == 0)
        return;
    if (moved)
        effect_us = 0;
    strlcpy(name, showing_name(), sizeof(name));
    ESP_LOGI(TAG, "%s %s", draft ? "Web draft" : "Patch", name);
    last_slot = slot;
    was_draft = draft;
    last_phase = NULL;
    last_step = -1;
}

static void turn_knobs(int brightness_delta, int speed_delta)
{
    if (brightness_delta) {
        player_set_brightness(brightness + brightness_delta);
        ESP_LOGI(TAG, "Brightness %d%%", brightness);
    }
    if (speed_delta) {
        int *steps = &speed_steps[slot];
        *steps = clamp(*steps + speed_delta, -SPEED_STEPS_MAX, SPEED_STEPS_MAX);
        ESP_LOGI(TAG, "Speed %.2fx", speed_multiplier(*steps));
    }
}

static uint32_t advance_clock(int64_t now)
{
    effect_us += (int64_t)((now - last_frame_us) * speed_multiplier(speed_steps[slot]));
    last_frame_us = now;
    return effect_us / 1000;
}

// Pulses hit as hard as the music does, boldly only when the tracker
// believes in the beat, and not at all in a breakdown
static void render_patch(uint32_t ms, const beat_state_t *beat, rgb16_t *frame, int count)
{
    uint32_t beat_milli = (uint32_t)(uint64_t)(beat->position * 1000);
    patch_render(showing_set, showing_index, ms, beat_milli, beat->pulse, frame, count);
    int playing = patch_playlist_step(showing_set, showing_index, ms);
    if (playing >= 0 && playing != last_step) {
        const patch_t *p = &showing_set->patches[showing_index];
        ESP_LOGI(TAG, "%s: %s", p->name, showing_set->patches[p->playlist[playing].patch].name);
    }
    last_step = playing;
}

static void render_test_pattern(uint32_t ms, rgb16_t *frame, int count)
{
    test_pattern_render(showing_index, ms, frame, count);
    const char *phase = test_pattern_phase(showing_index, ms);
    if (phase && phase != last_phase)
        ESP_LOGI(TAG, "%s", phase);
    last_phase = phase;
}

void player_frame(int64_t now, int brightness_delta, int speed_delta, const beat_state_t *beat,
                  rgb16_t *frame, int count)
{
    library_lock();
    follow_library();
    choose_what_to_show();
    notice_changes();
    turn_knobs(brightness_delta, speed_delta);
    // The beat clock comes from the music, so the speed knob doesn't touch it
    uint32_t ms = advance_clock(now);
    full_scale = !showing_set && test_pattern_full_scale(showing_index);
    if (showing_set)
        render_patch(ms, beat, frame, count);
    else
        render_test_pattern(ms, frame, count);
    library_unlock();
}

player_status_t player_status(void)
{
    int steps = speed_steps[slot];
    return (player_status_t){
        .mode = mode,
        .draft = draft,
        .name = name,
        .brightness = brightness,
        .speed = speed_multiplier(steps),
        .speed_position = (float)(steps + SPEED_STEPS_MAX) / (2 * SPEED_STEPS_MAX),
        .level = full_scale ? LEVEL_FULL : (uint32_t)brightness * LEVEL_FULL / BRIGHTNESS_MAX,
    };
}
