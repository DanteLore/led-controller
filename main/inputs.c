#include "inputs.h"
#include "board.h"
#include "encoder.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>

#define DEBOUNCE_US  30000   // buttons and switch must settle this long before a change counts

static const char *TAG = "inputs";
static const char *const PANEL_NAMES[] = { "Play", "Setup", "Edit" };

// A raw on/off input that only changes once it has held steady for DEBOUNCE_US
typedef struct {
    bool raw, settled;
    int64_t raw_since;
} debounced_t;

static encoder_t *left, *right;
static debounced_t left_button, right_button, switch_setup, switch_edit;
static panel_t panel = PANEL_PLAY;

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

static void switch_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH_PIN_SETUP) | (1ULL << SWITCH_PIN_EDIT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

static panel_t read_switch(int64_t now)
{
    bool moved = debounce(&switch_setup, gpio_get_level(SWITCH_PIN_SETUP) == 0, now);
    moved |= debounce(&switch_edit, gpio_get_level(SWITCH_PIN_EDIT) == 0, now);
    if (moved) {
        panel = switch_setup.settled ? PANEL_SETUP : switch_edit.settled ? PANEL_EDIT : PANEL_PLAY;
        ESP_LOGI(TAG, "Switch: %s", PANEL_NAMES[panel]);
    }
    return panel;
}

void inputs_init(void)
{
    left = encoder_init(LEFT_PIN_CLK, LEFT_PIN_DT, LEFT_PIN_SW);
    right = encoder_init(RIGHT_PIN_CLK, RIGHT_PIN_DT, RIGHT_PIN_SW);
    switch_init();
}

inputs_t inputs_read(int64_t now)
{
    return (inputs_t){
        .brightness_delta = encoder_get_delta(left),
        .speed_delta = encoder_get_delta(right),
        .step = pressed(&right_button, right, now) - pressed(&left_button, left, now),
        .panel = read_switch(now),
    };
}
