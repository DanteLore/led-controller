#include "beat.h"
#include "band.h"
#include "fit.h"
#include "spectrum.h"
#include <math.h>
#include <string.h>

#define UPDATE_EVERY 22      // frames between tempo and phase updates: about 0.25s
#define BPM_PREFERRED 120.0f // the starting guess
#define PHASE_GAIN    0.3f   // how much of the phase error each update corrects
#define TEMPO_SMOOTH  0.25f  // how far each agreeing estimate moves the tempo
#define TEMPO_SWITCH  4      // disagreeing estimates in a row before jumping tempo
#define CONFIDENCE_START 0.25f   // a beat is believed once confidence reaches this...
#define CONFIDENCE_STOP  0.15f   // ...and dropped below this. Noise scores about 0.1, music 0.3 up
#define QUIET_RMS     0.0018f // below about -55dBFS is silence, whatever the onsets look like
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
#define LEVEL_KEEP     0.97f // the music's levels only creep down, by this much per update (halving in ~6s)

static float frame_rate;
static int frames;

// ---- Onsets
static band_t all, bass, punch;   // the whole spectrum, the kick's boom (tempo) and its punch (phase)

// ---- Loudness
static float loudness;             // smoothed RMS of the audio
static float fast_loudness;        // the same over about 60ms, to catch the end of a song
static float music_level;          // loudness while following a clear beat
static int silent_frames;

// ---- Tempo and the beat clock
static float period;               // frames per beat
static float pending_period;       // a different tempo being seen...
static int disagreements;          // ...this many updates in a row
static float current_fit;          // how well the current tempo fits, against the best, from the last estimate
static bool tempo_known;           // the tempo has been learnt from this music, not just the starting guess
static double position;
static float correction;           // phase still to be corrected, spread over the next update period
static int rephase_votes;          // updates in a row the phase has disagreed badly
static float confidence;
static bool holding, coasting;
static int coast_updates;
static int clear_updates;          // updates in a row the beat has been clear enough to lock onto

// ---- Belief, breakdowns and the pulse
static float belief;               // see BELIEF_UP
static long confirmed_beat, judged_beat;   // the last beat a hit confirmed, and the last one judged
static int quiet_until;            // frame before which surprise hits aren't counted again
static int misses;                 // expected beats in a row with no hit
static bool breakdown;
static bool grooving;              // belief has built up in this song, so a breakdown is possible
static float pulse_gate;           // 1 normally, fading to 0 in a breakdown
static float energy;

// ---- Helpers ----------------------------------------------------------------------

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// How set in its ways the tracker is: stiff when it believes in the beat
static bool trusting(void) { return tempo_known && belief >= TRUST_BELIEF; }
static int votes_needed(int at_none, int at_full) { return at_none + (int)(belief * (at_full - at_none)); }
static int one_second(void) { return (int)frame_rate; }

// Whether the newest hop has a punch or boom onset this strong against the music's own kicks
static bool hit_at_least(float fraction)
{
    return (punch.level > 0 && band_at(&punch, 0) >= fraction * punch.level)
        || (bass.level > 0 && band_at(&bass, 0) >= fraction * bass.level);
}

// Whether the newest quarter of the history has its share of onsets: when the
// drums stop, the tempo can only be guessed from older, fading evidence
static bool onsets_recent(void)
{
    float newest = 0, total = 0;
    for (int i = 0; i < ONSET_HISTORY; i++) {
        total += band_at(&all, i);
        if (i < ONSET_HISTORY / 4) newest += band_at(&all, i);
    }
    return total > 0 && newest > 0.15f * total;
}

// While coasting: whether hits as strong as the ones before the breakdown have
// come back in the last second
static bool drums_back(void)
{
    return bass.level > 0 && band_strongest(&bass, one_second()) >= 0.75f * bass.level;
}

// ---- Belief, breakdowns, energy -----------------------------------------------------

// A strong hit that the beat grid, down to its quarters, can't explain
static void judge_surprise(float from_beat)
{
    float quarters = fabsf(from_beat) * 4;
    bool on_grid = fabsf(quarters - roundf(quarters)) * 0.25f < GRID_WINDOW;
    if (on_grid || !hit_at_least(SURPRISE_FRAC)) return;
    belief -= BELIEF_DOWN * belief;
    quiet_until = frames + (int)(0.15f * period);   // one hit lasts a few hops: count it once
}

static void confirm_beat(long beat)
{
    if (beat == confirmed_beat) return;
    belief += BELIEF_UP * (1 - belief);
    confirmed_beat = beat;
    misses = 0;
    breakdown = false;   // a firm hit on the beat: the drop, or the music carrying on
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
    bool hit = hit_at_least(coasting ? COAST_HIT : HIT_FRAC);
    if (hit && fabsf(from_beat) <= BEAT_WINDOW) {
        confirm_beat(beat);
    } else if (hit && frames >= quiet_until) {
        judge_surprise(from_beat);
        return;
    }
    if (from_beat > BEAT_WINDOW && beat != confirmed_beat && beat != judged_beat) {
        belief *= BELIEF_MISS;   // this beat's window has passed with no hit
        judged_beat = beat;
        misses++;
    }
}

static bool breakdown_starting(void)
{
    bool quieter = music_level > 0 && loudness < BREAKDOWN_QUIET * music_level;
    return !breakdown && grooving && misses >= BREAKDOWN_MISSES && (quieter || belief < BREAKDOWN_BELIEF);
}

// Breakdown in or out, and the pulse the lights should show
static void update_pulse(void)
{
    if (holding || !tempo_known)
        breakdown = grooving = false;
    else if (belief >= GROOVE_BELIEF)
        grooving = true;
    if (breakdown_starting())
        breakdown = true;
    // Only dance to an obvious beat: a clear one, or kicks still landing where
    // the clock expects them. While the beat is unclear (coasting) the clock
    // keeps time, but chatter or the TV landing on a beat now and then can't
    // set the lights going. A real drop makes the beat clear again within a
    // second or so, and the pulse comes straight back.
    float target = breakdown || holding || (coasting && belief < COAST_PULSE) ? 0 : 1;
    pulse_gate = target > pulse_gate ? target : pulse_gate + (target - pulse_gate) * PULSE_FALL;
}

// How hard the music hit over the last beat, against its hits while the beat
// was clear: the kick, or across the spectrum for music without much bass
static void update_energy(void)
{
    float target = 0;
    if (!holding && bass.level > 0) {
        int beat = (int)period;
        float from_bass = band_strongest(&bass, beat) / bass.level;
        float from_all = all.level > 0 ? band_strongest(&all, beat) / all.level : 0;
        target = from_bass > from_all ? from_bass : from_all;
        if (target > 1) target = 1;
    }
    energy = target > energy ? target : energy + (target - energy) * ENERGY_FALL;
}

// ---- Tempo ----------------------------------------------------------------------

static bool near_ratio(float ratio, float to) { return fabsf(ratio / to - 1) < 0.05f; }

// Once locked, a half, double or 3:2 reading is the same beat in a new pattern,
// and so is any reading the locked tempo still fits nearly as well
static bool same_beat_new_pattern(float ratio)
{
    static const float related[] = { 0.5f, 2.0f, 1.5f, 0.6667f, 0.75f, 1.3333f };
    for (int i = 0; i < 6; i++)
        if (near_ratio(ratio, related[i]))
            return true;
    if (current_fit * LOCKED_BETTER > 1 && (ratio < 0.95f || ratio > 1.05f)) {
        disagreements = 0;
        return true;
    }
    return false;
}

// A different tempo has to be seen a few times in a row before it's
// believed, and many more once the beat is trusted: songs rarely change tempo
static void consider_new_tempo(float estimate)
{
    if (disagreements && near_ratio(estimate, pending_period))
        disagreements++;
    else
        disagreements = 1;
    pending_period = estimate;
    if (disagreements >= votes_needed(TEMPO_SWITCH, LOCKED_SWITCH)) {
        period = estimate;
        disagreements = 0;
    }
}

static void update_tempo(float estimate)
{
    float ratio = estimate / period;
    if (trusting() && same_beat_new_pattern(ratio))
        return;
    if (ratio > 0.95f && ratio < 1.05f) {
        period += (estimate - period) * (trusting() ? LOCKED_SMOOTH : TEMPO_SMOOTH);
        disagreements = 0;
        return;
    }
    consider_new_tempo(estimate);
}

// The first confident reading sets the tempo outright; only confident readings
// count towards trusting it, so the starting guess can never be what it locks onto
static void learn_tempo(float estimate)
{
    if (tempo_known) {
        update_tempo(estimate);
        return;
    }
    period = estimate;
    tempo_known = true;
    disagreements = 0;
}

static void learn_band_level(band_t *b)
{
    float strongest = band_strongest(b, 2 * one_second());
    b->level = strongest > b->level * LEVEL_KEEP ? strongest : b->level * LEVEL_KEEP;
}

// How loud the music is, and how hard its kicks hit, while the beat is clear.
// The kick levels only creep down, so a weak moment can't reset them.
static void learn_levels(void)
{
    music_level += (loudness - music_level) * 0.1f;
    learn_band_level(&punch);
    learn_band_level(&bass);
    learn_band_level(&all);
}

// ---- Phase ----------------------------------------------------------------------

static float wrap_half(float error)
{
    if (error > 0.5f) error -= 1;
    if (error < -0.5f) error += 1;
    return error;
}

// Running ahead (negative error) means the beat is a touch slower than we think.
// Only small, steady errors: a big one is a busy bar, not a wrong tempo.
static void trim_tempo(float error)
{
    if (fabsf(error) >= 0.1f) return;
    period *= 1 - clampf(TEMPO_TRIM * error, -0.003f, 0.003f);
}

// Once the beat is trusted, the grid only drifts: a bang or a busy bar can
// nudge it a little, and only a long, consistent disagreement moves it far.
// Returns false once the disagreement has lasted long enough to rephase.
static bool nudge_phase(float error, bool coasting_now)
{
    rephase_votes = fabsf(error) > 0.25f ? rephase_votes + 1 : 0;
    if (rephase_votes >= votes_needed(4, LOCKED_REPHASE)) {
        rephase_votes = 0;
        return false;
    }
    if (!coasting_now)
        trim_tempo(error);
    float cap = LOCKED_NUDGE + (1 - belief) * (0.2f - LOCKED_NUDGE);   // bolder when unsure
    correction = clampf(error * PHASE_GAIN, -cap, cap);
    return true;
}

// Big errors (a new song, a missed beat) are put right at once, but only
// forwards; small ones are eased out by running a touch fast or slow
static void rephase(float error)
{
    if (error > 0.35f) {
        position += error;
        correction = 0;
    } else {
        correction = error * PHASE_GAIN;
    }
}

// Returns false, changing nothing, if coasting and the onsets don't point
// clearly anywhere. While coasting the tempo is left alone.
static bool update_phase(bool coasting_now)
{
    float clarity;
    float target = fit_phase(&all, &punch, period, &clarity) / period;   // where the clock should be within a beat
    if (coasting_now && clarity < PHASE_CLEAR)
        return false;
    float error = wrap_half(target - (float)(position - floor(position)));
    if (!tempo_known || !nudge_phase(error, coasting_now))
        rephase(error);
    return true;
}

// ---- Following, coasting, holding ---------------------------------------------------

// The song has ended: wipe everything learnt from it, so the next one is judged
// on its own: its tempo, its levels, and none of the old onsets to re-lock onto
static void forget_song(void)
{
    coasting = tempo_known = breakdown = grooving = false;
    coast_updates = misses = rephase_votes = disagreements = 0;
    belief = music_level = correction = confidence = 0;
    band_forget(&all);
    band_forget(&bass);
    band_forget(&punch);
    confirmed_beat = judged_beat = -1;
    period = 60 * frame_rate / BPM_PREFERRED;
}

// Tempo and phase only learn from fresh, confident evidence, so the seconds
// after the drums stop (or a fade-out) can't drag them
static void follow(float estimate)
{
    holding = coasting = false;
    coast_updates = 0;
    bool fresh = onsets_recent();
    bool confident = confidence >= CONFIDENCE_START && fresh;
    bool hitting = bass.level <= 0 || band_strongest(&bass, one_second()) >= 0.5f * bass.level;
    if (confident) {
        learn_tempo(estimate);
        learn_levels();
    }
    if (fresh && hitting)
        update_phase(false);
    else
        correction = 0;
}

static bool can_coast(void)
{
    return tempo_known && silent_frames < SILENCE_FRAMES
        && coast_updates * UPDATE_EVERY < COAST_SECONDS * frame_rate;
}

// A breakdown, a fill or a new pattern: as long as the music plays, run on at
// the locked tempo and phase, as a listener would, and don't let the unclear
// audio pull them about. But if the drums are back (the drop), line the phase
// up at once, without waiting for the slower tempo confidence to recover.
static void coast(void)
{
    coast_updates++;
    coasting = true;
    if (!drums_back() || !update_phase(true))
        correction = 0;
}

// No beat for a while: stop, and lock afresh when music returns. Only forget
// on the way in: while holding, the onsets must build up for the next lock.
static void hold(void)
{
    if (!holding) forget_song();
    holding = true;
}

static bool beat_clear(void)
{
    bool clear = confidence >= (holding || coasting ? CONFIDENCE_START : CONFIDENCE_STOP)
              && loudness >= QUIET_RMS && silent_frames < SILENCE_FRAMES;
    clear_updates = clear ? clear_updates + 1 : 0;
    return clear && (!holding || clear_updates >= LOCK_UPDATES);
}

static void update_lock(void)
{
    period_fit_t fit = fit_period(&all, &bass, frame_rate, period);
    current_fit = fit.current_fit;
    confidence += (fit.clarity - confidence) * 0.3f;
    if (beat_clear())
        follow(fit.period);
    else if (can_coast())
        coast();
    else
        hold();
}

// ---- Each hop -------------------------------------------------------------------

static void measure_loudness(const float *samples)
{
    float sum = 0;
    for (int i = 0; i < BEAT_HOP; i++) sum += samples[i] * samples[i];
    float rms = sqrtf(sum / BEAT_HOP);
    loudness += (rms - loudness) * 0.02f;   // about half a second
    fast_loudness += (rms - fast_loudness) * FAST_SMOOTH;
}

// The end of a song: a sudden fall to near silence. Stop straight away,
// rather than coasting on, so the lights can fade out with the music.
static void stop_on_silence(void)
{
    bool silent = fast_loudness < QUIET_RMS || (music_level > 0 && fast_loudness < SILENCE_DROP * music_level);
    silent_frames = silent ? silent_frames + 1 : 0;
    if (silent_frames == SILENCE_FRAMES && !holding) {
        holding = true;
        forget_song();
    }
}

static void detect_onsets(const float *samples)
{
    spectrum_flux_t flux = spectrum_process(samples);
    band_add(&all, flux.all, all.level);
    band_add(&bass, flux.bass, bass.level);
    band_add(&punch, flux.punch, 0);
    frames++;
}

// The clock runs on between updates and never runs backwards, so a beat can't
// be counted twice. While holding it waits just short of the next beat, so
// nothing pulses until the music has a beat again.
static void advance_clock(void)
{
    float step = 1.0f / period + correction / UPDATE_EVERY;
    float slowest = 0.25f / period;
    double next = position + (step > slowest ? step : slowest);
    if (holding && floor(next) != floor(position))
        next = floor(next) - 1e-6;
    position = next;
}

// ---- Public ---------------------------------------------------------------------

void beat_init(float sample_rate)
{
    frame_rate = sample_rate / BEAT_HOP;
    spectrum_init(sample_rate);
    memset(&all, 0, sizeof(all));
    memset(&bass, 0, sizeof(bass));
    memset(&punch, 0, sizeof(punch));
    forget_song();
    frames = silent_frames = clear_updates = quiet_until = 0;
    position = loudness = fast_loudness = energy = pulse_gate = 0;
    holding = true;
}

void beat_process(const float *samples)
{
    measure_loudness(samples);
    stop_on_silence();
    detect_onsets(samples);
    if (frames % UPDATE_EVERY == 0 && frames > ONSET_HISTORY / 2)
        update_lock();
    update_energy();
    update_belief();
    update_pulse();
    advance_clock();
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
    };
}

float beat_frame_rate(void)
{
    return frame_rate;
}
