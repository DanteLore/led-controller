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
    for start, sound, gain in hits:
        s = int(start * RATE)
        for i in range(min(int(0.25 * RATE), n - s)):
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
