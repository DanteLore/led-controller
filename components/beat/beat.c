#include "beat.h"
#include <math.h>
#include <string.h>

#define BINS         (BEAT_FFT / 2)
#define HISTORY      512     // onset frames kept: about 6s at 86 frames/s
#define THRESHOLD_N  16      // frames in the moving average subtracted from the flux
#define UPDATE_EVERY 22      // frames between tempo and phase updates: about 0.25s
#define BPM_MIN      70.0f
#define BPM_MAX     180.0f
#define BPM_PREFERRED 120.0f
#define PRIOR_OCTAVES 1.0f   // width of the preference around BPM_PREFERRED
#define PHASE_BEATS   6      // past beats the phase is fitted against
#define PHASE_DECAY   0.8f   // weight of each older beat
#define PHASE_GAIN    0.3f   // how much of the phase error each update corrects
#define TEMPO_SMOOTH  0.25f  // how far each agreeing estimate moves the tempo
#define TEMPO_SWITCH  4      // disagreeing estimates in a row before jumping tempo
#define CONFIDENCE_START 0.25f   // a beat is believed once confidence reaches this...
#define CONFIDENCE_STOP  0.15f   // ...and dropped below this. Noise scores about 0.1, music 0.3 up
#define QUIET_RMS     0.0018f // below about -55dBFS is silence, whatever the onsets look like
#define LOG_SCALE   100.0f   // log(1 + LOG_SCALE·magnitude) compresses loud and quiet alike
// Two parts of the kick do different jobs, as tapped recordings showed: its
// boom (below 110Hz) reveals the tempo, while its punch (110-600Hz) shows
// where the beat falls, because the boom band also carries bass lines, which
// in dance music often sit on the off-beat
#ifndef BASS_TOP_HZ
#define BASS_BOTTOM_HZ  0.0f
#define BASS_TOP_HZ  110.0f  // the boom, for the tempo
#define BASS_WEIGHT  3.0f
#endif
#ifndef PUNCH_TOP_HZ
#define PUNCH_BOTTOM_HZ 110.0f   // the punch, for the phase
#define PUNCH_TOP_HZ    600.0f
#define PUNCH_WEIGHT      3.0f
#endif
#define ONSET_CAP    3.0f    // no onset counts for more than this many times the level of recent
                             // strong hits, so one bang on the table can't outweigh the music
#define PEAK_RISE    0.05f   // that level climbs this fraction of the way to a bigger hit, per frame...
#define PEAK_FALL    0.998f  // ...and sinks by this factor per frame (halving in about 4s)
// Belief in the beat, 0-1, earned like a listener's: a firm hit where a beat
// was expected raises it, a predicted beat with no hit lets it drift down (a
// breakdown), and a strong hit where nothing was expected knocks it. High
// belief makes the tracker stiff and the lights bold; low belief leaves it
// open to correction.
#define BELIEF_UP      0.15f // a confirmed beat closes this much of the gap to 1
#define BELIEF_MISS    0.93f // a beat with no hit keeps this much (halving in about 10 beats)
#define BELIEF_DOWN    0.12f // a surprise hit takes away this much of it
#define BEAT_WINDOW    0.12f // a hit within this many beats of the beat confirms it (about 50ms)
#define GRID_WINDOW    0.07f // hits this close to a half or quarter beat are neither: bass lines,
                             // hats and syncopation live there
#define SURPRISE_FRAC  0.8f  // and only hits this strong against the music's kicks are a surprise
#define HIT_FRAC       0.5f  // a hit is a punch or boom onset this strong against the music's own kicks
#define TRUST_BELIEF   0.4f  // above this the tempo is trusted: half, double and 3:2 readings are ignored
// A breakdown, as a dancer hears it: beats keep coming with no hits on them,
// and the music has gone quieter or the belief has faded (a loud pad with no
// kick is still a breakdown). The clock keeps time but the pulse stops, until
// a firm hit on an expected beat (the drop) brings it straight back.
#define BREAKDOWN_MISSES 3      // beats in a row with no hit (about 1.3s)...
#define BREAKDOWN_QUIET  0.6f   // ...and quieter than this fraction of the music's level (about -4dB)
#define BREAKDOWN_BELIEF 0.75f  // ...or belief below this
#define GROOVE_BELIEF    0.6f   // and only once belief has reached this in the song: no groove, no breakdown
#define COAST_HIT        0.75f  // while the beat is unclear, only hits this strong against the music's
                                // kicks count towards belief, so chatter landing on a beat can't fake it...
#define COAST_PULSE      0.6f   // ...and with belief this high the kicks are still landing on the beat,
                                // so the lights keep dancing (two tempos overlapping in a mix, say)
#define LOCK_UPDATES     8      // a beat must be clear for this many updates in a row (2s) before it's
                                // followed from silence, so a lucky moment of chatter can't start it
#define COAST_SECONDS    120    // and with no drop for this long, the music has stopped: forget the song
#define PULSE_FALL       0.04f  // the pulse fades this fraction of the way per hop (about 0.4s)
#define LOCKED_SWITCH  32    // at full belief, a different tempo must persist this many updates (8s),
#define LOCKED_BETTER  1.5f  // fitting the music this much better than the locked tempo still does.
                             // Half, double or 3:2 never counts: that's a new drum pattern over
                             // the same beat, not a new tempo
#define LOCKED_NUDGE    0.03f // ...and the phase moves at most this far per update (0.2 at no belief),
#define LOCKED_REPHASE 32    // unless it disagrees by over a quarter beat for this many updates (8s;
                             // 1s at no belief)
#define SILENCE_DROP   0.05f // a sudden fall to this fraction of the music's level (-26dB), or below
#define SILENCE_FRAMES  26   // QUIET_RMS, for this many hops (0.3s) is the song ending: stop at once
#define FAST_SMOOTH    0.2f  // the quick loudness behind that follows about 60ms
#define ENERGY_FALL    0.03f // energy falls this fraction of the way per hop (about half a second)
#define PHASE_CLEAR    2.5f  // while coasting, recent onsets this many times above average steer the phase
#define TEMPO_TRIM     0.08f // phase corrections that keep pushing one way also trim the tempo...
#define LOCKED_SMOOTH  0.04f // ...which, once locked, the coarse estimate barely moves

typedef struct { float re, im; } complex_t;

static float rate, frame_rate;
static float window[BEAT_FFT], hann[BEAT_FFT];
static float last_log[BINS];
static float onsets[HISTORY];      // ring, newest at onset_head - 1
static float bass_onsets[HISTORY]; // the same from the boom alone
static float bass_recent[THRESHOLD_N];
static float punch_onsets[HISTORY];  // and from the punch
static float punch_recent[THRESHOLD_N];
static float punch_peak;
static int bass_bins, bass_first, punch_first, punch_last;
static int onset_head;
static float flux_recent[THRESHOLD_N];
static float onset_peak, bass_peak;   // levels behind the outlier cap
static float belief;                  // see BELIEF_UP
static long confirmed_beat, judged_beat;   // the last beat a hit confirmed, and the last one judged
static int quiet_until;               // frame before which surprise hits aren't counted again
static float punch_level;             // the music's punch hits while the beat is clear
static bool breakdown;
static int misses;                    // expected beats in a row with no hit
static bool grooving;                 // belief has built up in this song, so a breakdown is possible
static float pulse_gate;              // 1 normally, fading to 0 in a breakdown
static bool tempo_known;              // the tempo has been learnt from this music, not just the starting guess
static int rephase_votes;             // updates in a row the phase has disagreed badly
static int frames;

static float period;               // frames per beat
static int disagreements;
static float pending_period;
static double position;
static float correction;           // phase still to be corrected, spread over the next update period
static float confidence;
static bool holding, coasting;
static int coast_updates;
static int clear_updates;             // updates in a row the beat has been clear enough to lock onto
static float drum_level;              // bass hits before the beat went quiet: their return means the drop
static float onset_level;             // the same for the full-spectrum onsets
static float energy;
static float latest_onset;
static float loudness;             // smoothed RMS of the audio
static float fast_loudness;        // the same over about 60ms, to catch the end of a song
static float music_level;          // loudness while following a clear beat
static int silent_frames;

// ---- FFT ----------------------------------------------------------------------

static void fft(complex_t *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { complex_t t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2 * (float)M_PI / len;
        complex_t step = { cosf(angle), sinf(angle) };
        for (int i = 0; i < n; i += len) {
            complex_t w = { 1, 0 };
            for (int k = 0; k < len / 2; k++) {
                complex_t a = x[i + k], b = x[i + k + len / 2];
                complex_t t = { b.re * w.re - b.im * w.im, b.re * w.im + b.im * w.re };
                x[i + k] = (complex_t){ a.re + t.re, a.im + t.im };
                x[i + k + len / 2] = (complex_t){ a.re - t.re, a.im - t.im };
                w = (complex_t){ w.re * step.re - w.im * step.im, w.re * step.im + w.im * step.re };
            }
        }
    }
}

// ---- Onsets ---------------------------------------------------------------------

static int ring_index(int ago)   // 0 = newest
{
    return ((onset_head - 1 - ago) % HISTORY + HISTORY) % HISTORY;
}

static float onset_at(int ago)
{
    return onsets[ring_index(ago)];
}

// Full-spectrum flux, plus the boom and punch parts of it
static float spectral_flux(float *bass, float *punch)
{
    static complex_t x[BEAT_FFT];
    for (int i = 0; i < BEAT_FFT; i++)
        x[i] = (complex_t){ window[i] * hann[i], 0 };
    fft(x, BEAT_FFT);
    float flux = 0;
    *bass = *punch = 0;
    for (int k = 1; k < BINS; k++) {
        float magnitude = sqrtf(x[k].re * x[k].re + x[k].im * x[k].im);
        float level = logf(1 + LOG_SCALE * magnitude);
        float rise = level - last_log[k];
        if (rise > 0) {
            flux += rise;
            if (k <= bass_bins && k >= bass_first) *bass += rise;
            if (k >= punch_first && k <= punch_last) *punch += rise;
        }
        last_log[k] = level;
    }
    return flux;
}

// Flux minus its recent average, so a steady loud passage doesn't count as
// onsets, and capped against the level of recent strong hits, so a one-off
// thump doesn't either. The level climbs gradually, so music that gets louder
// is soon let through in full.
static float onset_strength(float flux, float *recent, float *peak, float known_level)
{
    float mean = 0;
    for (int i = 0; i < THRESHOLD_N; i++) mean += recent[i];
    mean /= THRESHOLD_N;
    recent[frames % THRESHOLD_N] = flux;
    float o = flux - mean;
    if (o < 0) o = 0;
    // The cap never drops below 1.5× the music's known hits, so after a
    // breakdown the returning kicks count in full
    float reference = *peak > known_level / 2 ? *peak : known_level / 2;
    if (reference > 0 && o > ONSET_CAP * reference) o = ONSET_CAP * reference;
    *peak = o > *peak ? *peak + (o - *peak) * PEAK_RISE : *peak * PEAK_FALL;
    return o;
}

// ---- Tempo ----------------------------------------------------------------------

static float history[HISTORY];   // onsets unrolled, newest first, for the tempo maths

static float autocorrelation(int lag)
{
    float sum = 0;
    for (int i = 0; i + lag < HISTORY; i++)
        sum += history[i] * history[i + lag];
    return sum / (HISTORY - lag);
}

// Whether the newest quarter of the history has its share of onsets: when the
// drums stop, the tempo can only be guessed from older, fading evidence
static bool onsets_recent(void)
{
    float newest = 0, all = 0;
    for (int i = 0; i < HISTORY; i++) {
        all += onset_at(i);
        if (i < HISTORY / 4) newest += onset_at(i);
    }
    return all > 0 && newest > 0.15f * all;
}

static float strongest_bass(int frames_back)
{
    float strongest = 0;
    for (int i = 0; i < frames_back; i++)
        if (bass_onsets[ring_index(i)] > strongest) strongest = bass_onsets[ring_index(i)];
    return strongest;
}

static float strongest_punch(int frames_back)
{
    float strongest = 0;
    for (int i = 0; i < frames_back; i++)
        if (punch_onsets[ring_index(i)] > strongest) strongest = punch_onsets[ring_index(i)];
    return strongest;
}

// Judges the newest hop against the beat clock: see BELIEF_UP
static void update_belief(void)
{
    if (holding || !tempo_known) {
        belief = 0;
        return;
    }
    double nearest = floor(position + 0.5);
    float from_beat = (float)(position - nearest);   // -0.5 to 0.5
    long beat = (long)nearest;
    int newest = ring_index(0);
    float need = coasting ? COAST_HIT : HIT_FRAC;
    bool hit = (punch_level > 0 && punch_onsets[newest] >= need * punch_level)
            || (drum_level > 0 && bass_onsets[newest] >= need * drum_level);
    if (hit && fabsf(from_beat) <= BEAT_WINDOW) {
        if (beat != confirmed_beat) {
            belief += BELIEF_UP * (1 - belief);
            confirmed_beat = beat;
            misses = 0;
            breakdown = false;   // a firm hit on the beat: the drop, or the music carrying on
        }
    } else if (hit && frames >= quiet_until) {
        // A surprise: a strong hit that the beat grid, down to its quarters, can't explain
        float quarters = fabsf(from_beat) * 4;
        bool on_grid = fabsf(quarters - roundf(quarters)) * 0.25f < GRID_WINDOW;
        int newest_hit = ring_index(0);
        bool strong = (punch_level > 0 && punch_onsets[newest_hit] >= SURPRISE_FRAC * punch_level)
                   || (drum_level > 0 && bass_onsets[newest_hit] >= SURPRISE_FRAC * drum_level);
        if (on_grid || !strong) return;
        belief -= BELIEF_DOWN * belief;
        quiet_until = frames + (int)(0.15f * period);   // one hit lasts a few hops: count it once
        return;
    }
    if (from_beat > BEAT_WINDOW && beat != confirmed_beat && beat != judged_beat) {
        belief *= BELIEF_MISS;   // this beat's window has passed with no hit
        judged_beat = beat;
        misses++;
    }
}

// Breakdown in or out, and the pulse the lights should show
static void update_pulse(void)
{
    if (holding || !tempo_known)
        breakdown = grooving = false;
    else if (belief >= GROOVE_BELIEF)
        grooving = true;
    if (!breakdown && grooving && misses >= BREAKDOWN_MISSES
             && ((music_level > 0 && loudness < BREAKDOWN_QUIET * music_level) || belief < BREAKDOWN_BELIEF))
        breakdown = true;
    // Only dance to an obvious beat: a clear one, or kicks still landing where
    // the clock expects them. While the beat is unclear (coasting) the clock
    // keeps time, but chatter or the TV landing on a beat now and then can't
    // set the lights going. A real drop makes the beat clear again within a
    // second or so, and the pulse comes straight back.
    float target = breakdown || holding || (coasting && belief < COAST_PULSE) ? 0 : 1;
    pulse_gate = target > pulse_gate ? target : pulse_gate + (target - pulse_gate) * PULSE_FALL;
}

static float strongest_onset(int frames_back)
{
    float strongest = 0;
    for (int i = 0; i < frames_back; i++)
        if (onsets[ring_index(i)] > strongest) strongest = onsets[ring_index(i)];
    return strongest;
}

// How hard the music hit over the last beat, against its hits while the beat
// was clear: the kick, or across the spectrum for music without much bass
static void update_energy(void)
{
    float target = 0;
    if (!holding && drum_level > 0) {
        int beat = (int)period;
        float bass = strongest_bass(beat) / drum_level;
        float all = onset_level > 0 ? strongest_onset(beat) / onset_level : 0;
        target = bass > all ? bass : all;
        if (target > 1) target = 1;
    }
    energy = target > energy ? target : energy + (target - energy) * ENERGY_FALL;
}

// While coasting: whether hits as strong as the ones before the breakdown have
// come back in the last second
static bool drums_back(void)
{
    return drum_level > 0 && strongest_bass((int)frame_rate) >= 0.75f * drum_level;
}

static float locked_fit;   // how well the current tempo fits, against the best, from the last estimate

// How set in its ways the tracker is: stiff when it believes in the beat
static bool trusting(void) { return tempo_known && belief >= TRUST_BELIEF; }
static int votes_needed(int at_none, int at_full) { return at_none + (int)(belief * (at_full - at_none)); }

// Best beat period in frames, and how clear it is (0–1)
static float estimate_period(float *clarity)
{
    int lag_min = (int)floorf(60 * frame_rate / BPM_MAX);
    int lag_max = (int)ceilf(60 * frame_rate / BPM_MIN);
    // The same bass-weighted onsets as the phase uses: the kick is usually
    // the clearest marker of the beat. Without the average taken out, any
    // steady noise would look rhythmic.
    float all = 0, bass = 0;
    for (int i = 0; i < HISTORY; i++) {
        all += onsets[i];
        bass += bass_onsets[i];
    }
    all = all > 0 ? HISTORY / all : 0;
    bass = bass > 0 ? BASS_WEIGHT * HISTORY / bass : 0;
    float mean = 0;
    for (int i = 0; i < HISTORY; i++) {
        history[i] = onset_at(i) * all + bass_onsets[ring_index(i)] * bass;
        mean += history[i];
    }
    mean /= HISTORY;
    for (int i = 0; i < HISTORY; i++)
        history[i] -= mean;
    static float r[HISTORY];
    int lag_top = lag_max * 3 + 2;
    if (lag_top >= HISTORY) lag_top = HISTORY - 1;
    for (int lag = 0; lag <= lag_top; lag++)
        r[lag] = autocorrelation(lag);
    if (r[0] <= 0) {
        *clarity = 0;
        return period;
    }

    // A comb over the beat and its multiples favours the beat itself over a
    // half or double tempo, and the prior settles what's left
    float preferred_lag = 60 * frame_rate / BPM_PREFERRED;
    int best = lag_min;
    float best_score = -1;
    static float score[HISTORY], comb[HISTORY];
    for (int lag = lag_min; lag <= lag_max; lag++) {
        comb[lag] = r[lag] + 0.5f * r[2 * lag < HISTORY ? 2 * lag : lag] + 0.25f * r[3 * lag < HISTORY ? 3 * lag : lag];
        float octaves = log2f(lag / preferred_lag) / PRIOR_OCTAVES;
        score[lag] = comb[lag] * expf(-0.5f * octaves * octaves);

        if (score[lag] > best_score) {
            best_score = score[lag];
            best = lag;
        }
    }
    *clarity = r[best] / r[0];
    int current = (int)(period + 0.5f);
    locked_fit = current >= lag_min && current <= lag_max && best_score > 0 ? score[current] / best_score : 0;

    // Parabolic interpolation for a period between whole frames, on the comb
    // without the prior: the prior picks the peak, but tilted towards 120 BPM
    // it would also pull the peak's position, reading fast songs slow
    float refined = best;
    if (best > lag_min && best < lag_max) {
        float a = comb[best - 1], b = comb[best], c = comb[best + 1];
        float d = a - 2 * b + c;
        if (d < 0) refined = best + 0.5f * (a - c) / d;
    }
    return refined;
}

static void update_tempo(float estimate)
{
    if (period == 0) {
        period = estimate;
        return;
    }
    float ratio = estimate / period;
    // Once locked, a half, double or 3:2 reading is the same beat in a new pattern
    // or any reading the locked tempo still fits nearly as well
    if (trusting()) {
        static const float related[] = { 0.5f, 2.0f, 1.5f, 0.6667f, 0.75f, 1.3333f };
        for (int i = 0; i < 6; i++)
            if (fabsf(ratio / related[i] - 1) < 0.05f)
                return;
        if (locked_fit * LOCKED_BETTER > 1 && (ratio < 0.95f || ratio > 1.05f)) {
            disagreements = 0;
            return;
        }
    }
    if (ratio > 0.95f && ratio < 1.05f) {
        period += (estimate - period) * (trusting() ? LOCKED_SMOOTH : TEMPO_SMOOTH);
        disagreements = 0;
        return;
    }
    // A different tempo has to be seen a few times in a row before it's
    // believed, and many more once the beat is trusted: songs rarely change tempo
    if (disagreements && fabsf(estimate / pending_period - 1) < 0.05f)
        disagreements++;
    else
        disagreements = 1;
    pending_period = estimate;
    if (disagreements >= votes_needed(TEMPO_SWITCH, LOCKED_SWITCH)) {
        period = estimate;
        disagreements = 0;
    }
}

// ---- Phase ----------------------------------------------------------------------

// Frames since the last beat, judged by where onsets have been landing. Each
// signal is scaled by its own average so the bass can be weighted fairly.
// *clarity is how far the best fit stands out from the average fit.
static float phase_signal[HISTORY];   // newest first

static float frames_since_beat(float *clarity)
{
    float all = 0, punch = 0;
    for (int i = 0; i < HISTORY; i++) {
        all += onsets[i];
        punch += punch_onsets[i];
    }
    all = all > 0 ? HISTORY / all : 0;
    punch = punch > 0 ? PUNCH_WEIGHT * HISTORY / punch : 0;
    for (int i = 0; i < HISTORY; i++)
        phase_signal[i] = onsets[ring_index(i)] * all + punch_onsets[ring_index(i)] * punch;

    int span = (int)period;
    float best_score = -1, best = 0, total = 0;
    for (int offset = 0; offset < span; offset++) {
        float score = 0, weight = 1;
        for (int k = 0; k < PHASE_BEATS; k++, weight *= PHASE_DECAY) {
            float ago = offset + k * period;
            int i = (int)ago;
            if (i + 1 >= HISTORY) break;
            float f = ago - i;
            score += weight * (phase_signal[i] * (1 - f) + phase_signal[i + 1] * f);
        }
        total += score;
        if (score > best_score) {
            best_score = score;
            best = offset;
        }
    }
    *clarity = total > 0 ? best_score * span / total : 0;
    return best;
}

// Returns false, changing nothing, if the onsets don't point clearly anywhere
// and coasting is set. While coasting the tempo is left alone.
static bool update_phase(bool coasting_now)
{
    bool require_clear = coasting_now;
    float clarity;
    float target = frames_since_beat(&clarity) / period;     // where the beat clock should be within a beat
    if (require_clear && clarity < PHASE_CLEAR)
        return false;
    float phase = (float)(position - floor(position));
    float error = target - phase;
    if (error > 0.5f) error -= 1;
    if (error < -0.5f) error += 1;
    // Once the beat is trusted, the grid only drifts: a bang or a busy bar can
    // nudge it a little, and only a long, consistent disagreement moves it far
    if (tempo_known) {
        rephase_votes = fabsf(error) > 0.25f ? rephase_votes + 1 : 0;
        if (rephase_votes < votes_needed(4, LOCKED_REPHASE)) {
            // Running ahead (negative error) means the beat is a touch slower than we think
            // Only small, steady errors: a big one is a busy bar, not a wrong tempo
            if (fabsf(error) < 0.1f && !coasting_now) {
                float trim = TEMPO_TRIM * error;
                trim = trim > 0.003f ? 0.003f : trim < -0.003f ? -0.003f : trim;
                period *= 1 - trim;
            }
            float nudge = error * PHASE_GAIN;
            float cap = LOCKED_NUDGE + (1 - belief) * (0.2f - LOCKED_NUDGE);   // bolder when unsure
            correction = nudge > cap ? cap : nudge < -cap ? -cap : nudge;
            return true;
        }
        rephase_votes = 0;
    }
    // Big errors (a new song, a missed beat) are put right at once, but only
    // forwards; small ones are eased out by running a touch fast or slow
    if (error > 0.35f) {
        position += error;
        correction = 0;
    } else {
        correction = error * PHASE_GAIN;
    }
    return true;
}

// The song has ended: wipe everything learnt from it, so the next one is judged
// on its own: its tempo, its levels, and none of the old onsets to re-lock onto
static void forget_song(void)
{
    coasting = tempo_known = breakdown = grooving = false;
    coast_updates = misses = rephase_votes = disagreements = 0;
    belief = music_level = correction = confidence = 0;
    drum_level = onset_level = punch_level = 0;
    onset_peak = bass_peak = punch_peak = 0;
    confirmed_beat = judged_beat = -1;
    memset(onsets, 0, sizeof(onsets));
    memset(bass_onsets, 0, sizeof(bass_onsets));
    memset(punch_onsets, 0, sizeof(punch_onsets));
    period = 60 * frame_rate / BPM_PREFERRED;
}

// ---- Public ---------------------------------------------------------------------

void beat_init(float sample_rate)
{
    rate = sample_rate;
    frame_rate = rate / BEAT_HOP;
    for (int i = 0; i < BEAT_FFT; i++)
        hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (BEAT_FFT - 1));
    memset(window, 0, sizeof(window));
    memset(last_log, 0, sizeof(last_log));
    memset(onsets, 0, sizeof(onsets));
    memset(flux_recent, 0, sizeof(flux_recent));
    memset(bass_onsets, 0, sizeof(bass_onsets));
    memset(bass_recent, 0, sizeof(bass_recent));
    memset(punch_onsets, 0, sizeof(punch_onsets));
    memset(punch_recent, 0, sizeof(punch_recent));
    punch_peak = 0;
    punch_first = (int)(PUNCH_BOTTOM_HZ * BEAT_FFT / rate + 0.5f);
    punch_last = (int)(PUNCH_TOP_HZ * BEAT_FFT / rate);
    bass_bins = (int)(BASS_TOP_HZ * BEAT_FFT / rate);
    bass_first = (int)(BASS_BOTTOM_HZ * BEAT_FFT / rate + 0.5f);
    if (bass_first < 1) bass_first = 1;
    onset_head = frames = disagreements = 0;
    period = 60 * frame_rate / BPM_PREFERRED;
    position = confidence = latest_onset = loudness = correction = onset_peak = bass_peak = 0;
    fast_loudness = music_level = energy = onset_level = drum_level = 0;
    silent_frames = 0;
    rephase_votes = coast_updates = clear_updates = quiet_until = 0;
    belief = punch_level = pulse_gate = 0;
    breakdown = grooving = false;
    misses = 0;
    confirmed_beat = judged_beat = -1;
    coasting = tempo_known = false;
    holding = true;
}

void beat_process(const float *samples)
{
    memmove(window, window + BEAT_HOP, (BEAT_FFT - BEAT_HOP) * sizeof(float));
    memcpy(window + BEAT_FFT - BEAT_HOP, samples, BEAT_HOP * sizeof(float));

    float energy = 0;
    for (int i = 0; i < BEAT_HOP; i++) energy += samples[i] * samples[i];
    float rms = sqrtf(energy / BEAT_HOP);
    loudness += (rms - loudness) * 0.02f;   // about half a second
    fast_loudness += (rms - fast_loudness) * FAST_SMOOTH;

    // The end of a song: a sudden fall to near silence. Stop straight away,
    // rather than coasting on, so the lights can fade out with the music.
    bool silent = fast_loudness < QUIET_RMS || (music_level > 0 && fast_loudness < SILENCE_DROP * music_level);
    silent_frames = silent ? silent_frames + 1 : 0;
    if (silent_frames == SILENCE_FRAMES && !holding) {
        holding = true;
        forget_song();
    }

    float bass, punch;
    float flux = spectral_flux(&bass, &punch);
    latest_onset = onset_strength(flux, flux_recent, &onset_peak, onset_level);
    onsets[onset_head] = latest_onset;
    bass_onsets[onset_head] = onset_strength(bass, bass_recent, &bass_peak, drum_level);
    punch_onsets[onset_head] = onset_strength(punch, punch_recent, &punch_peak, 0);
    onset_head = (onset_head + 1) % HISTORY;
    frames++;

    if (frames % UPDATE_EVERY == 0 && frames > HISTORY / 2) {
        float clarity;
        float estimate = estimate_period(&clarity);
        confidence += (clarity - confidence) * 0.3f;
        bool clear = confidence >= (holding || coasting ? CONFIDENCE_START : CONFIDENCE_STOP)
                  && loudness >= QUIET_RMS && silent_frames < SILENCE_FRAMES;
        clear_updates = clear ? clear_updates + 1 : 0;
        if (holding && clear_updates < LOCK_UPDATES) clear = false;

        if (clear) {
            // Following the music
            holding = coasting = false;
            coast_updates = 0;
            // The tempo only learns from a confident beat, so a fade-out can't drag it
            // Tempo and phase only learn from fresh, confident evidence, so the
            // seconds after the drums stop can't drag them
            bool fresh = onsets_recent();
            bool confident = confidence >= CONFIDENCE_START && fresh;
            bool hitting = drum_level <= 0 || strongest_bass((int)frame_rate) >= 0.5f * drum_level;
            if (confident) {
                // The first confident reading sets the tempo outright; only
                // confident readings count towards trusting it, so the starting
                // guess can never be what it locks onto
                if (!tempo_known) {
                    period = estimate;
                    tempo_known = true;
                    disagreements = 0;
                } else {
                    update_tempo(estimate);
                }
                music_level += (loudness - music_level) * 0.1f;
                // How hard the kicks hit while the music is clear. It only creeps
                // down (halving in about 6s), so a weak moment can't reset it
                float bass = strongest_bass(2 * (int)frame_rate), all = strongest_onset(2 * (int)frame_rate);
                float punch = strongest_punch(2 * (int)frame_rate);
                punch_level = punch > punch_level * 0.97f ? punch : punch_level * 0.97f;
                drum_level = bass > drum_level * 0.97f ? bass : drum_level * 0.97f;
                onset_level = all > onset_level * 0.97f ? all : onset_level * 0.97f;
            }
            if (fresh && hitting)
                update_phase(false);
            else
                correction = 0;
        } else if (tempo_known && silent_frames < SILENCE_FRAMES
                   && coast_updates * UPDATE_EVERY < COAST_SECONDS * frame_rate) {
            // A breakdown, a fill or a new pattern: as long as the music plays,
            // run on at the locked tempo and phase, as a listener would, and
            // don't let the unclear audio pull them about
            coast_updates++;
            coasting = true;
            // ...but if the drums are back (the drop), line the phase up at
            // once, without waiting for the slower tempo confidence to recover
            if (!drums_back() || !update_phase(true))
                correction = 0;
        } else {
            // No beat for a while: stop, and lock afresh when music returns
            // Only on the way in: while holding, the onsets must build up for the next lock
            if (!holding) forget_song();
            holding = true;
        }
    }

    // The clock runs on between updates; while holding it waits just short of
    // the next beat, so nothing pulses until the music has a beat again
    update_energy();
    update_belief();
    update_pulse();

    // The clock never runs backwards, so a beat can't be counted twice
    float step = 1.0f / period + correction / UPDATE_EVERY;
    float slowest = 0.25f / period;
    double next = position + (step > slowest ? step : slowest);
    if (holding && floor(next) != floor(position))
        next = floor(next) - 1e-6;
    position = next;
}

void beat_get(beat_state_t *state)
{
    *state = (beat_state_t){
        .position = position,
        .bpm = 60 * frame_rate / period,
        .confidence = confidence,
        .holding = holding,
        .coasting = coasting,
        .energy = energy,
        .belief = belief,
        .breakdown = breakdown,
        .pulse = energy * (0.35f + 0.65f * belief) * pulse_gate,
        .onset = latest_onset,
    };
}

float beat_frame_rate(void)
{
    return frame_rate;
}
