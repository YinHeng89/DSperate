#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Diff two DS_PROFILE_LINE runs: what a phase actually moved.

  tools/profile_diff.py before.jsonl after.jsonl [--label L] [--counts]

Each file holds one JSON object per run (DS_PROFILE_LINE appends). Runs are
matched by their DS_PROFILE_LABEL, so a sweep over scenes diffs scene by
scene; a file with one unlabelled run in it diffs against the other's one run.

The counts are printed with --counts and they are not decoration. A model that
makes a game draw fewer frames lowers the frame time without emulating
anything faster (docs/frame-profile-2026-09-16.md), so a fall in `gx_swap` or
`poly_lines` beside a fall in `ms.median` means the saving is at least partly
the game doing less -- which is a different claim, and usually not the one
being made.
"""
import argparse, json, sys

def load(path):
    runs = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or not line.startswith('{'):
                continue
            r = json.loads(line)
            runs.setdefault(r.get('label', ''), r)   # first wins; re-runs append
    return runs

def fmt(before, after):
    d = after - before
    if abs(before) < 1e-9:
        return f'{before:9.4f} {after:9.4f} {d:+9.4f}        --'
    return f'{before:9.4f} {after:9.4f} {d:+9.4f} {100.0 * d / before:+8.1f}%'

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('before'); ap.add_argument('after')
    ap.add_argument('--label', help='only this run')
    ap.add_argument('--counts', action='store_true', help='also diff the workload counters')
    ap.add_argument('--group', default='typ', choices=('typ', 'p99'), help='which frame group (default typ)')
    args = ap.parse_args()

    a, b = load(args.before), load(args.after)
    labels = [args.label] if args.label else sorted(set(a) & set(b))
    if not labels:
        sys.exit(f'no run labels in common ({sorted(a)} vs {sorted(b)})')

    worse = 0
    for lb in labels:
        ra, rb = a[lb], b[lb]
        print(f'== {lb or "(unlabelled)"} ==  {ra["frames"]} -> {rb["frames"]} frames')
        print(f'  {"":24s} {"before":>9s} {"after":>9s} {"delta":>9s} {"":>9s}')
        for k in ('mean', 'median', 'p90', 'p99'):
            print(f'  ms.{k:<21s} ' + fmt(ra['ms'][k], rb['ms'][k]))
        sa, sb = ra.get(args.group, {}), rb.get(args.group, {})
        rows = sorted(set(sa) | set(sb), key=lambda k: -abs(sb.get(k, 0.0) - sa.get(k, 0.0)))
        for k in rows:
            va, vb = sa.get(k, 0.0), sb.get(k, 0.0)
            if abs(vb - va) < 5e-4 and max(va, vb) < 0.01:
                continue
            print(f'  {args.group}.{k:<{23 - len(args.group)}s} ' + fmt(va, vb))
        for k in ('untimed', 'workers'):
            ka = f'{args.group}_{k}'
            if ka in ra and ka in rb:
                print(f'  {ka:<24s} ' + fmt(ra[ka], rb[ka]))
        if args.counts:
            ca, cb = ra.get('counts', {}), rb.get('counts', {})
            for k in sorted(set(ca) | set(cb)):
                va, vb = ca.get(k, 0), cb.get(k, 0)
                if va == vb:
                    continue
                pc = f'{100.0 * (vb - va) / va:+8.1f}%' if va else '        --'
                print(f'  counts.{k:<17s} {va:>12d} {vb:>12d} {vb - va:+12d} {pc}')
                if k in ('gx_swap', 'poly_lines') and vb < va:
                    print(f'      ^ fewer {k}: part of any ms saving here is the game drawing less')
        # The tail is what a player feels; call out a median win that is a p99 loss.
        if rb['ms']['median'] < ra['ms']['median'] and rb['ms']['p99'] > ra['ms']['p99']:
            print('  NOTE: median improved but p99 regressed -- that is not an improvement')
            worse += 1
        print()
    return 1 if worse else 0

if __name__ == '__main__':
    sys.exit(main())
