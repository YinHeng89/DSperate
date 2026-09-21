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
  swaps          frames whose two screens look exchanged against the frame
                 before (null without numpy)

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

`swaps` covers a fourth, which none of the above can see and which the sweep
was blind to until 2026-09-21: the two screens exchanging. That is DraStic's
threaded-3D failure on capture-heavy titles, and it is the shape the deferred
2D work risks, so the sweep has to be able to see it. The perceptual gate
detects it by comparing against a reference dump; the sweep has no reference
and no input, so this compares each frame against the one BEFORE IT, crossed:
if this frame's top resembles the last frame's bottom, and its bottom the
last frame's top, far better than like resembles like, the screens exchanged.
A game that genuinely swaps them scores a handful; one ALTERNATING scores
hundreds.

Its limit, stated because it matters: a stream whose screens are exchanged on
EVERY frame is self-consistent, so comparing consecutive frames cannot see it
-- `swaps` reads 0 for "always wrong" exactly as for "always right". That case
needs a reference, so `--screens-out` writes a per-frame pair of per-screen
hashes and `tools/sweep_swapcheck.py` diffs two runs' files: if run B's top
hash matches run A's bottom hash frame after frame, the screens are
persistently exchanged between the two builds. Together the two cover the
failure the compatibility sweep is guarding: the alternating half needs no
reference, the persistent half needs one run of a build believed good.
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

# Coarse per-screen signature for the swap test: an 8x6 grid of block means of
# the green byte. Small enough to be free, structured enough that two different
# screens do not collide.
SIG_X, SIG_Y = 8, 6

def signature(a, off):
    """a: the frame as uint8; off: 0 for the top screen, SCREEN for the bottom."""
    g = a[off + 1:off + SCREEN:4].reshape(H, W).astype(np.float32)
    return g.reshape(SIG_Y, H // SIG_Y, SIG_X, W // SIG_X).mean(axis=(1, 3))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', help='FIFO or file of raw frames')
    ap.add_argument('--json', help='write the report here (default stdout)')
    ap.add_argument('--screens-out', help='write two 8-byte per-screen hashes per frame here, '
                                          'for tools/sweep_swapcheck.py to diff against another run')
    args = ap.parse_args()

    n = blank = frozen = swaps = 0
    screens = open(args.screens_out, 'wb') if args.screens_out else None
    prev_sig = None
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
            # Screens exchanged against the previous frame? Only judged when
            # the two screens are themselves unalike -- on a frame where they
            # look the same (both mostly black, both a flat menu) "crossed"
            # and "straight" are both small and the test means nothing.
            if np is not None:
                sig = (signature(a, 0), signature(a, SCREEN))
                if prev_sig is not None:
                    apart = float(np.abs(sig[0] - sig[1]).mean())
                    straight = float(np.abs(sig[0] - prev_sig[0]).mean() + np.abs(sig[1] - prev_sig[1]).mean())
                    crossed = float(np.abs(sig[0] - prev_sig[1]).mean() + np.abs(sig[1] - prev_sig[0]).mean())
                    if apart > 4.0 and crossed * 4.0 < straight:
                        swaps += 1
                prev_sig = sig
            if screens is not None:
                screens.write(hashlib.blake2b(d[:SCREEN], digest_size=8).digest())
                screens.write(hashlib.blake2b(d[SCREEN:], digest_size=8).digest())
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

    if screens is not None:
        screens.close()
    late = seq[max(0, n - max(1, n // 4)):]
    rep = {
        'frames': n,
        'blank': round(blank / n, 4) if n else 1.0,
        'frozen': round(frozen / n, 4) if n else 1.0,
        'distinct': len(hashes),
        'first_motion': first_motion,
        'late_distinct': len(set(late)),
        'late_changes': sum(1 for i in range(1, len(late)) if late[i] != late[i - 1]),
        'swaps': swaps if np is not None else None,
    }
    out = json.dumps(rep)
    if args.json:
        open(args.json, 'w').write(out + '\n')
    else:
        print(out)

if __name__ == '__main__':
    sys.exit(main())
