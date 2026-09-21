#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Judge a framebuffer dump against the golden reference by eye, not by bytes.

The exactness gate (tools/scene_hashes.sh) asks whether two builds produced
identical frames. Under the speed-first rework they will not, by design -- see
docs/speed-first-rework-scoping.md -- so this asks the weaker question the
project now cares about: *is the picture still plausible at 1x?*

  tools/perceptual_gate.py ref.bin cand.bin [--scene NAME] [--floors F] ...

`ref.bin` comes from tools/golden_dump.sh. Both are --dump-frames streams: a
frame is the top screen then the bottom, 256x192 of little-endian 0xAARRGGBB.

Two kinds of judgement, and they are not interchangeable:

* **Metrics** (SSIM and PSNR, per screen per frame) say how far the picture
  moved. They are aggregated over the run and checked against per-scene
  floors. A metric alone is a bad gate: a scene can hold a mean SSIM of 0.99
  and still drop a whole layer for one frame.
* **Structural faults** say the picture broke, and no metric excuses them.
  A blank screen, a frozen screen, the two screens swapped, or one large
  connected region wrong (which is what a missing layer or a dead engine
  looks like) fail the run on their own.

Timing tolerance. A speed-first model changes *when* a frame appears, not
just what is in it, so comparing candidate frame N against reference frame N
is unfairly harsh. Each candidate frame is matched against the best reference
frame in a window (--align, default 2); the winner is chosen on mean absolute
error, which is cheap, and only the winner is scored. --align 0 disables it
and restores a strict frame-for-frame comparison.

Calibration. With no --floors this only reports, and exits 0. That is how the
floors get set: dump the golden reference, run today's default build against
it, read the summary, and write the floors from what a build you already
trust actually scores. Floors invented before that measurement are worth
nothing.
"""
import argparse, json, os, sys
import numpy as np
try:
    from scipy.ndimage import gaussian_filter, label
except ImportError:
    sys.exit('perceptual_gate.py needs numpy and scipy (pip install scipy)')

W, H = 256, 192
SCREEN = W * H * 4
FRAME = SCREEN * 2

# ---- metrics ---------------------------------------------------------------

def luma(frames):
    """(..., H, W, 4) BGRA uint8 -> (..., H, W) float32 luma in 0..255."""
    b = frames[..., 0].astype(np.float32)
    g = frames[..., 1].astype(np.float32)
    r = frames[..., 2].astype(np.float32)
    return 0.299 * r + 0.587 * g + 0.114 * b

def _blur(a):
    """Gaussian, sigma 1.5, edge-replicated -- the SSIM window."""
    return gaussian_filter(a, sigma=1.5, mode='nearest', truncate=3.0)

def ssim(a, b):
    """Global mean SSIM on luma, the standard constants for an 8-bit range."""
    C1, C2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = _blur(a), _blur(b)
    aa, bb, ab = _blur(a * a), _blur(b * b), _blur(a * b)
    va, vb, vab = aa - mu_a * mu_a, bb - mu_b * mu_b, ab - mu_a * mu_b
    num = (2 * mu_a * mu_b + C1) * (2 * vab + C2)
    den = (mu_a * mu_a + mu_b * mu_b + C1) * (va + vb + C2)
    return float(np.mean(num / den))

PSNR_CAP = 100.0   # identical screens; a finite cap keeps means and floors meaningful
# Total frames of one fault kind, anywhere in the run, before it is a fault --
# see --fault-total. `swapped` is tightest because a swap is never the timing
# skew the persistence rule exists to forgive: all eight golden scenes produce
# ZERO swapped marks, so the budget only has to cover a genuine POWCNT screen
# swap landing a frame early or late.
FAULT_TOTALS = {'swapped': 3, 'blank': 8, 'frozen': 8, 'region': 8}

def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    if mse <= 1e-9:
        return PSNR_CAP
    return min(PSNR_CAP, 10.0 * float(np.log10((255.0 ** 2) / mse)))

# ---- structural faults -----------------------------------------------------

def largest_bad_region(a, b, thresh=24.0):
    """Area of the largest 4-connected run of pixels differing by more than
    `thresh` in luma, as a fraction of the screen. A scatter of wrong pixels
    (dither, a rounding difference) stays small; a missing layer does not."""
    bad = np.abs(a - b) > thresh
    if not bad.any():
        return 0.0
    lab, n = label(bad)
    if n == 0:
        return 0.0
    sizes = np.bincount(lab.ravel())
    sizes[0] = 0
    return float(sizes.max()) / float(bad.size)

def is_blank(screen_luma):
    """A screen showing essentially one colour."""
    return float(screen_luma.std()) < 1.0

# ---- the run ---------------------------------------------------------------

DEFAULT_ALIGN = 2

def load(path):
    n = os.path.getsize(path) // FRAME
    if n == 0:
        sys.exit(f'{path}: not a whole frame ({os.path.getsize(path)} bytes)')
    m = np.memmap(path, dtype=np.uint8, mode='r', shape=(n, 2, H, W, 4))
    return m, n

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ref'); ap.add_argument('cand')
    ap.add_argument('--scene', default='?', help='name, for the summary and the floors table')
    ap.add_argument('--align', type=int, default=DEFAULT_ALIGN,
                    help='match each candidate frame against the best reference frame within +/-N (default 2; 0 = strict)')
    ap.add_argument('--offset', type=int, default=0, help='constant skew applied before the alignment window')
    ap.add_argument('--frames', help='range A-B (inclusive)')
    ap.add_argument('--floors', help='JSON of per-scene floors; without it the tool only reports')
    ap.add_argument('--json', help='write the per-scene summary here')
    ap.add_argument('--fault-persist', type=int, default=4,
                    help='consecutive frames a structural fault must hold before it is reported (default 4); '
                         'a shorter run is timing skew at a transition, not a regression')
    ap.add_argument('--fault-total', type=int, default=None,
                    help='total frames of one fault kind, ANYWHERE in the run, before it is reported '
                         'regardless of how they are spread (default: swapped 3, others 8). The '
                         'persistence rule above only sees CONSECUTIVE runs, so a fault that flaps -- '
                         'one frame wrong, one right, one wrong -- never reaches it. That is exactly '
                         "DraStic's threaded-3D failure on capture-heavy titles, where the screens flip "
                         'rapidly or a mis-latch renders onto the wrong one, so it is the shape this '
                         'gate most needs to catch and the one persistence alone is blind to.')
    ap.add_argument('--region-limit', type=float, default=0.02,
                    help='largest wrong connected region, as a fraction of a screen, before it is a structural fault (default 0.02)')
    ap.add_argument('--verbose', action='store_true', help='one line per frame')
    ap.add_argument('--dump-worst', type=int, metavar='N',
                    help='write the N lowest-SSIM frames as reference|candidate|difference PNGs '
                         'into --dump-dir. A number cannot tell a blinking arrow from a missing '
                         'wall; the picture can. Use this before arguing about a floor.')
    ap.add_argument('--dump-dir', default='.', help='where --dump-worst writes (default: .)')
    args = ap.parse_args()

    ref, nref = load(args.ref)
    cand, ncand = load(args.cand)
    lo, hi = 0, ncand - 1
    if args.frames:
        a, _, b = args.frames.partition('-'); lo = int(a); hi = int(b) if b else lo
    hi = min(hi, ncand - 1)

    ssims, psnrs = [], []
    # Per-frame flags; collapsed into consecutive runs below. A single frame
    # is not a fault: golden and candidate differ in *when* a transition
    # lands, so a screen that is briefly blank, briefly frozen or briefly
    # missing a region in one run and not the other is timing skew, not a
    # regression. A dead engine, a stuck screen or a dropped layer persists.
    marks = {'blank': [], 'frozen': [], 'swapped': [], 'region': []}
    prev_cand = None
    prev_ref = None
    shifts = []

    for f in range(lo, hi + 1):
        base = f + args.offset
        if base < 0 or base >= nref:
            continue
        cl = luma(cand[f])
        # Pick the reference frame in the window with the lowest mean absolute
        # error; scoring every candidate in the window would cost 2N SSIMs.
        # Ordered by distance from `base`, so a tie -- which is the normal case
        # on a scene that holds a still picture -- resolves to shift 0 rather
        # than to the earliest frame in the window. Without this the tool
        # reports constant realignment against a dump of itself, which would
        # hide the real drift it exists to show.
        window = sorted((k for k in range(base - args.align, base + args.align + 1) if 0 <= k < nref),
                        key=lambda k: (abs(k - base), k))
        if len(window) == 1:
            best = window[0]; rl = luma(ref[best])
        else:
            best, rl, bad = None, None, None
            for k in window:
                l = luma(ref[k])
                mae = float(np.mean(np.abs(l - cl)))
                if bad is None or mae < bad:
                    best, rl, bad = k, l, mae
        shifts.append(best - base)

        fs = [ssim(rl[s], cl[s]) for s in range(2)]
        fp = [psnr(rl[s], cl[s]) for s in range(2)]
        ssims.append(min(fs)); psnrs.append(min(fp))

        for s, name in enumerate(('top', 'bottom')):
            if is_blank(cl[s]) and not is_blank(rl[s]):
                marks['blank'].append((f, name))
            if largest_bad_region(rl[s], cl[s]) > args.region_limit:
                marks['region'].append((f, name))
        # Frozen: the candidate repeated a frame exactly while the reference moved.
        if prev_cand is not None and np.array_equal(cand[f], prev_cand) and not np.array_equal(rl, prev_ref):
            marks['frozen'].append((f, 'both'))
        # Swapped: each candidate screen scores better against the other reference screen.
        if min(ssim(rl[1], cl[0]), ssim(rl[0], cl[1])) > max(fs) + 0.05:
            marks['swapped'].append((f, 'both'))
        prev_cand = np.array(cand[f]); prev_ref = rl

        if args.verbose:
            print(f'frame {f}: ssim {min(fs):.4f} psnr {min(fp):5.1f} shift {best - base:+d}')

    # Collapse each fault's frames into consecutive runs per screen, and keep
    # only runs at least --fault-persist long.
    faults = {}
    for kind, hits in marks.items():
        runs = []
        for screen in ('top', 'bottom', 'both'):
            fr = sorted(f for f, sc in hits if sc == screen)
            i = 0
            while i < len(fr):
                j = i
                while j + 1 < len(fr) and fr[j + 1] == fr[j] + 1:
                    j += 1
                length = j - i + 1
                if length >= args.fault_persist:
                    runs.append((fr[i], screen, length))
                i = j + 1
        faults[kind] = sorted(runs)

    # Totals, independent of adjacency. A swapped frame scores ~0.49 SSIM, so a
    # flapping swap does currently fail the ssim_min floor -- but it fails it
    # unnamed, and only for as long as that floor stays tight. The floors exist
    # to catch gradual degradation and are expected to be tuned per scene
    # (sm64's min is 0.900 against a measured 0.9146); a structural fault is
    # not supposed to depend on one. Counting the frames says what is wrong.
    flapping = {}
    for kind, hits in marks.items():
        budget = args.fault_total if args.fault_total is not None else FAULT_TOTALS[kind]
        n = len({f for f, _ in hits})
        if n > budget and not faults[kind]:
            flapping[kind] = n

    if not ssims:
        sys.exit('no frames compared')
    a_ssim = np.array(ssims); a_psnr = np.array(psnrs)
    summary = {
        'scene': args.scene,
        'frames': len(ssims),
        'ssim_mean': float(a_ssim.mean()),
        'ssim_p01': float(np.percentile(a_ssim, 1)),
        'ssim_min': float(a_ssim.min()),
        'psnr_mean': float(a_psnr.mean()),
        'psnr_min': float(a_psnr.min()),
        'exact_frames': int(np.sum(a_ssim >= 0.99999)),
        'shift_nonzero': int(sum(1 for s in shifts if s)),
        'faults': {k: len(v) for k, v in faults.items()},
        'fault_frames': {k: len({f for f, _ in m}) for k, m in marks.items()},
        'flapping': dict(flapping),
    }

    print(f"{args.scene}: {summary['frames']} frames, "
          f"ssim mean {summary['ssim_mean']:.4f} p01 {summary['ssim_p01']:.4f} min {summary['ssim_min']:.4f}, "
          f"psnr mean {summary['psnr_mean']:.1f} min {summary['psnr_min']:.1f}, "
          f"{summary['exact_frames']} exact, {summary['shift_nonzero']} realigned")
    for k, v in faults.items():
        if v:
            where = ', '.join(f'{fr}/{sc}x{n}' for fr, sc, n in v[:6])
            print(f"  FAULT {k}: {len(v)} run(s) ({where}{', ...' if len(v) > 6 else ''})")
    for k, n in flapping.items():
        budget = args.fault_total if args.fault_total is not None else FAULT_TOTALS[k]
        print(f"  FAULT {k} (scattered): {n} frame(s) over the run, budget {budget} "
              f"-- no run reached --fault-persist, so this is a flapping fault")
    transient = {k: len(m) for k, m in marks.items()
                 if m and not faults[k] and k not in flapping}
    if transient:
        print('  (transient, under --fault-persist: '
              + ', '.join(f'{k} {n} frame(s)' for k, n in transient.items()) + ')')

    if args.dump_worst:
        try:
            from PIL import Image
        except ImportError:
            print('  (--dump-worst needs Pillow; skipped)')
        else:
            os.makedirs(args.dump_dir, exist_ok=True)
            order = sorted(range(len(ssims)), key=lambda i: ssims[i])[:args.dump_worst]
            for i in order:
                f = lo + i
                best = f + args.offset + shifts[i]
                def stack(m, k):            # both screens, BGRA -> RGB
                    a = np.asarray(m[k])[..., :3][..., ::-1]
                    return np.concatenate([a[0], a[1]], axis=0)
                r, c = stack(ref, best), stack(cand, f)
                d = np.abs(r.astype(np.int16) - c.astype(np.int16)).sum(2).clip(0, 255).astype(np.uint8)
                d = np.stack([d, d, d], axis=2)
                gap = np.full((r.shape[0], 8, 3), 32, np.uint8)
                img = np.concatenate([r, gap, c, gap, d], axis=1)
                px = int(np.count_nonzero(d[:, :, 0]))
                out = os.path.join(args.dump_dir, f'{args.scene}.f{f}.png')
                Image.fromarray(img).resize((img.shape[1] * 2, img.shape[0] * 2), Image.NEAREST).save(out)
                print(f'  worst: frame {f} ssim {ssims[i]:.4f} shift {shifts[i]:+d} '
                      f'{px} px ({100 * px / (r.shape[0] * r.shape[1]):.2f}%) -> {out}')

    if args.json:
        with open(args.json, 'w') as fh:
            json.dump(summary, fh, indent=2)

    if not args.floors:
        print('  (reporting only -- no --floors given, so nothing is gated)')
        return 0

    floors = json.load(open(args.floors))
    want = floors.get(args.scene, floors.get('default', {}))
    failed = []
    for key, limit in want.items():
        got = summary.get(key)
        if got is None:
            continue
        if key.startswith('psnr') or key.startswith('ssim'):
            if got < limit: failed.append(f'{key} {got:.4f} < {limit}')
    if any(faults.values()):
        failed.append('structural faults: ' + ', '.join(f'{k}={len(v)}' for k, v in faults.items() if v))
    if flapping:
        failed.append('flapping faults: ' + ', '.join(f'{k}={n} frames' for k, n in flapping.items()))
    if failed:
        for m in failed: print(f'  FAIL {m}')
        return 1
    print('  pass')
    return 0

if __name__ == '__main__':
    sys.exit(main())
