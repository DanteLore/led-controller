# Synthetic test tracks with known beats, 16-bit mono at 22050Hz, standard
# library only. Each comes with a .beats file of the true beat times.
import math, random, struct, sys, wave

RATE = 22050
random.seed(1)

def kick(t):  return math.sin(2 * math.pi * (50 + 120 * math.exp(-t * 40)) * t) * math.exp(-t * 12)
def snare(t): return (random.uniform(-1, 1) * 0.7 + 0.3 * math.sin(2 * math.pi * 200 * t)) * math.exp(-t * 20)
def hat(t):   return random.uniform(-1, 1) * math.exp(-t * 80) * 0.3

def render(path, beats, hits, seconds, noise=0.02):
    n = int(seconds * RATE)
    out = [random.uniform(-noise, noise) for _ in range(n)]
    for hit in hits:
        start, sound, gain = hit[:3]
        length = hit[3] if len(hit) > 3 else 0.25
        s = int(start * RATE)
        for i in range(min(int(length * RATE), n - s)):
            out[s + i] += gain * sound(i / RATE)
    peak = max(abs(v) for v in out)
    with wave.open(path + '.wav', 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', int(v / peak * 20000)) for v in out))
    open(path + '.beats', 'w').write('\n'.join(f'{b:.4f}' for b in beats))

def pattern(bpm, seconds, start=0.0, snares=True, hats=True, offset=0.0):
    beat = 60 / bpm
    beats, hits = [], []
    t = start + offset
    k = 0
    while t < start + seconds:
        beats.append(t)
        hits.append((t, kick, 1.0 if k % 2 == 0 or not snares else 0.7))
        if snares and k % 2 == 1: hits.append((t, snare, 0.6))
        if hats:
            hits.append((t + beat / 2, hat, 1.0))
            hits.append((t, hat, 0.6))
        t += beat; k += 1
    return beats, hits

out = sys.argv[1]
b, h = pattern(128, 30); render(f'{out}/house-128', b, h, 30)
b, h = pattern(95, 30); render(f'{out}/hiphop-95', b, h, 30)
b, h = pattern(174, 30, snares=True); render(f'{out}/dnb-174', b, h, 30)
b1, h1 = pattern(120, 20); b2, h2 = pattern(140, 20, start=20)
render(f'{out}/tempo-change-120-140', b1 + b2, h1 + h2, 40)
b, h = pattern(110, 20, start=10); render(f'{out}/silence-then-110', b, h, 30)
render(f'{out}/noise-only', [], [], 30, noise=0.3)   # rain or a fan: no beat to find

# A thump on the table: a huge broadband hit, off the beat, in the middle of a track
def bang(t): return random.uniform(-1, 1) * math.exp(-t * 30) + math.sin(2 * math.pi * 90 * t) * math.exp(-t * 15)
b, h = pattern(128, 40)
h.append((15.2 + 60 / 128 / 2, bang, 8.0))
render(f'{out}/house-128-table-bang', b, h, 40)

# A breakdown: 20s of drums, 16s of a soft pad with no drums, then the drop.
# The true beats carry on through the breakdown; the beat should be ready for the drop.
def pad(t): return 0.15 * (math.sin(2 * math.pi * 220 * t) + math.sin(2 * math.pi * 277 * t)) * min(1, t / 2, (16 - t) / 2)
drop = 77 * 60 / 128   # 36.09s: the drop lands on the beat, as it would in a song
b1, h1 = pattern(128, 20); b2, h2 = pattern(128, 20, start=drop)
beats = [t * 60 / 128 for t in range(int(56 * 128 / 60))]
h = h1 + h2 + [(20, pad, 1.0, 16)]   # one sustained chord, fading in and out
render(f'{out}/house-128-breakdown', beats, h, 56)

# A song that stops dead: 20s of drums then silence. Pulses should stop at once.
b, h = pattern(128, 20); render(f'{out}/house-128-then-silence', b, h, 40, noise=0.0005)

# A new drum pattern mid-song: 12s of four-to-the-floor, 12s half-time (kick on
# 1, snare on 3, busy hats), then back. The beat never changes.
def halftime(bpm, seconds, start):
    beat = 60 / bpm
    hits, t, k = [], start, 0
    while t < start + seconds:
        if k % 4 == 0: hits.append((t, kick, 1.0))
        if k % 4 == 2: hits.append((t, snare, 0.8))
        for q in range(4): hits.append((t + q * beat / 4, hat, 0.7 if q == 0 else 0.4))
        t += beat; k += 1
    return hits
beat = 60 / 124
b1, h1 = pattern(124, 12)
h2 = halftime(124, 12, 24 * beat * 1.0 + 0)   # placeholder start, fixed below
start2 = round(12 / beat) * beat
h2 = halftime(124, 12, start2)
start3 = round((start2 + 12) / beat) * beat
b3, h3 = pattern(124, 12, start=start3)
beats = [i * beat for i in range(int((start3 + 12) / beat))]
render(f'{out}/pattern-change-124', beats, h1 + h2 + h3, start3 + 12)

# A song change: house, 3s of silence, then quieter hip-hop. The second song
# should get going again within a few seconds.
b1, h1 = pattern(128, 20)
b2, h2 = pattern(95, 25, start=23)
render(f'{out}/song-change', b2, h1 + [(t, s, g * 0.4) for t, s, g in h2], 48, noise=0.0005)
