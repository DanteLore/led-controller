#include "ui.h"
#include "screen.h"
#include "mic.h"
#include "wifi.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PREVIEW_EVERY   4        // LCD preview at 50fps; it takes ~3ms to draw
#define WIFI_CHECK_US   500000
#define NOTHING_DRAWN  -1

#define LINES(...) (screen_line_t[]){ __VA_ARGS__ }, sizeof((screen_line_t[]){ __VA_ARGS__ }) / sizeof(screen_line_t)

// The address once joined, or the setup hotspot to join while it's up
typedef struct {
    wifi_status_t wifi;
    char qr[64];   // what the QR code holds; empty while still joining
} network_t;

// What the play screen shows now, to redraw only what changes
typedef struct {
    char name[PATCH_NAME_MAX + 1];
    bool draft;
    int brightness;
    float speed;
} play_shown_t;

static network_t network;
static int64_t next_wifi_check;
static int drawn = NOTHING_DRAWN;   // panel_t
static play_shown_t shown;
static uint32_t frames;

// ---- Network

static void read_network(network_t *n)
{
    wifi_get_status(&n->wifi);
    if (n->wifi.state == WIFI_CONNECTED)
        snprintf(n->qr, sizeof(n->qr), "http://%s/", n->wifi.ip);
    else if (n->wifi.hotspot)
        // Phone cameras offer to join a network from a code like this. Our
        // hotspot's name has none of the characters that would need escaping.
        snprintf(n->qr, sizeof(n->qr), "WIFI:T:nopass;S:%s;;", n->wifi.hotspot_ssid);
    else
        n->qr[0] = 0;
}

// Returns true if what the screens show about the network has changed
static bool poll_network(int64_t now)
{
    if (now < next_wifi_check)
        return false;
    next_wifi_check = now + WIFI_CHECK_US;
    char was[sizeof(network.qr)];
    strlcpy(was, network.qr, sizeof(was));
    read_network(&network);
    return strcmp(was, network.qr) != 0;
}

// ---- Setup and Edit

static void draw_setup(void)
{
    const wifi_status_t *w = &network.wifi;
    if (w->state == WIFI_CONNECTED)
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "SCAN TO OPEN", TEXT_NORMAL }, { "THE EDITOR", TEXT_NORMAL },
                          { "", 0 }, { "OR BROWSE TO", TEXT_DIM }, { w->ip, TEXT_NORMAL }, { "", 0 },
                          { "WIFI", TEXT_DIM }, { w->ssid, TEXT_NORMAL }), network.qr);
    else if (w->hotspot)
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "SCAN TO JOIN", TEXT_NORMAL }, { "THE SETUP WIFI", TEXT_NORMAL },
                          { "", 0 }, { "OR JOIN", TEXT_DIM }, { w->hotspot_ssid, TEXT_NORMAL },
                          { "AND THE SETUP", TEXT_DIM }, { "PAGE POPS UP", TEXT_DIM }), network.qr);
    else
        screen_text(LINES({ "SETUP", TEXT_HEADING }, { "", 0 }, { "JOINING WIFI", TEXT_NORMAL }, { w->ssid, TEXT_NORMAL }), NULL);
}

// A placeholder until patches can be edited on the device. No QR code, so
// it can't be mistaken for Setup.
static void draw_edit(void)
{
    screen_text(LINES({ "EDIT MODE", TEXT_HEADING }, { "", 0 }, { "COMING SOON.", TEXT_NORMAL }, { "", 0 },
                      { "FOR NOW, EDIT IN", TEXT_NORMAL }, { "THE WEB EDITOR:", TEXT_NORMAL },
                      { "SETUP SHOWS WHERE", TEXT_DIM }), NULL);
}

// ---- Play

static void draw_network(void)
{
    if (network.wifi.state == WIFI_CONNECTED)
        screen_network(network.wifi.ip, true, false);
    else if (network.wifi.hotspot)
        screen_network("WIFI SETUP", false, true);
    else
        screen_network("JOINING WIFI", false, false);
}

static void draw_play(void)
{
    screen_play();
    draw_network();
    shown = (play_shown_t){ .brightness = -1, .speed = -1 };   // matches nothing, so it's all drawn
}

static void update_settings(const player_status_t *p)
{
    if (strcmp(p->name, shown.name) != 0 || p->draft != shown.draft) {
        screen_patch(p->name, p->draft);
        strlcpy(shown.name, p->name, sizeof(shown.name));
        shown.draft = p->draft;
    }
    if (p->brightness != shown.brightness) {
        screen_brightness(p->brightness);
        shown.brightness = p->brightness;
    }
    if (p->speed != shown.speed) {
        screen_speed(p->speed, p->speed_position);
        shown.speed = p->speed;
    }
}

static void update_preview(const beat_state_t *beat, const rgb16_t *frame, int count)
{
    bool on_beat = !beat->holding && beat->position - floor(beat->position) < 0.15;
    screen_preview(frame, count);
    screen_mic(mic_level(), beat->bpm, !beat->holding, beat->belief, beat->breakdown, on_beat);
}

// ---- Public

void ui_init(void)
{
    screen_init();
}

void ui_update(int64_t now, panel_t panel, const player_status_t *player, const beat_state_t *beat,
               const rgb16_t *frame, int count)
{
    bool network_changed = poll_network(now);
    if (panel != drawn || (network_changed && panel == PANEL_SETUP)) {
        if (panel == PANEL_SETUP) draw_setup();
        if (panel == PANEL_EDIT)  draw_edit();
        if (panel == PANEL_PLAY)  draw_play();
        drawn = panel;
    } else if (panel == PANEL_PLAY && network_changed) {
        draw_network();
    }
    if (panel != PANEL_PLAY)
        return;
    update_settings(player);
    if (++frames % PREVIEW_EVERY == 0)
        update_preview(beat, frame, count);
}
