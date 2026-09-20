#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove that perceptual_gate.py's structural detectors actually fire.

The gate is load-bearing: after the speed-first rework there is no byte-exact
frame check behind it, so a detector that silently never fires would let a
whole class of regression through unnoticed. This builds synthetic dumps with
one known fault each and asserts the gate reports it -- and, just as
importantly, that a *clean* candidate and a lightly dithered one do not.

No ROMs and no emulator needed; run it anywhere.

    tools/perceptual_gate_selftest.py
"""
import os, subprocess, sys, tempfile
import numpy as np

W, H = 256, 192
HERE = os.path.dirname(os.path.abspath(__file__))
GATE = os.path.join(HERE, 'perceptual_gate.py')

def make_ref(n=24, seed=1):
    """A moving picture: each frame differs from the last, so `frozen` is
    detectable and the alignment window has something to bite on."""
    rng = np.random.default_rng(seed)
    f = np.zeros((n, 2, H, W, 4), np.uint8)
    yy, xx = np.mgrid[0:H, 0:W]
    for i in range(n):
        for s in range(2):
            base = ((xx * 3 + yy * 2 + i * 11 + s * 64) % 256).astype(np.uint8)
            f[i, s, ..., 0] = base
            f[i, s, ..., 1] = np.roll(base, 7, axis=1)
            f[i, s, ..., 2] = np.roll(base, 13, axis=0)
            f[i, s, ..., 3] = 0xFF
        # a moving bright block, so a dropped region is a real change
        y0 = (i * 5) % (H - 40)
        f[i, :, y0:y0 + 40, 30:110, :3] = 240
    return f

def run(ref, cand, extra=()):
    with tempfile.TemporaryDirectory() as d:
        rp, cp = os.path.join(d, 'r.bin'), os.path.join(d, 'c.bin')
        ref.tofile(rp); cand.tofile(cp)
        r = subprocess.run([sys.executable, GATE, rp, cp, '--scene', 'selftest', *extra],
                           capture_output=True, text=True)
        return r.stdout + r.stderr

def check(name, out, want_fault=None, want_clean=False):
    if want_clean:
        ok = 'FAULT' not in out
        why = 'expected no faults'
    else:
        ok = f'FAULT {want_fault}' in out
        why = f'expected FAULT {want_fault}'
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}: {why}")
    if not ok:
        print('        ' + out.replace('\n', '\n        ').strip())
    return ok

def main():
    ref = make_ref()
    results = []

    # A clean candidate must be silent, and must score exactly.
    out = run(ref, ref.copy())
    results.append(check('identical candidate', out, want_clean=True))
    exact = 'ssim mean 1.0000' in out and '0 realigned' in out
    print(f"  {'ok  ' if exact else 'FAIL'}  identical candidate: ssim 1.0 and no realignment")
    results.append(exact)

    # Light dither must not fault: this is the false-positive guard, and it is
    # the whole reason the gate is tolerant rather than exact.
    rng = np.random.default_rng(7)
    dither = ref.astype(np.int16)
    dither[..., :3] += rng.integers(-2, 3, size=dither[..., :3].shape, dtype=np.int16)
    dither = np.clip(dither, 0, 255).astype(np.uint8)
    out = run(ref, dither)
    results.append(check('+/-2 dither', out, want_clean=True))

    # blank: one screen goes flat while the reference still has a picture.
    c = ref.copy(); c[5:9, 0, ..., :3] = 0
    out = run(ref, c)
    results.append(check('blank screen', out, 'blank'))

    # swapped: the two screens exchanged.
    c = ref.copy(); c[10:14, [0, 1]] = c[10:14, [1, 0]]
    out = run(ref, c)
    results.append(check('swapped screens', out, 'swapped'))

    # frozen: the candidate repeats a frame the reference moved past.
    c = ref.copy(); c[15] = c[14]
    out = run(ref, c, extra=('--align', '0'))
    results.append(check('frozen frame', out, 'frozen'))

    # region: one large contiguous area wrong -- what a dropped layer looks
    # like. 60x60 of 256x192 is 7.3 %, over the 2 % default limit.
    c = ref.copy(); c[18:20, 1, 40:100, 40:100, :3] = 0
    out = run(ref, c, extra=('--align', '0'))
    results.append(check('dropped region', out, 'region'))

    bad = results.count(False)
    print(f"\n{len(results) - bad}/{len(results)} detector checks passed")
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
