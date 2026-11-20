# Compares detected beats with the true ones, after a settling time at the
# start and after any tempo change
import sys, statistics
out, truth_path, name = sys.argv[1:4]
lines = open(out).read().split('\n')
found = [float(l.split()[1]) for l in lines if l.startswith('beat')]
truth = [float(t) for t in open(truth_path).read().split()]
if not truth:
    held = [l for l in lines if l.startswith('time')]
    print(f'{name:24} no beat in it: holding {sum("holding" in l for l in held)}/{len(held)}s, beats claimed {len(found)}')
    sys.exit(0)
reports = [(float(l.split()[1]), float(l.split()[3])) for l in lines if l.startswith('time')]
SETTLE = 8.0
gaps = [b - a for a, b in zip(truth, truth[1:])]
# Settle from the first beat and from every tempo change
changes = [truth[0]] + [truth[i + 1] for i in range(len(gaps) - 1) if abs(gaps[i + 1] / gaps[i] - 1) > 0.03]
# The bang track is judged straight through: the beat must survive it.
# The breakdown is judged from the drop: the beat must be ready for it.
if 'breakdown' in name:
    drop = 77 * 60 / 128
    after = [t for t in truth if drop <= t < drop + 4]
    hit = sum(1 for t in after if found and min(abs(f - t) for f in found) < 0.07)
    print(f'{name:24} first 4s after the drop: beats hit {hit}/{len(after)}')
    sys.exit(0)
if 'then-silence' in name:
    end = truth[-1] + 60 / 128   # where the next beat would have been
    late = [f for f in found if f > end + 0.5]
    print(f'{name:24} beats claimed more than 0.5s after the music stopped: {len(late)}')
    sys.exit(0)
# Once locked, a new tempo takes 8s of evidence by design, so allow 12s after a change
def wait(c): return SETTLE if c == truth[0] else 12.0
def settled(t): return any(c <= t and t - c >= wait(c) for c in changes) and not any(c <= t < c + wait(c) for c in changes)
def true_bpm(t):
    i = max(0, min(len(gaps) - 1, sum(1 for b in truth if b <= t) - 1))
    return 60 / gaps[i]
judged = [t for t in truth if settled(t)]
errors, hits = [], 0
for t in judged:
    near = min(found, key=lambda f: abs(f - t)) if found else None
    if near is not None and abs(near - t) < 0.07:
        hits += 1; errors.append(near - t)
extra = sum(1 for f in found if settled(f) and min(abs(f - t) for t in truth) > 0.07)
tempo = [(t, b) for t, b in reports if settled(t)]
right = sum(1 for t, b in tempo if abs(b / true_bpm(t) - 1) < 0.03)
print(f'{name:24} tempo right {right}/{len(tempo)}s, beats hit {hits}/{len(judged)}, extra {extra}, '
      f'timing {1000 * statistics.mean(errors) if errors else 0:+.0f}ms '
      f'(spread {1000 * statistics.pstdev(errors) if len(errors) > 1 else 0:.0f}ms)')
