#include "encoder.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <stdatomic.h>

// Lookup table indexed by (prev_state << 2) | curr_state.
// State bits: CLK=bit1, DT=bit0. Returns +1, -1, or 0 for invalid/bounce.
static const int8_t enc_table[16] = {
     0,  1, -1,  0,
    -1,  0,  0,  1,
     1,  0,  0, -1,
     0, -1,  1,  0,
};

struct encoder {
    int pin_clk, pin_dt, pin_sw;
    atomic_int delta;
    volatile uint8_t last_state;
    volatile int8_t accumulated;
    volatile int64_t last_detent_us;
};

static encoder_t encoders[ENCODER_MAX];
static int count;

static void IRAM_ATTR encoder_isr(void *arg)
{
    encoder_t *e = arg;
    uint8_t curr = (gpio_get_level(e->pin_clk) << 1) | gpio_get_level(e->pin_dt);
    if (curr == e->last_state) return;

    int change = enc_table[(e->last_state << 2) | curr];
    if (change != 0) {
        e->accumulated += change;
        if (curr == 0b11) {
            int dir = (e->accumulated > 0) ? -1 : (e->accumulated < 0) ? 1 : 0;
            if (dir != 0) {
                // Scale step by how fast the knob is turning.
                // Fast = small gap between detents → bigger step.
                int64_t now = esp_timer_get_time();
                int64_t elapsed = now - e->last_detent_us;
                int step;
                if      (elapsed < 2000)  step = 8;
                else if (elapsed < 5000)  step = 4;
                else if (elapsed < 15000) step = 2;
                else                      step = 1;
                atomic_fetch_add(&e->delta, dir * step);
                e->last_detent_us = now;
            }
            e->accumulated = 0;
        }
    }

    e->last_state = curr;
}

encoder_t *encoder_init(int pin_clk, int pin_dt, int pin_sw)
{
    if (count == ENCODER_MAX) return NULL;
    encoder_t *e = &encoders[count++];
    e->pin_clk = pin_clk;
    e->pin_dt  = pin_dt;
    e->pin_sw  = pin_sw;
    atomic_init(&e->delta, 0);

    // Internal pull-ups on all three: many KY-040 boards leave the SW pull-up
    // unfitted, so the button floats and reads as random presses. Harmless in
    // parallel with the onboard 10k on CLK/DT.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin_clk) | (1ULL << pin_dt) | (1ULL << pin_sw),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    e->last_state = (gpio_get_level(pin_clk) << 1) | gpio_get_level(pin_dt);

    gpio_set_intr_type(pin_clk, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(pin_dt,  GPIO_INTR_ANYEDGE);
    if (count == 1)
        gpio_install_isr_service(0);
    gpio_isr_handler_add(pin_clk, encoder_isr, e);
    gpio_isr_handler_add(pin_dt,  encoder_isr, e);
    return e;
}

int encoder_get_delta(encoder_t *e)
{
    return atomic_exchange(&e->delta, 0);
}

int encoder_button_pressed(encoder_t *e)
{
    return gpio_get_level(e->pin_sw) == 0;
}
