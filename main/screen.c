#include "screen.h"
#include "sdkconfig.h"
#include "st7789.h"
#include "qr.h"
#include <stdio.h>

#if CONFIG_LED_CONTROLLER_LCD

// Landscape 320x170, 8x8 font
#define MARGIN_X      20
#define VALUE_X      (MARGIN_X + 96)
#define TITLE_Y        8
#define PATCH_Y       30
#define BRIGHT_Y      50
#define BRIGHT_BAR_Y  62
#define SPEED_Y       78
#define SPEED_BAR_Y   90
#define PREVIEW_Y    110
#define PREVIEW_H     22
#define MIC_Y        139
#define MIC_BAR_X    (MARGIN_X + 32)
#define MIC_BAR_W    (BAR_W - 32 - 76)   // leaves room for the BPM, the belief box and the beat dot
#define BPM_X        (MIC_BAR_X + MIC_BAR_W + 8)
#define BELIEF_X     (MARGIN_X + BAR_W - 20)
#define BEAT_DOT_X   (MARGIN_X + BAR_W - 8)
#define MIC_BAR_H      6
#define HINT_Y       152
#define BAR_W        280
#define BAR_H          8
#define NETWORK_X    140   // right half of the title line
#define NETWORK_CHARS 20
#define QR_SCALE       4   // pixels per module: 37 modules × 4 fits the 170-pixel height
#define QR_QUIET       4   // light modules round the code, which scanners need
#define TEXT_X        16
#define TEXT_LINE_H   14

#define COLOR_GREY    RGB565(110, 110, 110)

const bool screen_present = true;

void screen_init(void)
{
    st7789_init();
}

// The knobs sit either side of the screen, so the hint matches them up
void screen_play(void)
{
    st7789_fill(COLOR_BLACK);
    st7789_draw_string(MARGIN_X, TITLE_Y, "LED CONTROLLER", COLOR_CYAN, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, BRIGHT_Y, "BRIGHTNESS", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, SPEED_Y, "SPEED", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, MIC_Y - 1, "MIC", COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(MARGIN_X, HINT_Y, "BRIGHTNESS     < PATCH >     SPEED", COLOR_GREY, COLOR_BLACK);
}

void screen_network(const char *text, bool good, bool warn)
{
    char padded[NETWORK_CHARS + 1];
    snprintf(padded, sizeof(padded), "%*s", NETWORK_CHARS, text);
    st7789_draw_string(NETWORK_X, TITLE_Y, padded, good ? COLOR_WHITE : warn ? COLOR_YELLOW : COLOR_GREY, COLOR_BLACK);
}

// Padded so a shorter name overwrites a longer one. WEB while the web editor's draft plays.
void screen_patch(const char *name, bool web_draft)
{
    char padded[PATCH_NAME_MAX + 1];
    snprintf(padded, sizeof(padded), "%-*s", PATCH_NAME_MAX, name);
    st7789_draw_string(MARGIN_X, PATCH_Y, web_draft ? "WEB  " : "PATCH", web_draft ? COLOR_YELLOW : COLOR_GREY, COLOR_BLACK);
    st7789_draw_string(VALUE_X, PATCH_Y, padded, COLOR_WHITE, COLOR_BLACK);
}

static void draw_bar(int y, int filled)
{
    if (filled > 0)
        st7789_fill_rect(MARGIN_X, y, filled, BAR_H, COLOR_YELLOW);
    if (filled < BAR_W)
        st7789_fill_rect(MARGIN_X + filled, y, BAR_W - filled, BAR_H, COLOR_GREY);
}

void screen_brightness(int percent)
{
    char text[12];
    snprintf(text, sizeof(text), "%5d%%", percent);
    st7789_draw_string(VALUE_X, BRIGHT_Y, text, COLOR_WHITE, COLOR_BLACK);
    draw_bar(BRIGHT_BAR_Y, BAR_W * percent / 100);
}

void screen_speed(float multiplier, float position)
{
    char text[12];
    snprintf(text, sizeof(text), "%5.2fx", multiplier);
    st7789_draw_string(VALUE_X, SPEED_Y, text, COLOR_WHITE, COLOR_BLACK);
    draw_bar(SPEED_BAR_Y, BAR_W * position);
}

// Colours before brightness, so the preview stays readable when dim
void screen_preview(const rgb16_t *frame, int count)
{
    static uint16_t buf[BAR_W * PREVIEW_H];
    for (int x = 0; x < BAR_W; x++) {
        rgb16_t c = frame[x * count / BAR_W];
        uint16_t colour = RGB565(c.r >> 8, c.g >> 8, c.b >> 8);
        uint16_t swapped = (colour >> 8) | (colour << 8);
        for (int y = 0; y < PREVIEW_H; y++)
            buf[y * BAR_W + x] = swapped;
    }
    st7789_draw_pixels(MARGIN_X, PREVIEW_Y, BAR_W, PREVIEW_H, buf);
}

void screen_mic(float level, float bpm, bool beat_found, float belief, bool on_beat)
{
    char text[8];
    if (beat_found) snprintf(text, sizeof(text), "%3.0f", bpm);
    else snprintf(text, sizeof(text), "---");
    // Coasting through a breakdown is still following the beat, so it stays white
    st7789_draw_string(BPM_X, MIC_Y - 1, text, beat_found ? COLOR_WHITE : COLOR_GREY, COLOR_BLACK);
    // Red through yellow to green as belief grows; dark when there's no beat
    int r = belief < 0.5f ? 255 : (int)(255 * (1 - belief) * 2), g = belief > 0.5f ? 255 : (int)(255 * belief * 2);
    st7789_fill_rect(BELIEF_X, MIC_Y - 1, 8, 8, beat_found ? RGB565(r, g, 0) : RGB565(40, 40, 40));
    st7789_fill_rect(BEAT_DOT_X, MIC_Y - 1, 8, 8, on_beat ? COLOR_YELLOW : RGB565(40, 40, 40));
    int filled = MIC_BAR_W * level;
    if (filled > 0)
        st7789_fill_rect(MIC_BAR_X, MIC_Y, filled, MIC_BAR_H, COLOR_GREEN);
    if (filled < MIC_BAR_W)
        st7789_fill_rect(MIC_BAR_X + filled, MIC_Y, MIC_BAR_W - filled, MIC_BAR_H, RGB565(40, 40, 40));
}

static void draw_qr(const char *text)
{
    static bool modules[QR_SIZE_MAX][QR_SIZE_MAX];
    static uint16_t row[(QR_SIZE_MAX + 2 * QR_QUIET) * QR_SCALE * QR_SCALE];
    int n = qr_encode(text, modules);
    if (!n) return;
    int width = (n + 2 * QR_QUIET) * QR_SCALE;
    int x0 = ST7789_WIDTH - width - 12, y0 = (ST7789_HEIGHT - width) / 2;
    // A strip of QR_SCALE pixel rows per module row, quiet zone included
    for (int my = -QR_QUIET; my < n + QR_QUIET; my++) {
        for (int mx = -QR_QUIET; mx < n + QR_QUIET; mx++) {
            bool on = mx >= 0 && mx < n && my >= 0 && my < n && modules[my][mx];
            for (int py = 0; py < QR_SCALE; py++)
                for (int px = 0; px < QR_SCALE; px++)
                    row[py * width + (mx + QR_QUIET) * QR_SCALE + px] = on ? COLOR_BLACK : COLOR_WHITE;
        }
        st7789_draw_pixels(x0, y0 + (my + QR_QUIET) * QR_SCALE, width, QR_SCALE, row);
    }
}

#define TEXT_WIDE_CHARS 37   // a text line with no QR code beside it

void screen_text_line(int count, int i, const char *text, text_style_t style)
{
    static const uint16_t colours[] = { [TEXT_HEADING] = COLOR_CYAN, [TEXT_NORMAL] = COLOR_WHITE, [TEXT_DIM] = COLOR_GREY };
    char padded[TEXT_WIDE_CHARS + 1];
    snprintf(padded, sizeof(padded), "%-*s", TEXT_WIDE_CHARS, text);
    int y = (ST7789_HEIGHT - count * TEXT_LINE_H) / 2 + i * TEXT_LINE_H;
    st7789_draw_string(TEXT_X, y, padded, colours[style], COLOR_BLACK);
}

void screen_text(const screen_line_t *lines, int count, const char *qr_text)
{
    static const uint16_t colours[] = { [TEXT_HEADING] = COLOR_CYAN, [TEXT_NORMAL] = COLOR_WHITE, [TEXT_DIM] = COLOR_GREY };
    st7789_fill(COLOR_BLACK);
    int y = (ST7789_HEIGHT - count * TEXT_LINE_H) / 2;
    for (int i = 0; i < count; i++, y += TEXT_LINE_H) {
        char clipped[TEXT_WIDE_CHARS + 1];
        snprintf(clipped, qr_text ? SCREEN_LINE_CHARS + 1 : sizeof(clipped), "%s", lines[i].text);
        st7789_draw_string(TEXT_X, y, clipped, colours[lines[i].style], COLOR_BLACK);
    }
    if (qr_text)
        draw_qr(qr_text);
}

#else

const bool screen_present = false;

void screen_init(void) {}
void screen_play(void) {}
void screen_network(const char *text, bool good, bool warn) {}
void screen_patch(const char *name, bool web_draft) {}
void screen_brightness(int percent) {}
void screen_speed(float multiplier, float position) {}
void screen_preview(const rgb16_t *frame, int count) {}
void screen_mic(float level, float bpm, bool beat_found, float belief, bool on_beat) {}
void screen_text(const screen_line_t *lines, int count, const char *qr_text) {}
void screen_text_line(int count, int i, const char *text, text_style_t style) {}

#endif
