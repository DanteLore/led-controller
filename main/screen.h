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

// Play screen: patch name, both knobs' settings, a live preview of the strip
void screen_play(void);                                    // the fixed parts; clears the screen
void screen_network(const char *text, bool good, bool warn);   // address or WiFi state, top right
void screen_patch(const char *name, bool web_draft);
void screen_brightness(int percent);
void screen_speed(float multiplier, float position);       // position 0–1 along the bar
void screen_preview(const rgb16_t *frame, int count);
void screen_mic(float level);                              // 0–1, a thin bar under the preview

// Lines of text down the left, and a QR code on the right if qr_text isn't NULL
void screen_text(const screen_line_t *lines, int count, const char *qr_text);
