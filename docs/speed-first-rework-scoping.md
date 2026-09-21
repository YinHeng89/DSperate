# The speed-first rework — scoping

2026-09-20. What it takes to move DSperate's goal from *"melonDS accuracy with
a portion of DraStic's speed"* to *"as fast or faster than DraStic, and more
stable"*, and what has to be given up to get there.

Decisions taken before this document was written, and assumed throughout:

* **Speed-first by default.** The rendered picture needs to be *plausible at
  1x*, not byte-identical to melonDS. Everything that is not the picture —
  timing models, DMA granularity, SPU granularity, geometry ordering — may be
  as inexact as speed requires.
* **RK3566 (the RG DS, four Cortex-A55 at 1.992 GHz) is the target.** The
  H700 and the ARMv7 A30 are best-effort; they must keep building and keep
  running, but they do not shape the design.
* **Stability means:** no crashes, no hangs, no thread hazards; steady frame
  delivery and no audio gaps; and broad *retail* compatibility. Romhacks and
  shovelware are nice-to-haves, not a bar.
* Nice features (dual window, multiple render buffers, the layouts, the
  overlays) are worth keeping, but rebuilding them on the new core is
  acceptable.

---

## 1. Where we actually are

Two sets of numbers, both already in the tree, define the whole problem.

**DraStic**, `docs/techniques/00-method-and-measurements.md`, same device:

| scene | ms/frame |
|---|---|
| Super Mario 64 DS, recompiler | **2.98** |
| Super Mario 64 DS, interpreter | 10.64 |
| Meteos | **0.75** |

and its cycle split: 3D raster + resolve 32.6 %, 2D engines 25.2 %, translated
guest code 14.7 %, JIT support 12.0 %, 3D geometry 2.7 %, scheduler 1.7 %,
SPU 1.1 %.

**DSperate**, `docs/frame-profile-2026-09-16.md`, RG DS Plus, SDL,
`--dual-window`, median (p90):

| scene | exact | `--timing-oc` |
|---|---|---|
| NSMB (2400 fr) | 18.7-18.9 (21.2) | 15.2-15.6 (16.9-17.1) |
| Golden Sun | 16.9-17.3 (18.4-18.9) | 14.3-14.6 (15.8-16.2) |
| Spirit Tracks | 21.3 (23.3) | 17.0-17.2 (18.7-19.3) |
| Etrian Odyssey | 9.3 (10.0) | 8.3 (9.4) |

A DS frame is **16.74 ms**. Three of those four scenes miss it today.

### The frame budget, and who spends it

The critical path is the **emulation thread**, and it is not the rasteriser:
under `DS_PROFILE=1` the `3d band wait` is 0.000-0.001 ms on every scene. The
three band workers burn 9-27 ms of worker time a frame and the emulation
thread waits for none of it. On a four-core device that work is free; it is
also why a GPU raster measured *slower*.

Emulation thread, NSMB, typical frame:

| stage | ms | what it is |
|---|---|---|
| cpu arm9 | 6.7 | the recompiler and its helpers |
| gx geometry | 3.4 | pricing every GX command inline to keep the FIFO level exact |
| cpu arm7 | 1.5 | |
| dma | 1.5 | per-unit burst timing through the bus |
| spu | 0.9 | ~34 batched events, channels still stepped per sample |
| slice loop | 0.7 | ~1,090 slices a frame |
| scanline hooks | 0.7 | minus the draws |
| 2D on this thread | 0.7 | engine A, one batch |
| gx vblank | 0.4 | swap, sort |

Roughly 16.5 ms of the 18.7 accounted for. **To reach DraStic we need to
remove about two thirds of this, and three of the nine rows are pure accuracy
tax.**

### The instruction census says the same thing

`qemu` non-spin instructions per frame after all the exact-model optimisation
work: Golden Sun ~42 M, Dragon Ball Origins ~22 M. At ~1 IPC on a 2 GHz A55
that is 21 ms and 11 ms. DraStic's 2.98 ms frame is on the order of 6 M
instructions. **This is a 3-4x instruction reduction, not a tuning exercise.**
Nothing in `docs/techniques/06-implementation-checklist.md` that is still open
is worth more than a few percent; every remaining `no`/`partial` row that
*is* worth multiple milliseconds is one the project deliberately rejected on
accuracy grounds. That is the list we are now reopening.

---

## 2. The strategy, in one paragraph

Take the four rows the checklist marks `no` **because we chose hardware-exact
behaviour** — geometry log-replay (4.17/4.18), whole-transfer DMA (4.8/4.9),
per-frame SPU (5.1 and its inexact half, 5.4/5.6/5.9/5.11), and the
melonDS-exact rasteriser — and implement DraStic's model instead. Then delete
the exact path rather than keeping it behind a flag, and move verification
from "byte-identical frames" to a reference *binary* plus perceptual and
compatibility gates.

### The dual-path tax is itself a cost

DSperate currently carries a per-subsystem accuracy switch nearly everywhere:
`exec_timed_` / `untimed_` / `no_fifo_` / `worker_on_` in `Gpu3D`, lazy vs
per-line 2D (`DS_2D_LAZY`, `DS_2D_THREAD`, `DS_2D_LAZY_CAPTURE`),
`DS_SPU_BATCH`, `LOCKSTEP_QUANTUM` vs `EVENT_BOUND_QUANTUM`, `DS_GX_THREAD`
0/1/2, `DS_R3D_ADAPT`, `DS_IDLE_SKIP`, the `--cpu-oc` / `--cpu-uc` /
`--timing-oc` / `--fast-load` tier. Each is a branch in a hot path, a state
the tests have to cover, a combination that can deadlock, and a reason a bug
report is not reproducible. The frame-profile doc already records one such
trap — *"worker mode as shipped does not reproduce run to run; exactness
checks in that path need `DS_GX_THREAD=2`."*

**Collapsing these to one path is a speed win, a stability win and a velocity
win at the same time.** It is the single most important structural decision in
this plan, and it is what makes the rest affordable.

### What replaces the exactness gate

Today's gate is `tools/scene_hashes.sh` — per-frame SHA-1 of both
framebuffers, identical across builds and modes. That gate is exactly what
forbids everything in §3, so it cannot survive as written. It is replaced by
three weaker gates that together are enough:

1. **A pinned reference binary.** Tag `main` today as `exact-reference`, build
   `dsperate-ref` from it once, and keep it as a frozen artifact the
   comparison tools run against. It never needs to build again from the
   evolving tree, so the exact model costs nothing to maintain.
2. **A perceptual frame gate.** `tools/compare_frames.py` grows a tolerance
   mode: per-frame PSNR/SSIM against `dsperate-ref`'s dump, with per-scene
   floors, plus a hard fail on structural faults (a wholly black screen, a
   frozen framebuffer, a swapped screen, a missing layer). This is what
   "plausible at 1x" means operationally, and it has to be written before
   anything in §3 lands.
3. **Self-consistency.** The same build replaying the same `.dsin` twice must
   produce identical frames. This is cheap, it is what catches thread races,
   and it is *stronger* than today's gate in the one dimension that matters
   for stability — today's worker path does not pass it.
4. **A cadence census** (`tools/cadence_census.sh`). The picture gate compares
   *what* was drawn and is completely blind to *when*. The failure mode Phase 1
   is most likely to cause is a timing model that makes a game draw fewer
   frames, or pace itself differently, while every frame it does draw is
   correct — and the frame-profile doc already warns that frame time alone
   cannot tell that apart from a real saving. Swap counts, no-swap VBlanks and
   GXSTAT read counts are diffed per scene. **This was added after the picture
   gate was built and measured; see §3.1 for why.**

Plus a **compatibility sweep**: a scripted boot-and-play over a retail set
(target: 50+ titles, 2-3 minutes each from a recorded input scene), checked
for hangs, black screens, and gross audio failure. Retail only — that is the
bar the user set.

---

## 3. The phases

Each phase names its target, the files it touches, its exit gate, and the
milliseconds it should return on NSMB. The ordering is by **measured payoff
per unit of risk**, not by subsystem tidiness.

### Phase 0 — The bar, the reference, and the census (week 1)

Nothing else can be trusted until this exists.

* Tag and build `dsperate-ref` from today's `main`. Record its frame dumps,
  audio dumps and per-frame timings for all recorded scenes into a fixtures
  directory.
* Write the perceptual gate (`compare_frames.py --tolerant`), the structural
  fault checks, and the self-consistency runner.
* Build the compatibility sweep harness on top of `dsperate-headless`
  (`--replay`, `--frames`, watchdog) with a pass/fail summary. Record input
  scenes for the retail set.
* Extend `DS_PROFILE` so the nine emulation-thread rows above are a single
  machine-readable line per run — every later phase's claim is a diff of that
  line, paired and both orders, as the existing docs already require.
* **Split the 6.7 ms ARM9 row before Phase 2 is committed to.** Three
  instruments for this are already built and should be used rather than
  rebuilt: `DS_JIT_COSTPROBE` emits the cost lookup and the charge a second
  time (to a dead scratch, so the emulated cycle count is unchanged), which
  prices the timing model by differencing — that is the ceiling on the lever
  in Phase 2's first bullet, measurable *before* any of it is written;
  `DS_JIT_FASTCOST` gives the floor; and `fastmem_census` already reports
  direct / walked-after-rewrite / left-the-view accesses per frame per CPU,
  with the worst sites named, which bounds how much of the row is memory at
  all.
* **Freeze a "no regressions" baseline table** in this doc.

*Exit gate:* the gate scripts pass on `main` against `dsperate-ref` trivially,
and the compatibility sweep produces a clean baseline. *Expected gain: 0 ms.*

### 3.1 What P0 has already measured, and what it changed

Built and validated 2026-09-20 on the host build against the research ROM set
(`dsperate-research/binary/games-bench`, `games-bugtest`, `games-sweep`,
`real-bios`):

* `tools/golden_dump.sh` — the golden reference configuration: `--interp`,
  `--quantum 128`, `DS_R3D_THREADS=0`, `DS_2D_THREAD=0`, `DS_2D_LAZY=0`,
  `DS_SPU_BATCH=1`, hardware AA on (which is the headless default; `--aa` is
  the SDL spelling and does not exist in the harness).
* `tools/perceptual_gate.py` — SSIM/PSNR with per-scene floors, plus the four
  structural faults, over a timing-tolerant alignment window.
* `tools/perceptual_gate_selftest.py` — synthetic dumps with one known fault
  each. All seven checks pass, including the two false-positive guards. The
  gate is load-bearing and nothing else checks it, so this runs in CI.
* `tools/cadence_census.sh` — the swap/GXSTAT counters.

**Three findings, each of which changed something.**

*The alignment window is not a nicety, it is what makes the gate usable.*
sm64, golden against `--timing-oc`, 600 frames:

| | ssim mean | p01 | min | exact frames | faults |
|---|---|---|---|---|---|
| strict (`--align 0`) | 0.9912 | 0.9532 | 0.3315 | 340/600 | **blank ×1, region ×2** |
| aligned (`--align 2`) | 0.9999 | 0.9960 | 0.9760 | 587/600 | none |

The strict run's structural faults are entirely timing skew at scene
transitions — a candidate one or two frames ahead reads as a blank screen or a
dropped layer. Without the window, Phase 1 would trip spurious faults on every
transition. These aligned figures are the **first real floor candidates**;
floors invented before this measurement would have been worthless.

*Today's default build is byte-identical to golden*, so it calibrates nothing.
meteos (120 fr), sm64 and dbori (600 fr each): 100 % exact, zero realignment.
That confirms lazy 2D and the worker paths are as exact as the checklist
claims, and it means the floors have to come from an inexact configuration —
which is why `--timing-oc` was used above.

*The documented Phase 1 casualty no longer reproduces.* Dragon Ball Origins is
the recorded `--timing-oc` casualty, and under the headless replay it is
**byte-identical at 1800 frames**, with swap counts matching exactly (172/172)
and GXSTAT reads matching (542150/542150). The reason is not that the gate
missed it: `--timing-oc` stopped being DraStic's model in `149bb5e`, which
synthesises a safe `GXSTAT` (empty FIFO, and the swap busy bit held past the
swap) rather than reporting what a log replay would say. That landed after the
frame-profile doc recorded the desync. See Phase 1 — this synthesis is a
DSperate advantage to preserve, not an accuracy tax to remove.

Separately, cadence still cannot be settled headlessly: the frame-profile
doc's own measurement trap notes the harness boots differently and runs that
intro at 30 Hz against SDL's 60 Hz. Two consequences, both now in the plan:

1. The cadence census (gate 4 above) exists because of this. It catches what
   the picture gate cannot: on sm64, `--timing-oc` moves swaps 504 → 505 and
   GXSTAT reads 16312 → 11364.
2. **A cadence claim about a game as played must be checked in the SDL
   frontend.** The headless gates bound a regression; they do not settle one.
   Phase 1 needs an SDL-frontend cadence check that does not exist yet, and
   `dbori`'s intro is its first test case. This is unbudgeted work discovered
   by building P0 — which is the argument for having built it first.

### 3.2 The cost model, measured — and Phase 2's target with it

Measured 2026-09-20, `qemu-aarch64` 9.1.0 with the `libinsn` TCG plugin
against the AArch64 cross build (`jit+neon`), 200 frames, `DS_R3D_THREADS=0`
`DS_2D_THREAD=0` so the count is single-threaded and free of spin noise.
`DS_JIT_COSTPROBE` emits the per-access cost model a second time into a dead
scratch: the emulated cycle count is unchanged, so the delta is the model's
own cost and nothing else.

| sm64 | insns | delta | share | per frame |
|---|---|---|---|---|
| baseline | 1,333,161,382 | | | 6,665,807 |
| `PART=1` (the table lookup) | 1,384,310,100 | +51,148,718 | +3.84 % | 255,744 |
| `PART=2` (the charge arithmetic) | 1,355,833,188 | +22,671,806 | +1.70 % | 113,359 |
| `PART=3` (both) | 1,406,984,441 | +73,823,059 | **+5.54 %** | 369,115 |

Parts 1 and 2 sum to 73,820,524 against part 3's 73,823,059 — additive to
0.003 %, which is the check that the probe measures what it claims. meteos,
the same run: +24,220,890, **+2.33 %**, 121,104 insn/frame. The share tracks
memory-access density rather than rendering load, which is why the lighter
scene scores lower.

**So the whole per-access cost model is 2.3-5.5 % of emulated instructions.**
At the ~0.5 IPC the device implies (sm64's 6.67 M insn/frame against its
measured 7.01 ms headless), removing it entirely is worth roughly **0.2-0.4 ms
a frame, not the 3.5 ms Phase 2 was scoped for.** One caveat pushes the other
way and cannot be settled here: the lookup is a *dependent load* through
`R_TIM`, and on an in-order A55 a dependent load can cost far more than its
instruction count — the whole of `docs/techniques` insists on exactly this
point. The device number could therefore be higher than the instruction share
suggests. It is not going to be ten times higher.

**Two things this changes.**

1. **Phase 2 loses its best-founded lever**, which is risk 1 materialising
   exactly as written. The remaining levers — the four open JIT rows (1.5,
   1.12, 1.23, 1.26), the fallback rate, LDM/STM page crossings, wider idle
   skipping — are all still unmeasured, and none of them has an argument
   behind it as strong as the one the cost model just lost. Phase 2 should be
   rescoped to "census first, commit second": run `perf` on the device against
   the ARM9 row and find where the 6.7 ms actually is, before planning weeks
   of work against it. If §4's arithmetic has to survive on Phases 1 and 3
   alone it lands near 12 ms, which is inside the 16.74 ms budget but without
   the margin the plan assumed.
2. **`DS_JIT_FASTCOST` is not the floor**, and the earlier draft of this
   document was wrong to call it one. Measured, it is +3.16 % *slower* than
   the exact model. Reading the emitter says why: `fastcost` always emits
   `add_imm` + `sub_reg`, two instructions, while the exact path's common ARM9
   case (`numC <= 1`) emits a single `sub_reg`. It is an alternative inexact
   model that happens to cost more in the common case, not a cheaper one. The
   real floor is `const_nd >= 0`, which emits no cost code at the site at all,
   and the probe above already bounds what reaching it is worth.

### 3.3 Phase 2's census, on the device — the lever is not where the plan put it

RG DS Plus, `perf -F 499`, 900 frames, filtered to the emulation thread. Full
tables and the device stage baseline are in `fixtures/device/`.

**Translated guest code is ~35 % of the emulation thread** (36 % on dbori,
34 % on mlbis). That is Phase 2's floor and nothing in the plan's queued rows
touches it — the four open JIT rows (1.5, 1.12, 1.23, 1.26) are all about the
machinery *around* translated code.

**The `Io::` read path is the largest identified non-JIT cost on the
emulation thread**: ~24 % on dbori, ~11 % on mlbis, with
`Io::cart_catch_up_slow` the hottest C++ symbol on *both* scenes (10.3 % and
3.0 % of the process). This was not on the plan's list at all, and it is not
a DraStic checklist row. It is now Phase 2's first candidate.

**`ds_slice_next` (2.5 % / 1.4 %) and `interp::ldm_stm` (1.7 % / 0.5 %)** are
the next two: the slice loop, and the JIT falling back to the interpreter for
block transfers that cross a 2 KB page (row 1.28). Both were on the list; both
are small.

So Phase 2's revised shape is: the cost model is worth 0.2-0.4 ms (§3.2), the
I/O read path is worth more than that and was unplanned, and roughly a third
of the row is translated code that only better block quality would move.
**The 3.5 ms target is not reachable from the rows the plan had.**

*One hypothesis tested and rejected already* — hoisting
`cart_catch_up_slow`'s `event_armed || late` guard into its inline caller.
Provably identical semantics, byte-identical on five scenes, and on the device
it measured **flat to 1.3 % worse** with swap counts unchanged. The samples
are in the function's real work, not its early return. The lesson is
methodological and applies to every later patch: **a `perf` share says where
the time is, not why**, and `perf annotate` is what separates them. ROCKNIX
ships no `objdump`, so annotate off-device against the cross binary.

### 3.4 Phase 1's premise, measured on the device — the model is the cheap half

Phase 1 was budgeted at -3.0 ms on the strength of a claim that `--timing-oc`
"is worth 3-4 ms a frame on every 3D scene". Measured on the RG DS Plus on
2026-09-20, that claim is wrong twice over: the flag is worth far less than
3 ms, and most of what it *is* worth does not come from the geometry model at
all.

Two things made the original number untrustworthy. First, **`gx_geom` does not
measure geometry.** The `GX_RUN` scope wraps only the two `run_to()` calls in
the scheduler slice loop (`scheduler.cpp:559`, `:661`); `gxfifo_write` from a
CPU store is charged to `cpu9`, `gxfifo_dma_burst` to `dma`, a `drain_all()`
from a `GXSTAT` read to whatever scope the read sits under, and the sort and
swap to `gx_vblank`. The device baseline's 1.05-1.50 ms `gx_geom` row is
therefore not Phase 1's before-number and never was. Second, **`--timing-oc`
is three changes, not one**: both frontends wire it to `set_geometry_worker`
as well (`headless/main.cpp:558`, `sdl/main.cpp:2012`), and `worker_activate`
drops the band workers from three to two on a four-core host.

Five arms, three reps, rotating arm order, 1800 frames from 200, `--quantum 0`:

| arm | flags | isolates |
|---|---|---|
| `base` | — | control: inline, FIFO kept, timed |
| `wctl` | `--gx-worker`, `DS_GX_THREAD=1` | the worker as shipped, shape controller deciding |
| `walw` | `--gx-worker`, `DS_GX_THREAD=2` | worker always on; the controller cannot opt out |
| `tinl` | `--timing-oc`, `DS_GX_THREAD=0` | no-FIFO + untimed, inline, bands stay at three |
| `tall` | `--timing-oc`, `DS_GX_THREAD=1` | the shipped combination |

Deltas against `base`, one session:

**median (ms)**

| scene | `wctl` | `walw` | `tinl` | `tall` |
|---|---|---|---|---|
| sm64 | -0.09 | **-1.24** | **-1.21** | -2.11 |
| mlbis | -0.66 | -0.90 | -0.35 | -1.58 |
| dbori | +0.06 | +0.08 | -0.11 | -0.24 |
| etody | -0.05 | -0.07 | -0.05 | -0.12 |

**p99 (ms)**

| scene | `wctl` | `walw` | `tinl` | `tall` |
|---|---|---|---|---|
| sm64 | +1.61 | +2.28 | **+0.27** | +1.07 |
| mlbis | +0.49 | +0.63 | **-0.11** | +0.85 |
| dbori | +1.62 | **+4.03** | **-0.21** | +2.77 |
| etody | +1.49 | +2.59 | +0.57 | +2.36 |

**total wall (s), 1800 frames**

| scene | `wctl` | `walw` | `tinl` | `tall` |
|---|---|---|---|---|
| sm64 | +0.5 | +0.4 | -0.7 | -0.8 |
| mlbis | -0.2 | -0.5 | -0.2 | -0.8 |
| dbori | 0.0 | 0.0 | -0.4 | -1.1 |
| etody | **+0.9** | **+1.5** | 0.0 | +0.4 |

Three conclusions, and they reshape the phase.

**The geometry model is worth about a millisecond, not three.** `tinl` is
no-FIFO plus untimed geometry with nothing else changed — exactly what the
phase's first bullet describes — and it buys -1.21 / -0.35 / -0.11 / -0.07 ms
of median. It is also the only arm that regresses nothing: mean and p90
improve in all four scenes, total improves or is flat in all four, p99
improves in two and moves +0.27 / +0.57 in the others. It is safe, and it is
small.

**The worker does not make the machine faster; it moves work off the critical
thread, and charges p99 for it.** Every arm containing the worker degrades p99
in all four scenes without exception. On sm64 `walw` takes median down 1.24
while mean rises 0.26 and total rises 0.4 s — frames finish sooner on average
while *more work is done to finish them* — and on etody the trade goes fully
negative (total +1.5 s, p90 +4.0). That is the queue, the joins, and the
three-to-two band drop. Given the rework's stability scope puts frame pacing
on the bar, this settles the plan's instruction to delete the worker: it is
not a win being given up, it is a liability.

**The shape controller picks wrong wherever the worker helps at all.** `wctl`
gets -0.09 on sm64 where `walw` gets -1.24, and -0.66 on mlbis against -0.90,
while still paying most of the p99 cost. Nothing here argues for keeping it.

The arithmetic this leaves Phase 1 is therefore harder than the plan assumed,
and should be stated plainly. Deleting the worker gives back the ~0.9 ms
(sm64) and ~1.2 ms (mlbis) of median that `tall` was buying with it. So the
rewritten log, replay loop and batched transform must find roughly **4 ms on
the 3D-heavy titles, not 3** — starting from a measured floor of `tinl`, which
the current structure already reaches.

That floor is the argument for a rewrite rather than a promotion of
`--timing-oc` to the default. The flag changes the model's *semantics* while
leaving its *structure* untouched: each parameter word still becomes an 8-byte
`Entry{u32 param, u8 cmd}`; entries still land in a **512-entry ring** that
must be drained whenever it fills, so there is no frame log, only a small
buffer replayed dozens of times a frame; `drain_all` still replays through
`run_to_slow` with a fake cycle deficit, carrying pipe refill, stall
promotion, busy-bit bookkeeping and the settle; `exec_single` is still the
per-command switch with `add_cycles` and `vtx_cmd_*` wrappers at every arm,
predicated off but still branched; and `submit_vertex` still transforms,
advances strip assembly and calls `submit_polygon` inline. The measured -1.21
is what removing the arithmetic buys while all of that remains. The rest of
the target has to come from removing it.

### Phase 1 — Geometry as a replayed log (weeks 1-2) — target **-4.0 ms**; delivered **-0.30 to -1.49 ms**, closed (§3.5)

The largest single win, and the one whose premise §3.4 has now measured
rather than assumed. The earlier claim that `--timing-oc` "alone is worth
3-4 ms a frame on every 3D scene" does not survive the device: the flag is
worth -2.11 / -1.58 / -0.24 / -0.12 ms of median on sm64 / mlbis / dbori /
etody, and only -1.21 / -0.35 / -0.11 / -0.07 of that is the geometry model
rather than the geometry worker it silently also enables. DraStic's full model
(04 §6, checklist 4.17/4.18) goes further than `--timing-oc` does, and the
gap between them is not the semantics — it is the structure underneath, which
`--timing-oc` leaves in place.

Today `Gpu3D::run_to` executes queued commands after every ARM9 slice and
prices each command to keep the FIFO level exact; the checklist closed this
item "in its exact form" at ~65 instructions per command, ~110 per vertex and
~495 per polygon. DraStic instead **copies command and parameter words into
two flat logs** and returns — then replays the whole log once at VBlank, with
the engine state hot in cache, and runs the vertex transform as a *separate
batched kernel* over an array (which is the only thing that makes a NEON
vertex transform pay; our per-vertex NEON attempt measured 110 → 118
instructions precisely because it was per-vertex).

Work:

* `gxfifo_write` becomes a bounds-checked append to a command log and a
  parameter log. No dispatch, no timing, no FIFO-level arithmetic.
* `process_geometry_commands`: one pass over the logs at VBlank.
* `geometry_transform_vertexes`: batched NEON transform over the vertex array,
  then clip/sort into opaque and translucent lists.
* **Keep our synthesised `GXSTAT`. Do not adopt DraStic's 4.19.** This is the
  one place the plan should *not* follow DraStic, and it is easy to get wrong
  because the checklist marks 4.19 `same`. DraStic answers a `GXSTAT` read by
  replaying the log up to now and reporting what it finds; `--timing-oc`
  instead **synthesises a safe answer**, and that is strictly the better
  contract:
  - `level = no_fifo_ ? 0 : fifo_n_` — the FIFO always reads empty and
    under-half-full (bits 25 and 26 set), so the guest never observes a full
    FIFO, never stalls on one, and cannot be locked out of step by one.
  - `swap_wait_ || now() < swap_busy_until_` holds the busy bit (27) from
    `SWAP_BUFFERS` until 650 cycles past the swap, so a game polling "has my
    swap landed?" still sees the busy → idle edge at a plausible time instead
    of an immediate idle. Without it, a poller can flip panels early or out of
    step.

  `POLYGON_COUNT`, `VERTEX_COUNT` and the matrix read-backs still need a
  drain-on-observation (`drain_all`), and Dragon Ball Origins reads `GXSTAT`
  ~542 k times per 600 frames, so that path must be fast, not merely present.
  The log-replay in this phase is therefore adopted for the **batched vertex
  transform** underneath, not for DraStic's observation semantics on top.
* Synthesise the FIFO half-empty IRQ and GXFIFO DMA from the log's fill level.
* Delete `exec_timed_`, `untimed_`, `no_fifo_`, `set_geometry_worker`,
  `DS_GX_THREAD`, `--gx-worker`, `--timing-oc`.

*The known casualty is stale.* The frame-profile doc records that Dragon Ball
Origins' intro desyncs under `--timing-oc`, and that claim predates the fix:
`721c238` (the doc) and `149bb5e` (the `GXSTAT` busy-bit synthesis above) are
the same day, with the fix the newer of the two. Measured 2026-09-20, dbori
under `--timing-oc` is **byte-identical to golden over 1800 frames**, with
swap counts 172/172 and `GXSTAT` reads 542150/542150. Phase 1 starts from a
safer baseline than the documents suggest — provided it keeps the synthesis.
The *swap count* must still be carried in every measurement here, because a
model that makes a game draw fewer frames looks like a speed win and is not.

*Exit gate:* perceptual gate passes on all six scenes; cadence census
unchanged on NSMB, Golden Sun, Spirit Tracks, Etrian; compatibility sweep
clean; **and Dragon Ball Origins' intro checked for cadence in the SDL
frontend**, because no headless gate can settle cadence (§3.1). The picture
and swap counts under `--timing-oc` are already clean, so a regression here
means the synthesised `GXSTAT` was weakened.


#### Phase 1's shape, and the decisions taken (2026-09-20)

Three stages, separately measurable and separately abortable.

**1a — Demolition.** The no-FIFO, untimed model becomes the only model, and
the alternatives are deleted rather than predicated off: `exec_timed_`,
`untimed_`, `no_fifo_`, the geometry worker and its SPSC queue, `shape_step`
and its four arms, the pricer (`price_single`, `price_accum`, `pr_kept_*`),
`DS_GX_THREAD`, `--gx-worker`, `--timing-oc`. With them goes the FIFO
machinery they served: the stall queue, `fifo_write_full`, `promote_stalled`,
`note_enqueued`'s counters, `drain_settle_`, the whole `tm_*` family,
`finish_work`, and the `cycle_count_` / `vertex_pipeline_` /
`polygon_pipeline_` / `vertex_slots_free_` state. The expected landing point
is `tinl` plus whatever the branch removal itself buys -- which is known in
advance (§3.4), so this stage also checks the harness before anything
structural moves.

**1b — The logs.** The 512-entry ring of `Entry{u32 param, u8 cmd}` becomes a
frame-sized `u8` command log and `u32` parameter log. `gxfifo_write`'s sink
becomes two appends and a pointer bump, bounds-checked per burst rather than
per entry. The replay becomes a straight-line pass, not `drain_all` looping
`run_to_slow` with a fake cycle deficit. This is where the structural win has
to appear; if it does not, say so before 1c rather than after.

**1c — The transform split.** Two-pass replay: walk the log applying state and
matrix commands while capturing raw vertices and a snapshot of the state each
depends on, then one batched NEON transform over the array, then polygon
assembly. Gated on an instruction census under qemu before the real version is
written -- the per-vertex NEON attempt went 110 -> 118, and the per-vertex
state snapshot is exactly the cost that could repeat it. Last, so a negative
result costs nothing already banked.

Unchanged throughout: the synthesised `GXSTAT` contract in full, the
drain-on-observation path for `POLYGON_COUNT` / `VERTEX_COUNT` / matrix
read-backs, and the renderer -- dual-window and the multi-buffer path are not
in this phase's blast radius.

Decisions taken:

1. **Save-state format goes to 3, once, for the whole rework.** A version is
   only owed a bump when a release ships, so breakage between published
   releases stays inside version 3 and no migration is carried through seven
   phases.
2. **`--cpu-oc` keeps its plumbing.** 1a strips only its geometry-worker half
   (the call to `set_geometry_worker`); its JIT half -- translate-time
   data-access pricing -- is untouched and its fate is folded into Phase 2,
   where the rest of the JIT work happens and where the flag actually lives.
3. **`--timing-oc` is removed outright**, name and all. An unread key in an
   existing SDL config costs a line of bloat until the user deletes it;
   nothing fails to parse.
4. **The SDL cadence check is built after 1a is soundly in place**, so it is
   measuring a settled engine rather than a moving one.


### 3.5 Phase 1, closed — what it delivered, and why 1c was abandoned

**Delivered.** The `def` arm of the band sweep was run before 1a and again
after 1b, same binaries' build configuration (`a64build`, RelWithDebInfo,
GCC 13.3.0), same scenes, same `--frames 1800 --stats-from 200`. Replay
scenes only; gsdd is excluded because it stopped being the same scene when it
became a boot run.

| scene | median | | mean | total |
|---|---|---|---|---|
| sm64 | 11.057 -> **9.564** | **-1.49 (-13.5 %)** | 10.84 -> 10.03 | 17.3 -> 16.1 s |
| mlbis | 13.643 -> **12.339** | **-1.30 (-9.6 %)** | 11.47 -> 10.72 | 18.3 -> 17.2 s |
| dbori | 12.430 -> **11.933** | -0.50 (-4.0 %) | 11.60 -> 10.30 | 18.6 -> 16.5 s |
| etody | 5.394 -> **5.096** | -0.30 (-5.5 %) | 9.13 -> 8.86 | 14.6 -> 14.2 s |

Median, mean and total all improve on all four, and by more than §3.4's
`tinl` arm predicted (-1.21 / -0.35 / -0.11 / -0.07). Against the phase's
-4.0 ms target that is under half on the best scene, but it is real, it holds
at the mean and the total as well as the median, and the picture is unchanged:
the gate passes all eight scenes and dbori's intro cadence in the SDL frontend
is 26.44 Hz on both sides with the gap histogram within one frame of 705.

**1c was built, measured and abandoned.** The batched transform was written as
far as its first half -- vertices recorded rather than transformed, the run
flushed and transformed as an array, the existing per-vertex kernel kept so
that the restructuring could be priced on its own. It was bit-exact on every
deterministic scene, and it cost:

| scene | 1b median | 1c step 1 | |
|---|---|---|---|
| gsdd | 21.845 | 22.498 | **+0.65 ms (+3.0 %)** |
| sm64 | 9.595 | 9.859 | +0.26 (+2.8 %) |
| mlbis | 12.344 | 12.503 | +0.16 (+1.3 %) |

The wide kernel could not have repaid that. The device perf breakdown puts
`submit_vertex` at 2.41 % of the process against the emulation thread's
40.3 %, so ~6 % of that thread, ~1.3 ms of gsdd's frame -- and the position
transform, the only part a wide kernel touches, is about half of it. The
restructuring costs 0.65 ms before the kernel saves anything.

The cost is structural, not a tuning detail. A batch has to hold transformed
results while strip assembly recycles the four `temp_vtx_` slots, so each
vertex is copied into a slot on the way out -- exactly the 56-byte copy the
engine avoids today by permuting `vptr_` pointers. Removing it means building
polygons from an index into the batch instead of from slots, which is a much
larger change than the remaining upside (<= 0.6 ms on the heaviest scene)
justifies. Reverted; Phase 1 closes on 1a and 1b.

**The instrument that failed.** Instruction count under qemu was used to size
this phase and was wrong three times. It called 1b +0.0016 % on sm64 and
-0.188 % on gsdd -- both from an uncontrolled comparison of a `-O3` build
against a `-O2` one. Rebuilt at matching flags it reads -0.050 % and
**+0.473 %**, i.e. 1b executes *more* instructions on the matrix-heavy scene
while the phase containing it takes 1.5 ms off sm64's frame. On an in-order
A55 the wins here are locality and branch behaviour, which instruction count
cannot see. Use frame time on the device; treat instruction counts as a hint
about a kernel, never as a phase's scorecard. (This supersedes the figures in
commit `de81889`, which were taken before the flag mismatch was found.)

**Also settled.** The band sweep was re-run after 1a gave a core back: three
workers remains right, `def` and `DS_R3D_THREADS=3` are within noise (the lag
rule is inert), two workers still loses badly on etody and dbori, and four
never wins. No change.


### 3.6 The rasteriser does not steal from the emulation thread (2026-09-20)

A standing suspicion -- that the band workers take time from the emulation
thread through memory and cache contention even though nothing waits for them
-- was tested directly. `3d band wait` measures blocking; it says nothing
about interference. The instrument is `cpu9`, the emulation thread's own ARM9
execution stage, which contains no waiting: if the raster interferes, cpu9
must rise as band workers are added.

Golden Sun, `tools/raster_contention.sh`, rotating arm order:

| band workers | cpu9 | r3d_spans | r3d_wait | frame |
|---|---|---|---|---|
| 0 (inline) | 8.846 | 15.984 | 0.0000 | 27.808 |
| 1 | 8.857 | 0 | 0.0004 | 21.949 |
| 2 | 8.818 | 0 | 0.0004 | 22.738 |
| 3 | 8.750 | 0 | 0.0003 | 22.216 |

cpu9 is flat to 1.2 % and is LOWEST at three workers. The hypothesis is dead:
the ARM9 row is ARM9 work, and a cheaper raster will not narrow it.

This settles the phase order against a reorder that the DraStic comparison
seemed to argue for. `gpu-path` measured our software raster at 19 ms of
band-worker CPU on Golden Sun at 1x against DraStic's 9.5 ms for a whole 2x
frame -- about eight times the cost per pixel, "the price of the exact span
rules and the per-pixel resolve". That is the largest quantified inefficiency
in the project and the accuracy scope explicitly licenses relaxing it. But it
is not on the critical path here: at three workers the raster is hidden
entirely, and at zero it costs 15.98 ms on the emulation thread and 5.6 ms of
frame. So Phase 4 keeps its original scoping -- p99, thermals, 2x and devices
with fewer cores -- and does not move ahead of the ARM9 row.

Where Golden Sun's emulation thread actually goes, three workers:

| stage | ms |
|---|---|
| **cpu9** | **8.776** |
| **dma** | **3.011** |
| cpu7 | 1.484 |
| spu | 1.171 |
| sched | 1.031 |
| gpu_line | 0.571 |
| all 2D (bg, select, window, journal) | 0.64 |

cpu9 + cpu7 + dma is 13.3 ms, which is the "~13 ms against DraStic's 8.3" that
`gpu-path` recorded independently. That gap is Phases 2 and 3, in the order
already written. **Phase 5 (the 2D engines) is dropped**: 0.64 ms on the
heaviest scene, with the line-worker join at 0.0 %.

Two cautions for anyone reading these numbers:

* The profile line's `typ` values are per-stage MEDIANS, and a sum of medians
  is not the median of the sum. Summing them and subtracting from the frame
  appears to show ~5 ms unaccounted on Golden Sun -- but the same arithmetic
  gives NEGATIVE unaccounted time at zero band workers, which is impossible.
  Any claim about unprofiled time needs per-frame accounting.
* One band worker (`DS_R3D_THREADS=1`) is a real separate-core worker -- only
  `maxb == 0` rasterises inline -- and it has the BEST median of any arm
  (21.95) with the worst tail (mean 30.0, p90 43.1): ~16 ms of serial raster
  keeps up on light frames and not on heavy ones. The stall does not appear in
  `r3d_wait`, which is the per-line compositing wait; the `sync_all()` at
  render_frame is in no scope at all. That blind spot is real.

### 3.7 The goal line, restated

`gpu-path` (2026-09-18, user-verified) measured DraStic live rather than
through its `--benchmark`, which skips the screen path: on Golden Sun hi-res
its own overlay reads 64.8 % speed, about 25.7 ms a frame, against ours at
20.4 ms at 2x and 15.3 at 1x. The earlier reading of "DraStic 4.5 ms ahead at
every resolution" was an artefact of benchmarking a path players never use.

So "as fast or faster than DraStic" is already met on the hardest scene. What
remains is margin, thermals, and the scenes where we are not ahead -- and the
two measured gaps behind it: CPU emulation 8.3 ms against our ~13, and the
raster at roughly eight times the cost per pixel. Its threading is our shape
already (kicked a scanline earlier, per-band masks, engine B on a 2D worker),
so there is nothing there to adopt.

Note for whoever merges the GPU work: `gpu-raster` concluded its own column
would only match software once the list conversion moved onto the geometry
worker -- and Phase 1a deleted that worker. The 1x decision on `gpu-path` is
software raster through the GPU present stage; the GPU raster stays the opt-in
route to 2x.

### Phase 2 — The ARM9 row (weeks 2-4) — target **revised, see §3.2 and §3.3**

The biggest row, and the one with the least prior work, because the JIT was
already audited as close to DraStic (`06` rows 1.1-1.29 are mostly `same`).
The cost is therefore *not* in the translated code — it is in what surrounds
it. Start by splitting the 6.7 ms into translated code / helpers / fallback /
dispatch / timing with `perf` and the existing `hbline.py` per-line
attribution, the same way the geometry work was driven.

Candidate levers, in the order the evidence currently favours:

* **Kill the per-access timing charge — but not the way the parked branch
  does.** The `jit-timing-fold` branch folds the ARM9 timing row into the
  page-table slot, and **fastmem has since removed the premise**: with views
  on (`DS_FASTMEM`, the default where the JIT runs), `emit_single` skips
  `emit_walk_probe`/`emit_entry_load` entirely, so there is no page-table load
  left to fold anything into. What fastmem left behind is that the cost model
  is now *the majority of the emitted memory hot path* — the access is one
  instruction, and `cost()` (`lsr`, `add`, `ldrb` through the pinned `R_TIM`,
  a dependent load chain) plus `charge()` (`emit_charge_data`'s
  `max(nc + nd - 6, nc, nd)` arithmetic) are roughly six to nine more.

  The right lever is therefore **a translate-time constant charge at every
  site**, not a cheaper lookup. The machinery already exists: when
  `const_nd >= 0` the charge joins the block's static cycles via
  `add_pending`/`const_charge` and the site emits *no cost code at all*. Today
  that fires only for pc-relative literals and under `--cpu-oc` / `--cpu-uc`
  (`oc_data_cost`). Decoupling that cost path from the overclock — a flat cost
  model that is not also a speed change — is the experiment, and it is cheap
  to try because the code is written.
* **Coarsen the cycle model.** We reproduce the interpreter's per-page timing
  formulas exactly (row 1.8). DraStic uses a per-CPU model with a handful of
  per-game hacks. Accuracy of the cycle count is no longer a goal in itself —
  only "games run at the right speed and do not desync" is. Not via
  `DS_JIT_FASTCOST`, which measured *slower* than the exact model (§3.2); the
  target is `const_nd`, which emits nothing at the site.
* **The four open JIT rows**: tag-free ITCM direct tables (1.12), three code
  arenas with separate flushes (1.23 — this one also removes a real failure
  mode, a full arena throwing away every translation), known-constant tracking
  (1.26), and the check-free second entry point (1.5).
* **Fallback rate.** Every fallback is an interpreter step plus a poll.
  Census which encodings still fall back per frame and inline the top ones.
  LDM/STM currently falls back whenever the transfer crosses a 2 KB page
  (row 1.28); DraStic has sixteen specialised helpers.
* **Wider idle skipping.** `DS_IDLE_SKIP=all` was measured to skip almost
  nothing *because the whole-machine rule vetoes it* — Spirit Tracks and NSMB
  have a genuine 3-instruction ITCM RAM-flag wait the analyser accepts but the
  rule rejects (the ARM7 is busy, or a GXFIFO DMA is running). With the
  geometry FIFO gone in Phase 1 and exactness no longer a constraint, that
  rule can be relaxed.

*Exit gate:* self-consistency holds; perceptual gate passes; compatibility
sweep clean — this is the phase most likely to break games, so the sweep is
the real gate, not the four scenes.


### 3.8 Phase 2's survey: what Golden Sun's JIT is actually doing (2026-09-20)

Three explanations were eliminated before looking at the JIT itself: raster
contention (§3.6, cpu9 flat across band counts), the timing cost model (§3.2,
0.2-0.4 ms by costprobe), and idle spin (the loop analyser rejects 93 % of
Golden Sun's candidates -- its ARM9 is executing varied code, not waiting).

The census, 900 frames, `DS_JIT_HIST=1` on the cross build under qemu:

    blocks 11346, inline instrs 60763, fallback executions 2482000,
    slow accesses 5834043, entries 1290487, invalidated 53248,
    revived 51193, flushes 0
    code 3138 KB (hot 1581 KB): 52.9 bytes per guest instruction, 26.6 hot

**Churn is not the problem, and the README's fix holds.** Zero arena flushes;
53,248 blocks invalidated against 51,193 revived, so 96 % come back through
park-and-revive; 4 retime invalidations killing 19 blocks in a whole run; and
ARM9 timing-table rebuilds -- each of which drops every block -- happen 11-16
times per run on every scene measured. The 1.9k-retranslations-a-frame
catastrophe the JIT README records for this exact game (ITCM data sharing a
page with the hot ITCM routines) is genuinely fixed by the same-value and
overlap-only filters. Retranslate lead time averages 287.9 ms over 859
samples: pages come back long after the kill, not immediately.

Block linking is likewise already there -- `jit_h_link` rewrites a block's
exit `bl link` into `b native` on first execution, so a taken link is one
branch with no dispatcher round trip.

**What is left is density and fallbacks.**

*Density: 52.9 bytes per guest instruction, 26.6 in hot code* -- roughly
thirteen emitted AArch64 instructions per guest ARM instruction, seven in the
hot set. This is the only figure large enough to explain 8.78 ms of cpu9
against DraStic's 8.3 ms for all of its CPU emulation. The per-access timing
charge is part of it (4-7 instructions on every memory op) but costprobe
prices that at 0.2-0.4 ms, so most of the density is elsewhere.

*Fallbacks: 2,482,000, i.e. 2,758 a frame*, in two clusters:

| encoding | count | what it is |
|---|---|---|
| `e8830007`, `e881000c/101c`, `e891000c/101c` | ~630 k | LDM / STM |
| `e121f000/1/2`, `e16ff001`, `e25ef004`, `e8d07fff` | ~600 k | MSR cpsr/spsr, `LDM ^`, `SUBS pc, lr, #4` |
| `ee190f11` at `ffff0278` | 117 k | CP15 read in the BIOS vector |

The second cluster is the **IRQ handler entry and exit** -- the BIOS vectors at
`ffff0278`-`ffff0294` and the handler at `0205404c` -- at 120,452 executions
over 900 frames, i.e. **134 interrupts a frame**, every instruction of the mode
switch stepping through the interpreter.

**DraStic has a specialised emitter for every one of these** (01 §7):
`cpu_translate_block_memory_op` with sixteen helpers per direction
(`arm64_load_block1` ... `_16`), one per register count, so a transfer is a
call into straight-line code rather than a loop; `cpu_translate_msr_op`,
`cpu_translate_mrs_op`, `cpu_translate_bx_op`, and
`cpu_translate_raise_exception`; plus constant collapsing for the literal-pool
and `MOV`/`ORR` pairs ARM code generates constantly.

Against the checklist:

* **1.28 is marked `equiv` and should not be.** "LDM/STM inlined when the
  transfer sits in one 2 KB page, interpreter fallback otherwise" -- the
  census says that fallback fires ~630 k times in 900 frames. It is a hot
  path, not an equivalent.
* **1.26 `partial`** (constants only for pc-relative operands) is the density
  lever DraStic names explicitly.
* **There is no row at all for the MSR / MRS / BX / exception emitters.** The
  largest single fallback cluster we have was never tracked as a gap. Add it.

Phase 2's order follows: the exception path and LDM/STM first (work *removed*,
sizes measured, and both have a DraStic design to follow), then density via
known-constant tracking, then the timing charge. Rows 1.5, 1.12 and 1.23 stay
open but none of them addresses anything the census found.


### 3.9 Phase 2's first delivery: two emitters, measured (2026-09-20)

The census's two fallback clusters (SS3.8), answered with DraStic's designs
where they fit and with ours where they do not.

**Cross-page block transfers.** DraStic gives LDM/STM sixteen helpers per
direction, one per register count, because its fallback is the loop. Ours is
not: the inline path already unrolls, and it falls back only when a transfer
straddles a 2 KB page. `emit_block_slow` applies the same idea -- a call into
straight-line code rather than a re-decode -- to the case we actually lose.

**MSR SPSR inlined.** Refused by a blanket SPSR test, though it banks nothing
and changes no mode. A load, a merge and a store, skipped in user and system
mode. The liveness table needed correcting with it: it claimed the `f` field
writes the host flags, which is true for CPSR and false for SPSR.

Measured, Golden Sun, 900 frames:

| | before | after |
|---|---|---|
| fallback executions | 2,482,000 | **1,601,937** (-35.5 %) |
| total instructions | 30,940,069,510 | **29,601,064,211** (-4.33 %) |
| hot code density | 26.6 B/guest instr | 26.6 (unchanged; the new leg is cold) |

On the device, three reps, alternating order:

| scene | base | with emitters | median | mean | p99 |
|---|---|---|---|---|---|
| dbori | 11.879 | **11.632** | -0.247 | -0.065 | 20.97 -> 20.43 |
| gsdd | 21.769 | **21.592** | -0.177 | -0.212 | 34.43 -> 34.47 |
| sm64 | 9.509 | **9.421** | -0.088 | -0.109 | 21.20 -> 20.37 |
| mlbis | 12.276 | 12.344 | +0.068 | +0.018 | 18.86 -> 18.50 |

Median better on three of four, mean on three, p99 on four. Small, and real.
Note again that instruction count over-predicted: -4.33 % of instructions
became -0.8 % of Golden Sun's frame. Part of that is the window (the census
ran from boot, where cross-page transfers are denser, the device from the
title), but it is the fourth time in this rework that instruction count has
been a poor proxy for time on this core.

**Verification.** `test_jit` 1600 fuzz trials and 55 directed cases pass, and
the JIT's 800-frame dump of Golden Sun is byte-identical to the same tree
without the emitters. The fuzzer caught a real defect first: the cold path
charged `N + (n-1) S` from the base page, copying the inline path, but a
straddling transfer has its words on two pages with different costs and the
interpreter prices each from its own -- a Thumb LDM consumed 162 cycles
against 161. It now sums the true per-word costs before the transfer, while
the scratch registers are still free (the slow stubs preserve only x1 and x7).

**A gate that looked broken and was not.** The JIT's dump differs from the
INTERPRETER's by 11,347,831 bytes -- 3.6 % -- on Golden Sun, on the unmodified
tree as well, and this was first recorded here as "one of the three
verification routes the JIT README names is not usable for this game". That
was the wrong standard. Judged by RESULT rather than by bytes, the same pair
reads ssim mean 0.9999, p01 0.9986, min 0.9983, no structural faults, 359 of
800 frames merely realigned: the two engines run at marginally different
speeds and frames land at different moments. The JIT is correct; byte equality
was never the question.

No new comparison was needed for it either. `golden_dump.sh` already forces
`--interp` on the reference side and `run_candidate.sh` runs the candidate as
played, so handing `gate.sh` the SAME JIT-capable binary twice IS the JIT's
correctness gate -- floors, structural faults and cadence counters included.
`tools/jit_gate.sh` is that one line, with the reasoning attached.

The real constraint is narrower than "no gate": the JIT has AArch64 and ARM32
backends only, so the x86 host build is interpreter-only and neither this nor
`title_sweep.sh` -- which Phase 2's exit gate calls the real gate -- can
exercise the recompiler on the host. Both have to run under qemu or on the
device.

**What is left of the census** is all one problem: MSR CPSR with a mode change
(~361 k), `SUBS pc, lr, #4`, `LDM ^` and a CP15 read (~117 k each). Every one
needs banked-register handling, so each needs a helper with a full register
spill rather than an emitter -- and the helper has to honour the same
flag-materialisation contract (`reads = F_ALL`) the interpreter fallback does.


### 3.10 The mode-switch core: rejected on noise, then restored (2026-09-20)

**RETRACTED AND REVERSED.** Everything below was written from a single
threaded instruction census reading +1.53 %. Run serially -- the only valid
way (SS3.12) -- the same comparison reads **-1.38 %**, and on the device the
change is faster on all four scenes: gsdd -0.180 ms median, sm64 -0.084,
mlbis -0.061, dbori -0.040, mean better on three. It is restored.

The rule this section proposed -- "replacing a fallback pays when the
fallback's own work can be done more cheaply, not when only its decode can be
avoided" -- is a counterexample to itself and is withdrawn. `LDM ^` and
`SUBS pc, lr, #4` were ruled out by inheriting it, without measurement, and
are live candidates again at ~120 k fallbacks each.

The original text follows, as the record of a decision made on an instrument
that had never been validated.


The census's second cluster is the IRQ entry and exit -- `MSR CPSR` with a
mode change (~361 k), `LDM ^`, `SUBS pc, lr, #4` -- and DraStic has an emitter
for each (`cpu_translate_msr_op`, `cpu_translate_raise_exception`). The
`MSR CPSR` one was built as a targeted helper and it does not pay.

**What was built.** `jit_h_msr_cpsr` runs the interpreter's `msr()` CPSR arm
without the decode, reached from `arm_msr`'s cold path. The spill is narrowed
to the banked registers only: r0-r7 live in callee-saved host registers and
`switch_mode` does not touch them, `call_pure` already spills and reloads
r8-r12 and writes the host flags into `hot.cpsr` (the contract `set_cpsr`
needs), so only r13 and r14 travel by hand -- 4 memory operations against
`call_full`'s ~32.

**What it measured.** Golden Sun, 900 frames:

| | committed tree | with the mode switch |
|---|---|---|
| fallback executions | 1,601,937 | **1,070,861** (-33 %) |
| total instructions | 28,920,336,717 | **29,364,147,319 (+1.53 %)** |
| picture | -- | byte-identical over 800 frames |

A third of the remaining fallbacks removed, and 444 million MORE instructions
executed. Reverted.

**Why, and why it generalises.** The LDM/STM win came from the fallback doing
work the replacement could do more cheaply -- n page-table walks in C against
n slow-stub accesses. A mode switch has no such slack: `set_cpsr` ->
`switch_mode` is the same call either way, so a helper can only save the
decode and part of the spill, and here that saving is smaller than what the
emitted call sequence costs. `LDM ^` and `SUBS pc, lr, #4` need the same
banked-register handling and do the same work, so they inherit the same
ceiling and should not be attempted on this evidence.

The rule this suggests, stated for the next phase: **replacing a fallback pays
when the fallback's own work can be done more cheaply, not when only its
decode can be avoided.** Every fallback looks like waste in a census; only
some of it is.

**The fuzzer is what makes this cheap to learn.** It caught a real defect in
each of the three emitters tried today -- the per-word cost on a cross-page
transfer (162 cycles against 161), the SPSR liveness claim on the `f` field,
and the missing `numC` charge here (92 consumed against 128). None would have
shown in the picture gate. That matters the more because the JIT's other
correctness route, `--interp` against the recompiler with `--dump-frames`, is
broken for Golden Sun on the unmodified tree (SS3.9).


### 3.11 Phase 2's second lever: wider idle skipping, measured (2026-09-20)

Two changes, stacked so either reverts alone.

**The GX veto is gone** (`017c4e5`). It refused a skip whenever the geometry
engine had anything queued -- which mattered when a queued command carried a
cycle cost, an observable FIFO level and a stall. Phase 1 removed all three.
The loop analyser, which rejects a poll of any port with a side effect, is
what kept this safe and still does.

**A skipped CPU now advances its own DMA** (`ffb29f0`). On hardware a transfer
runs while the core is halted; here DMA is driven from inside `run_cpu`, so a
skipped slice froze it, which is why the veto existed. `advance_dma_only`
makes the same call with the CPU left alone. The veto narrows to the DSi,
where `a9_dma_iter_` hands the slice back mid-transfer and has no meaning when
no CPU ran.

Sized first by survey (counting only), split by which veto held each slice:

| scene | opportunity | behind GX | behind DMA |
|---|---|---|---|
| sm64 | 721,368 | 721,368 | 0 |
| mlbis | 447,883 | 414,818 | 33,065 |
| dbori | 272,358 | 26,909 | **245,449** |
| gsdd | 1,688 | 1,688 | 0 |

Device, three reps, alternating order:

| scene | base | with both | median | mean | p99 |
|---|---|---|---|---|---|
| sm64 | 9.441 | **9.300** | -0.141 | -0.035 | +0.73 |
| dbori | 11.622 | **11.479** | -0.143 | -0.088 | -0.28 |
| gsdd | 21.628 | **21.519** | -0.109 | -0.107 | **-1.28** |
| mlbis | 12.303 | 12.299 | -0.004 | -0.018 | +0.28 |

Median better on three, mean and total on all four, and Golden Sun's p99 down
1.28 ms -- the largest p99 movement anything in this rework has produced.

**The instrument was wrong again, in a new costume.** The survey counted
SLICES, and a recovered skip saves only the CPU work that slice would have
run -- and those slices are idle loops, a few cheap instructions an iteration.
721 k recovered slices on sm64 buys 0.141 ms; mlbis recovered 414 k for
nothing measurable. Cycles skipped (`C_CYC_IDLE_SKIPPED`) would have been the
honest instrument. Count what the change removes in TIME, not in events.

**Cadence was checked apart from the picture**, because a DMA-timing change
moves when geometry arrives and the gate's alignment window absorbs exactly
that: gsdd, dbori, mlbis and sm64 show only the differences already recorded
for Phase 1's model, mlbis is identical, and no scene draws fewer frames.


### 3.12 The instruction census is only valid run serially -- and what that voids

Two runs of the SAME binary, Golden Sun, 900 frames, default threading:

    a64.head  29,860,253,706
    a64.head  28,884,498,240      <- 976 M apart, 3.4 %

The same binary with `DS_R3D_THREADS=0 DS_2D_THREAD=0`:

    3,566,577,697
    3,566,577,317                 <- 380 apart, 0.00001 %

The count includes every thread, and the band-worker count is chosen
adaptively per frame from measured wall times, so the work partition differs
run to run. Emulation is deterministic; the instruction count is not, unless
the renderer is serial -- which `golden_dump.sh` has always done for the
picture and which no census here had done for the counts.

**Run every instruction census in the golden config's serial settings.** A
single threaded pair cannot resolve anything below about 4 %.

**What this voids.** Figures quoted in SS3.9-SS3.10 from single threaded pairs,
against a 3.4 % noise floor:

| claim | figure | standing |
|---|---|---|
| mode switch rejected (SS3.10) | +1.53 % | **void -- inside the noise** |
| MSR SPSR kept | -2.3 % | unsupported by that measurement |
| 1b measured (SS3.5) | -0.05 % / +0.47 % | unsupported |
| block transfer kept | -4.33 % | marginal at best |

The CONCLUSIONS may still hold -- the block transfer and MSR SPSR were also
measured on the device at -0.09 to -0.25 ms, which is the instrument that has
held up -- but the instruction figures behind them should not be cited. The
mode switch has no device measurement at all: it was discarded on a number
that cannot distinguish it from zero, and so were `LDM ^` and
`SUBS pc, lr, #4`, which inherited the reasoning without being measured. All
three want a device A/B before the rule in SS3.10 is allowed to stand.

**Row 1.26, measured properly, is a regression.** Known-constant tracking was
built (MOV/MVN immediates and the immediate data-processing forms folded onto
a known register; literal-pool CONTENTS deliberately not read, since baking a
value a later store could change would need SMC cover the block does not
have), fuzzer-clean, byte-identical on 800 frames. On the device, three reps,
alternating order:

| scene | base | with row 1.26 | median | mean |
|---|---|---|---|---|
| gsdd | 21.477 | 21.734 | **+0.257** | +0.084 |
| dbori | 11.502 | 11.611 | +0.109 | +0.052 |
| sm64 | 9.283 | 9.385 | +0.102 | +0.088 |
| mlbis | 12.269 | 12.352 | +0.083 | +0.060 |

Worse on all four, median and mean. Reverted. The per-access saving it aims
at is real -- four instructions and a dependent load become one immediate
subtract -- but it fires too rarely to pay for what it costs: at 400 frames
the block, fallback, slow-access and invalidation counters are IDENTICAL with
and without it, so on that stretch it essentially never fires, while
`const_step` runs at translate time for every instruction translated.

### Phase 3 — DMA, SPU and scheduler granularity (week 4-5) — target **-2.5 ms**

Three small rows that together are as big as Phase 1.

* **DMA 1.5 → ~0.2 ms** (checklist 4.8/4.9 in full). Today every word is
  charged, stall-checked and budget-bounded against a per-unit burst model,
  even inside a direct-mapped run. DraStic does a whole-transfer copy over a
  16-entry 8 MB region table with static seq/non-seq cost tables and schedules
  completion as an event — 7.7 k instructions a frame against our ~30 per
  word. The code-page invalidation over the destination becomes the coarse
  (64 KB) / fine (2 KB) bitmap OR (4.10).
* **SPU 0.9 → ~0.1 ms** (5.1 in full, plus 5.4, 5.6, 5.9, 5.11). Mix once per
  frame at VBlank with a catch-up on the ARM7 audio timer; resolve each
  channel's source to a host pointer at key-on; fold resampling into playback
  with a 32.32 cursor; decode ADPCM a word (8 samples) at a time into a ring;
  PSG as duty tables and noise as a precomputed 32 K LFSR table. The note that
  these "would change what Rhythm Heaven's just-in-time stream writer sees" is
  now a compatibility-sweep question, not a veto.
* **Scheduler 1.4 → ~0.5 ms.** ~1,090 slices a frame and the scanline hooks.
  With the SPU no longer an event per 16 samples and the geometry no longer
  needing per-slice `run_to`, the event count falls on its own; then remove
  `LOCKSTEP_QUANTUM` and the dual-mode interleave, keep `EVENT_BOUND_QUANTUM`
  at 2048 (the SDK's IPCSYNC boot handshake needs it — SM64DS boots at 2048
  and does not at 2560), and let both-halted slices run to the deadline.

*Exit gate:* audio is judged by ear and by a spectral comparison against
`dsperate-ref`, not by sample equality — which will not survive any of this.

### Phase 4 — The rasteriser, rebuilt for plausibility (weeks 5-7) — target: **p99, thermals, and the other devices**

Be honest about what this buys on the RG DS: **it does not shorten the median
frame**, because the band workers are already off the critical path. What it
buys is the p99 tail (three workers at 9-27 ms on a four-core device contend
with the emulation thread and the display path), the power and thermal
headroom that the `ondemand` governor work showed is worth 1-3 ms of median
on its own, and — the real prize — the H700 and A30 becoming viable.

The present rasteriser matches melonDS pixel for pixel. Under the new bar it
does not have to. The DraStic design to move to (02 §§1-4):

* **12 bins × 16 scanlines**, ~32-36 KB live footprint sized to the A55's
  32 KB L1D, against our ring of 8 lines per worker (~50 KB) in horizontal
  bands. 12 divides evenly by 1/2/3/4/6/12, which is why the split needs no
  remainder handling and no adaptive worker controller — and deleting
  `DS_R3D_ADAPT` and the non-reproducible per-frame shape controller is a
  stability win in itself.
* **AND/OR uniformity per polygon at bin time** (2.4). We measured constant-W
  and flat-RGB empty and found constant *alpha* was the axis with signal — but
  that was under the exactness constraint. Re-measure with tolerance allowed.
* **8-way unrolled scalar texel gather** (2.14) — the one kernel row never
  measured, and the one explicitly designed for an in-order core.
* Simplifications now permitted: drop or approximate edge marking, fog
  precision, the shadow-volume stencil's exact semantics, and the anti-aliased
  coverage blend, each measured independently against the perceptual gate.

Keep: `kernels_ref.cpp` beside `kernels_neon.cpp` and `tests/kernels_test.cpp`
(row X.7). The C reference is what makes hand-written kernels maintainable and
it costs nothing at runtime.

*Exit gate:* perceptual gate with per-scene floors; worker time per frame
halved; p99 on NSMB within 2 ms of the median.

### Phase 5 — The 2D engines — DROPPED (§3.6: 0.64 ms on Golden Sun, 0.0 % join wait)

2D is 25.2 % of DraStic's frame and our lazy-2D work already took the biggest
bite (11-21 % on the headless scenes). What is left is representation:

* **1-bit-per-pixel visibility masks** (3.1/3.2/3.3) against our 16-bit lanes
  with bit 15 as the opaque flag. A scanline becomes two NEON registers, the
  priority encoder becomes bit-parallel, and per-32-px groups with a zero mask
  word can be skipped (3.7).
* **Planar 6-bit channel split once per line** (3.8) against our 18-bit packed
  records.
* The VRAM write trap (4.11) that makes our lazy path hardware-exact can
  become DraStic's rule — ignore CPU stores into VRAM mid-frame — if the
  perceptual gate allows it. Census first: the trap fires 13-17 times a run on
  mlbis/meteos/sm64, so this is worth little except on Golden Sun (~3.5 k VRAM
  stores a frame, ~14 catch-ups).

This phase is optional if the budget is met by Phase 3; it is listed because
it is where DraStic's remaining quarter of a frame lives.

### Phase 6 — Stability as a workstream (throughout, converging weeks 8-9)

**The FF3 both-recompiler freeze is scheduled after Phase 3**, not before: it
is a JIT-interaction defect (alive under `--interp`, `--jit9` and `--jit7`
separately, frozen with both, confirmed on hardware and reproducible under
qemu), so it is chased once the JIT work of Phases 2-3 has settled rather than
against a moving target. It is bisectable off-device whenever it is picked up.

Not a phase that follows the others — a track that runs alongside, with its
own gates. The user named three things; each maps to concrete work already
visible in the tree.

**Crashes, hangs, thread hazards.**

* The thread topology shrinks by design: Phase 1 deletes the geometry worker
  and its three-mode controller, Phase 4 deletes the adaptive raster worker
  count. What remains is one emulation thread, a fixed band pool, one line
  worker, and the display presenter.
* Make the band pool's hand-off reproducible. The self-consistency gate from
  Phase 0 is the enforcement mechanism, and today's shipped worker path fails
  it. `Renderer3D`'s pool already carries `debug_dump`; keep it.
* Hand-offs: the `LineWorker` spins then parks while the 3D workers use
  mutex+condvar (row 4.23, and there is a recorded qemu LineWorker hang). One
  mechanism, one place.
* The JIT arena flush (row 1.23) — a full arena throws away every
  translation. Phase 2 fixes this as a side effect of the three-arena split.
* Save-state robustness: the version-2 event-count bug
  (`7015028`) was found by accident via a 1.5 ms "smoothness" anomaly. Every
  chunk should carry its own count, and `tools/state_roundtrip.sh` should run
  in CI. Note that **save states will break format** across this rework;
  decide early whether to bump `OLDEST_READABLE_VERSION` and drop old states
  rather than carry migration code through seven phases. Recommendation: drop
  them, once, at the start of Phase 1.
* Fuzz the ARMv7 and AArch64 translators after every Phase 2 step, as
  `docs/arm32-jit-scoping.md` already prescribes (1600 trials under qemu).

**Frame pacing and audio gaps.** The pacing work is recent and sound (the
frame limiter replaced `Audio::pace()`; `docs/cpu-governor-scoping.md` shows
the `ondemand` governor moves the median 1-3 ms and multiplies missed frames
by 4-6 and audio gaps by 20-30). Two things follow:

* Every phase's measurement must report **missed frames and dry audio queues**
  alongside the median, because those are what the user feels. A 1 ms median
  win that adds jitter is a loss.
* Once the median is comfortably inside 16.74 ms, **reinstate frameskip that
  drops rendering only** (checklist 4.22 — DraStic's, at 9,000 µs behind, with
  a resync at 200,000 µs). We have frameskip in the frontend; the point is a
  core-level skip state that makes `render_scanline` a no-op, so a late frame
  costs nothing instead of costing a full render.

**Retail compatibility.** The sweep from Phase 0 is the gate. Run it at the
end of every phase, not at the end of the project. Any title that regresses is
either fixed or recorded in this document as a known casualty with a reason —
the same discipline `06`'s "every omission is a decision" rule already
enforces.

### Phase 7 — Features back, and the knobs gone (weeks 9-10)

* Re-verify dual-window, the layouts, PiP, the scaling filters and the display
  tier selection (DISP hardware scaler / fbdev / dmabuf / KMSDRM / SDL) on the
  new core. These live in the frontend and should survive Phases 1-5 largely
  untouched; that is the argument for doing the core first.
* **Delete the accuracy tier.** `--cpu-oc`, `--cpu-uc`, `--timing-oc`,
  `--fast-load` and the menu rows behind them exist to trade accuracy for
  speed. After this rework the default *is* the fast model, and `--cpu-uc` in
  particular was measured not to be a smoothness gain at all — it makes games
  drop 25-33 % of their frames and the emulator's frame time falls because
  there is less 3D to do.
* Smooth 3D edges (`video.aa = smooth` / `shaped` / `enhanced`) is a genuinely
  differentiating feature and is cheap (0.2-0.6 ms). It depends on the
  rasteriser knowing sub-pixel edge positions, so Phase 4 must keep that
  information. Flag this as a Phase 4 design constraint, not a Phase 7
  surprise.
* Rewrite `docs/techniques/06-implementation-checklist.md`'s status column and
  the audit summary. The document's purpose does not change — "every omission
  is a decision" — but most of the `no`-by-choice rows become `same`.

---

## 4. The arithmetic

NSMB, emulation thread, if every phase returns its target:

| | today | after |
|---|---|---|
| cpu arm9 | 6.7 | 3.2 |
| gx geometry | 3.4 | 0.4 |
| cpu arm7 | 1.5 | 0.9 |
| dma | 1.5 | 0.2 |
| spu | 0.9 | 0.1 |
| slice loop | 0.7 | 0.3 |
| scanline hooks | 0.7 | 0.4 |
| 2D on this thread | 0.7 | 0.4 |
| gx vblank | 0.4 | 0.3 |
| **frame median** | **18.7** | **~7-8** |

That is DraStic's neighbourhood on a 3D-heavy scene, with headroom for the
p99 tail, and it is the number that makes upscaling a conversation worth
having later. It is also, deliberately, arithmetic from targets rather than
from measurements — **the only rows with prior measured evidence are geometry
(`--timing-oc`, 3-4 ms) and DMA/SPU (the checklist's own censuses).** Phase 2
is the largest single line and the least evidenced; if it returns 1.5 ms
instead of 3.5 ms the project still lands near 10 ms, inside the budget with
margin, and that should be treated as success rather than as a reason to
extend the phase.

## 5. Risks, in order

1. **Phase 2 is the least evidenced phase.** 6.7 ms of ARM9 has no per-stage
   census yet, and the JIT is already audited as close to DraStic. Phase 0
   must produce that census before Phase 2 is committed to; if it shows the
   time is in translated guest code rather than around it, this plan's largest
   line item has no lever and the schedule changes. The cost-model lever is
   **This has now happened.** `DS_JIT_COSTPROBE` was run (§3.2) and the cost
   model is 2.3-5.5 % of instructions, worth perhaps 0.2-0.4 ms — not 3.5 ms.
   Phase 2 is rescoped to census-first; the device `perf` run against the
   ARM9 row is now the gating task, not an optional one.
   *(This risk was already sharpened once: the plan's first draft proposed the
   `jit-timing-fold` approach, which fastmem has obsoleted. Assume other
   pre-fastmem assumptions in the JIT notes are stale too, and re-read the
   emitted code before trusting any of them.)*
2. **The perceptual gate is load-bearing.** Built and self-tested
   2026-09-20 (§3.1), which retires most of this risk — but it compares
   pictures only. Cadence is a separate gate, and *neither* headless gate can
   reach the one casualty the docs already name. Anything that depends on how
   a game paces itself has to be checked in the SDL frontend, and that check
   is not written.
3. **Compatibility loss is the plan's main cost, and it is not measurable in
   advance.** Games that pace themselves on FIFO stalls, timer races, or the
   SPU's write timing will change behaviour. The sweep bounds this; it does
   not eliminate it.
4. **The rasteriser rewrite (Phase 4) is the most code for the least median
   gain** on the primary target. It is justified by p99, thermals and the
   other devices — all real, none of them the headline number. If time runs
   short, this is the phase to cut, and §4's arithmetic survives without it.
5. **Deleting the exact path is irreversible in practice.** The
   `exact-reference` binary mitigates it, but once the dual paths are gone,
   "just turn accuracy back on for this game" is no longer available. This was
   an explicit decision; it is recorded here so it stays one.
