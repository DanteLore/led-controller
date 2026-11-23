#pragma once

// The LCD. Every function does nothing on boards built without one.

#include "patch.h"
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_LINE_CHARS 18   // fits left of the QR code on text screens

typedef enum { TEXT_HEADING, TEXT_NORMAL, TEXT_DIM } text_style_t;

typedef struct {
    const char *text;
    text_style_t style;
} screen_line_t;

extern const bool screen_present;

void screen_init(void);
void screen_splash(void);   // the DANTELORE logo, full screen

// Play screen: patch name, both knobs' settings, a live preview of the strip
void screen_play(void);                                    // the fixed parts; clears the screen
void screen_network(const char *text, bool good, bool warn);   // address or WiFi state, top right
void screen_patch(const char *name, bool web_draft);
void screen_brightness(int percent);
void screen_speed(float multiplier, float position);       // position 0–1 along the bar
void screen_preview(const rgb16_t *frame, int count);
// level 0–1: a bar under the preview, then the BPM, a belief box (red unsure, green sure, blue
// in a breakdown) and a beat dot
void screen_mic(float level, float bpm, bool beat_found, float belief, bool breakdown, bool on_beat);

// Lines of text down the left, and a QR code on the right if qr_text isn't NULL
void screen_text(const screen_line_t *lines, int count, const char *qr_text);
