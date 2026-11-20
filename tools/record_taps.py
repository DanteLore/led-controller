#!/usr/bin/env python3
"""Records tuning clips for the beat tracker, started from the board.

Flip the board's switch to Edit and tap a knob on every beat: the first tap
asks for a recording, which this picks up within a fifth of a second. It saves
what the mic heard and your taps to test-audio/, then scores the tracker
against them with components/beat/test/run.sh. Standard library only.

    tools/record_taps.py [--board 192.168.68.87] [--secs 30] [--once]
"""
import argparse, json, pathlib, subprocess, sys, time, urllib.request
from datetime import datetime

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / 'test-audio'

def get(url, timeout=5):
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--board', default='192.168.68.87')
    p.add_argument('--secs', type=int, default=30)
    p.add_argument('--once', action='store_true', help='stop after one recording')
    a = p.parse_args()
    base = f'http://{a.board}'
    OUT.mkdir(exist_ok=True)
    print(f'Watching {base}: flip to Edit and tap a knob on every beat', flush=True)
    while True:
        try:
            wanted = json.loads(get(f'{base}/api/state')).get('record_wanted')
        except OSError as e:
            print('Board not answering:', e, flush=True)
            time.sleep(2)
            continue
        if not wanted:
            time.sleep(0.2)
            continue
        name = 'tune-' + datetime.now().strftime('%Y%m%d-%H%M%S')
        print(f'Recording {a.secs}s as {name}...', flush=True)
        wav = get(f'{base}/api/audio?secs={a.secs}', timeout=a.secs + 15)
        taps = json.loads(get(f'{base}/api/taps'))
        (OUT / f'{name}.wav').write_bytes(wav)
        (OUT / f'{name}.taps.json').write_text(json.dumps(taps))
        print(f'Saved {len(wav)} bytes of audio and {len(taps["taps"])} taps', flush=True)
        result = subprocess.run([str(ROOT / 'components/beat/test/run.sh')], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            if name in line:
                print(line, flush=True)
        if a.once:
            return

if __name__ == '__main__':
    main()
