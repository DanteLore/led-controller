# Compares render.c and render.js output. Small differences are float32 (C)
# against float64 (JS) rounding; anything over the tolerance is reported.
import sys

TOLERANCE = 64   # out of 65535, about 0.1%

c_lines = open(sys.argv[1]).read().split('\n')
js_lines = open(sys.argv[2]).read().split('\n')
if len(c_lines) != len(js_lines):
    sys.exit(f'different line counts: C {len(c_lines)}, JS {len(js_lines)}')
worst, bad = 0, []
for a, b in zip(c_lines, js_lines):
    if a.startswith('error') or b.startswith('error'):
        if a != b: bad.append((a, b))
        continue
    if not a: continue
    pa, pb = a.split(), b.split()
    if pa[:3] != pb[:3]:
        sys.exit(f'out of step: {a!r} vs {b!r}')
    diff = max(abs(int(x) - int(y)) for x, y in zip(pa[3:], pb[3:]))
    worst = max(worst, diff)
    if diff > TOLERANCE: bad.append((a, b))
print(f'{len(c_lines)} LEDs compared, biggest difference {worst}')
for a, b in bad[:20]:
    print(f'  C:  {a}\n  JS: {b}')
sys.exit(1 if bad else 0)
