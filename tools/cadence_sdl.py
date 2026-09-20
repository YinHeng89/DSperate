#!/usr/bin/env python3
"""Summarise, and diff, the SDL frontend's cadence log (DS_CADENCE_LOG).

    tools/cadence_sdl.py <log>                 summarise one recording
    tools/cadence_sdl.py <ref-log> <cand-log>  compare two, and gate

Why this exists. The headless gate (tools/gate.sh) compares pictures and
counts swaps over a whole run, and it cannot settle *cadence* -- the pattern
of which frames a game draws on. Two reasons. The headless harness boots
differently from the SDL frontend, and under the same model Dragon Ball
Origins runs its intro at 30 Hz headless and 60 Hz in SDL; and a whole-run
swap total is blind to the difference between a steady 30 Hz and a run that
alternates 60 and 15. Phase 1's exit gate asks for dbori's intro checked in
the frontend it is played in, and this is that check.

The signal is the per-frame swap count. A 60 Hz game reads 1,1,1,1; a 30 Hz
game 1,0,1,0; a 20 Hz game 1,0,0. What matters between two builds is that
the *pattern* holds, not that the totals match -- a run that drops from 60 Hz
to 30 Hz can keep a similar total by drawing twice as much on half as many
frames, and that is exactly the failure a swap total hides.

The `presented` column is the frontend's, not the game's: frameskip and fast
forward change it without the game changing anything, so it is reported but
never gated.
"""
import sys, collections

def load(path):
    frames = []
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        f, sw, poly, pres = line.split()
        frames.append((int(f), int(sw), int(poly), int(pres)))
    if not frames:
        sys.exit(f'{path}: no frames')
    return frames

def summarise(frames, skip):
    body = frames[skip:]
    if not body:
        sys.exit('nothing left after --skip')
    swaps = [f[1] for f in body]
    total = sum(swaps)
    # Gap between one drawn frame and the next: 1 = every frame (60 Hz),
    # 2 = every other (30 Hz). Counted over the drawn frames only, so a long
    # still stretch shows as its own gap rather than diluting the rest.
    gaps, last = collections.Counter(), None
    for i, s in enumerate(swaps):
        if s:
            if last is not None:
                gaps[min(i - last, 8)] += 1
            last = i
    drawn = sum(1 for s in swaps if s)
    polys = [f[2] for f in body if f[1]]
    return {
        'frames': len(body),
        'swaps': total,
        'drawn_frames': drawn,
        'presented': sum(f[3] for f in body),
        'hz': round(60.0 * drawn / len(body), 2),
        'gaps': dict(sorted(gaps.items())),
        'poly_mean': round(sum(polys) / len(polys)) if polys else 0,
        'poly_max': max(polys) if polys else 0,
    }

def show(name, s):
    print(f'{name}: {s["frames"]} frames, {s["swaps"]} swaps on {s["drawn_frames"]} frames '
          f'= {s["hz"]} Hz, {s["presented"]} presented')
    print(f'  gap histogram (frames between draws): '
          + ', '.join(f'{k}{"+" if k == 8 else ""}x{v}' for k, v in s['gaps'].items()))
    print(f'  polygons per drawn frame: mean {s["poly_mean"]}, max {s["poly_max"]}')

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    skip = 0
    for a in sys.argv[1:]:
        if a.startswith('--skip='):
            skip = int(a.split('=')[1])
    if not args:
        sys.exit(__doc__)
    if len(args) == 1:
        show(args[0], summarise(load(args[0]), skip))
        return 0
    a, b = summarise(load(args[0]), skip), summarise(load(args[1]), skip)
    show('ref ', a); show('cand', b)
    bad = []
    # The rate is the claim. A whole-run swap total is not: see the header.
    if abs(a['hz'] - b['hz']) > 0.5:
        bad.append(f'rate {a["hz"]} -> {b["hz"]} Hz')
    # The shape of the pattern, not just its average. A run that trades a
    # steady 30 Hz for alternating 60/15 keeps the rate and fails here.
    for k in set(a['gaps']) | set(b['gaps']):
        x, y = a['gaps'].get(k, 0), b['gaps'].get(k, 0)
        if abs(x - y) > max(8, 0.05 * max(x, y)):
            bad.append(f'gap {k}: {x} -> {y}')
    if bad:
        print('  CADENCE FAIL: ' + '; '.join(bad))
        return 1
    print('  cadence unchanged')
    return 0

if __name__ == '__main__':
    sys.exit(main())
