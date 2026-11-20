# Scores the tracker on a real recording against beats a person tapped
# (from the editor page's "Record for tuning"). Usage: score_taps.py out taps.json name
import json, statistics, sys
out, taps_path, name = sys.argv[1:4]
lines = open(out).read().split('\n')
found = [float(l.split()[1]) for l in lines if l.startswith('beat')]
taps = json.load(open(taps_path))['taps']
reports = [(float(l.split()[1]), float(l.split()[3])) for l in lines if l.startswith('time')]
if len(taps) < 8:
    sys.exit(f'{name}: only {len(taps)} taps')
SETTLE = 8.0
start = taps[0] + SETTLE
gap = statistics.median(b - a for a, b in zip(taps, taps[1:]))
tapped_bpm = 60 / gap
judged = [t for t in taps if t >= start]
errors = []
for t in judged:
    if found:
        near = min(found, key=lambda f: abs(f - t))
        if abs(near - t) < 0.5 * gap: errors.append(near - t)
offset = statistics.median(errors) if errors else 0
# Within 70ms of the taps, after allowing for the person's own steady lead or lag
hits = sum(1 for e in errors if abs(e - offset) < 0.07)
# Tracker beats sitting near the middle between taps: locked to the off-beat
late = [f for f in found if f >= start]
offbeat = sum(1 for f in late if abs(((f - taps[0]) / gap) % 1 - 0.5) < 0.15)
bpms = [b for t, b in reports if t >= start]
print(f'{name:28} you tapped {tapped_bpm:5.1f} BPM, tracker {statistics.median(bpms) if bpms else 0:5.1f}; '
      f'beats on your taps {hits}/{len(judged)}, off-beat {offbeat}/{len(late)}; '
      f'tracker {1000 * offset:+.0f}ms from your taps (spread {1000 * statistics.pstdev(errors) if len(errors) > 1 else 0:.0f}ms)')
