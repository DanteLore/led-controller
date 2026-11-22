// JavaScript copy of patch.c and patch_json.c, for the web editor's preview.
// Keep it line for line with the C, and run test/run.sh after changing either.
// Colours come out as [r, g, b] perceptual levels, 0–65535.
(function (exports) {
'use strict';

const LEVEL_FULL = 65535;
const PATCH_GAMMA = 2.2;
const PATCH_MAX = 16, PATCH_NAME_MAX = 16, PATCH_PALETTE_MAX = 12, PATCH_PLAYLIST_MAX = 8;
const PERIOD_MAX_S = 3600;
const SINE_STEPS = 1024, GAMMA_STEPS = 4096;
const SHAPES = ['sine', 'triangle', 'saw', 'square', 'noise', 'decay'];
const SHAPE_SINE = 0, SHAPE_TRIANGLE = 1, SHAPE_SAW = 2, SHAPE_SQUARE = 3, SHAPE_DECAY = 5;

// ---- Rendering (patch.c) --------------------------------------------------

const sine_table = new Float32Array(SINE_STEPS + 1);
const perceptual_table = new Uint16Array(GAMMA_STEPS + 1);
for (let i = 0; i <= SINE_STEPS; i++)
    sine_table[i] = (1 - Math.cos(6.2831853 * i / SINE_STEPS)) * 0.5;
for (let i = 0; i <= GAMMA_STEPS; i++)
    perceptual_table[i] = Math.floor(Math.pow(i / GAMMA_STEPS, 1 / PATCH_GAMMA) * LEVEL_FULL + 0.5);

function lookup_sine(p) {
    const x = p * SINE_STEPS;
    const i = Math.floor(x);
    return sine_table[i] + (sine_table[i + 1] - sine_table[i]) * (x - i);
}

function to_perceptual(linear) {
    if (linear <= 0) return 0;
    if (linear >= 1) return LEVEL_FULL;
    const x = linear * GAMMA_STEPS;
    const i = Math.floor(x);
    return perceptual_table[i] + (perceptual_table[i + 1] - perceptual_table[i]) * (x - i);
}

function hash32(x) {
    x >>>= 0;
    x ^= x >>> 16; x = Math.imul(x, 0x7feb352d) >>> 0;
    x ^= x >>> 15; x = Math.imul(x, 0x846ca68b) >>> 0;
    x ^= x >>> 16;
    return x >>> 0;
}

function smooth(f) {
    return f * f * (3 - 2 * f);
}

function osc_value(o, slot, led, count, ms, beat_milli) {
    if (o.beat) ms = beat_milli;
    const h = hash32(led * 3 + slot + 1);
    const period = o.period_min_ms + Math.floor((o.period_max_ms - o.period_min_ms) * (h & 0xFFFF) / 65536);
    const start = o.phase + o.spread * led / count + o.scatter * (h >>> 16) / 65536;

    let cycles = 0;
    let frac = start;
    if (period) {
        cycles = Math.floor(ms / period);
        frac += (ms % period) / period;
    }
    const whole = Math.floor(frac);
    frac -= whole;
    if (frac >= 1) frac = 0;

    switch (o.shape) {
        case SHAPE_SINE:     return lookup_sine(frac);
        case SHAPE_TRIANGLE: return frac < 0.5 ? 2 * frac : 2 - 2 * frac;
        case SHAPE_SAW:      return frac;
        case SHAPE_SQUARE:   return frac < 0.5 ? 0 : 1;
        case SHAPE_DECAY: {
            const left = 1 - frac;
            return left * left * left;
        }
        default: {
            const k = (cycles + whole) >>> 0;
            const from = hash32(h ^ hash32(k)) / 4294967296;
            const to = hash32(h ^ hash32((k + 1) >>> 0)) / 4294967296;
            return from + (to - from) * smooth(frac);
        }
    }
}

function palette_colour(p, pos) {
    const n = p.palette_count;
    if (n === 1) return p.palette[0];
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    if (p.steps) {
        const i = Math.floor(pos * n);
        return p.palette[i < n ? i : n - 1];
    }

    const x = pos * (p.wrap ? n : n - 1);
    let i = Math.floor(x);
    const f = x - i;
    let j = i + 1;
    if (p.wrap) {
        i %= n;
        j %= n;
    } else if (i >= n - 1) {
        return p.palette[n - 1];
    }
    const a = p.palette_linear[i], b = p.palette_linear[j];
    return [
        Math.floor(to_perceptual(a[0] + (b[0] - a[0]) * f) + 0.5),
        Math.floor(to_perceptual(a[1] + (b[1] - a[1]) * f) + 0.5),
        Math.floor(to_perceptual(a[2] + (b[2] - a[2]) * f) + 0.5),
    ];
}

function glow_value(o, slot, led, count, ms, beat_milli, energy) {
    const v = osc_value(o, slot, led, count, ms, beat_milli);
    return o.beat ? v * energy : v;
}

// See blend() in patch.c
function blend(p, a, b, energy) {
    let wa = p.balance, wb = 1 - p.balance;
    if (p.a.beat === p.b.beat) return wa * a + wb * b;
    if (p.b.beat) wb *= energy; else wa *= energy;
    if (wa + wb <= 0) return 0;
    if (energy > 0) { if (p.b.beat) b /= energy; else a /= energy; }
    return (wa * a + wb * b) / (wa + wb);
}

function glow_at(p, led, count, ms, beat_milli, energy) {
    if (!p.has_glow) return { glow: 1, level: 1, heat: 1 };
    let mix = glow_value(p.a, 0, led, count, ms, beat_milli, energy);
    let heat = mix;
    if (p.has_b) {
        const b = glow_value(p.b, 1, led, count, ms, beat_milli, energy);
        heat = p.multiply ? mix * b : p.balance * mix + (1 - p.balance) * b;   // see patch.c
        mix = p.multiply ? mix * b : blend(p, mix, b, energy);
    }
    const glow = p.power === 1 ? mix : p.power === 2 ? mix * mix : Math.pow(mix, p.power);
    return { glow, level: p.floor + (p.ceiling - p.floor) * glow, heat };
}

function scaled(c, level) {
    return [
        Math.floor(c[0] * level + 0.5),
        Math.floor(c[1] * level + 0.5),
        Math.floor(c[2] * level + 0.5),
    ];
}

function render_patch(p, ms, beat_milli, energy, frame, count) {
    for (let led = 0; led < count; led++) {
        const g = glow_at(p, led, count, ms, beat_milli, energy);
        const pos = p.colour_by_osc ? osc_value(p.c, 2, led, count, ms, beat_milli)
                  : p.colour_by_heat ? g.heat : g.glow;
        frame[led] = scaled(palette_colour(p, pos), g.level);
    }
}

function step_time(s, t) {
    return Math.floor(s.speed_from * t + (s.speed_to - s.speed_from) * t * t / (2 * s.ms));
}

function find_step(p, ms) {
    let total = 0;
    for (const s of p.playlist) total += s.ms;
    let t = ms % total;
    let i = 0;
    while (t >= p.playlist[i].ms) {
        t -= p.playlist[i].ms;
        i++;
    }
    return { step: i, ms: t };
}

// Fills frame (an array) for a point in effect time, in ms, and on the beat
// clock, in thousandths of a beat; beat_energy (0–1) is how hard the music hits
function render(set, index, ms, beat_milli, beat_energy, frame, count) {
    const p = set.patches[index];
    if (p.playlist_count) {
        const found = find_step(p, ms);
        const s = p.playlist[found.step];
        render_patch(set.patches[s.patch], step_time(s, found.ms), beat_milli, beat_energy, frame, count);
    } else {
        render_patch(p, ms, beat_milli, beat_energy, frame, count);
    }
}

function playlist_step(set, index, ms) {
    const p = set.patches[index];
    return p.playlist_count ? find_step(p, ms).step : -1;
}

// ---- Reading (patch_json.c) -----------------------------------------------

class PatchError extends Error {}

function make_reader() {
    return {
        where: 'file',
        fail(message) { throw new PatchError(this.where + ': ' + message); },
    };
}

const has = (obj, key) => obj !== null && typeof obj === 'object' && !Array.isArray(obj) && key in obj;
const is_number = v => typeof v === 'number' && isFinite(v);
const is_object = v => v !== null && typeof v === 'object' && !Array.isArray(v);

function number(r, obj, key, fallback, min, max) {
    if (!has(obj, key)) return fallback;
    const v = obj[key];
    if (!is_number(v)) r.fail(key + ' should be a number');
    if (v < min || v > max) r.fail(key + ' should be from ' + min + ' to ' + max);
    return v;
}

function choice(r, obj, key, choices, fallback) {
    if (!has(obj, key)) return fallback;
    const i = choices.indexOf(obj[key]);
    if (i < 0) r.fail(key + ' should be one of ' + choices.join(', '));
    return i;
}

function flag(r, obj, key) {
    if (!has(obj, key)) return false;
    if (typeof obj[key] !== 'boolean') r.fail(key + ' should be true or false');
    return obj[key];
}

function seconds_to_ms(r, v, key) {
    if (!is_number(v) || v < 0 || v > PERIOD_MAX_S) r.fail(key + ' should be from 0 to ' + PERIOD_MAX_S + ' seconds');
    const ms = Math.round(v * 1000);
    return ms === 0 && v > 0 ? 1 : ms;
}

function read_osc(r, obj, name) {
    if (!is_object(obj)) r.fail(name + ' should be an object');
    const o = {};
    o.shape = choice(r, obj, 'shape', SHAPES, SHAPE_SINE);
    o.beat = choice(r, obj, 'clock', ['time', 'beat'], 0) === 1;
    const period = obj.period;
    if (period === undefined) {
        o.period_min_ms = o.period_max_ms = 1000;
    } else if (Array.isArray(period)) {
        if (period.length !== 2) r.fail(name + ' period should be a number or [shortest, longest]');
        o.period_min_ms = seconds_to_ms(r, period[0], 'period');
        o.period_max_ms = seconds_to_ms(r, period[1], 'period');
        if (o.period_min_ms > o.period_max_ms || o.period_min_ms === 0)
            r.fail(name + ' period range should be [shortest, longest], above 0');
    } else {
        o.period_min_ms = o.period_max_ms = seconds_to_ms(r, period, 'period');
    }
    o.phase = number(r, obj, 'phase', 0, -100, 100);
    o.spread = number(r, obj, 'spread', 0, -100, 100);
    o.scatter = number(r, obj, 'scatter', 0, 0, 1);
    return o;
}

function parse_colour(s) {
    if (typeof s !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(s)) return null;
    return [1, 3, 5].map(i => parseInt(s.substr(i, 2), 16) * 257);
}

function read_glow(r, glow, p) {
    if (!is_object(glow)) r.fail('glow should be an object');
    p.has_glow = true;
    if (!has(glow, 'a')) r.fail('glow needs oscillator a');
    p.a = read_osc(r, glow.a, 'glow a');
    if (has(glow, 'b')) {
        p.has_b = true;
        p.b = read_osc(r, glow.b, 'glow b');
    }
    p.multiply = choice(r, glow, 'mix', ['blend', 'multiply'], 0) === 1;
    p.balance = number(r, glow, 'balance', 0.5, 0, 1);
    p.power = number(r, glow, 'power', 1, 0.1, 10);
    p.floor = number(r, glow, 'floor', 0, 0, 1);
    p.ceiling = number(r, glow, 'ceiling', 1, 0, 1);
}

function read_colour(r, colour, p) {
    if (!is_object(colour)) r.fail('colour should be an object');
    const palette = colour.palette;
    if (!Array.isArray(palette) || palette.length < 1 || palette.length > PATCH_PALETTE_MAX)
        r.fail('palette should be a list of 1 to ' + PATCH_PALETTE_MAX + ' colours');
    p.palette_count = palette.length;
    p.palette = palette.map(s => {
        const c = parse_colour(s);
        if (!c) r.fail('palette colours should look like "#FF8800"');
        return c;
    });
    p.palette_linear = p.palette.map(c => c.map(v => Math.pow(v / 65535, PATCH_GAMMA)));
    p.steps = choice(r, colour, 'blend', ['smooth', 'steps'], 0) === 1;
    p.wrap = flag(r, colour, 'wrap');
    const source = choice(r, colour, 'source', ['glow', 'osc', 'heat'], 0);
    p.colour_by_osc = source === 1;
    p.colour_by_heat = source === 2;
    if (p.colour_by_osc) {
        if (!has(colour, 'osc')) r.fail('colour source osc needs an osc');
        p.c = read_osc(r, colour.osc, 'colour osc');
    }
}

function find_patch(patches, name) {
    return patches.findIndex(p => is_object(p) && p.name === name);
}

function read_speed(r, speed, number, s) {
    s.speed_from = s.speed_to = 1;
    if (is_number(speed)) {
        s.speed_from = s.speed_to = speed;
    } else if (Array.isArray(speed) && speed.length === 2 && is_number(speed[0]) && is_number(speed[1])) {
        s.speed_from = speed[0];
        s.speed_to = speed[1];
    } else if (speed !== undefined) {
        r.fail('playlist step ' + number + ' speed should be a number or [from, to]');
    }
    if (s.speed_from < 0 || s.speed_to < 0 || s.speed_from > 100 || s.speed_to > 100)
        r.fail('playlist step ' + number + ' speed should be from 0 to 100');
}

function read_step_patch(r, step, number, patches) {
    if (!has(step, 'patch') || typeof step.patch !== 'string') r.fail('playlist step ' + number + ' needs a patch name');
    const target = find_patch(patches, step.patch);
    if (target < 0) r.fail('playlist plays "' + step.patch + '", which isn\'t in the file');
    if (has(patches[target], 'playlist')) r.fail('playlist plays "' + step.patch + '", which is a playlist itself');
    return target;
}

function read_step(r, step, number, patches) {
    const s = { patch: read_step_patch(r, step, number, patches) };
    s.ms = has(step, 'secs') ? seconds_to_ms(r, step.secs, 'secs') : 0;
    if (s.ms === 0) r.fail('playlist step ' + number + ' needs secs above 0');
    read_speed(r, step.speed, number, s);
    return s;
}

function read_playlist(r, list, patches, p) {
    if (!Array.isArray(list) || list.length < 1 || list.length > PATCH_PLAYLIST_MAX)
        r.fail('playlist should have 1 to ' + PATCH_PLAYLIST_MAX + ' steps');
    p.playlist_count = list.length;
    p.playlist = list.map((step, i) => read_step(r, step, i + 1, patches));
}

function read_patch(r, obj, patches) {
    const p = { has_glow: false, has_b: false, multiply: false, colour_by_osc: false, colour_by_heat: false,
                steps: false, wrap: false, palette_count: 0, playlist_count: 0 };
    const name = has(obj, 'name') ? obj.name : undefined;
    if (typeof name !== 'string' || !name.length || utf8_length(name) > PATCH_NAME_MAX)
        r.fail('needs a name of 1 to ' + PATCH_NAME_MAX + ' characters');
    p.name = name;
    r.where = name;

    // "knob" from older files is ignored: the knobs now have fixed jobs
    const playlist = has(obj, 'playlist');
    if (playlist) {
        read_playlist(r, obj.playlist, patches, p);
        return p;
    }
    if (has(obj, 'glow')) read_glow(r, obj.glow, p);
    if (!has(obj, 'colour')) r.fail('needs a colour with a palette');
    read_colour(r, obj.colour, p);
    return p;
}

// The device counts bytes, not characters
function utf8_length(s) {
    return new TextEncoder().encode(s).length;
}

function read_patches(r, patches, set) {
    patches.forEach((obj, i) => {
        r.where = 'patch ' + (i + 1);
        const p = read_patch(r, obj, patches);
        if (set.patches.some(q => q.name === p.name)) r.fail('two patches have this name');
        set.patches.push(p);
    });
}

// "default": the patch to start on at boot
function read_default(r, root, patches, set) {
    r.where = 'file';
    set.startup = 0;
    if (!has(root, 'default')) return;
    if (typeof root.default !== 'string') r.fail('default should be a patch name');
    set.startup = find_patch(patches, root.default);
    if (set.startup < 0) r.fail('default is "' + root.default + '", which isn\'t in the file');
}

// Takes the parsed JSON object (not text). Throws PatchError with the same
// messages as the device.
function parse(root) {
    const r = make_reader();
    const patches = is_object(root) ? root.patches : undefined;
    if (!Array.isArray(patches) || patches.length < 1 || patches.length > PATCH_MAX)
        r.fail('should have a "patches" list of 1 to ' + PATCH_MAX + ' patches');
    const set = { count: patches.length, patches: [] };
    read_patches(r, patches, set);
    read_default(r, root, patches, set);
    return set;
}

Object.assign(exports, { parse, render, playlist_step, palette_colour, PatchError, SHAPES, LEVEL_FULL, PATCH_GAMMA,
                         PATCH_MAX, PATCH_NAME_MAX, PATCH_PALETTE_MAX, PATCH_PLAYLIST_MAX });
})(typeof module !== 'undefined' ? module.exports : (window.Patch = {}));
