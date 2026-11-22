#include "board.h"
#include "control.h"
#include "frame_stats.h"
#include "inputs.h"
#include "library.h"
#include "mic.h"
#include "player.h"
#include "ui.h"
#include "web.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define HOSTNAME  "led-controller"

// 200fps keeps temporal dithering fast enough not to flicker at dim levels.
// Each pixel takes 30µs on the wire, so this holds up to ~150 pixels.
#define FRAME_MS  5

// WiFi runs on core 0, so the LEDs get core 1 to themselves: WiFi interrupts
// would otherwise delay the RMT refills mid-frame and corrupt the data
#define LED_CORE        1
#define LED_PRIORITY   10
#define LED_STACK    6144
#define MIC_CORE        0

static const char *TAG = "led-controller";

static rgb16_t frame[LED_COUNT];

static void apply_web_commands(void)
{
    command_t command;
    while (control_next_command(&command)) {
        if (command.type == COMMAND_BRIGHTNESS)
            player_set_brightness(command.value);
        else
            player_select(command.value);
    }
}

static void show_frame(uint16_t level)
{
    for (int i = 0; i < LED_COUNT; i++)
        led_strip_set_pixel16(i, (uint32_t)frame[i].r * level / LEVEL_FULL,
                                 (uint32_t)frame[i].g * level / LEVEL_FULL,
                                 (uint32_t)frame[i].b * level / LEVEL_FULL);
    led_strip_show();
}

static void publish_state(const player_status_t *p, const beat_state_t *beat)
{
    control_state_t state = { p->mode, p->draft, p->brightness, p->speed, LED_COUNT, *beat };
    control_publish_state(&state);
}

static void heartbeat(frame_stats_t *stats, int64_t now, const player_status_t *p)
{
    frame_timing_t timing;
    if (!frame_stats_heartbeat(stats, now, &timing))
        return;
    control_publish_timing(&timing);
    ESP_LOGI(TAG, "Running: %s, brightness %d%%, speed %.2fx, %.0f fps", p->name, p->brightness, p->speed, timing.fps);
}

// Interrupts are set up on the core that registers them, so the strip,
// encoders and LCD are all initialised inside the task
static void led_task(void *arg)
{
    ui_init();
    inputs_init();
    led_strip_init(LED_PIN, LED_COUNT, LED_ORDER);
    player_init();
    frame_stats_t stats;
    frame_stats_init(&stats, esp_timer_get_time());

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        int64_t now = esp_timer_get_time();
        inputs_t in = inputs_read(now);
        player_step(in.step);
        apply_web_commands();
        beat_state_t beat;
        mic_beat(&beat);

        int64_t t = esp_timer_get_time();
        player_frame(now, in.brightness_delta, in.speed_delta, &beat, frame, LED_COUNT);
        player_status_t status = player_status();
        t = frame_stats_lap(&stats, STAGE_RENDER, t);
        show_frame(status.level);
        t = frame_stats_lap(&stats, STAGE_STRIP, t);
        ui_update(now, in.panel, &status, &beat, frame, LED_COUNT);
        publish_state(&status, &beat);
        frame_stats_lap(&stats, STAGE_SCREEN, t);
        frame_stats_frame_done(&stats, now);

        heartbeat(&stats, now, &status);
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
    control_init();
    wifi_start(HOSTNAME);
    web_start(wifi_web_server());
    mic_start(MIC_PIN_SCK, MIC_PIN_WS, MIC_PIN_SD, MIC_CORE);
    xTaskCreatePinnedToCore(led_task, "leds", LED_STACK, NULL, LED_PRIORITY, NULL, LED_CORE);
}
