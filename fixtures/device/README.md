# Device baseline — RG DS Plus, `exact-reference`, 2026-09-20

`baseline.jsonl` is one `DS_PROFILE_LINE` row per scene from the RG DS Plus
(ROCKNIX 7.0.2, RK3566, four Cortex-A55 at 1.992 GHz), headless,
`--quantum 0`, 1800 frames with the first 200 dropped. Diff a later phase
against it with `tools/profile_diff.py`.

Host numbers do not substitute for these: `gx_geom` is ~0.04 ms on an x86
host and 1.0-1.5 ms here.

## The emulation thread, typical frame (ms)

| | sm64 | dbori | etody | mlbis |
|---|---|---|---|---|
| frame median | 11.23 | 12.80 | 5.33 | 13.65 |
| cpu9 | 3.02 | **7.54** | 0.83 | **6.28** |
| cpu7 | 1.37 | 0.73 | 0.70 | 1.16 |
| gx_geom | 1.05 | 1.29 | 0.16 | 1.50 |
| dma | 0.46 | 0.66 | 0.98 | 0.55 |
| spu | 0.62 | 0.16 | 0.14 | 0.37 |
| sched | 0.65 | 0.53 | 0.32 | 0.52 |
| band workers (overlapped) | 10.16 | 10.17 | 4.25 | 11.23 |

The band workers do as much work as the emulation thread and `r3d_wait` is
0.0002-0.18 ms, so they remain entirely off the critical path — as the
2026-09-16 profile found, and the reason Phase 4 is scoped for p99 and
thermals rather than the median.

## Phase 2's census: where the ARM9 row actually goes

`perf -F 499`, 900 frames, filtered to the emulation thread (the threads are
named, so `--comms dsperate-headle` isolates it from `r3d-band*` and
`line-worker`). Shares are of the whole process.

| | dbori | mlbis |
|---|---|---|
| emulation thread, total | 90.4 % | 56.1 % |
| ‣ `[JIT]` translated guest code | 32.7 % | 19.2 % |
| ‣ ... as a share of the emulation thread | **36 %** | **34 %** |
| ‣ `Io::cart_catch_up_slow` | **10.3 %** | 3.0 % |
| ‣ `Io::read32_special` | 5.5 % | 1.6 % |
| ‣ `Io::read` | 4.8 % | 1.5 % |
| ‣ `ds_slice_next` | 2.5 % | 1.4 % |
| ‣ `jit_h_ld32` | 1.8 % | 1.0 % |
| ‣ `interp::ldm_stm` (JIT fallback) | 1.7 % | 0.5 % |

**Translated guest code is about a third of the emulation thread on both
scenes.** That is the floor Phase 2 cannot go under without better block
quality, and it is not addressed by any row the plan had queued.

**The `Io::` read path is the largest identified non-JIT cost**: on dbori
`cart_catch_up_slow` + `read32_special` + `read` + `cart_schedule_receive` is
21.5 % of the process, i.e. ~24 % of the emulation thread; on mlbis ~11 %.
dbori reads `GXSTAT` ~950 times a frame and starts ~413 DMAs a frame, so it
is the extreme, but mlbis reads `GXSTAT` only 12 k times in 1800 frames and
`cart_catch_up_slow` is still its hottest non-rendering symbol.

### One hypothesis tested and rejected

`cart_catch_up()` inlines only `transfer_pos < transfer_len`, while
`cart_catch_up_slow()` opens with `if (event_armed || late) return`. The
obvious reading is that a title reading ROMCTRL during an armed transfer pays
an out-of-line call that does nothing, so the guard was hoisted into the
caller — provably identical semantics, and byte-identical frames on all five
scenes for 900 frames each.

**It does not pay.** Device, paired, both orders, 2 reps, 1800 frames, swap
counts unchanged: dbori median +0.69 % and cpu9 +1.06 %, mlbis +0.42 % /
+1.30 %, sm64 −0.10 % / −0.31 %. So the samples are in the function's real
work — the receive loop and the DMA-armed scan — not in the early return, and
the extra pair of loads at every call site costs more than it saves. Reverted.

The lesson for the rest of Phase 2: a `perf` share says *where* the time is,
not *why*. `perf annotate` is what distinguishes them, and it needs
`objdump` on the device (ROCKNIX has none) or the `perf.data` copied back and
annotated against the cross binary with `aarch64-linux-gnu-objdump`. Do that
before writing the next patch.

## The five-arm geometry run (`gx-arms.jsonl`, `gx-arms.log`)

`tools/gx_arms.sh`, run on the device 2026-09-20: five arms, three reps,
rotating arm order so no arm sits at a fixed point in the thermal curve.
The arms separate the three changes `--timing-oc` bundles — the no-FIFO
model, the geometry worker, and the band drop from three workers to two that
`worker_activate` performs when the worker starts.

| arm | flags |
|---|---|
| `base` | — (inline, FIFO kept, timed) |
| `wctl` | `--gx-worker`, `DS_GX_THREAD=1` |
| `walw` | `--gx-worker`, `DS_GX_THREAD=2` |
| `tinl` | `--timing-oc`, `DS_GX_THREAD=0` |
| `tall` | `--timing-oc`, `DS_GX_THREAD=1` |

The result and what it changed are in the plan at §3.4. In short: the no-FIFO
model alone is worth about a millisecond of median and regresses nothing; the
worker buys median by moving work off the emulation thread and charges p99 in
every scene, turning negative on total wall time in two of four.

Two traps this run walked into, both worth remembering:

* A backgrounded watcher of the form `while pgrep -f foo.sh; do sleep 10;
  done; bar.sh` **waits on itself** — the watching shell's own command line
  contains `foo.sh`, so `pgrep -f` matches it forever and `bar.sh` never
  runs. The same self-match makes a `pgrep -f`-based status check report
  both jobs as running, so the check that should catch it hides it instead.
* A `dsperate-headless` left over from an earlier session was still alive
  throughout the first run, blocked in `wait_for_partner` opening a
  `--dump-frames` FIFO with no reader. It was harmless here (`state=S`,
  14 ticks of CPU in 92 minutes, loadavg 0.17) but only because it was
  blocked rather than spinning; check `/proc/<pid>/stat` before trusting or
  discarding a run, rather than assuming either way.

---

# Device baseline after Phase 2 — `baseline-phase3.jsonl`, 2026-09-21

`baseline.jsonl` above is `exact-reference`. Phase 1 replaced the geometry
model and Phase 2 the JIT's fallback handling, so **none of its per-stage rows
is this tree's any more** and it cannot serve as Phase 3's before-line. It is
kept as the rework's origin, not as a current reading.

`baseline-phase3.jsonl` is that before-line: `dsperate-p2g`, byte-identical to
the AArch64 build of `83898bf`, `--quantum 0`, 1800 frames from 200 (gsdd from
700), **three reps per scene**, medians of the three below. Two deliberate
differences from the old baseline: three reps rather than one, because every
Phase 3 claim gets diffed against this; and **gsdd is included**, having been
absent before.

| ms | gsdd | mlbis | dbori | sm64 | etody |
|---|---|---|---|---|---|
| **frame median** | **20.67** | 12.30 | 11.33 | 9.26 | 5.05 |
| cpu9 | 8.08 | 5.45 | 8.08 | 3.17 | 0.88 |
| cpu7 | 1.42 | 1.28 | 0.66 | 1.27 | 0.68 |
| dma | **2.54** | 0.40 | 0.37 | 0.26 | 0.87 |
| spu | **1.13** | 0.41 | 0.14 | 0.59 | 0.17 |
| sched | 0.82 | 0.40 | 0.45 | 0.50 | 0.31 |
| events | 0.148 | 0.016 | 0.040 | 0.014 | 0.009 |
| gx_geom | **0.00** | **0.00** | **0.00** | **0.00** | **0.00** |
| **Phase 3 surface** (dma+spu+sched+events) | **4.64** | 1.23 | 1.00 | 1.36 | 1.37 |

**`gx_geom` is gone.** It was 1.05-1.50 ms on the four old scenes and is 0.00
on all five now — Phase 1 did not shrink the row, it deleted it.

**Golden Sun is the only scene outside the 16.74 ms budget**, by 3.93 ms, and
it holds 4.64 ms of the 9.59 ms of Phase 3 surface across all five scenes --
just under half. The
other four clear the budget already and offer 1.0-1.4 ms each *in total*
across DMA, SPU and the scheduler.

Read the plan's Phase 3 target against that before scoping work to it: -2.5 ms
is not reachable on four of the five scenes because the rows do not contain it,
and on gsdd it requires removing about 85 % of the whole surface. The -2.5 ms
figure comes from the 2026-09-16 NSMB profile, taken in the SDL frontend with
`--dual-window` on a tree two phases old, and NSMB has never been device-scened
in this rework.
