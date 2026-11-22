#include "mic.h"
#include "beat.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>

#define FRAMES       256      // per read: about 12ms
#define FULL_SCALE   8388608.0f   // 24-bit samples
#define METER_FLOOR_DB  -70.0f
#define METER_TOP_DB    -10.0f
#define METER_FALL       0.92f    // per read, so about 1s from full to empty
#define HIGH_PASS_HZ    60.0f     // below the beat; removes the mic's offset drift and rumble
#define LOG_US         1000000
#define MIC_SLOT         0        // the INMP441 answers in the first slot with L/R to GND
#define LOOKAHEAD_US 20000        // allowance for the audio buffer and analysis; tune by eye

static const char *TAG = "mic";

static i2s_chan_handle_t channel;
static _Atomic float level;

// The beat clock as of the last hop, and how fast it moves
static portMUX_TYPE beat_lock = portMUX_INITIALIZER_UNLOCKED;
static beat_state_t beat_at;
static int64_t beat_at_us;
static double beats_per_us;

#define RECORD_BUFFER 16384   // bytes: about 0.4s of slack for the network
static StreamBufferHandle_t recording;
static atomic_bool recording_on;

// Second-order Butterworth high-pass, one per channel
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;

static void high_pass_init(biquad_t *f, float cutoff_hz, float rate)
{
    float w = 2 * (float)M_PI * cutoff_hz / rate;
    float alpha = sinf(w) / (2 * 0.7071f);
    float a0 = 1 + alpha;
    *f = (biquad_t){
        .b0 = (1 + cosf(w)) / 2 / a0,
        .b1 = -(1 + cosf(w)) / a0,
        .b2 = (1 + cosf(w)) / 2 / a0,
        .a1 = -2 * cosf(w) / a0,
        .a2 = (1 - alpha) / a0,
    };
}

static float high_pass(biquad_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

static float to_db(float rms)
{
    return 20.0f * log10f(rms / FULL_SCALE + 1e-9f);
}

// Two channels, since which slot the mic answers in depends on its L/R pin and
// on how this chip orders them. Sums each channel's power into block, and puts
// the mic's slot, scaled to ±1, in hop. Returns the frames read, or 0.
static int read_block(biquad_t filter[2], float *hop, double block[2])
{
    static int32_t samples[FRAMES * 2];
    size_t got = 0;
    if (i2s_channel_read(channel, samples, sizeof(samples), &got, portMAX_DELAY) != ESP_OK)
        return 0;
    int frames = got / (2 * sizeof(int32_t));
    block[0] = block[1] = 0;
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < 2; c++) {
            int32_t raw = samples[2 * i + c];
            float ac = high_pass(&filter[c], (float)(raw >> 8));   // 24 bits, left-justified in 32
            block[c] += ac * ac;
            if (c == MIC_SLOT) hop[i] = ac / FULL_SCALE;
        }
    return frames;
}

static void record(const float *hop, int frames)
{
    static int16_t out[FRAMES];
    for (int i = 0; i < frames; i++) {
        float v = hop[i] * 32767;
        out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
    }
    xStreamBufferSend(recording, out, frames * sizeof(int16_t), 0);   // drops if the reader falls behind
}

static void track_beat(const float *hop)
{
    beat_process(hop);
    beat_state_t state;
    beat_get(&state);
    taskENTER_CRITICAL(&beat_lock);
    beat_at = state;
    beat_at_us = esp_timer_get_time();
    beats_per_us = state.bpm / 60e6;
    taskEXIT_CRITICAL(&beat_lock);
}

// The louder channel is taken as the mic
static float update_meter(double block[2], int frames)
{
    static float meter;
    float db = to_db(sqrtf((block[0] > block[1] ? block[0] : block[1]) / frames));
    float target = (db - METER_FLOOR_DB) / (METER_TOP_DB - METER_FLOOR_DB);
    target = target < 0 ? 0 : target > 1 ? 1 : target;
    meter = target > meter ? target : meter * METER_FALL;
    atomic_store(&level, meter);
    return db;
}

// Both slots' levels over the last second, and the beat, for the log
typedef struct {
    double energy[2];
    float peak_db;
    int blocks;
} level_log_t;

static void log_reset(level_log_t *log)
{
    *log = (level_log_t){ .peak_db = METER_FLOOR_DB - 30 };
}

static void log_add(level_log_t *log, double block[2], int frames, float db)
{
    log->energy[0] += block[0] / frames;
    log->energy[1] += block[1] / frames;
    if (db > log->peak_db) log->peak_db = db;
    log->blocks++;
}

static void log_write(const level_log_t *log)
{
    beat_state_t b;
    mic_beat(&b);
    ESP_LOGI(TAG, "Level %.0fdB (other slot %.0fdB), loudest %.0fdB; beat %.1f BPM, confidence %.2f, energy %.2f, belief %.2f%s",
             to_db(sqrtf(log->energy[MIC_SLOT] / log->blocks)), to_db(sqrtf(log->energy[1 - MIC_SLOT] / log->blocks)),
             log->peak_db, b.bpm, b.confidence, b.energy, b.belief,
             b.holding ? ", holding" : b.breakdown ? ", breakdown" : b.coasting ? ", coasting" : "");
}

static void reader(void *arg)
{
    biquad_t filter[2];
    high_pass_init(&filter[0], HIGH_PASS_HZ, MIC_SAMPLE_RATE);
    high_pass_init(&filter[1], HIGH_PASS_HZ, MIC_SAMPLE_RATE);
    static float hop[FRAMES];
    level_log_t log;
    log_reset(&log);
    int64_t next_log = esp_timer_get_time() + LOG_US;

    while (1) {
        double block[2];
        int frames = read_block(filter, hop, block);
        if (!frames)
            continue;
        if (atomic_load(&recording_on))
            record(hop, frames);
        if (frames == BEAT_HOP)
            track_beat(hop);
        log_add(&log, block, frames, update_meter(block, frames));

        int64_t now = esp_timer_get_time();
        if (now >= next_log) {
            log_write(&log);
            log_reset(&log);
            next_log = now + LOG_US;
        }
    }
}

void mic_start(int pin_sck, int pin_ws, int pin_sd, int core)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = FRAMES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &channel));

    // The INMP441 sends 24-bit samples in 32-bit slots, MSB first, Philips timing
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = pin_sck,
            .ws   = pin_ws,
            .dout = I2S_GPIO_UNUSED,
            .din  = pin_sd,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(channel, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(channel));
    beat_init(MIC_SAMPLE_RATE);
    recording = xStreamBufferCreate(RECORD_BUFFER, 1);
    xTaskCreatePinnedToCore(reader, "mic", 4096, NULL, 5, NULL, core);
    ESP_LOGI(TAG, "Listening at %dHz", MIC_SAMPLE_RATE);
}

float mic_level(void)
{
    return atomic_load(&level);
}

bool mic_record_start(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&recording_on, &expected, true))
        return false;
    xStreamBufferReset(recording);
    return true;
}

size_t mic_record_read(int16_t *samples, size_t max, int timeout_ms)
{
    return xStreamBufferReceive(recording, samples, max * sizeof(int16_t), pdMS_TO_TICKS(timeout_ms)) / sizeof(int16_t);
}

void mic_record_stop(void)
{
    atomic_store(&recording_on, false);
}

void mic_beat(beat_state_t *beat)
{
    taskENTER_CRITICAL(&beat_lock);
    *beat = beat_at;
    int64_t since = esp_timer_get_time() - beat_at_us + LOOKAHEAD_US;
    double rate = beats_per_us;
    taskEXIT_CRITICAL(&beat_lock);
    double next = beat->position + since * rate;
    // While holding, don't run on into the next beat
    if (beat->holding && floor(next) != floor(beat->position))
        next = floor(next) - 1e-6;
    beat->position = next;
}
