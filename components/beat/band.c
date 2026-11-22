#include "band.h"
#include <string.h>

#define ONSET_CAP   3.0f    // no onset counts for more than this many times the level of recent
                            // strong hits, so one bang on the table can't outweigh the music
#define PEAK_RISE   0.05f   // that level climbs this fraction of the way to a bigger hit, per frame...
#define PEAK_FALL   0.998f  // ...and sinks by this factor per frame (halving in about 4s)

static int ring_index(const band_t *b, int ago)
{
    return ((b->head - 1 - ago) % ONSET_HISTORY + ONSET_HISTORY) % ONSET_HISTORY;
}

static float moving_average(const band_t *b)
{
    float mean = 0;
    for (int i = 0; i < ONSET_RECENT; i++) mean += b->recent[i];
    return mean / ONSET_RECENT;
}

// Flux minus its recent average, so a steady loud passage doesn't count as
// onsets, and capped against the level of recent strong hits, so a one-off
// thump doesn't either. The level climbs gradually, so music that gets louder
// is soon let through in full.
float band_add(band_t *b, float flux, float known_level)
{
    float mean = moving_average(b);
    b->recent[b->head % ONSET_RECENT] = flux;
    float o = flux - mean;
    if (o < 0) o = 0;
    // The cap never drops below 1.5× the music's known hits, so after a
    // breakdown the returning kicks count in full
    float reference = b->peak > known_level / 2 ? b->peak : known_level / 2;
    if (reference > 0 && o > ONSET_CAP * reference) o = ONSET_CAP * reference;
    b->peak = o > b->peak ? b->peak + (o - b->peak) * PEAK_RISE : b->peak * PEAK_FALL;
    b->ring[b->head] = o;
    b->head = (b->head + 1) % ONSET_HISTORY;
    return o;
}

float band_at(const band_t *b, int ago)
{
    return b->ring[ring_index(b, ago)];
}

float band_strongest(const band_t *b, int frames_back)
{
    float strongest = 0;
    for (int i = 0; i < frames_back; i++)
        if (band_at(b, i) > strongest) strongest = band_at(b, i);
    return strongest;
}

// In storage order, not time order: a sum over the whole ring
float band_total(const band_t *b)
{
    float total = 0;
    for (int i = 0; i < ONSET_HISTORY; i++) total += b->ring[i];
    return total;
}

void band_forget(band_t *b)
{
    memset(b->ring, 0, sizeof(b->ring));
    b->peak = b->level = 0;
}
