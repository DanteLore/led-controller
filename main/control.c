#include "control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define COMMAND_QUEUE 8

static QueueHandle_t commands;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static control_state_t state;
static frame_timing_t timing;

void control_init(void)
{
    commands = xQueueCreate(COMMAND_QUEUE, sizeof(command_t));
}

// ---- Web server side

static void send_command(command_type_t type, int value)
{
    command_t c = { type, value };
    xQueueSend(commands, &c, 0);
}

void control_select(int mode)
{
    send_command(COMMAND_SELECT, mode);
}

void control_set_brightness(int brightness)
{
    send_command(COMMAND_BRIGHTNESS, brightness);
}

void control_get_state(control_state_t *out)
{
    taskENTER_CRITICAL(&lock);
    *out = state;
    taskEXIT_CRITICAL(&lock);
}

void control_get_timing(frame_timing_t *out)
{
    taskENTER_CRITICAL(&lock);
    *out = timing;
    taskEXIT_CRITICAL(&lock);
}

// ---- LED task side

bool control_next_command(command_t *command)
{
    return xQueueReceive(commands, command, 0);
}

void control_publish_state(const control_state_t *in)
{
    taskENTER_CRITICAL(&lock);
    state = *in;
    taskEXIT_CRITICAL(&lock);
}

void control_publish_timing(const frame_timing_t *in)
{
    taskENTER_CRITICAL(&lock);
    timing = *in;
    taskEXIT_CRITICAL(&lock);
}
