#pragma once

// Onsets in one band of the spectrum (all of it, the kick's boom or its
// punch): a ring of the last ~6 seconds, and what's been learnt of how hard
// this music hits there. Private to the beat component.

#define ONSET_HISTORY  512   // onset frames kept: about 6s at 86 frames/s
#define ONSET_RECENT    16   // frames in the moving average subtracted from the flux

typedef struct {
    float ring[ONSET_HISTORY];     // newest at head - 1
    int head;
    float recent[ONSET_RECENT];    // latest flux, for the moving average
    float peak;                    // level of recent strong hits, behind the outlier cap
    float level;                   // the music's hits while the beat is clear
} band_t;

// Turns this hop's flux into an onset strength and adds it. known_level, if
// above 0, keeps the outlier cap from falling below the music's known hits.
float band_add(band_t *b, float flux, float known_level);

float band_at(const band_t *b, int ago);              // 0 = newest
float band_strongest(const band_t *b, int frames_back);
float band_total(const band_t *b);

// Clears the onsets and what's been learnt, keeping the moving average
void band_forget(band_t *b);
