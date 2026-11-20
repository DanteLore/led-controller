#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "encoder.h"
#include "led_strip.h"
#include "mic.h"
#include "effects.h"
#include "library.h"
#include "control.h"
#include "screen.h"
#include "web.h"
#include "wifi.h"

// Strip — count chips, not LEDs (24V strips usually have 6 LEDs per chip)
#define LED_PIN     13
#define LED_COUNT   10
#define LED_ORDER   LED_ORDER_BRG

// Left knob: brightness; press for the previous patch
#define LEFT_PIN_CLK   33
#define LEFT_PIN_DT    27
#define LEFT_PIN_SW    14
// Right knob: speed; press for the next patch
#define RIGHT_PIN_CLK  19
#define RIGHT_PIN_DT   21
#define RIGHT_PIN_SW   22

// Three-position switch, common to GND. Neither side connected (the middle) is play.
#define SWITCH_PIN_SETUP  16   // position A
#define SWITCH_PIN_EDIT   17   // position B

// INMP441 microphone, L/R tied to GND
#define MIC_PIN_SCK  26
#define MIC_PIN_WS   25
#define MIC_PIN_SD   35
#define MIC_CORE      0   // with WiFi; core 1 is kept for the LEDs

// 200fps keeps temporal dithering fast enough not to flicker at dim levels.
// Each pixel takes 30µs on the wire, so this holds up to ~150 pixels.
#define FRAME_MS        5
#define PREVIEW_EVERY   4   // LCD preview at 50fps; it takes ~3ms to draw


#define HOSTNAME  "led-controller"

// WiFi runs on core 0, so the LEDs get core 1 to themselves: WiFi interrupts
// would otherwise delay the RMT refills mid-frame and corrupt the data
#define LED_CORE        1
#define LED_PRIORITY   10
#define LED_STACK    6144

// Perceptual, so 50% looks half as bright (roughly 22% PWM after gamma)
#define BRIGHTNESS_DEFAULT 50
#define BRIGHTNESS_MAX     100

// Speed is in knob steps; every SPEED_STEPS_PER_DOUBLING steps doubles it,
// so each click feels like the same-sized change at any speed
#define SPEED_STEPS_PER_DOUBLING  5
#define SPEED_STEPS_MAX          15   // ±15 steps = 1/8x to 8x

#define DEBOUNCE_US          30000   // buttons and switch must settle this long before a change counts
#define WIFI_CHECK_US       500000
#define HEARTBEAT_US       5000000

static const char *TAG = "led-controller";

static rgb16_t frame[LED_COUNT];

// Modes are the library's patches in order, then the hardware tests
#define MODE_MAX  (PATCH_MAX + TEST_COUNT)

typedef struct {
    int mode;
    int brightness;                     // shared by all modes
    int speed_steps[MODE_MAX];          // each mode remembers its own speed
} settings_t;

// What the LED task is showing this frame. Read and written with the library locked.
typedef struct {
    const patch_set_t *set;   // NULL for a test
    int index;                // patch in set, or test
    const char *name;
} showing_t;

static showing_t what_to_show(const settings_t *s)
{
    const patch_set_t *draft = library_draft();
    const patch_set_t *set = draft ? draft : library_patches();
    int index = draft ? library_draft_index() : s->mode;
    if (index < set->count)
        return (showing_t){ set, index, set->patches[index].name };
    test_t test = index - set->count;
    return (showing_t){ NULL, test, effects_test_name(test) };
}

static int mode_count(void)
{
    return library_patches()->count + TEST_COUNT;
}

// ---- Web requests ----------------------------------------------------------

typedef enum { COMMAND_SELECT, COMMAND_BRIGHTNESS } command_type_t;
typedef struct { command_type_t type; int value; } command_t;

static QueueHandle_t commands;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static control_state_t state;   // published by the LED task each frame

void control_select(int mode)
{
    command_t c = { COMMAND_SELECT, mode };
    xQueueSend(commands, &c, 0);
}

void control_set_brightness(int brightness)
{
    command_t c = { COMMAND_BRIGHTNESS, brightness };
    xQueueSend(commands, &c, 0);
}

void control_get_state(control_state_t *out)
{
    taskENTER_CRITICAL(&state_lock);
    *out = state;
    taskEXIT_CRITICAL(&state_lock);
}

static frame_timing_t timing;

void control_get_timing(frame_timing_t *out)
{
    taskENTER_CRITICAL(&state_lock);
    *out = timing;
    taskEXIT_CRITICAL(&state_lock);
}

static void publish_state(const settings_t *s, bool draft, float speed, const mic_beat_t *beat)
{
    taskENTER_CRITICAL(&state_lock);
    state = (control_state_t){ s->mode, draft, s->brightness, speed, LED_COUNT,
                               beat->bpm, beat->confidence, beat->holding, beat->energy, beat->belief,
                               beat->breakdown };
    taskEXIT_CRITICAL(&state_lock);
}

// ---- Output -----------------------------------------------------------------

static float speed_multiplier(int steps)
{
    return powf(2.0f, (float)steps / SPEED_STEPS_PER_DOUBLING);
}

static void show_frame(uint16_t level)
{
    for (int i = 0; i < LED_COUNT; i++)
        led_strip_set_pixel16(i, (uint32_t)frame[i].r * level / LEVEL_FULL,
                                 (uint32_t)frame[i].g * level / LEVEL_FULL,
                                 (uint32_t)frame[i].b * level / LEVEL_FULL);
    led_strip_show();
}

static void draw_speed(int steps)
{
    screen_speed(speed_multiplier(steps), (float)(steps + SPEED_STEPS_MAX) / (2 * SPEED_STEPS_MAX));
}

// ---- Input ------------------------------------------------------------------

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// A raw on/off input that only changes once it has held steady for DEBOUNCE_US
typedef struct {
    bool raw, settled;
    int64_t raw_since;
} debounced_t;

// Returns true when the settled value changes
static bool debounce(debounced_t *d, bool raw, int64_t now)
{
    if (raw != d->raw) {
        d->raw = raw;
        d->raw_since = now;
    }
    if (raw == d->settled || now - d->raw_since < DEBOUNCE_US)
        return false;
    d->settled = raw;
    return true;
}

static bool pressed(debounced_t *d, encoder_t *knob, int64_t now)
{
    return debounce(d, encoder_button_pressed(knob), now) && d->settled;
}

typedef enum { PANEL_PLAY, PANEL_SETUP, PANEL_EDIT } panel_t;
static const char *const PANEL_NAMES[] = { "Play", "Setup", "Edit" };

static void switch_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH_PIN_SETUP) | (1ULL << SWITCH_PIN_EDIT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

static panel_t switch_raw(void)
{
    return gpio_get_level(SWITCH_PIN_SETUP) == 0 ? PANEL_SETUP
         : gpio_get_level(SWITCH_PIN_EDIT) == 0  ? PANEL_EDIT
         :                                         PANEL_PLAY;
}

// ---- Network ----------------------------------------------------------------

// The address once joined, or the setup hotspot to join while it's up
typedef struct {
    wifi_status_t wifi;
    char qr[64];   // what the QR code holds; empty while still joining
} network_t;

static void read_network(network_t *n)
{
    wifi_get_status(&n->wifi);
    if (n->wifi.state == WIFI_CONNECTED)
        snprintf(n->qr, sizeof(n->qr), "http://%s/", n->wifi.ip);
    else if (n->wifi.hotspot)
        // Phone cameras offer to join a network from a code like this. Our
        // hotspot's name has none of the characters that would need escaping.
        snprintf(n->qr, sizeof(n->qr), "WIFI:T:nopass;S:%s;;", n->wifi.hotspot_ssid);
    else
        n->qr[0] = 0;
}

static void draw_network(const network_t *n)
{
    if (n->wifi.state == WIFI_CONNECTED)
        screen_network(n->wifi.ip, true, false);
    else if (n->wifi.hotspot)
        screen_network("WIFI SETUP", false, true);
    else
        screen_network("JOINING WIFI", false, false);
}

#define LINES(...) (screen_line_t[]){ __VA_ARGS__ }, sizeof((screen_line_t[]){ __VA_ARGS__ }) / sizeof(screen_line_t)

static void draw_setup(const network_t *n)
{
    const wifi_status_t *w = &n->wifi;
    if (w->state == WIFI_CONNECTED)
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "SCAN TO OPEN", TEXT_NORMAL }, { "THE EDITOR", TEXT_NORMAL },
                          { "", 0 }, { "OR BROWSE TO", TEXT_DIM }, { w->ip, TEXT_NORMAL }, { "", 0 },
                          { "WIFI", TEXT_DIM }, { w->ssid, TEXT_NORMAL }), n->qr);
    else if (w->hotspot)
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "SCAN TO JOIN", TEXT_NORMAL }, { "THE SETUP WIFI", TEXT_NORMAL },
                          { "", 0 }, { "OR JOIN", TEXT_DIM }, { w->hotspot_ssid, TEXT_NORMAL },
                          { "AND THE SETUP", TEXT_DIM }, { "PAGE POPS UP", TEXT_DIM }), n->qr);
    else
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "JOINING WIFI", TEXT_NORMAL }, { w->ssid, TEXT_NORMAL }), NULL);
}

// A placeholder until patches can be edited on the device. No QR code, so
// it can't be mistaken for Setup.
static void draw_edit(void)
{
    screen_text(LINES({ "EDIT MODE", TEXT_HEADING }, { "", 0 }, { "COMING SOON.", TEXT_NORMAL }, { "", 0 },
                      { "FOR NOW, EDIT IN", TEXT_NORMAL }, { "THE WEB EDITOR:", TEXT_NORMAL },
                      { "SETUP SHOWS WHERE", TEXT_DIM }), NULL);
}

// ---- Main -------------------------------------------------------------------

typedef enum { SCREEN_NONE, SCREEN_PLAY, SCREEN_SETUP, SCREEN_EDIT } screen_t;

static void led_task(void *arg)
{
    screen_init();
    encoder_t *left = encoder_init(LEFT_PIN_CLK, LEFT_PIN_DT, LEFT_PIN_SW);
    encoder_t *right = encoder_init(RIGHT_PIN_CLK, RIGHT_PIN_DT, RIGHT_PIN_SW);
    switch_init();
    led_strip_init(LED_PIN, LED_COUNT, LED_ORDER);

    // Start on the library's "default" patch, or its first
    library_lock();
    int startup = library_patches()->startup;
    library_unlock();
    settings_t settings = { .mode = startup, .brightness = BRIGHTNESS_DEFAULT };

    // Effect time advances by real time × the mode's speed, so changing speed
    // changes the rate of motion without making the effect jump
    int64_t effect_us = 0;
    int64_t last_frame_us = esp_timer_get_time();

    // What was on last frame, to spot changes from the knobs, switch or web
    uint32_t last_version = library_version();
    bool was_draft = false;
    int last_slot = -1;
    char last_name[PATCH_NAME_MAX + 1] = "";
    const void *last_phase = NULL;
    int last_step = -1;

    debounced_t left_button = {0}, right_button = {0};
    debounced_t switch_setup = {0}, switch_edit = {0};
    panel_t panel = PANEL_PLAY;
    network_t network = {0};
    screen_t drawn = SCREEN_NONE;
    int64_t next_wifi_check = 0;
    bool network_changed = false;
    bool redraw_patch = true, redraw_brightness = true, redraw_speed = true;

    uint32_t frames = 0, frames_at_heartbeat = 0;
    int64_t spent_render = 0, spent_strip = 0, spent_screen = 0, spent_frame = 0;
    int64_t next_heartbeat_us = esp_timer_get_time() + HEARTBEAT_US;

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        int64_t now = esp_timer_get_time();

        // ---- Inputs
        network_changed = false;
        int brightness_delta = encoder_get_delta(left);
        int speed_delta = encoder_get_delta(right);
        int step = pressed(&right_button, right, now) - pressed(&left_button, left, now);

        bool switch_moved = debounce(&switch_setup, switch_raw() == PANEL_SETUP, now);
        switch_moved |= debounce(&switch_edit, switch_raw() == PANEL_EDIT, now);
        if (switch_moved) {
            panel = switch_setup.settled ? PANEL_SETUP : switch_edit.settled ? PANEL_EDIT : PANEL_PLAY;
            ESP_LOGI(TAG, "Switch: %s", PANEL_NAMES[panel]);
        }

        if (now >= next_wifi_check) {
            next_wifi_check = now + WIFI_CHECK_US;
            char was[sizeof(network.qr)];
            strlcpy(was, network.qr, sizeof(was));
            read_network(&network);
            network_changed |= strcmp(was, network.qr) != 0;
        }

        // ---- Which patch. A press leaves the web editor's draft for the
        // saved version of the same patch; otherwise it steps through the modes.
        library_lock();
        bool draft = library_draft() != NULL;
        if (step && draft)
            settings.mode = library_draft_index();
        else if (step)
            settings.mode = (settings.mode + step + mode_count()) % mode_count();
        library_unlock();
        if (step && draft)
            library_stop_draft();

        command_t command;
        while (xQueueReceive(commands, &command, 0)) {
            if (command.type == COMMAND_BRIGHTNESS) {
                settings.brightness = clamp(command.value, 0, BRIGHTNESS_MAX);
                redraw_brightness = true;
            } else {
                settings.mode = command.value;
                library_stop_draft();
            }
        }

        library_lock();
        if (library_version() != last_version || settings.mode >= mode_count()) {
            last_version = library_version();
            if (settings.mode >= mode_count())
                settings.mode = 0;
            last_slot = -1;   // the patch at this position may have changed
        }
        showing_t show = what_to_show(&settings);
        draft = library_draft() != NULL;
        // Speed is remembered per position, so a draft uses its patch's setting
        int slot = show.set ? show.index : library_patches()->count + show.index;

        // A different mode starts from its beginning; edits to the same draft
        // patch carry on from where they are, so changes can be seen smoothly
        if (slot != last_slot || draft != was_draft || strcmp(show.name, last_name) != 0) {
            if (slot != last_slot || draft != was_draft)
                effect_us = 0;
            ESP_LOGI(TAG, "%s %s", draft ? "Web draft" : "Patch", show.name);
            strlcpy(last_name, show.name, sizeof(last_name));
            last_slot = slot;
            was_draft = draft;
            last_phase = NULL;
            last_step = -1;
            redraw_patch = redraw_speed = true;
        }

        if (brightness_delta) {
            settings.brightness = clamp(settings.brightness + brightness_delta, 0, BRIGHTNESS_MAX);
            ESP_LOGI(TAG, "Brightness %d%%", settings.brightness);
            redraw_brightness = true;
        }
        if (speed_delta) {
            int *steps = &settings.speed_steps[slot];
            *steps = clamp(*steps + speed_delta, -SPEED_STEPS_MAX, SPEED_STEPS_MAX);
            ESP_LOGI(TAG, "Speed %.2fx", speed_multiplier(*steps));
            redraw_speed = true;
        }

        // ---- Render
        float speed = speed_multiplier(settings.speed_steps[slot]);
        effect_us += (int64_t)((now - last_frame_us) * speed);
        last_frame_us = now;
        uint32_t ms = effect_us / 1000;
        // The beat clock comes from the music, so the speed knob doesn't touch it
        mic_beat_t beat;
        mic_beat(&beat);
        uint32_t beat_milli = (uint32_t)(uint64_t)(beat.position * 1000);

        bool full_scale = false;
        int64_t t_render = esp_timer_get_time();
        if (show.set) {
            // Pulses hit as hard as the music does, boldly only when the tracker
            // believes in the beat, and not at all in a breakdown
            patch_render(show.set, show.index, ms, beat_milli, beat.pulse, frame, LED_COUNT);
            int playing = patch_playlist_step(show.set, show.index, ms);
            if (playing >= 0 && playing != last_step) {
                const patch_t *p = &show.set->patches[show.index];
                ESP_LOGI(TAG, "%s: %s", p->name, show.set->patches[p->playlist[playing].patch].name);
            }
            last_step = playing;
        } else {
            effects_render_test(show.index, ms, frame, LED_COUNT);
            const char *phase = effects_test_phase(show.index, ms);
            if (phase && phase != last_phase)
                ESP_LOGI(TAG, "%s", phase);
            last_phase = phase;
            full_scale = effects_test_full_scale(show.index);
        }
        library_unlock();
        int64_t t_strip = esp_timer_get_time();
        spent_render += t_strip - t_render;

        uint16_t level = full_scale ? LEVEL_FULL : (uint32_t)settings.brightness * LEVEL_FULL / BRIGHTNESS_MAX;
        show_frame(level);
        int64_t t_screen = esp_timer_get_time();
        spent_strip += t_screen - t_strip;

        // ---- Screen, as the switch picks it
        screen_t want = panel == PANEL_SETUP ? SCREEN_SETUP
                      : panel == PANEL_EDIT  ? SCREEN_EDIT
                      :                        SCREEN_PLAY;
        if (want != drawn || (network_changed && want == SCREEN_SETUP)) {
            if (want == SCREEN_SETUP) draw_setup(&network);
            if (want == SCREEN_EDIT)  draw_edit();
            if (want == SCREEN_PLAY) {
                screen_play();
                draw_network(&network);
                redraw_patch = redraw_brightness = redraw_speed = true;
            }
            drawn = want;
        } else if (want == SCREEN_PLAY && network_changed) {
            draw_network(&network);
        }
        if (drawn == SCREEN_PLAY) {
            if (redraw_patch)      screen_patch(show.name, draft);
            if (redraw_brightness) screen_brightness(settings.brightness);
            if (redraw_speed)      draw_speed(settings.speed_steps[slot]);
            redraw_patch = redraw_brightness = redraw_speed = false;
            if (++frames % PREVIEW_EVERY == 0) {
                screen_preview(frame, LED_COUNT);
                screen_mic(mic_level(), beat.bpm, !beat.holding, beat.belief, beat.breakdown,
                           !beat.holding && beat.position - floor(beat.position) < 0.15);
            }
        } else {
            frames++;
        }

        publish_state(&settings, draft, speed, &beat);
        int64_t t_end = esp_timer_get_time();
        spent_screen += t_end - t_screen;
        spent_frame += t_end - now;

        if (now >= next_heartbeat_us) {
            uint32_t n = frames - frames_at_heartbeat;
            if (n) {
                taskENTER_CRITICAL(&state_lock);
                timing = (frame_timing_t){
                    .fps = n * 1e6f / (now - next_heartbeat_us + HEARTBEAT_US),
                    .render_us = (float)spent_render / n, .strip_us = (float)spent_strip / n,
                    .screen_us = (float)spent_screen / n, .frame_us = (float)spent_frame / n };
                taskEXIT_CRITICAL(&state_lock);
            }
            spent_render = spent_strip = spent_screen = spent_frame = 0;
            ESP_LOGI(TAG, "Running: %s, brightness %d%%, speed %.2fx, %lu fps",
                     last_name, settings.brightness, speed,
                     (unsigned long)((frames - frames_at_heartbeat) * 1000000LL / (now - next_heartbeat_us + HEARTBEAT_US)));
            frames_at_heartbeat = frames;
            next_heartbeat_us = now + HEARTBEAT_US;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_MS));
    }
}

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    nvs_init();
    library_init();
    commands = xQueueCreate(8, sizeof(command_t));
    wifi_start(HOSTNAME);
    web_start(wifi_web_server());
    mic_start(MIC_PIN_SCK, MIC_PIN_WS, MIC_PIN_SD, MIC_CORE);
    // Interrupts are set up on the core that registers them, so the strip,
    // encoders and LCD are all initialised inside the task
    xTaskCreatePinnedToCore(led_task, "leds", LED_STACK, NULL, LED_PRIORITY, NULL, LED_CORE);
}
