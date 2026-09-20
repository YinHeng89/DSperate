#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Judge a --dump-frames stream without storing it: is anything on screen?

  dsperate-headless ... --dump-frames FIFO &
  tools/frame_health.py FIFO --json out.json

Reads frames as they arrive and reports, as JSON:

  frames         how many whole frames arrived
  blank          fraction whose two screens are both a single flat colour
  frozen         fraction byte-identical to the frame before
  distinct       distinct frame hashes
  first_motion   index of the first frame that differs from frame 0, or null
  late_distinct  distinct hashes over the last quarter of the run
  late_changes   frame-to-frame changes over that same window

The late window is what decides liveness; counting over the whole run does
not. A title that draws a boot sequence and then stops still has plenty of
whole-run distinct frames -- ZhuZhu Babies scored 19 and Spore Creatures 21 --
while showing one frozen picture from frame ~600 to the end. Conversely a
blinking "press start" screen has only two distinct frames late but changes
between them constantly (DK Jungle Climber), and is alive. late_changes
separates those two; distinct alone does not.

A title that boots to a picture and animates scores low blank, low frozen and
high distinct. The three failures the compatibility sweep is looking for each
have a signature: a title that never draws is blank ~1.0; one that draws a
logo and stops is low blank with frozen near 1.0 and distinct ~2; one that
hangs produces too few frames for the run it was asked for.
"""
import argparse, hashlib, json, sys

# numpy makes the flat-colour test quicker but is not required: this has to
# run on the handheld, where there is no numpy, and the test is a min/max over
# a subsample either way.
try:
    import numpy as np
except ImportError:
    np = None

W, H = 256, 192
SCREEN = W * H * 4
FRAME = SCREEN * 2

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', help='FIFO or file of raw frames')
    ap.add_argument('--json', help='write the report here (default stdout)')
    args = ap.parse_args()

    n = blank = frozen = 0
    prev = None
    first = None
    first_motion = None
    hashes = set()
    seq = []            # hashes in order, for the late-window test
    with open(args.path, 'rb') as f:
        while True:
            d = f.read(FRAME)
            if len(d) < FRAME:
                break
            # Subsample for the flat-colour test: a screen that is one colour
            # is one colour everywhere, and every 16th pixel says so 16x faster.
            if np is not None:
                a = np.frombuffer(d, np.uint8)
                top, bot = a[0:SCREEN:64], a[SCREEN::64]
                flat = int(top.max()) - int(top.min()) < 2 and int(bot.max()) - int(bot.min()) < 2
            else:
                top, bot = d[0:SCREEN:64], d[SCREEN::64]
                flat = max(top) - min(top) < 2 and max(bot) - min(bot) < 2
            if flat:
                blank += 1
            h = hashlib.blake2b(d, digest_size=8).digest()
            hashes.add(h)
            seq.append(h)
            if prev is not None and h == prev:
                frozen += 1
            if first is None:
                first = h
            elif first_motion is None and h != first:
                first_motion = n
            prev = h
            n += 1

    late = seq[max(0, n - max(1, n // 4)):]
    rep = {
        'frames': n,
        'blank': round(blank / n, 4) if n else 1.0,
        'frozen': round(frozen / n, 4) if n else 1.0,
        'distinct': len(hashes),
        'first_motion': first_motion,
        'late_distinct': len(set(late)),
        'late_changes': sum(1 for i in range(1, len(late)) if late[i] != late[i - 1]),
    }
    out = json.dumps(rep)
    if args.json:
        open(args.json, 'w').write(out + '\n')
    else:
        print(out)

if __name__ == '__main__':
    sys.exit(main())
