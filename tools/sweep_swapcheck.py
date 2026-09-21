#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Diff two sweep runs' per-screen hashes for a PERSISTENTLY swapped picture.

  tools/sweep_swapcheck.py <ref.screens> <cand.screens>

tools/frame_health.py --screens-out writes two 8-byte hashes per frame, top
then bottom. Its own `swaps` counter compares consecutive frames, which catches
screens that ALTERNATE and is blind to screens that are exchanged on every
frame -- that stream is self-consistent, so it looks like a correct one. This
is the other half: against a run of a build believed good, a candidate whose
top hash keeps matching the reference's BOTTOM hash has the screens swapped.

Reports, over the frames the two runs share:

  same       both screens match the reference
  swapped    this frame's screens match the reference's, exchanged
  differ     neither -- the picture moved on for some other reason

A handful of swapped frames is a genuine POWCNT screen swap landing a frame
early. A run that is mostly `swapped` is the bug this exists to catch; a run
that is mostly `differ` means the two builds diverged and this test cannot say
anything, which is reported rather than hidden.
"""
import sys

REC = 16

def read(path):
    with open(path, 'rb') as f:
        d = f.read()
    return [(d[i:i + 8], d[i + 8:i + 16]) for i in range(0, len(d) - REC + 1, REC)]

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = read(sys.argv[1]), read(sys.argv[2])
    n = min(len(a), len(b))
    if not n:
        print('no frames in common'); return 2
    same = swapped = differ = 0
    for i in range(n):
        (at, ab), (bt, bb) = a[i], b[i]
        if at == bt and ab == bb:
            same += 1
        elif at == bb and ab == bt and at != ab:   # at == ab would be both screens alike
            swapped += 1
        else:
            differ += 1
    print(f'frames {n} (ref {len(a)}, cand {len(b)}): same {same} swapped {swapped} differ {differ}')
    if swapped > max(4, n // 100):
        print(f'  SWAPPED: {swapped} frames ({100.0 * swapped / n:.1f} %) have the screens exchanged')
        return 1
    if differ > n // 2:
        print(f'  inconclusive: {differ} frames ({100.0 * differ / n:.1f} %) differ outright, '
              f'so the runs diverged and the swap test cannot speak')
    return 0

if __name__ == '__main__':
    sys.exit(main())
