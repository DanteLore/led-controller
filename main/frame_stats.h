#pragma once

// Times the LED task's stages, frame by frame, and averages them over a
// heartbeat for /api/cpu and the log

#include "control.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { STAGE_RENDER, STAGE_STRIP, STAGE_SCREEN, STAGE_COUNT } frame_stage_t;

typedef struct {
    int64_t spent[STAGE_COUNT];
    int64_t frame_us;          // whole loop bodies
    uint32_t frames;
    int64_t since_us;          // start of this heartbeat
} frame_stats_t;

void frame_stats_init(frame_stats_t *stats, int64_t now_us);

// Ends a stage that began at started_us and returns the time now, when the next begins
int64_t frame_stats_lap(frame_stats_t *stats, frame_stage_t stage, int64_t started_us);

void frame_stats_frame_done(frame_stats_t *stats, int64_t frame_started_us);

// Once a heartbeat has passed: the averages, and a fresh start. Otherwise false.
bool frame_stats_heartbeat(frame_stats_t *stats, int64_t now_us, frame_timing_t *timing);
