#include "frame_stats.h"
#include "esp_timer.h"

#define HEARTBEAT_US  5000000

void frame_stats_init(frame_stats_t *stats, int64_t now)
{
    *stats = (frame_stats_t){ .since_us = now };
}

int64_t frame_stats_lap(frame_stats_t *stats, frame_stage_t stage, int64_t started)
{
    int64_t now = esp_timer_get_time();
    stats->spent[stage] += now - started;
    return now;
}

void frame_stats_frame_done(frame_stats_t *stats, int64_t frame_started)
{
    stats->frame_us += esp_timer_get_time() - frame_started;
    stats->frames++;
}

bool frame_stats_heartbeat(frame_stats_t *stats, int64_t now, frame_timing_t *timing)
{
    int64_t elapsed = now - stats->since_us;
    uint32_t n = stats->frames;
    if (elapsed < HEARTBEAT_US || !n)
        return false;
    *timing = (frame_timing_t){
        .fps = n * 1e6f / elapsed,
        .render_us = (float)stats->spent[STAGE_RENDER] / n,
        .strip_us = (float)stats->spent[STAGE_STRIP] / n,
        .screen_us = (float)stats->spent[STAGE_SCREEN] / n,
        .frame_us = (float)stats->frame_us / n,
    };
    frame_stats_init(stats, now);
    return true;
}
