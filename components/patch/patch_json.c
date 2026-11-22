// Reading patch files. patch.js has the same defaults; keep them in step.

#include "patch.h"
#include "cJSON.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PERIOD_MAX_S  3600

typedef struct {
    char *error;
    size_t size;
    const char *where;   // patch being read, for messages
    bool failed;
} reader_t;

static void fail(reader_t *r, const char *format, ...)
{
    if (r->failed) return;   // keep the first message
    r->failed = true;
    int n = snprintf(r->error, r->size, "%s: ", r->where);
    if (n < 0 || (size_t)n >= r->size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(r->error + n, r->size - n, format, args);
    va_end(args);
}

static float number(reader_t *r, const cJSON *obj, const char *key, float fallback, float min, float max)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return fallback;
    if (!cJSON_IsNumber(item)) {
        fail(r, "%s should be a number", key);
        return fallback;
    }
    double v = item->valuedouble;
    if (v < min || v > max) {
        fail(r, "%s should be from %g to %g", key, min, max);
        return fallback;
    }
    return v;
}

// Returns the index of the item's string in choices, or fallback if it's missing
static int choice(reader_t *r, const cJSON *obj, const char *key, const char *const *choices, int count, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return fallback;
    if (cJSON_IsString(item))
        for (int i = 0; i < count; i++)
            if (strcmp(item->valuestring, choices[i]) == 0)
                return i;
    char list[80] = "";
    for (int i = 0; i < count; i++) {
        strlcat(list, i ? ", " : "", sizeof(list));
        strlcat(list, choices[i], sizeof(list));
    }
    fail(r, "%s should be one of %s", key, list);
    return fallback;
}

static bool flag(reader_t *r, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) return false;
    if (!cJSON_IsBool(item)) fail(r, "%s should be true or false", key);
    return cJSON_IsTrue(item);
}

static uint32_t seconds_to_ms(reader_t *r, const cJSON *item, const char *key)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > PERIOD_MAX_S) {
        fail(r, "%s should be from 0 to %d seconds", key, PERIOD_MAX_S);
        return 0;
    }
    uint32_t ms = (uint32_t)lround(item->valuedouble * 1000);
    return ms == 0 && item->valuedouble > 0 ? 1 : ms;
}

static const char *const SHAPES[] = { "sine", "triangle", "saw", "square", "noise", "decay" };
static const char *const CLOCKS[] = { "time", "beat" };

static void read_osc(reader_t *r, const cJSON *obj, const char *name, patch_osc_t *o)
{
    if (!cJSON_IsObject(obj)) {
        fail(r, "%s should be an object", name);
        return;
    }
    o->shape = choice(r, obj, "shape", SHAPES, 6, SHAPE_SINE);
    o->beat = choice(r, obj, "clock", CLOCKS, 2, 0) == 1;   // periods then count beats
    const cJSON *period = cJSON_GetObjectItemCaseSensitive(obj, "period");
    if (!period) {
        o->period_min_ms = o->period_max_ms = 1000;
    } else if (cJSON_IsArray(period)) {
        if (cJSON_GetArraySize(period) != 2) {
            fail(r, "%s period should be a number or [shortest, longest]", name);
            return;
        }
        o->period_min_ms = seconds_to_ms(r, cJSON_GetArrayItem(period, 0), "period");
        o->period_max_ms = seconds_to_ms(r, cJSON_GetArrayItem(period, 1), "period");
        if (o->period_min_ms > o->period_max_ms || o->period_min_ms == 0)
            fail(r, "%s period range should be [shortest, longest], above 0", name);
    } else {
        o->period_min_ms = o->period_max_ms = seconds_to_ms(r, period, "period");
    }
    o->phase = number(r, obj, "phase", 0, -100, 100);
    o->spread = number(r, obj, "spread", 0, -100, 100);
    o->scatter = number(r, obj, "scatter", 0, 0, 1);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_colour(const char *s, rgb16_t *out)
{
    if (strlen(s) != 7 || s[0] != '#') return false;
    uint16_t v[3];
    for (int i = 0; i < 3; i++) {
        int hi = hex_digit(s[1 + 2 * i]), lo = hex_digit(s[2 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        v[i] = (hi << 4 | lo) * 257;   // 0xFF → 65535
    }
    *out = (rgb16_t){ v[0], v[1], v[2] };
    return true;
}

static const char *const MIXES[]   = { "blend", "multiply" };
static const char *const BLENDS[]  = { "smooth", "steps" };
static const char *const SOURCES[] = { "glow", "osc", "heat" };

static void read_glow(reader_t *r, const cJSON *glow, patch_t *p)
{
    if (!cJSON_IsObject(glow)) {
        fail(r, "glow should be an object");
        return;
    }
    p->has_glow = true;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(glow, "a");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(glow, "b");
    if (!a) fail(r, "glow needs oscillator a");
    read_osc(r, a, "glow a", &p->a);
    if (b) {
        p->has_b = true;
        read_osc(r, b, "glow b", &p->b);
    }
    p->multiply = choice(r, glow, "mix", MIXES, 2, 0) == 1;
    p->balance = number(r, glow, "balance", 0.5f, 0, 1);
    p->power = number(r, glow, "power", 1, 0.1f, 10);
    p->floor = number(r, glow, "floor", 0, 0, 1);
    p->ceiling = number(r, glow, "ceiling", 1, 0, 1);
}

static void read_colour(reader_t *r, const cJSON *colour, patch_t *p)
{
    if (!cJSON_IsObject(colour)) {
        fail(r, "colour should be an object");
        return;
    }
    const cJSON *palette = cJSON_GetObjectItemCaseSensitive(colour, "palette");
    int n = cJSON_GetArraySize(palette);
    if (!cJSON_IsArray(palette) || n < 1 || n > PATCH_PALETTE_MAX) {
        fail(r, "palette should be a list of 1 to %d colours", PATCH_PALETTE_MAX);
        return;
    }
    p->palette_count = n;
    for (int i = 0; i < n; i++) {
        const cJSON *c = cJSON_GetArrayItem(palette, i);
        if (!cJSON_IsString(c) || !parse_colour(c->valuestring, &p->palette[i])) {
            fail(r, "palette colours should look like \"#FF8800\"");
            return;
        }
        p->palette_linear[i][0] = powf(p->palette[i].r / 65535.0f, PATCH_GAMMA);
        p->palette_linear[i][1] = powf(p->palette[i].g / 65535.0f, PATCH_GAMMA);
        p->palette_linear[i][2] = powf(p->palette[i].b / 65535.0f, PATCH_GAMMA);
    }
    p->steps = choice(r, colour, "blend", BLENDS, 2, 0) == 1;
    p->wrap = flag(r, colour, "wrap");
    int source = choice(r, colour, "source", SOURCES, 3, 0);
    p->colour_by_osc = source == 1;
    p->colour_by_heat = source == 2;
    if (p->colour_by_osc) {
        const cJSON *osc = cJSON_GetObjectItemCaseSensitive(colour, "osc");
        if (!osc) fail(r, "colour source osc needs an osc");
        read_osc(r, osc, "colour osc", &p->c);
    }
}

static int find_patch(const cJSON *patches, const char *name)
{
    int i = 0;
    const cJSON *p;
    cJSON_ArrayForEach(p, patches) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(p, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            return i;
        i++;
    }
    return -1;
}

static void read_speed(reader_t *r, const cJSON *speed, int number, patch_step_t *s)
{
    s->speed_from = s->speed_to = 1;
    if (cJSON_IsNumber(speed)) {
        s->speed_from = s->speed_to = speed->valuedouble;
    } else if (cJSON_IsArray(speed) && cJSON_GetArraySize(speed) == 2
               && cJSON_IsNumber(cJSON_GetArrayItem(speed, 0)) && cJSON_IsNumber(cJSON_GetArrayItem(speed, 1))) {
        s->speed_from = cJSON_GetArrayItem(speed, 0)->valuedouble;
        s->speed_to = cJSON_GetArrayItem(speed, 1)->valuedouble;
    } else if (speed) {
        fail(r, "playlist step %d speed should be a number or [from, to]", number);
    }
    if (s->speed_from < 0 || s->speed_to < 0 || s->speed_from > 100 || s->speed_to > 100)
        fail(r, "playlist step %d speed should be from 0 to 100", number);
}

// The patch a step plays: one in the file, and not a playlist itself. -1 if not.
static int read_step_patch(reader_t *r, const cJSON *step, int number, const cJSON *patches)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(step, "patch");
    if (!cJSON_IsString(name)) {
        fail(r, "playlist step %d needs a patch name", number);
        return -1;
    }
    int target = find_patch(patches, name->valuestring);
    if (target < 0) {
        fail(r, "playlist plays \"%s\", which isn't in the file", name->valuestring);
        return -1;
    }
    if (cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(patches, target), "playlist")) {
        fail(r, "playlist plays \"%s\", which is a playlist itself", name->valuestring);
        return -1;
    }
    return target;
}

static void read_step(reader_t *r, const cJSON *step, int number, const cJSON *patches, patch_step_t *s)
{
    int target = read_step_patch(r, step, number, patches);
    if (target < 0) return;
    s->patch = target;
    const cJSON *secs = cJSON_GetObjectItemCaseSensitive(step, "secs");
    s->ms = secs ? seconds_to_ms(r, secs, "secs") : 0;
    if (s->ms == 0) fail(r, "playlist step %d needs secs above 0", number);
    read_speed(r, cJSON_GetObjectItemCaseSensitive(step, "speed"), number, s);
}

static void read_playlist(reader_t *r, const cJSON *list, const cJSON *patches, patch_t *p)
{
    int n = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || n < 1 || n > PATCH_PLAYLIST_MAX) {
        fail(r, "playlist should have 1 to %d steps", PATCH_PLAYLIST_MAX);
        return;
    }
    p->playlist_count = n;
    for (int i = 0; i < n && !r->failed; i++)
        read_step(r, cJSON_GetArrayItem(list, i), i + 1, patches, &p->playlist[i]);
}

static void read_patch(reader_t *r, const cJSON *obj, const cJSON *patches, patch_t *p)
{
    memset(p, 0, sizeof(*p));
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0] || strlen(name->valuestring) > PATCH_NAME_MAX) {
        fail(r, "needs a name of 1 to %d characters", PATCH_NAME_MAX);
        return;
    }
    strlcpy(p->name, name->valuestring, sizeof(p->name));
    r->where = p->name;

    // "knob" from older files is ignored: the knobs now have fixed jobs
    const cJSON *playlist = cJSON_GetObjectItemCaseSensitive(obj, "playlist");
    if (playlist) {
        read_playlist(r, playlist, patches, p);
        return;
    }
    const cJSON *glow = cJSON_GetObjectItemCaseSensitive(obj, "glow");
    if (glow) read_glow(r, glow, p);
    const cJSON *colour = cJSON_GetObjectItemCaseSensitive(obj, "colour");
    if (!colour) fail(r, "needs a colour with a palette");
    else read_colour(r, colour, p);
}

static void read_patches(reader_t *r, const cJSON *patches, patch_set_t *set)
{
    set->count = cJSON_GetArraySize(patches);
    for (int i = 0; i < set->count && !r->failed; i++) {
        char where[24];
        snprintf(where, sizeof(where), "patch %d", i + 1);
        r->where = where;
        read_patch(r, cJSON_GetArrayItem(patches, i), patches, &set->patches[i]);
        for (int j = 0; j < i && !r->failed; j++)
            if (strcmp(set->patches[j].name, set->patches[i].name) == 0)
                fail(r, "two patches have this name");
    }
}

// "default": the patch to start on at boot
static void read_default(reader_t *r, const cJSON *root, const cJSON *patches, patch_set_t *set)
{
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(root, "default");
    set->startup = 0;
    r->where = "file";
    if (r->failed || !start)
        return;
    if (!cJSON_IsString(start)) {
        fail(r, "default should be a patch name");
        return;
    }
    set->startup = find_patch(patches, start->valuestring);
    if (set->startup < 0) fail(r, "default is \"%s\", which isn't in the file", start->valuestring);
}

bool patch_parse(const char *json, patch_set_t *set, char *error, size_t error_size)
{
    reader_t r = { .error = error, .size = error_size, .where = "file" };
    cJSON *root = cJSON_Parse(json);
    const cJSON *patches = cJSON_GetObjectItemCaseSensitive(root, "patches");
    int n = cJSON_GetArraySize(patches);
    if (!root) {
        fail(&r, "not valid JSON");
    } else if (!cJSON_IsArray(patches) || n < 1 || n > PATCH_MAX) {
        fail(&r, "should have a \"patches\" list of 1 to %d patches", PATCH_MAX);
    } else {
        read_patches(&r, patches, set);
        read_default(&r, root, patches, set);
    }
    cJSON_Delete(root);
    return !r.failed;
}
