# The speed-first rework — scoping

Started 2026-09-20. What it takes to move DSperate's goal from *"melonDS
accuracy with a portion of DraStic's speed"* to *"as fast or faster than
DraStic, and more stable"*, and what has to be given up to get there.

**How to read this document.** §0–§2 are the current state and the plan. §3 is
the phase list. §4 is the research record: every `SS3.x` section is preserved
under its original number because commits, fixtures and notes cite them, but
each is compressed to its conclusion and the numbers that constrain a
decision. §5 indexes every closed route so none is re-litigated. §6 is how to
trust a measurement here — read it before quoting any number.

Decisions taken before this document was written, and assumed throughout:

* **Speed-first by default.** The picture needs to be *plausible at 1x*, not
  byte-identical to melonDS. Timing models, DMA granularity, SPU granularity
  and geometry ordering may be as inexact as speed requires.
* **RK3566 (the RG DS / RG DS Plus, four Cortex-A55 at 1.992 GHz) is the
  target.** The H700 and the ARMv7 A30 are best-effort: they must keep
  building and running, but they do not shape the design. What they actually
  expose is measured in SS3.34.
* **Stability means** no crashes, hangs or thread hazards; steady frame
  delivery and no audio gaps; broad *retail* compatibility. Romhacks and
  shovelware are not a bar.
* Nice features (dual window, multiple render buffers, layouts, overlays) are
  worth keeping, but rebuilding them on the new core is acceptable.

---

## 0. Status, and the route to 60 fps

A DS frame is **16.74 ms**. **The bar is the tail, not the median** — a frame
time spike is what a player feels, and a change that improves the median while
degrading the tail is a **loss**. This document recorded that trade three
times (SS3.4, SS3.6, SS3.11) without letting it govern the plan. It governs it
now.

**But the tail is not p99.** A DS run is part loading screen, and cart loads
legitimately take frame time — a loading screen is the one place a spike is
acceptable. A percentile taken over a run that is one-third loading measures
the loading. SS3.33 is the worked example: judged on p99, GPU present looked
like a 7 ms regression on gsdd; judged on over-budget frames it nearly halves
them, and judged on burst length it turns a 211-frame chug into a 22-frame
one.

**So the bar is, in order:** over-budget frame count; **longest burst**, which
is the closest thing to what a player feels; then **p95** as the single-number
tier; with p99 and max still reported and never silently passed, because that
is where a stall shows. `frame_report` already prints the first two and the
per-window distribution that separates a loading section from a stutter.

Device medians and tails, RG DS Plus, headless `frame ms`, from
`fixtures/device/baseline-phase3.jsonl`:

| scene | median | p90 | p99 | max | **p99 vs budget** |
|---|---|---|---|---|---|
| **gsdd** | 20.69 | 25.15 | **33.11** | 39.53 | **+16.37** |
| **etody** | **5.08** | **17.28** | **22.83** | 30.86 | **+6.09** |
| **sm64** | 9.25 | 13.88 | **20.26** | 35.31 | **+3.52** |
| **dbori** | 11.33 | 14.60 | **20.25** | 35.15 | **+3.51** |
| **mlbis** | 12.28 | 14.02 | **18.26** | 30.04 | **+1.52** |
| st-intro | 19.08 | — | — | — | not in this baseline |

**Read at the median, four of six scenes clear the budget. Read at p99, none
of them do.** Etrian Odyssey is the sharpest illustration: a 5.08 ms median
and a 17.28 ms p90, so it misses the budget one frame in ten while appearing
to be the easiest scene in the set by a factor of three.

### Where the tail actually is

gsdd's p99 frame is 33.11 ms, and the stage breakdown says what fills it:

| stage | typ | **p99** |
|---|---|---|
| **untimed** (= the 2D hand-off, SS3.19) | 4.86 | **17.20** |
| cpu9 | 8.08 | 9.66 |
| dma | 2.54 | 3.00 |
| cpu7 | 1.42 | 1.69 |
| spu | 1.13 | 1.26 |
| everything else, each | <1.0 | <0.8 |
| *workers, all threads* | *23.75* | ***79.37*** |

**The hand-off is the tail.** `cpu9` rises 1.6 ms from typ to p99; `untimed`
rises 12.3. SS3.18 said it in one line — *"it accounts for essentially the
entire tail: nothing else contributes more than 1.5 ms to p99"* — and the
`p99_workers` figure of 79.37 ms says why: on tail frames four busy threads
contend for four A55s and the emulation thread waits behind all of them.

### The route, ordered by evidence per unit of risk

1. ~~Confirm and default `--gpu-present`.~~ **DONE.** Measured on three
   scenes (SS3.33) and defaulted on: over-budget frames 72→42 % on st-intro,
   47→28 % on gsdd, 44→32 % on nsmb, longest burst 291→3-9 frames. Its one
   cost, a 37-80 ms Mali stall frame per ~1600 on gsdd, is reported and does
   not gate (SS3.37).
2. **Attack the 2D hand-off.** It is 4.86 ms of gsdd's median and **17.20 ms
   of its p99**, and it is in **no phase's scope** — Phase 3 does not cover it
   and Phase 5 was dropped on an instrument that could not see it (SS3.18).
   SS3.19: the lever is pipelining, not correctness. This is both the largest
   unclaimed item in the project and the single biggest tail term measured
   anywhere in it.
3. **Phase 4, as the durable fix.** A cheaper raster collapses the wait rather
   than pipelining around it, and it is what lets the thread count drop from
   five to three — which is the *structural* cure for a tail caused by
   oversubscription (SS3.30 route 2). Start with the data layout (SS3.35).
4. **2–4x on the light games.** Independent and separately shippable. The
   scenes worth upscaling have no display capture, so a one-frame-deferred GPU
   raster is free on exactly them (SS3.36). Note etody is *not* one of them
   until its p90 is understood.
5. **Phase 3's remaining rows** (3c/3d/3e/3f), ~1–1.5 ms realistically, with
   3c the best.

**Steps 2 and 3 solve the same problem**; 2 is cheap and 3 is durable. Measure
2 first — but judge it on p99, because that is where its prize is.

### The open question this table raises

**Nothing in this document explains etody's p90.** A 5.08 ms median with a
17.28 ms p90 and a 22.83 ms p99 is a scene that is idle most of the time and
then stalls hard, and no phase has ever censused it — every census in SS3.18
onward was run on gsdd. SS3.6 recorded the same shape from the other side
(one band worker: best median of any arm, worst tail), and SS3.4 found every
arm containing the geometry worker degraded p99 on all four scenes. **A p99
census on etody is unclaimed work and it is cheap**, and until it is done the
"four of six scenes are comfortably inside budget" framing this document used
for three phases should not be repeated.

---

## 1. Where we are

**DraStic**, `docs/techniques/00-method-and-measurements.md`, same device:
SM64DS 2.98 ms (recompiler), Meteos 0.75 ms. Its cycle split: 3D raster +
resolve 32.6 %, 2D engines 25.2 %, translated guest code 14.7 %, JIT support
12.0 %, 3D geometry 2.7 %, scheduler 1.7 %, SPU 1.1 %.

**The goal line is already met on the hardest scene** (SS3.7). Measured live
rather than through DraStic's `--benchmark` — which skips the screen path —
DraStic reads 64.8 % speed on Golden Sun hi-res, ~25.7 ms a frame, against
ours at 20.4 ms at 2x and 15.3 at 1x. The earlier "DraStic 4.5 ms ahead at
every resolution" was an artefact of benchmarking a path players never use.

What remains is **margin, thermals, and the two scenes above** — plus the two
measured gaps behind them: CPU emulation 8.3 ms against our ~13, and the
raster at roughly **eight times DraStic's cost per pixel**. DraStic's
threading is our shape already (kicked a scanline earlier, per-band masks,
engine B on a 2D worker), so there is nothing there to adopt; the win is
algorithmic.

---

## 2. Strategy, and what replaces the exactness gate

Take the rows the checklist marks `no` **because we chose hardware-exact
behaviour** — geometry log-replay (4.17/4.18), whole-transfer DMA (4.8/4.9),
per-frame SPU (5.1, 5.4/5.6/5.9/5.11), the melonDS-exact rasteriser — and
implement DraStic's model instead. Then **delete the exact path rather than
keeping it behind a flag**.

**The dual-path tax is itself a cost.** Every accuracy switch is a branch in a
hot path, a state the tests must cover, a combination that can deadlock, and a
reason a bug report is not reproducible. Collapsing to one path is a speed,
stability and velocity win at once, and it is what makes the rest affordable.

### The gates

1. **A pinned reference binary** — tag `exact-reference`, built once, frozen.
2. **A perceptual frame gate** — `tools/perceptual_gate.py`, SSIM/PSNR with
   per-scene floors plus four structural faults (black screen, frozen buffer,
   swapped screen, missing layer), over a timing-tolerant alignment window.
   The window is load-bearing, not a nicety (SS3.1).
3. **Self-consistency** — the same build replaying the same `.dsin` twice must
   produce identical frames. **Never audited; see the warning in §6.**
4. **A cadence census** — `tools/cadence_census.sh`. The picture gate compares
   *what* was drawn and is blind to *when*. Swap counts, no-swap VBlanks and
   GXSTAT reads, diffed per scene. Three of the four counters gate; GXSTAT is
   reported and never fatal (SS3.17).

Plus a **compatibility sweep** over a retail set (46 titles today, `tools/title_sweep.sh`),
checked for hangs, black screens and gross audio failure. Retail only.

---

## 3. The phases

### Phase 0 — The bar, the reference, and the census — **CLOSED**
Built and validated 2026-09-20 (SS3.1). `golden_dump.sh`, `perceptual_gate.py`
(+ selftest, in CI), `cadence_census.sh`.

### Phase 1 — Geometry as a replayed log — target −4.0 ms; **delivered −0.30 to −1.49 ms, CLOSED** (SS3.5)
`gxfifo_write` became appends to a command/parameter log replayed at VBlank;
the FIFO machinery, the geometry worker, the pricer, `--timing-oc` and
`DS_GX_THREAD` are deleted. **The synthesised `GXSTAT` was kept deliberately**
— DraStic's 4.19 was *not* adopted, because synthesising a safe answer (FIFO
always empty and under-half-full; busy bit held 650 cycles past the swap) is
strictly the better contract and stops a poller flipping panels early.
1c (the batched transform) was built, measured at **+0.65 ms on gsdd**, and
abandoned: a batch must hold transformed results while strip assembly recycles
the four `temp_vtx_` slots, so each vertex is copied — exactly the 56-byte copy
the engine avoids today by permuting `vptr_`.

### Phase 2 — The ARM9 row — target revised then retired; **delivered −0.02 to −1.13 ms, CLOSED** (SS3.16)
Two emitter clusters, wider idle skipping, the mode-switch core, the exception
return and `LDM ^`. End to end: gsdd **−1.126**, dbori −0.534, sm64 −0.279,
mlbis −0.016. **Only gsdd separates from the noise; only the end-to-end column
should be quoted.** Translated guest code — a third of the emulation thread —
is untouched and remains the floor.

### Phase 3 — DMA, SPU and scheduler granularity — **STOWED at 3b** (SS3.29)
Rescoped per scene against SS3.17's before-line. Four of five scenes hold
1.0–1.4 ms across DMA, SPU and the scheduler *combined*; there is no −2.5 ms
in them. gsdd holds 4.64 ms of surface and misses by 3.93.

* **3a — the gate met.** Done (SS3.17).
* **3b — census before commit.** Done (SS3.18–SS3.28), and it reordered the
  phase: gsdd's critical path is the 2D worker, not DMA.
* **3c — the cart, first. UNTOUCHED and still the best row.**
  `Io::cart_catch_up_slow` was the hottest C++ symbol on both censused scenes
  — ~24 % of dbori's emulation thread, ~11 % of mlbis's (SS3.3). The lever is
  already written: `DS_CART_BULK` (`io.cpp`, `cart_receive_word`) produces the
  words as the DMA reads them and keeps **one** event at the nominal time of
  the last word — 1 scheduler event and 1 slice per 512-byte block instead of
  128 of each. Measure as an A/B (it is an environment variable, so this costs
  one run, not a patch), sweep it because cart timing is where loaders live,
  and if it holds make it the default and delete the flag.
  *Risk:* most likely to change what a title observes; failure mode is a
  loader that hangs, not a picture that differs.
* **3d — DMA.** The original premise is stale: `dma.cpp` already does
  whole-run `memcpy` between direct-mapped pages, closed-form cyclic burst
  costs, a binary-searched budget cut, one page-table walk per end per run and
  a once-per-run VRAM trap. The per-word path is the fallback now. With
  geometry lifted out (3.56 ms of `cpu9` is geometry replay) the genuine DMA
  row is well under 2.595 ms. Still unclaimed and worth having: code-page
  invalidation over the destination as a coarse/fine bitmap OR (4.10).
* **3e — the scheduler, and the FF3 freeze together.** Remove
  `LOCKSTEP_QUANTUM` and the dual-mode interleave; let both-halted slices run
  to the deadline. See Phase 6 for the freeze's measurement.
* **3f — SPU, last and split.** Worth at most 1.13 ms and only on gsdd.
  *Cheap half* (no model change): resolve each channel's source to a host
  pointer at key-on, decode ADPCM a word at a time into a ring, PSG as duty
  tables, noise as a precomputed 32 K LFSR table. *Model change* (mix once per
  frame at VBlank with a catch-up on the ARM7 audio timer) only if 3b shows
  per-sample loop cost rather than per-event cost. Its exit gate is the
  weakest in this document — "by ear and by a spectral comparison" — and
  Rhythm Heaven must be checked by ear specifically.
* **Stop rule:** Phase 3 ends when gsdd is inside 16.74 ms, or when every
  remaining row is under ~0.3 ms — whichever comes first. Not when all the
  rows have been done.

### Phase 4 — The rasteriser, rebuilt — **NOW THE CRITICAL PATH** (§0 step 3)
Originally scoped for p99, thermals, 2x and devices with fewer cores, and
deliberately placed *after* the ARM9 row on the grounds that the raster is
hidden behind the band workers (SS3.6). That reasoning was about the raster's
*visibility* and it still holds; what it did not consider is that the raster's
**volume** is what forbids a smaller thread topology and what makes gsdd's
emulation thread wait (SS3.30). The DraStic design to move to (`02` §§1–4):

* **12 bins × 16 scanlines**, ~32–36 KB live, sized to the A55's 32 KB L1D,
  against our ring of 8 lines per worker (~50 KB) in horizontal bands. 12
  divides by 1/2/3/4/6/12, so the split needs no remainder handling and no
  adaptive controller — deleting `DS_R3D_ADAPT` and the non-reproducible
  per-frame shape controller is a stability win in itself.
* **AND/OR uniformity per polygon at bin time** (2.4). Constant-W and flat-RGB
  measured empty and constant *alpha* had signal — but that was under the
  exactness constraint. Re-measure with tolerance allowed.
* **8-way unrolled scalar texel gather** (2.14) — never measured, and the one
  kernel row explicitly designed for an in-order core.
* Now permitted: drop or approximate edge marking, fog precision, the
  shadow-volume stencil's exact semantics, and the AA coverage blend, each
  measured independently against the perceptual gate.
* **Start with the data layout** (SS3.35), which serves both the software
  raster's cache behaviour and any GPU path.

Keep `kernels_ref.cpp` beside `kernels_neon.cpp` and `tests/kernels_test.cpp`
(row X.7): the C reference is what makes hand-written kernels maintainable and
costs nothing at runtime.

*Exit gate:* perceptual gate with per-scene floors; worker time per frame
halved; p99 on NSMB within 2 ms of the median.

### Phase 5 — The 2D engines — **DROPPED, then REOPENED** (SS3.18)
Dropped on "0.64 ms on Golden Sun, 0.0 % join wait" (SS3.6). **Both figures
were measured with an instrument that could not see the path**: the 0.64 ms is
2D work on the *emulation thread*, and the 0.0 % is `JOIN0`, which scopes one
of nine join sites and reads 0.002 ms. The line worker is 22.7 % of the
process by `perf` and the emulation thread blocks on it. This does not by
itself reinstate the representation work (1-bit masks, planar split); **the
cheaper answer is likely the hand-off, not the rendering** — which is §0
step 2.

### Phase 6 — Stability as a workstream
Runs alongside, with its own gates.

**The FF3 freeze is not a JIT defect — it is interleave granularity**, and it
belongs with the Phase 3 scheduler row. Measured under qemu, 6000 frames:
interpreter, `--jit9` alone and `--jit7` alone are all healthy at quantum 2048
(1107 distinct frames); **both recompilers at 2048 freeze** (95 distinct). At
1024/1536/1792 both recompilers are healthy. `--quantum 0` *is* 2048. The
cliff is sharp: 1792 clean, 2048 frozen, nothing in between. So it is not
miscompilation (same quantum, interpreter healthy) and not the quantum alone
(same quantum, one recompiler healthy) — it is the two CPUs' relative progress
inside one slice under the recompiler's cycle accounting.

**The pinning constraint has expired.** "SM64DS boots at 2048 and not at 2560"
is no longer true: it boots identically at 1792, 2048 and 2560. Lowering to
1792 fixes FF3 at no measured cost. **Treat that as a mitigation in the
pocket, not the fix** — it moves a cliff without explaining it. Re-verify the
SM64DS constraint before relying on either number.

*Not* mGBA's "Holy grail bugs II" FF3 freeze: `cart_write_romctrl` already
applies gap1 in every command mode and gap2 per 512-byte block. Worth
re-reading only if the scheduler line runs out.

Other tracks: shrink the thread topology by design; make the band pool's
hand-off reproducible (the self-consistency gate is the enforcement mechanism
and today's worker path fails it); unify hand-off mechanisms (`LineWorker`
spins then parks while the 3D workers use mutex+condvar, row 4.23, with a
recorded qemu hang); the JIT arena flush (row 1.23); save-state robustness
(every chunk carries its own count; `state_roundtrip.sh` in CI); fuzz both
translators after every JIT step.

**Frame pacing.** Every phase's measurement must report **missed frames and
dry audio queues** alongside the median — a 1 ms median win that adds jitter
is a loss. Once the median is comfortably inside budget, reinstate
**render-only frameskip** (4.22) as a core-level skip state that makes
`render_scanline` a no-op.

### Phase 7 — Features back, and the knobs gone
### Phase 8 — The scaffolding out
Unchanged. The accuracy switches enumerated in §2 come out with them.
---

## 4. The research record

Every section keeps its original number because commits, fixtures and notes
cite them. Each is its conclusion and the numbers that constrain a decision;
the derivations are in the git history of this file.

**SS3.1 — What P0 measured.** The alignment window is what makes the gate
usable: sm64 golden against `--timing-oc`, strict `--align 0` reads ssim mean
0.9912 with **blank ×1, region ×2 structural faults**, all of which are timing
skew at scene transitions; `--align 2` reads 0.9999 with none. Today's default
build is byte-identical to golden, so it calibrates nothing — the floors had
to come from an inexact configuration. The documented Phase 1 casualty is
stale: dbori under `--timing-oc` is byte-identical over 1800 frames (swaps
172/172, GXSTAT 542150/542150) because `149bb5e` landed after the doc that
recorded the desync. **Cadence cannot be settled headlessly** — the harness
boots differently and runs dbori's intro at 30 Hz against SDL's 60 Hz.

**SS3.2 — The cost model, measured.** `DS_JIT_COSTPROBE`, qemu `libinsn`,
serial. The whole per-access cost model is **2.3–5.5 % of emulated
instructions** (sm64 +5.54 %, meteos +2.33 %), worth roughly **0.2–0.4 ms a
frame, not the 3.5 ms Phase 2 was scoped for**. Parts 1 and 2 sum to part 3
within 0.003 %, which is the check that the probe measures what it claims.
Caveat that cannot be settled by counting: the lookup is a *dependent load*
through `R_TIM`, and on an in-order A55 that can cost more than its
instruction count. **`DS_JIT_FASTCOST` is not a cheaper model** — +3.16 %
*slower*, because it always emits `add_imm` + `sub_reg` where the common ARM9
case (`numC <= 1`) emits a single `sub_reg`.

**SS3.3 — Phase 2's device census.** Translated guest code is **~35 % of the
emulation thread** (36 % dbori, 34 % mlbis) and nothing in the plan's queued
rows touches it. **The `Io::` read path is the largest identified non-JIT
cost** — ~24 % dbori, ~11 % mlbis, with `cart_catch_up_slow` the hottest C++
symbol on both. Rejected here: hoisting its `event_armed || late` guard —
provably identical semantics, byte-identical on five scenes, **flat to 1.3 %
worse** on the device. *The lesson is methodological:* a `perf` share says
where the time is, not why.

**SS3.4 — Phase 1's premise, measured.** `--timing-oc` is **three changes, not
one** (both frontends also wire it to `set_geometry_worker`, which drops the
band workers from three to two). Five arms, three reps: the geometry model
alone (`tinl`) is worth **−1.21 / −0.35 / −0.11 / −0.07 ms** on
sm64/mlbis/dbori/etody, not the 3–4 ms claimed. **The worker does not make the
machine faster** — every arm containing it degrades p99 in all four scenes;
on etody the trade goes fully negative (total +1.5 s, p90 +4.0). The shape
controller picks wrong wherever the worker helps at all. Also: **`gx_geom`
does not measure geometry** — `GX_RUN` wraps only the two `run_to()` calls in
the slice loop.

**SS3.5 — Phase 1 closed.** −1.49 / −1.30 / −0.50 / −0.30 ms of median on
sm64 / mlbis / dbori / etody, with mean and total improving on all four.
**1c abandoned** at +0.65 ms on gsdd (see Phase 1 above). **The instrument
that failed:** instruction count called 1b +0.0016 % on sm64 from an
uncontrolled `-O3` vs `-O2` comparison; rebuilt at matching flags it reads
−0.050 % and **+0.473 %** — 1b executes *more* instructions on the
matrix-heavy scene while taking 1.5 ms off sm64's frame.

**SS3.6 — The rasteriser does not steal from the emulation thread.** `cpu9` is
flat to 1.2 % across 0/1/2/3 band workers and is **lowest at three**. The
hypothesis is dead; a cheaper raster will not narrow the ARM9 row. *Two
cautions:* `typ` values are per-stage **medians**, and a sum of medians is not
the median of the sum — the same arithmetic gives *negative* unaccounted time
at zero workers, which is impossible. And one band worker has the **best
median of any arm** (21.95) with the worst tail (p90 43.1).

**SS3.7 — The goal line, restated.** See §1.

**SS3.8–SS3.9 — Phase 2's survey and first delivery.** Two emitters
(cross-page block transfers; `MSR SPSR` inlined): fallbacks −35.5 %,
instructions −4.33 %, device median better on three of four, mean on three,
p99 on four. **Instruction count over-predicted again** — −4.33 % of
instructions became −0.8 % of gsdd's frame. *A gate that looked broken and was
not:* the JIT's dump differs from the interpreter's by 3.6 % of bytes on
Golden Sun **on the unmodified tree**; judged by result it is ssim 0.9999, no
faults, 359 of 800 frames merely realigned. `tools/jit_gate.sh` is the right
comparison.

**SS3.10 — The mode-switch core: RETRACTED AND REVERSED.** Rejected on a
single *threaded* census reading +1.53 %. Run serially — the only valid way —
the same comparison reads **−1.38 %**, and on the device it is faster on all
four scenes. Restored. The rule it proposed ("replacing a fallback pays when
the fallback's own work can be done more cheaply, not when only its decode can
be avoided") is a counterexample to itself and is **withdrawn**; it caused two
further wrong rejections before being caught.

**SS3.11 — Wider idle skipping.** The GX veto removed, and a skipped CPU now
advances its own DMA. Median better on three, mean and total on all four, and
**gsdd's p99 down 1.28 ms — the largest p99 movement in the rework.** *The
instrument was wrong again:* the survey counted **slices**, and 721 k
recovered slices on sm64 bought 0.141 ms; mlbis recovered 414 k for nothing
measurable. Cycles skipped would have been the honest instrument.

**SS3.12 — The instruction census is only valid run serially.** The same
binary, 900 frames, default threading: 29,860,253,706 and 28,884,498,240 —
**3.4 % apart**. With `DS_R3D_THREADS=0 DS_2D_THREAD=0` it reproduces to 380
instructions in 3.57 billion. A threaded pair cannot resolve anything under
~4 %, and one rejected a working change (SS3.10).

**SS3.13 — `LDM ^`: ruled out twice, wrongly.** Both rejections reasoned from
the *interpreter's* implementation rather than the architecture. The banking
says: r0–r7 are never banked; r8–r12 are banked only for FIQ; **only r13 and
r14 belong elsewhere** in IRQ/SVC/ABT/UND. Built: gsdd **−0.094 ms** median,
the other three inside noise. Fallbacks −137 k (−27 %) bought 0.1 ms — about
0.7 µs per thousand. **The general lesson: derive what a fallback needs from
the architecture, not from the interpreter, which is written for clarity in C
and routinely does more than the hardware demands.**

**SS3.14 — A defect the user-bank work uncovered.** `emit_block_slow` had no
`i == 15` case and `host_reg(15)` returns 28, which is r14's host register —
so an `STM` with r15 in the list stored r14's value whenever the transfer
could not stay inline. Pre-existing, stores only. *What it says about the
fuzzer:* machines were reused across trials with no RAM reset, so a seed's
result depended on the trials before it; and self-modifying trials were being
judged rather than declined. Both fixed.

**SS3.15 — The exception return: the largest single win in Phase 2.** gsdd
**−0.614 ms** median. **Why it is worth seven times `LDM ^` per occurrence:**
the encoding appears 120,458 times but total fallbacks fell by 561 k, because
this was a **block-ending** fallback — every IRQ exit forced a dispatcher
round trip, 134 times a frame.

| | occurrences removed | gsdd median won | per thousand |
|---|---|---|---|
| exception return (block-ending) | 120,458 | 0.614 ms | **5.1 µs** |
| `LDM ^` (mid-block) | 137,231 | 0.094 ms | 0.69 µs |

**So the rule SS3.10 was reaching for is about block shape, not work:** a
block-ending fallback costs the dispatcher round trip and the truncated block
too, so a census *understates* it; a mid-block fallback costs roughly its own
execution, so a census *overstates* it. **That is the axis to sort any future
fallback census on.**

**SS3.16 — Phase 2 closed.** See the phase entry. **The step-sum and
end-to-end columns disagree, and the disagreement is the honest error bar**
(they agree on gsdd within 4 % and diverge where every step was inside the
noise, because each step's base was built in its own session and those bases
drift by more than the steps move). *Two traps in the device records:* every
`ab*.sh` carries the same copied header, accurate only for `abidle.sh` — read
the arms, not the header; and `phase2-modeswitch-ab.jsonl` labels its arm
`p2c` while the binary was `p2d`. Accepted lineage: `1c2` → `p2a` → `p2b` →
`p2d` → `p2e` → `p2g`; **`p2c` is the abandoned `const_nd` branch and appears
in no later arm.**

**SS3.17 — Phase 3a: the gate met, and two gates that were not gating.**
Sweep: 46 titles, 39 ok / 7 static / 0 fail, no status change — Phase 2's exit
gate is met. But **the status column is too coarse**: joined on `distinct`,
**FF3 now freezes 43 % earlier** (167 → 95 distinct frames). **The cadence
counters were never gating at all** — `gate.sh` printed a diff and returned
success regardless. Three of four now gate; GXSTAT is reported only. *Twice in
one session a gate reported instead of judging, and both were found by reading
what the script does rather than trusting what this document says it does.*

**SS3.18 — gsdd's critical path is the 2D worker, not DMA.** The first `perf`
ever run on gsdd, plus three profile rows it showed to be lying (`GX_RUN` and
`GX_JOIN` had no scope site; the sum and `untimed` double-counted `JIT_TX`).

| stage | typ | p99 |
|---|---|---|
| **2d worker join** *(of which)* | **2.782** | **16.317** |
| cpu arm9 | 8.104 | 8.985 |
| **gx geometry** *(of which)* | **3.560** | 3.452 |
| dma | 2.595 | 2.720 |
| spu | 1.124 | 1.237 |
| sched | 0.865 | 1.055 |
| **untimed** | **4.858** | **17.148** |

`perf` agrees: `Dma::run_channel_impl` 9.81 %, `__schedule` 9.49 %,
`submit_vertex` 7.69 %, `drain_all` 6.24 %, and **no translated guest code in
the top thirty symbols at all** — the opposite of dbori and mlbis. Three
consequences: the emulation thread's largest cost here is **waiting for the 2D
line worker**; **SS3.6's grounds for dropping Phase 5 do not hold**; and
**`cpu9` is not 8.1 ms of ARM9** — 3.56 of it is geometry replay.

**SS3.19 — The `untimed` row *is* the join.** Subtracted out of `GPU_LINE` and
charged to nothing. And the site is **`render_ranges`' pre-join, not
`catch_up`** — 99.8 % against 0.21 %. **The lever is pipelining, not
correctness.**

**SS3.20 — The 2D worker, on and off.** Its join time predicts whether it
pays.

**SS3.21 — What the VRAM write trap is for, and why GXSTAT does not bear on
it.** Both halves of the natural premise are wrong. **The trap is not there
for the 3D bands or for threading** — it arrived with lazy 2D (`e744436`),
which defers the whole frame's render to the last display line: registers,
palette, OAM and MASTER_BRIGHT can be journaled and replayed in hardware
order, VRAM cannot, so a read-after-write guard takes its place. It arms with
`DS_2D_THREAD=0` too. **GXSTAT is not synthesised here**, and an attempt to
synthesise it was built and reverted the same day: without the geometry worker
nothing advances between polls, so the guest spins to VBlank — dbori's GXSTAT
reads went **1.55 M to 37 M** and SM64DS's picture did not move. What replaced
it is *sync on observation*. **And the trap is not where the time is:** trap
joins are 1.2 ms of 4.82 s across a run — **0.03 %** — against `render_ranges`'
pre-join at 99.8 %. Deleting it returns none of the 2.84 ms. The live row for
removing it is Phase 5's (4.11), justified by the accuracy bar and expected to
be worth little outside Golden Sun.

**SS3.22 — `DS_2D_SPLIT`** measured null on the current tree, and inert by
construction; removed.

**SS3.23 — The gate could not name the failure the bar is set on.** A swapped
picture was not detectable; fixed in both shapes (see also SS3.26).

**SS3.24 — Spirit Tracks joins as hard as Golden Sun, by another route.**
19.08 ms, and it was in no baseline at all.

**SS3.25–SS3.27 — The remap census and why the join was there.** The obvious
lever is not present; the right one is a snapshot. The worker reads the live
`VramMap`, and the join was a synchronisation, not a semantic guard. **Still
open: a guest write into a bank the snapshot references that has left the
engines' view and is no longer write-trapped.**

**SS3.28 — The remap join removed: st-intro −0.44 ms, mlbis +0.33
unexplained.** Recorded as a trade. **Superseded by SS3.32 — it was a defect,
not a trade.**

**SS3.29 — Phase 3 stowed.** The time is in the 2D/worker architecture and it
does not take point fixes.

**SS3.30 — The refactor's brief: two off-main workers, one per screen.** Three
threads instead of five, ending the oversubscription of five threads on four
A55s. **The constraint stated before any code:** the 3D raster does not
partition by screen — it renders one image and costs **~16.6 ms a frame of its
own work** on gsdd (15.95 spans + 0.63 resolve, measured inline where nothing
overlaps); worker time across all threads totals 24.1 ms. So a literal
per-screen split puts the whole raster on one worker. Two ways out: **(1)**
"one per screen" governs the 2D engines only and the raster keeps its own
split; **(2)** the raster gets cheap enough to sit on one worker — which
**makes Phase 4 a prerequisite rather than a successor**. Supporting evidence
for two workers: on gsdd at 1x bands 0 and 1 run 9.5 ms each and **band 2 is
already idle**.

**SS3.31 — What `gpu-path` already answers.** "NEON raster into GPU dmabufs"
is the landed 1x design, not a new idea (P0.1: import as storage buffers and
as LINEAR images, both PASS, GPU writes visible without `DMA_BUF_IOCTL_SYNC`).
**The GPU present stage is worth ~2.3 ms on Spirit Tracks and ~2.2 on NSMB,
and nothing on Golden Sun**, whose frame is the emulation thread with the
scaler already off it. *Caveat:* that is SDL `work ms`, not headless
`frame ms` — the delta transfers, the absolute does not.

**SS3.32 — The remap join removal reverted: it was wrong, and the gate said
so.** SS3.28 recorded mlbis +0.33 as unexplained and guessed contention. The
guess was wrong and its premise — "gates pass on all eight scenes" — was
false.

| scene | 15db8bb (join present) | 550efeb (join removed) |
|---|---|---|
| dbori | **1.0000 / 1.0000, 1800 exact** | 0.9956 / 0.9727, 1375 exact — FAIL |
| etody | 1.0000 / 0.9998, 1772 exact | 0.9978 / **0.9743**, 1500 exact — FAIL |
| st-intro | 0.9999 / 0.9989, 1950 exact | 0.9851 / **0.9761**, 310 exact — FAIL |

**dbori is one of the gate's named deterministic scenes** and loses 425 exact
frames; the figures reproduced identically across two independent runs. Third
symptom, same commit: **`tests/gpu_test.cpp` had been red since it**, because
`Engine2D::vram()` was repointed to `Gpu::render_vram()` — a snapshot written
only at dispatch — while the test drives `render_line` directly. Every failing
value was 0 or `0x3e0000` = `rgb18(0x7C00)`, the blue backdrop: the
backgrounds drew nothing. **One cause under all three: a job rendering against
a mapping that is not the one its lines need.** SS3.27's principle may still
hold; this implementation did not. Reverted in `15004f8`; Phase 3's net from
that work returns to zero.

**SS3.33 — The GPU present stage merged.** `fdd5363` merges `gpu-path`
through `c96eaff` **only** — the present stage and the P0 Vulkan device layer,
not the compute raster, the triangle path or the intersection census. The cut
is what makes it cheap: all 40 commits give **20 conflict hunks across 8
files**, five of them in `render3d.cpp` against the `VramMap` work; through
`c96eaff` it is **one**, a config-key list in `sdl/main.cpp`, resolved as the
union. **Present is orthogonal to who rasterises** — it takes the finished
layer and does the panel scaling on the GPU — so it is the same win under the
software raster as under any GPU path. Still opt-in.

**Measured on this tree (2026-09-21), device, SDL, `--dual-window`,
`--no-vsync`, one binary, flag on and off, alternating order, three reps.**
`work ms` (emulation + present) is the metric and is only valid with
`--no-vsync`; each run gets its own BIOS copy, because the frontend writes
`firmware.bin.ovr` beside the firmware it is given.

Three scenes, 1600 counted frames each:

| scene | arm | median | p90 | p99 |
|---|---|---|---|---|
| st-intro | on | **15.40** | **18.12** | **19.64** |
| | off | 18.35 | 21.61 | 23.35 |
| gsdd | on | **14.26** | **17.60** | *26.83* |
| | off | 16.66 | 18.27 | 19.77 |
| nsmb | on | **13.04** | **18.53** | **20.65** |
| | off | 15.87 | 20.98 | 22.68 |

Median and p90 improve on all three (−2.4 to −2.9 median). p99 improves on two
and **regresses 7 ms on gsdd**, with max going 28 → 49–75 ms there.

**It is worth more at the tail than at the median** — better than SS3.31's
−2.3 ms. **But it does not close st-intro:** the median lands inside 16.74 ms
and **p90 18.12 and p99 19.64 do not.** Present is necessary, not sufficient.

**And the percentiles are the wrong statistic here, which `frame_report`
already knew.** On gsdd the p99 *regressed* (+7.06) and the max more than
doubled, which read as a tail disaster — until the burst rows were looked at.
Over-budget frames and burst structure, three reps, 1600 frames each:

| scene | over-budget frames | longest burst | steady windows (6–10) |
|---|---|---|---|
| gsdd on | 441/462/460 (**28 %**) | 12/14/22 | **1, 5, 0, 0, 0** |
| gsdd off | 726/716/837 (**47 %**) | 61/27/**211** | 4, 47, 23, 16, 9 |
| st-intro on | 671–694 (**42 %**) | **3/9/9** | flat 80/window |
| st-intro off | 1109–1183 (**72 %**) | 199/**291**/197 | 108–160/window |
| nsmb on | 491/527/540 (**32 %**) | 223–376 | 100/160/131/67 |
| nsmb off | 680–711 (**44 %**) | **607–629** | 152/160/160/160 |

**Present nearly halves the over-budget frames on every scene, and turns
sustained chugging into brief blips** — st-intro's longest burst goes from 291
frames (~5 s) to 3–9; gsdd's steady second half goes from 99 over-budget
frames to 6. Its p99 damage on gsdd is a handful of outliers **inside the
loading phase** (all the over-budget mass sits in windows 1–5 on both arms),
which is the one place a spike is acceptable.

**The lesson for the bar (§0):** a percentile taken over a run that is
one-third loading screen measures the loading screen. **Gate on over-budget
frame count and longest burst**, which `frame_report` already prints, and read
p95 rather than p99 when a single number is wanted. p99 and max stay reported
— they are how the Mali stall would show — but they are not the bar.

**The p99 regression is not the present stage** — and, contrary to what this
section first recorded, **not the Mali stall either; see SS3.38.** Re-run with
the p95 tier added, same binary, same arm, same scene:

| gsdd | median | p90 | **p95** | p99 | max | over-budget | longest burst |
|---|---|---|---|---|---|---|---|
| on rep 1 | 13.68 | 17.65 | **18.05** | **19.04** | 25.3 | 34 % | 28 |
| on rep 2 | 13.72 | 17.67 | **18.14** | **41.58** | **74.1** | 29 % | 21 |
| off rep 1 | 16.96 | 18.42 | 18.76 | 19.78 | 27.1 | 55 % | **432** |
| off rep 2 | 16.76 | 18.59 | 18.97 | 20.29 | 26.8 | 51 % | 91 |

The two on-reps differ by **22 ms at p99 and 49 ms at max** with nothing else
changed. This was read as the per-run bimodality `gpu-path` recorded for the
Mali stall; **SS3.38 shows it is a periodic emulation-thread catch-up on the
title screen instead.** On the clean rep, present is better than either
off-rep at *every* percentile including p99. **So present wins at p95 in every
run, and at p99 whenever the stall does not fire.**

This is the case for the tier: a 0.2–0.95 s hold is 12–57 frames of 1600, so
it lands at p99 and usually not at p95. **p95 is robust to one intermittent
stall while still catching a sustained problem** — the off arm's 432-frame
burst shows at every statistic.

*Also visible:* **frame #723 is the worst frame in three of four runs, on both
arms.** A spike that appears identically in both arms is the content, not the
change — which is the discriminator that separates a legitimate cart-load
spike from a defect.

*Not yet in this tree:* the stall mitigation (`emu.gpu_irq_avoid`,
`ds::avoid_cpus()`) arrived in `gpu-path` at `9781507`, **after** the
`c96eaff` cut, and is gated on `video.gpu_raster` anyway — so present-only
would not get it even if merged. **That is the one thing to fix before
shipping present as the default.**

**SS3.37 — The GPU-interrupt mitigation does not pay for the present stage.**
`9781507`'s `ds::gpu_irq_cpus()` / `ds::avoid_cpus()` cherry-picked, and its
gate widened from `video.gpu_raster` to include `video.gpu_present`. Proven to
fire (`gpu irq avoid: off cpu 0 (GPU interrupts), on 1,2,3`) against a control
binary from the same tree that only lacks the widened gate; all three GPU
interrupts are serviced on CPU 0. gsdd, five reps each:

| | median | p90 | **p95** | p99 | **max** | over-budget | longest burst |
|---|---|---|---|---|---|---|---|
| mitigation on (3 cores) | 13.9 | **24.7** | **25.3** | 26.5 | **28.8–33.5** | 31 % | 15–31 |
| mitigation off (4 cores) | 13.6 | **17.7** | **18.2** | 23.8 | **36.8–79.6** | 30 % | 11–27 |

**It works and it still loses.** Every unmitigated run carried a 36.8–79.6 ms
frame and no mitigated run exceeded 33.5 — the stall is real and this removes
it. But on four cores the core it costs is worth more: **p95 regresses
7.1 ms** while the over-budget count and the longest burst are unchanged, so
by §0's bar it is a loss. Both effects land in the loading windows (both arms
are near-perfect in windows 6–10), which is where a spike is tolerable.

**Reverted to the raster-only gate**, with the measurement in the code comment
so it is not re-tried blind. The infrastructure is kept: the GPU raster has
the tiler that actually faults, so it is a different trade.

*Cheaper mitigations not yet tried*, if the outlier ever needs removing
without a core: `emu.realtime=off` (gpu-path measured 0 stalls, costs
1.5–2 ms of median everywhere), and lowering RT bandwidth so the kbase fault
worker on CPU 0 gets a slot without the process leaving the core.

**SS3.38 — The gsdd tail is a periodic catch-up on the TITLE SCREEN, and it is
not the Mali stall.** SS3.33 and SS3.37 both attributed gsdd's 37-80 ms
outliers to the GPU. **That attribution is wrong**, and it was reached by
matching a bimodality pattern from `gpu-path` without checking which slice the
time was in.

*What the frames actually show* (`DS_HITCH_PNG=<ms>`, added for this: it writes
the screen for any frame over the threshold, because a percentile says a run
stutters and a window histogram says roughly where, but neither says what the
player was looking at). **Every captured hitch is the animated title screen** —
the Golden Sun logo over the live sunrise with the pulsing "Tap the Touch
Screen" prompt. Not a loading screen. So the "it sits in the loading windows
where a spike is tolerable" reasoning in SS3.33 does not hold for this scene.

*Where the time is.* `frame_ms` (emulation) and `work_ms` (emulation + present)
name the same frame with almost the same value on every spike — 52.17/53.80,
59.29/61.08, 68.79/69.95. **~97 % of the spike is inside emulation**, so it is
not a fence wait.

*What it is not.* The `aw88166` amplifier driver fills the kernel log with PLL
lock failures in ~27 ms retry loops, which looks like a perfect culprit and is
not one: across three runs the one with **zero** such messages had the **worst**
spike (max 76.7 ms) and the one with 23 had the mildest (56.2). Nor is it the
limiter (the period survives auto / 60 / off), the internal audio buffer
(50 ms and 100 ms show no trend), or the device period.

*What it is.* A **periodic, deadline-driven catch-up in the emulation thread**:

* periodic at **~53-54 frames**, autocorrelation 0.575 at lag 54 with a
  secondary at 101;
* **gone entirely at `--speed 50`** (max 25.6 ms, zero frames over 28) — fixed
  emulated work would still cost its 40 ms with twice the wall-clock slack, so
  this is something forced only when the frame is near its deadline;
* suppressed by `--quantum 128` (peak `cpu arm9` 39.9 → 13.3, at the cost of a
  25.8 ms median);
* **halved by `--no-audio`** (max 57.8 → 30.0, p99 24.5 → 19.2) with the median
  and p95 unchanged, so the audio pump amplifies it but is not all of it —
  audio-off still shows `spu 17.2`, `cpu arm9 28.3`, `dma 17.8` spikes;
* and **the stage that carries it varies** (`cpu arm9` 39.9, `spu` 31.6, `dma`
  32.3), which is a backlog being worked off wherever the scope happens to be
  open, not genuine load in one row.

**So it belongs with Phase 3e (scheduler granularity) and 3f (SPU), not the
render path.** `Audio::pump` drains the SPU inline on the emulation thread
(`while ((n = nds.spu.take(buf, 2048)) != 0)`) with a drop path when the queue
is over target, which is the shape of the amplification.

*One cheap win found on the way:* **`--limiter 60` beats the default `auto`** —
24.2 % of frames over budget against 31.3 %, p95 17.81 against 19.31 — and
`--limiter off` is much worse (p99 64). Worth taking on its own.

**SS3.34 — What the target devices actually expose.** Probed over ssh. The
present stage requests **Vulkan 1.1** (`vk_device.cpp` `apiVersion`, shaders
built `--target-env vulkan1.1`); the version was never the portability
blocker.

| device | arch / kernel | Vulkan | GLES | EGL dmabuf import | DRM | heap |
|---|---|---|---|---|---|---|
| rgdsplus / rgds (main) | aarch64, 7.0 | driver **1.3.303**; all four required extensions present | ≥3.x | — | `card0` | `dma_heap` |
| rg35xxsp (mid) | aarch64, 4.9 | **none installed** (Bifrost HW is capable) | **3.2** | **yes** | none | ION |
| a30 (floor) | armv7, **3.4** | **none** | **2.0**, Utgard | **no** | none | ION |

**The cliff is not a Vulkan version** — it is between a modern kernel with
DRM + dma-heap + a Vulkan driver and a vendor BSP with none of them. The
present stage's premise is zero-copy, so **no version of it can run on the
A30**, which already falls back to `FbdevOut` (the CPU scanline tier built for
exactly that class). `GpuPresent::open()` declines cleanly when the entry
points are missing, so this is a feature the floor does not get, not a bug.
**`rg35xxsp` is the interesting one**: GLES 3.2 *and* `EGL_EXT_image_dma_buf_import`
means a GLES present could be zero-copy there too, and GLES 3.1 compute would
port `present.comp` almost directly — but its hardware is Bifrost with
`mali_kbase` loaded and no ICD installed, so **installing a Vulkan ICD may
cover the mid tier for near-zero engineering.** Probe that before writing a
second backend. *Caveat:* `nm` does not exist on these busybox systems, so
symbol-based capability checks return false negatives; the GLES levels above
come from driver strings.

**SS3.35 — What the pipeline hands the raster, and why an upload costs what it
does.** Driving the GPU costs **~1 ms a frame of emulation thread** on the
heavy scenes — far from the "~0.03 ms" the `GpuStats` comment assumes.
Measured on the device, `--gpu-raster`, `DS_GPU_THREAD=0`, 1800 frames, 2 reps;
`conversion = upload − submit` (an existing "of which" pair — never add them):

| scene | upload | of which submit | conversion | polys/frame | µs/poly |
|---|---|---|---|---|---|
| mlbis | 1.040 | 0.377 | 0.663 | 712 | 0.93 |
| gsdd | 1.003 | 0.384 | 0.619 | 637 | 0.97 |
| sm64 | 0.871 | 0.432 | 0.439 | 563 | 0.78 |
| st-intro | 0.807 | 0.405 | 0.402 | 385 | 1.04 |
| dbori | 0.743 | 0.393 | 0.350 | 336 | 1.04 |
| etody | 0.516 | 0.379 | 0.137 | 134 | 1.02 |

Conversion tracks **polygon count** at ~0.95 µs each across a 5× range, and is
independent of span rows (4–7 K everywhere) and texels (0–2 K). **`submit` is
a flat ~0.38–0.43 ms regardless of scene** — fixed command-recording and queue
overhead that no CPU-side work touches. Attributed by ablation (the no-flag arm
reproduced the unpatched binary first):

| part | mlbis | gsdd |
|---|---|---|
| texture residency (`unordered_map` + memcpy) | 33 % | 22 % |
| storing into mapped Vulkan memory (vs heap) | 15 % | 11 % |
| span-row fill | 4 % | 5 % |
| tile bounding-box loop | 0 % | 1 % |
| residual: vertex gather + header/vertex writes | 48 % | 62 % |

**So NEON is not the lever** — the write-combined penalty it would target is
0.07–0.10 ms. The largest named piece is a **hash lookup per textured
polygon**, a data-structure fix.

**The residual is self-inflicted, and that is the actionable part.** `Vertex`
is **56 B of which the raster reads 24** (`sx`, `sy`, `fcol`, `tex` — there
are *zero* uses of `pos[4]`, `col[3]`, `clipped`, `oc` in `render3d.cpp`), and
`z`/`w` live on `Polygon` as `z[10]`/`w[10]` because the DS normalises W **per
polygon**. So a GPU vertex record is a *join* across two structures. But
`finish_polygon` already walks every vertex twice and holds all six fields
live in its second loop — **emitting a GPU-shaped record there would delete
the conversion pass entirely.** Blast radius: `Polygon`'s layout is
load-bearing for the identical-frame dedup (`memcmp` over `vtop..sort_key`)
and for save states.

**SS3.36 — What a frame-at-a-time 3D layer would cost, and the dual-screen
trick.** Censused per scene (`DS_DEFER_CENSUS`, host):

| scene | capture on | screen-swap toggles | both | mid-frame 3D reg writes |
|---|---|---|---|---|
| st-intro | 2170 (90.4 %) | **2170** | 2169 | 0.4 % of frames |
| gsdd | 1276 (53.1 %) | **1277** | 1275 | 0.7 % |
| dbori | 922 (51.2 %) | **921** | 920 | 1.0 % |
| etody | 825 (45.8 %) | **824** | 824 | 2.0 % |
| mlbis / meteos / artacd | 0 % | 1–3 | 0 | 0.2–1.1 % |
| **sm64** | 0.2 % | 2 | 1 | **26.0 %** |

**Capture count and screen-swap count are the same number** on the four heavy
scenes — the documented **dual-screen 3D trick** (one 3D engine alternating
between screens, engine B displaying the captured previous image). Three
consequences:

* **st-intro is not 60 Hz 3D.** Its ~2255 `SWAP_BUFFERS` in 2400 frames are
  the engine alternating scenes; per-screen 3D is 30 Hz.
* **A naive one-frame 3D deferral breaks those four scenes outright** — the
  capture would grab the other screen's scene. Not staleness, a scene swap.
* **The light scenes have no capture at all**, so deferral is free on exactly
  the games worth upscaling. (`sm64` needs a per-frame fallback for its 26 %
  mid-frame register writes; everything else is under 2 %.)

**Do not conflate two changes.** *Frame-at-a-time* (render the whole layer at
`SWAP_BUFFERS` rather than per band) is compatible with the trick but buys
only the VBlank window — lines 192→262, **~4.45 ms** against the ~3 ms
available now, which matches P2b's measured fence wait. *One-frame-deferred*
is where the full ~16.7 ms comes from, and it requires deferring the whole
pipeline consistently, capture and screen-B display included.

---

## 5. Closed routes — do not re-litigate

| route | why it is closed | where |
|---|---|---|
| `--timing-oc` promoted to default | worth −1.21 ms at most, and it silently enables the geometry worker | SS3.4 |
| The geometry worker | degrades p99 on every scene; on etody total +1.5 s | SS3.4 |
| Phase 1c, the batched transform | +0.65 ms on gsdd before the kernel saves anything | SS3.5 |
| The per-access cost model | 0.2–0.4 ms, not 3.5 | SS3.2 |
| `DS_JIT_FASTCOST` | +3.16 % *slower* than the exact model | SS3.2 |
| `const_nd` as a flat cost model | worse on all four scenes | SS3.16 |
| Row 1.26, known-constant tracking | `e51c6fd` | SS3.16 |
| Hoisting `cart_catch_up_slow`'s guard | flat to 1.3 % worse | SS3.3 |
| `DS_2D_SPLIT` | null, and inert by construction | SS3.22 |
| The remap join removal | wrong picture on three scenes | SS3.32 |
| GPU **span quads** | cost is per PRIMITIVE; 4–12 K tiny quads is what a tiler dislikes | `gpu-path` P0.3 |
| GPU **compute raster at S=2/3** | cost scales with pixels; gsdd 18.9 ms at S=2 | `gpu-path` P0.2 |
| GPU **compute raster at 1x** | not throughput — **latency**: dispatch at line 215, needed at line 0 ~3 ms later; fence waits 5.3–8.0 ms | `gpu-path` P2b |
| GPU **triangle path at 1x** (as the exact path) | equal cost (15.9 vs 15.6 ms) and loses on picture; the hardware rasteriser cannot make the partially covered edge pixels DS AA needs | `gpu-path` 1x verdict |
| `DS_FF_SPANCULL` on the fast path | undercuts coplanar decals; `gl_FragDepth` costs early-Z (0.08 → 1.39 ms) | `gpu-path` |
| Engine B's scaler on the LineWorker | the worker is already the long pole | `gpu-path` P1a |
| NEON-ing the GPU upload | the memory-type penalty is only 0.07–0.10 ms | SS3.35 |
| Porting P2c's composite as-is | it consumes `Gpu::frame_hires`, a VkBuffer from the GPU raster; under the software raster it never fires | §0 |

**Two rejections that were themselves wrong**, kept as warnings: the
mode-switch core (SS3.10, rejected on a threaded census, reversed) and `LDM ^`
(SS3.13, rejected twice by reasoning from the interpreter).

---

## 6. Instruments: how to trust a measurement here

* **Device frame time is the arbiter, and nothing else is.** Three reps,
  alternating order, against a binary built from the same tree with the same
  flags — a `-O3` build against a `-O2` one produced two published figures
  that both had to be retracted (SS3.5).
* **Instruction counts are only valid run serially** (`DS_R3D_THREADS=0
  DS_2D_THREAD=0`). A threaded pair cannot resolve under ~4 % and one rejected
  a working change (SS3.12).
* **Count what a change removes in TIME, not in events** (SS3.11, SS3.13).
  Sort a fallback census by **block shape**: block-ending fallbacks are
  understated, mid-block ones overstated (SS3.15).
* **A `prof::Scope` being open does not mean the time is attributed.**
  `Gpu::on_hblank` opens `GPU_LINE` and then *subtracts* `render_ranges` back
  out, so anything in that interval without its own scope lands in `untimed`.
  Check whether a stage *subtracts* a region, not just whether it encloses it.
* **Stages with no site read 0.00 forever.** `GX_RUN` and `GX_JOIN` had none
  since Phase 1, and the zero was twice written up as the work being deleted.
  `grep` for `DS_PROF(<STAGE>)` before citing a zero.
* **"Of which" rows must be excluded from sums** (`JIT_TX`, `GX_RUN`,
  `W2D_JOIN`, `submit_ns`). Adding them is why `untimed` once read negative.
* **Re-run a gate rather than trusting a claim that it passed**, and against
  the commit's **parent** — a tree-wide pass says nothing about which commit
  moved what. Run `ctest`; it takes four seconds and was red for three commits
  (SS3.32).
* **Prove a mutation applied before believing a negative result.** Do source
  mutations in Python with `assert s.count(old) == 1`, never `sed`; check the
  build's exit status.
* **Never rebuild a binary a measurement is running against.** A background
  gate was invalidated this way on 2026-09-21; copy the binary aside first.

### The warning that still stands

**The golden gate configuration is serial** (`DS_R3D_THREADS=0
DS_2D_THREAD=0`), so **it cannot exercise threading at all.** It passed every
stage of the `VramMap` work including the one with a live data race, which
surfaced as a performance regression and then as SS3.32's wrong picture. **A
thread-topology refactor needs the self-consistency runner audited before it
needs anything else** — and that runner has never been audited. Twice in this
rework a gate has been found reporting instead of judging (SS3.17).

Measurement traps that are not about instruments: the x86 host has **no JIT
and no NEON kernels**; hardware AA is **on by default** in `dsperate-headless`;
a scene replayed without its `--save` is a **different workload**; `gsdd` and
`st-intro` are **boot runs**, not replays, and their first ~700 frames are
black; and **frame time alone cannot tell a real saving from the game drawing
less** — always carry the swap count.

---

## 7. The arithmetic

Phase 1 delivered −0.30 to −1.49 ms. Phase 2 delivered −0.02 to −1.13 ms.
Phase 3 is stowed at 3b with its net from the remap work back to zero
(SS3.32). Against that, four of six scenes clear 16.74 ms with 4.4–11.7 ms of
margin, and the two that do not need §0's steps 1 and 2 — a landed −2.3 ms
and a 4.86 ms row that no phase has claimed.

**The project lands inside budget even if Phase 3 underdelivers**, which is
what the phase ordering was chosen for. What it does *not* yet have is the
margin for 2–4x, and that is Phase 4's alone.

---

## 8. Risks, in order

1. **A phase's premise evaporates when measured.** It has happened to Phase 1
   (SS3.4), Phase 2 (SS3.2/SS3.3) and Phase 3 (SS3.17/SS3.18). The mitigation
   is in the plan's shape: census before commit, and rescope in writing.
2. **A gate that reports instead of judging.** Twice found (SS3.17), and the
   self-consistency runner is still unaudited.
3. **A thread hazard that no gate can see**, because the golden configuration
   is serial. SS3.32 is the worked example.
4. **The most code for the least median gain** — the SPU row is the standing
   candidate, which is why it is last and split.
5. **Compatibility drift under the JIT's banked-register work**, whose failure
   mode is a rare crash rather than a clean gate failure. The sweep is the
   real gate, not the four scenes.
