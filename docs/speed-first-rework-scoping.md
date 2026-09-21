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

### Phase 2 — The ARM9 row (weeks 2-4) — target **revised, see §3.2 and §3.3**; delivered **-0.02 to -1.13 ms**, closed (§3.16)

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


### 3.13 `LDM ^` -- ruled out twice, wrongly, and what it actually needs

`LDM r0, {r0-r14}^` (120 k fallbacks a run, Golden Sun's IRQ exit) was first
ruled out by inheriting SS3.10's withdrawn rule, then a second time on the
grounds that it "writes r0-r14, so everything spills and only the decode is
saved". Both were wrong, and wrong the same way: reasoning from how the
INTERPRETER implements it rather than from what the hardware requires.

The interpreter switches to user mode, transfers, and switches back
(`interp_arm.cpp`, `user_bank`). That is convenient in C and irrelevant to the
JIT. What the banking actually says (`cpu.h`): `bank_r8_r12[2][5]` covers USR
and FIQ **only**, while `bank_r13[6]` / `bank_r14[6]` are per bank. So in IRQ,
SVC, ABT or UND mode:

* r0-r7 are never banked -- straight into the pinned host registers;
* r8-r12 are banked only for FIQ, so in those modes the live registers ARE the
  user's -- also straight into the pinned host registers;
* only **r13 and r14** belong elsewhere, at `bank_r13[0]` / `bank_r14[0]`.

That is an ordinary inline block transfer with two destinations redirected to
fixed memory slots -- the same narrow-spill shape that paid for the mode
switch and the exception return -- guarded by a runtime check that the mode is
not USR/SYS (where bank 0 is current) or FIQ (where r8-r12 are banked too),
which keep the interpreter.

**Built and measured (2026-09-21). Golden Sun -0.094 ms median, -0.120 mean;
the other three scenes inside the noise.** The emitter is what the paragraph
above describes: a runtime test of CPSR's mode field against a 32-bit mask of
the four inlinable modes (IRQ/SVC/ABT/UND), the ordinary inline block transfer
with r13/r14 redirected to `bank_r13[0]`/`bank_r14[0]`, and USR, SYS, FIQ and
the reserved encodings branching to the interpreter. Writeback also keeps the
interpreter: it is UNPREDICTABLE with S, and the interpreter writes the base
back while still switched to user mode, so a base of r13/r14 would land in a
different bank than an inline path would pick -- no cost, the shape does not
occur.

| scene | base median | with the emitter | median | mean |
|---|---|---|---|---|
| gsdd | 20.737 | 20.643 | **-0.094** | -0.120 |
| mlbis | 12.197 | 12.260 | +0.063 | -0.001 |
| sm64 | 9.266 | 9.230 | -0.036 | +0.040 |
| dbori | 11.315 | 11.345 | +0.030 | -0.036 |

Three reps, alternating order, both arms built from the same tree with the
same flags (the base is this tree with `ldm_user_inline()` forced false, so
the A/B isolates the emitter and nothing else). Only gsdd separates: all three
candidate reps (20.643 / 20.627 / 20.645) sit below all three base reps
(20.737 / 20.889 / 20.721). The other three scenes overlap rep for rep.

The fallback census predicted more than the clock delivered, exactly as
SS3.12's rule warns. Serial, 900 frames: Golden Sun 509,730 -> 372,499
fallback executions (**-137 k, -27 %**), FF3 -11 %, SM64DS -4 %. 137 k
fallbacks removed bought 0.1 ms -- about 0.7 us per thousand. Count the time,
not the events.

*Gate:* `tools/jit_gate.sh`, four scenes, all pass; sm64, dbori and mlbis
1800 of 1800 frames byte-exact, meteos 1799 of 1800. The cadence lines that
differ from the fixtures (GXSTAT poll counts, and sm64's swap counts by one
or two) predate this change: they come from the timing work earlier in
Phase 2 and the fixtures have not been re-baselined since. Verified rather
than assumed -- the same gate run against the BASE binary prints the identical
diff (sm64 851 -> 850 kept, 1416 -> 1417 swaps, 384 -> 386 empty vblanks,
GXSTAT 29720 -> 24372), so none of it is attributable to the emitter.

*Fuzzer:* the generator never left SYS mode, where the user bank IS the live
one and `LDM ^` is indistinguishable from a plain LDM -- so nothing had ever
reached this path. It now emits a self-contained gadget (copy the base into
r0, enter one of the seven modes, transfer, return to SYS) and there are 96
directed cases pinning each mode class. Admitting USR, SYS or FIQ to the
inline path, dropping either bank redirect, and dropping the writeback
exclusion are each caught; dropping a mode FROM the mask is not, and should
not be -- that leg just falls back, which is still correct.

**The general lesson, since it has now cost two wrong rejections in one
session: when deciding whether a fallback can be inlined, derive the
requirement from the architecture, not from the interpreter's implementation.
The interpreter is written for clarity in C and routinely does more than the
hardware demands.**

### 3.14 A defect the user-bank work uncovered: `STM` with r15 on the slow path

`emit_block_slow` -- the per-word path a block transfer takes when it straddles
a page or lands on a write-protected one -- had no `i == 15` case, and
`host_reg(15)` returns **28, which is r14's host register**. So an `STM` with
r15 in the list stored r14's value instead of the pipeline value, whenever the
transfer could not stay inline. The inline loop had always handled r15; only
the slow twin did not, and loads with r15 leave through `emit_fallback`
earlier, so this was stores only.

Pre-existing and unrelated to the user bank -- a plain `STM` with no S bit
reproduces it. It surfaced now because `STM ^` made one more shape reach that
path. Fixed by passing `pc_store_value` down and emitting it for r15; the
regression test is a page-straddling `STM r0, {r0-r15}` based 0x7D0 into the
buffer, in plain and user-bank form.

**What this says about the fuzzer.** It found this in 6000 trials, but only
after the mode-switching gadget existed, and only in the full set -- the seed
passed on its own. Two things were hiding it. The machines are reused across
trials and nothing reset main RAM, so a seed's result depended on the trials
before it; and a trial that stores over its own code is outside what the
recompiler promises (the interpreter fetches the new word, a translated block
keeps running what it was built from), which is a divergence the harness was
counting as a failure. Both are now handled: the code page is cleared per
trial, RAM is wiped after any trial that could have left the two sides
different, and self-modifying trials are declined and counted rather than
judged (25 / 32 / 10 / 28 per set at 6000 trials). `DS_FUZZ_TRACE=<prefix>`
dumps a per-instruction trace of both engines, which is what localised this;
the end-state comparison names the symptom, not the instruction.

### 3.15 The exception return: the largest single win in Phase 2 (2026-09-21)

`SUBS`/`ADDS`/`MOVS pc, rn, #imm` with S set -- the IRQ exit -- inlined
(`f72421e`). Compute the target, restore CPSR from SPSR through the same
narrow-spill helper the mode switch uses (r13/r14 by hand; `call_pure` already
carries r8-r12 and reloads the host flags from the *restored* cpsr, which is
exactly what an exception return needs), then branch.

One subtlety that is not in the mode-switch version: the helper returns the
target with the restored T already in bit 0, because the new state comes from
SPSR and not from the address. `emit_branch_indirect`'s non-interworking path
would have taken T from the translate-time `thumb_`, which is wrong here.

Scoped narrowly on purpose: unconditional, immediate operand, SUB/ADD/MOV.
The register-operand forms and the other opcodes keep the interpreter, each
having its own operand-shift cycle behaviour.

| scene | base median | with the emitter | median | mean |
|---|---|---|---|---|
| gsdd | 21.494 | **20.880** | **-0.614** | -0.725 |
| sm64 | 9.370 | 9.269 | -0.101 | |
| mlbis | 12.239 | 12.214 | -0.025 | |
| dbori | 11.446 | 11.451 | +0.005 | |

Three reps, alternating order. Golden Sun's total wall fell 24.1 -> 23.3 s.
Serial census (the only valid kind, SS3.12): 3,517,232,540 -> 3,500,366,331.
Fuzzer: 1600 trials and 55 directed cases pass. Picture byte-identical to HEAD
over 800 frames of Golden Sun.

**Why it is worth seven times what `LDM ^` was, per occurrence.** The
encoding appears 120,458 times a run, but total fallback executions fell
1,070,861 -> 509,730 -- 561 k, more than four times the encoding's own count.
This was a **block-ending** fallback: every IRQ exit forced a dispatcher round
trip, 134 times a frame, and the block now continues through it. Against
SS3.13's `LDM ^`, which is mid-block:

| | occurrences removed | median won (gsdd) | per thousand |
|---|---|---|---|
| exception return (block-ending) | 120,458 | 0.614 ms | **5.1 us** |
| `LDM ^` (mid-block) | 137,231 | 0.094 ms | 0.69 us |

**So the rule SS3.10 was reaching for, correctly stated, is about block
shape rather than about work:** a fallback that ends a block costs the
dispatcher round trip and the truncated block as well as its own execution,
so the census *understates* it; a mid-block fallback costs roughly its own
execution, so the census *overstates* what removing it buys. Both of Phase 2's
two largest wins are block-ending fallbacks. That is the axis to sort any
future fallback census on, and it is not the axis either SS3.10 or SS3.13
used.

### 3.16 Phase 2, closed -- the tally, and the gate it left open (met in SS3.17)

**Delivered.** Device medians, RG DS Plus, three reps per step, alternating
order. The before-column is SS3.9's base (the tree as Phase 1 closed it); the
after-column is SS3.13's candidate.

| scene | before | after | | |
|---|---|---|---|---|
| gsdd | 21.769 | **20.643** | **-1.126** | **-5.2 %** |
| dbori | 11.879 | **11.345** | -0.534 | -4.5 % |
| sm64 | 9.509 | **9.230** | -0.279 | -2.9 % |
| mlbis | 12.276 | 12.260 | -0.016 | -0.1 % |

Step by step, each measured against its own base in its own session:

| step | commit | gsdd | sm64 | mlbis | dbori |
|---|---|---|---|---|---|
| two emitters (SS3.9) | `855e266` | -0.177 | -0.088 | +0.068 | -0.247 |
| wider idle skipping (SS3.11) | `017c4e5`, `ffb29f0` | -0.109 | -0.141 | -0.004 | -0.143 |
| mode switch (SS3.10) | `58fca58` | -0.180 | -0.084 | -0.061 | -0.040 |
| exception return (SS3.15) | `f72421e` | **-0.614** | -0.101 | -0.025 | +0.005 |
| `LDM ^` (SS3.13) | `83898bf` | -0.094 | -0.036 | +0.063 | +0.030 |
| **sum of steps** | | -1.174 | -0.450 | +0.041 | -0.395 |
| **end to end** | | **-1.126** | **-0.279** | **-0.016** | **-0.534** |

**The two columns disagree, and the disagreement is the honest error bar.**
They agree on gsdd (-1.174 against -1.126, 4 %) and diverge on the three
scenes where every step was inside the noise -- sm64 -0.450 against -0.279,
dbori -0.395 against -0.534, mlbis +0.041 against -0.016. Each step's A/B was
paired against a base built in its own session, and those bases drift by more
than the steps themselves move. **Only gsdd separates from the noise at
step granularity, and only the end-to-end column should be quoted.** Phase 2
is a Golden Sun result with three scenes that did not regress.

Against the phase's original **-3.5 ms** that is a third on the best scene --
but SS3.2 and SS3.3 had already retired that number before any of this was
written, and what remained had no target. Read against what the census
actually offered, the phase took the two block-ending fallbacks, the two
emitter clusters and the idle-skip relaxation, and left translated guest code
-- a third of the emulation thread, and the floor SS3.3 named -- untouched.

**What Phase 2 rejected, with the evidence.**

* **The per-access cost model** (SS3.2): 2.3-5.5 % of emulated instructions by
  costprobe, worth 0.2-0.4 ms, not the 3.5 ms the phase was scoped for.
* **`DS_JIT_FASTCOST`** (SS3.2): +3.16 % *slower* than the exact model.
* **Hoisting `cart_catch_up_slow`'s guard** (SS3.3): flat to 1.3 % worse.
* **Row 1.26, known-constant tracking** (`e51c6fd`).
* **`const_nd` as a flat cost model** (SS3.2 named it "the real floor" and
  "cheap to try because the code is written"). Tried, and worse on all four
  scenes: gsdd +0.28, dbori +0.10, sm64 +0.10, mlbis +0.055 ms of median, with
  every candidate rep above every base rep on dbori and mlbis. No commit or
  branch survives it -- it was a dead end, branched off the accepted lineage
  and abandoned -- but the device still carries the run
  (`/storage/dsperate-test/abconst.sh`, `dsperate-p2c`, built 23:14 on
  2026-09-20 against `dsperate-p2b` as its base), which is what the fixture
  `fixtures/device/phase2-constnd-ab.jsonl` holds. **With this, every lever
  SS3.2 and SS3.3 nominated has been measured, and the cost model is closed as
  a dead end.**

**Two traps in the device A/B records, found while confirming that.** Both
would mislead anyone re-reading the Phase 2 fixtures.

* **Every `ab*.sh` on the device carries the same header comment** -- "the
  idle-skip work (GX veto removed + advance_dma_only) against the tree with
  only the JIT emitters" -- because each was copied from the last as a
  template and the comment was never updated. It is accurate only for
  `abidle.sh`. Read the arms, not the header.
* **`phase2-modeswitch-ab.jsonl` labels its candidate arm `p2c`, but the
  binary it ran was `p2d`** (`abmsr.sh`: `b=dsperate-p2d; bt=p2c`) -- the label
  string was copied from `abconst.sh` along with everything else. The base is
  `p2b`, so the measurement itself is sound and the rejected `const_nd` arm is
  *not* in the mode switch's lineage. Only the label is wrong.

The accepted lineage, for the record: `dsperate-1c2` (Phase 1 closed) ->
`p2a` emitters -> `p2b` idle skipping -> `p2d` mode switch -> `p2e` exception
return -> `p2g` user bank (against `p2f`, the same tree with
`ldm_user_inline()` forced false). `p2c` is the abandoned `const_nd` branch
and appears in no later arm.

**The exit gate is not yet met.** Phase 2's exit criteria are self-consistency,
the perceptual gate, and -- "the real gate, not the four scenes" -- the
compatibility sweep. `fixtures/sweep/a64-jit-phase2.tsv` was produced at
`1f82522` and **three changes landed after it**: the mode-switch restore, the
exception return, and the `LDM ^` and `STM`/r15 work (`83898bf`). All three
touch banked registers or mode switching, which is precisely the class SS3.13
warns about -- "the failure mode is writing the wrong bank, which corrupts an
interrupted context and surfaces as a rare crash rather than a clean gate
failure". A 46-title sweep is ~25 s a title under qemu.

**So Phase 2 closes on paper.** The `LDM ^` emitter and the `STM`/r15
slow-path fix are committed (`83898bf`); `test_jit`'s 96 directed cases and
1600 fuzz trials pass against it. Three things carried into Phase 3a -- the
sweep, the cadence fixtures and the device before-line -- and all three are
done in SS3.17.

### 3.17 Phase 3a: the gate met, and two gates that were not gating

**The sweep (`fixtures/sweep/a64-jit-phase3a.tsv`).** 46 titles, 3600 frames,
AArch64 JIT under qemu. 39 ok, 7 static, 0 fail -- no title changed status and
the seven static titles are the same seven. **Phase 2's exit gate is met.**

But the status column is too coarse. Joining on `distinct` instead, three
titles moved, and one of them matters: **Final Fantasy III now freezes 43 %
earlier** (167 -> 95 distinct frames, `frozen` 0.9506 -> 0.9706). It was
already `static` before, so `status` could not see it. 95 is exactly what
Phase 6's table reads for "both recompilers, quantum 2048" on the current
tree, and distinct frames stop accumulating at the freeze, so the same count
at two different run lengths means the same freeze point. **The known
interleave defect is now reachable sooner** -- a better bisect target for the
scheduler work, and a worse thing to hit while playing. (Animal Crossing
+17.5 % and Advance Wars +2.2 % also moved; more distinct frames is not a
concern, but it is a reminder that the column is not the gate.)

**The cadence counters were never gating at all.** `gate.sh` printed a cadence
diff and returned success regardless -- the census that SS2 names one of the
four gates replacing byte-exactness was, in the code, a log line. Three of the
four fail the gate now, and the fourth deliberately does not:

* `3d frames kept`, `gx swap_buffers` and `gx vblanks with no swap` **gate**.
  They do not depend on which engine produced them -- the same tree reads
  850 / 1417 / 386 on sm64 from the x86 interpreter and from the AArch64
  recompiler.
* `gx reads of GXSTAT` is **reported, never fatal**. It is a poll count and
  moves with where the two engines' slices fall: 24,772 host against 24,372
  AArch64, 1.6 % apart with nothing wrong. SS3.13 spent a whole verification
  run proving one such diff was not its emitter's doing; that is the cost this
  avoids.

The fixtures' cadence half is re-baselined on the current tree (the picture
hashes are untouched and remain `exact-reference`'s -- the two halves have
different references by design, and `fixtures/golden/README.md`, previously an
empty file, now says so). **No scene draws fewer frames:** swaps are unchanged
on six of eight and rise by one on the other two. The GXSTAT falls -- sm64
-17 %, gsdd -6 %, st-intro -5 % -- are Phase 1's no-FIFO model removing the
reason to spin.

**Twice in one session, then, a gate that reported instead of judging.** Both
were found by re-reading what the gate script does rather than by trusting
what this document says it does. That is worth doing to the perceptual gate
and the self-consistency runner before Phase 3 leans on them.

**The device before-line (`fixtures/device/baseline-phase3.jsonl`).** Three
reps a scene on `dsperate-p2g` (byte-identical to `83898bf`), with gsdd added:

| ms | gsdd | mlbis | dbori | sm64 | etody |
|---|---|---|---|---|---|
| **frame median** | **20.67** | 12.30 | 11.33 | 9.26 | 5.05 |
| cpu9 | 8.08 | 5.45 | 8.08 | 3.17 | 0.88 |
| cpu7 | 1.42 | 1.28 | 0.66 | 1.27 | 0.68 |
| dma | **2.54** | 0.40 | 0.37 | 0.26 | 0.87 |
| spu | **1.13** | 0.41 | 0.14 | 0.59 | 0.17 |
| sched + events | 0.97 | 0.42 | 0.49 | 0.51 | 0.32 |
| gx_geom | **0.00** | **0.00** | **0.00** | **0.00** | **0.00** |
| **Phase 3 surface** | **4.64** | 1.23 | 1.00 | 1.36 | 1.37 |

**`gx_geom` reads 0.00 on every scene** against 1.05-1.50 ms on the old
baseline. ~~Phase 1 deleted the row rather than shrinking it.~~ **Wrong, and
corrected in SS3.18: Phase 1 deleted the INSTRUMENT.** `GX_RUN` has had no
scope site since, so the row could only ever read zero; the geometry work moved
into its callers' rows and is 3.56 ms of a typical Golden Sun frame.

**Golden Sun is the only scene outside the 16.74 ms budget** *of the five
measured* -- SS3.24 adds Spirit Tracks at 19.08 ms, which is not in this
baseline at all -- by 3.93 ms, and it holds 4.64 of the 9.59 ms of Phase 3
surface across all five scenes -- just under half. The
other four clear the budget already and offer 1.0-1.4 ms each across DMA, SPU
and the scheduler *combined*. **Phase 3's -2.5 ms target is therefore not
reachable on four of the five scenes, because the rows do not contain it**, and
on gsdd it means removing about 85 % of the whole surface. The -2.5 ms comes
from the 2026-09-16 NSMB profile -- SDL, `--dual-window`, two phases old, and
NSMB has never been a device scene in this rework. Phase 3 needs rescoping per
scene before it is committed to, the way SS3.3 rescoped Phase 2.

### 3.18 Phase 3b's census: Golden Sun's critical path is the 2D worker, not DMA

The first `perf` ever run on gsdd, plus the three profile stages it showed to
be lying. The scene carrying 48 % of Phase 3's surface, and the only one
outside the budget, is not spending its time where any version of this plan
said it was.

**The instrument came first, because three rows were wrong.**

* **`GX_RUN` had no scope site** and had had none since Phase 1, so `gx_geom`
  read 0.00 ms on every scene. SS3.17 recorded that as Phase 1 deleting the
  row. Phase 1 deleted the *instrument*: `drain_all()` runs wherever the
  command log fills or is observed — inside `DMA` on a GXFIFO burst, inside
  `CPU9` on a store or a GXSTAT read, inside `GX_VBLANK` at the swap — so the
  work simply moved into its callers' rows.
* **`GX_JOIN` had no site either**; it measured the geometry worker Phase 1
  deleted. It is now `W2D_JOIN`, the 2D line worker join, scoped inside
  `Gpu::join_worker()` itself. **Eight of its nine call sites had no scope at
  all**, including both in the lazy-2D VRAM write trap, which run inside the
  DMA scope.
* **The sum and `untimed` double-counted the nested rows.** `JIT_TX` was in
  both despite `profile.h` saying never to add it. This is why SS3.6 found
  negative unaccounted time and called it impossible — it *was* impossible,
  and it was this, not the summing of medians that section blamed.

**Golden Sun, device, 1100 frames from 700, per frame:**

| stage | fast | **typ** | p99 | tail delta |
|---|---|---|---|---|
| **2d worker join** *(of which)* | 0.196 | **2.782** | **16.317** | **+13.534** |
| cpu arm9 | 7.892 | 8.104 | 8.985 | +0.881 |
| **gx geometry** *(of which)* | 2.938 | **3.560** | 3.452 | -0.108 |
| dma | 1.894 | 2.595 | 2.720 | +0.125 |
| spu | 0.924 | 1.124 | 1.237 | +0.113 |
| sched slice loop | 0.692 | 0.865 | 1.055 | +0.191 |
| untimed | 1.741 | 4.858 | 17.148 | +12.290 |

and `perf` on the emulation thread agrees: `Dma::run_channel_impl` 9.81 %,
`__schedule` 9.49 % with `do_sched_yield` and `__sched_yield` another 3.1 %,
`submit_vertex` 7.69 %, `drain_all` 6.24 %. **No translated guest code appears
in the top thirty symbols at all** — the opposite of dbori and mlbis, where it
is a third of the thread.

**Three things follow, and they reorder the phase.**

**1. The emulation thread's largest single cost on this scene is waiting for
the 2D line worker.** 2.78 ms a typical frame, 16.3 ms at p99, and it accounts
for essentially the entire tail: nothing else contributes more than 1.5 ms to
p99. The site was read from the code as `Gpu::catch_up()` — **wrong, and corrected
in SS3.19: it is `render_ranges`' pre-join, 99.8 % against `catch_up`'s
0.21 %.** The two want different fixes, so the distinction matters.

**2. SS3.6's grounds for dropping Phase 5 do not hold.** That section dropped
the 2D engines on "0.64 ms on Golden Sun, 0.0 % join wait". The 0.64 ms is the
2D work done *on the emulation thread*; the join wait it quotes is `JOIN0`,
which scopes exactly one of the nine join sites and reads 0.002 ms. The line
worker is 22.7 % of the process by `perf` — comparable to a band worker — and
the emulation thread blocks on it. **The 2D engines are on Golden Sun's
critical path, and the decision to drop them was made with an instrument that
could not see the path.** This does not by itself reinstate Phase 5's
representation work (1-bit masks, planar split); it reopens the question, and
the cheaper answer may be the hand-off rather than the rendering.

**3. `cpu9` is not 8.1 ms of ARM9.** 3.56 ms of it is geometry replay, and an
unmeasured share of the join sits inside it too. Phase 2 spent itself against a
row that, on this scene, is under half what it reads.

**What this does to Phase 3's shape.** 3c (the cart) is unaffected and stays
first — it was argued from dbori and mlbis, where the census still stands. 3d
shrinks: with geometry lifted out, the genuine DMA row is well under 2.595 ms
and the -1.3 ms the phase wanted from it is not there. 3f (SPU, 1.12 ms) and 3e
(the scheduler, 0.87 ms) are unchanged and remain small. **The largest lever on
the only scene that misses the budget is now the 2D hand-off**, which is in no
phase's scope: Phase 3 does not cover it and Phase 5 was dropped.

*Measured in SS3.19, which answers both open questions:* the `untimed` row is
the same join, subtracted out of `GPU_LINE` and charged to nothing; and the
site is the pre-join in `render_ranges`, not `catch_up`. The lever is
pipelining, not correctness.

### 3.19 The untimed row *is* the join -- and the site is not the one SS3.18 named

Two corrections to SS3.18, both found by instrumenting rather than reasoning.

**The `untimed` row was the 2D worker join, subtracted and charged to
nothing.** `Gpu::on_hblank` opens a `GPU_LINE` scope and then subtracts the
whole `render_ranges` interval back out -- *"the draws account for
themselves"*, and they do, through `BG_DRAW`, `SELECT` and the rest. But the
**worker join inside `render_ranges` has no scope of its own to add itself
back**, so subtracting it charged it to nothing at all. Removing only the draw
time:

| Golden Sun, device, 1100 frames | fast | typ | p99 |
|---|---|---|---|
| untimed, before | 1.812 | **5.013** | **21.519** |
| untimed, after | 1.609 | **2.138** | **4.546** |
| gpu line hooks, before | 0.829 | 0.790 | — |
| gpu line hooks, after | 0.829 | **3.424** | **13.922** |
| 2d worker join *(of which)* | 0.178 | 2.837 | 13.509 |

`untimed` falls by 2.88 ms typical and **17.0 ms at p99**, and `gpu_line` rises
to hold it. What remains unaccounted is 2.14 ms, which is no longer the largest
thing on the scene.

**The site is `render_ranges`' pre-join, not `catch_up`.** SS3.18 named
`catch_up` from reading the code. Measured per call site, over the run:

| site | join time | share |
|---|---|---|
| **`render_ranges` pre** | **4.81 s** | **99.8 %** |
| `catch_up` | 10.3 ms | 0.21 % |
| vram trap | 1.2 ms | 0.03 % |
| vram remap | 0.4 ms | |
| `render_ranges` post | 0.6 ms | |
| line 0 (`JOIN0`) | 0.1 ms | |

That is a different shape of problem. `catch_up` waits because a store must
not overtake a line -- a correctness join, hard to remove. The **pre-join
waits for the previous run to land before handing off the next**, which is a
pipelining problem. Lag mode already skips the *post*-join and leaves a run in
flight, giving one line of slack; the pre-join at the next HBlank is where that
slack is spent. **On this scene one line is not enough: the emulation thread
reaches line N+1 before the worker has finished line N.**

**An instrument that answered the wrong question, recorded because it nearly
stuck.** A leaf-scope depth counter was built first, and it reported the join
**99.99 % "inside a leaf scope"** -- which is true, and irrelevant. `GPU_LINE`'s
`Scope` object *is* open across the join; it is the *time* that is subtracted
out from under it. "Is a scope open" and "is the time attributed" are different
questions, and only the second one mattered. On the strength of the first the
join would have been written off as already accounted for. The depth machinery
is reverted; the per-call-site counters, which were decisive, stay.

That is the third instrument in two days to give a confident wrong answer --
after the threaded instruction census (SS3.12) and the slice-counting idle
survey (SS3.11). The pattern in all three is the same: **the instrument
measured something adjacent to the question and the adjacency was not
checked.**

### 3.20 The 2D worker, on and off: its join time predicts whether it pays

`DS_2D_THREAD=0` renders both engines inline -- no worker, no hand-off, no
join. Three reps, alternating arm order, `dsperate-p3b`, medians of three.

| scene | arm | median | mean | p90 | p99 | max | join |
|---|---|---|---|---|---|---|---|
| **gsdd** | worker | 20.754 | 20.806 | 25.410 | **32.526** | **46.072** | 2.794 |
| | inline | 20.918 | 21.483 | 25.526 | **28.087** | **31.563** | — |
| | *delta* | +0.164 | +0.677 | +0.117 | **-4.440** | **-14.509** | |
| **mlbis** | worker | 12.236 | 12.158 | 14.126 | 18.319 | 30.029 | 0.013 |
| | inline | 13.992 | 13.832 | 15.704 | 19.483 | 31.934 | — |
| | *delta* | **+1.756** | +1.674 | +1.577 | +1.165 | +1.905 | |
| **dbori** | worker | 11.355 | 11.528 | 14.776 | 20.939 | 36.889 | 0.346 |
| | inline | 12.064 | 12.222 | 15.855 | 20.249 | 33.161 | — |
| | *delta* | +0.709 | +0.695 | +1.080 | -0.690 | -3.727 | |

**The join time is the diagnostic, and it sorts the three scenes exactly.**

* **mlbis, join 0.013 ms** — the worker overlaps completely and is a pure win:
  inline is worse on every statistic, by 1.76 ms of median. Nothing to fix.
* **dbori, join 0.346 ms** — mostly overlapped. The worker wins the median by
  0.71 and loses the tail slightly.
* **gsdd, join 2.794 ms** — barely overlaps at all. The worker buys **0.16 ms
  of median** and costs **4.44 ms of p99 and 14.5 ms of max**.

That is the same trade SS3.4 measured for the geometry worker and the plan
deleted it on -- *"it is not a win being given up, it is a liability"* -- except
that here it is scene-dependent, so deletion is not the answer. **What the join
measures is precisely the part of the worker's work that failed to overlap**,
and on Golden Sun that is nearly all of it: the thread waits for the run
anyway, and pays the hand-off and the contention on top.

**Why Golden Sun and not the others is the open question**, and there are two
candidates that the runs above cannot separate:

1. **Core count.** gsdd runs three band workers *plus* the line worker plus the
   emulation thread -- five threads on four A55s. The band sweep re-run in
   SS3.5 settled three band workers as right, but it did not vary the line
   worker, so the interaction has never been measured. `DS_R3D_THREADS=2` with
   the line worker on is the arm that separates this, and it is one run.
2. **One line of slack is not enough.** Lag mode leaves a run in flight and the
   next HBlank's pre-join collects it (SS3.19). If the worker needs longer than
   the emulation thread's own per-line work, the thread stalls every line no
   matter how many cores are free.

If it is (1), the fix is a band/line worker budget and costs nothing else. If
it is (2), the fix is a deeper queue, which is a real change to the hand-off
and to `render_ranges`' contract.

**(1) is answered and it is not the cause.** gsdd was in the post-1b band
sweep after all (`fixtures/device/band-sweep-post1b.jsonl`): two band workers
against three reads median 22.059 against 22.096 — noise — while p99 goes
**35.707 against 33.438** and max **50.090 against 44.424**. Freeing a core by
dropping a band worker makes the tail *worse*, because the raster then becomes
the constraint. So the line worker is not starved of a core, and (2) stands as
the explanation: one line of lag slack is not enough on this scene.

`DS_2D_SPLIT`, which would have sidestepped the question by keeping engine A
batched, is measured in SS3.22 and does nothing — it has been inert since the
day after it was written.

**What this does not support** is deleting the 2D worker, which mlbis settles,
or reinstating Phase 5's representation work on this evidence. A cheaper line
renderer would shrink the join, but so would handing the worker less to do, and
neither has been priced against the other yet.

### 3.21 What the VRAM write trap is for, and why GXSTAT does not bear on it

Asked while reading SS3.19, and worth writing down because the premise is a
natural one and both halves of it are wrong.

**The trap is not there for the 3D bands or for threading.** It arrived with
lazy 2D (`e744436`), which defers the whole frame's 2D render to the last
display line, and its own commit message says why: *"VRAM cannot be journaled,
so it is trapped... the first trapped store of a frame renders every line whose
HBlank has passed **before** the bytes change."* Registers, palette, OAM,
POWCNT and MASTER_BRIGHT can be journaled and replayed in hardware order; VRAM
is far too large for that, so a read-after-write guard takes its place. It is
tied to **deferral**, not to threads -- it arms with `DS_2D_THREAD=0` as well,
and `8bdcd13` stops arming it on frames that never batch. Remove it while the
render is still deferred and the engines read bytes the guest has already
overwritten.

**GXSTAT is not synthesised, and an attempt to synthesise it was rejected.**
The append-time shadow was built on 2026-09-20 and reverted the same day; the
comment survives at the `drain_all()` in the register read path. It cannot work
without the geometry worker: the worker executed concurrently, so a poll
answered from a shadow still saw the wait end, while single-threaded nothing
advances between polls and the guest spins to VBlank. **Dragon Ball Origins'
GXSTAT reads went 1.55 M to 37 M and SM64DS's picture did not move at all.**
What replaced it is the opposite of synthesis -- *sync on observation*, a full
replay before any read is answered. There is no new guarantee there to lean on.

**And the trap is not where the time is.** Per call site (SS3.19), trap joins
are 1.2 ms of 4.82 s across the run -- **0.03 %** -- against `render_ranges`'
pre-join at 99.8 %. On Golden Sun the trap fires 129 times a run and costs
about 188 slow stores a frame. Deleting it returns none of the 2.84 ms.

**Where the question does land.** There is a live row for removing the trap,
and it is already in Phase 5: *"the VRAM write trap (4.11) that makes our lazy
path hardware-exact can become DraStic's rule — ignore CPU stores into VRAM
mid-frame — if the perceptual gate allows it."* `e744436` records keeping the
trap as *"a deliberate departure from DraStic... the output stays
hardware-exact"*, and the speed-first bar has retired that exactness. So the
justification is the accuracy bar, not anything about GXSTAT -- and the
plan's own census expects it to be worth little outside Golden Sun.

### 3.22 `DS_2D_SPLIT` measured on the current tree: null, and inert by construction

The per-engine lazy-2D split (`508b7bc`) was measured at **mean -5.1 %, median
-4.9 %, p99 -11.5 %** on Golden Sun and left opt-in *"until the other scenes
are measured"*. SS3.20 made it the obvious candidate for the pre-join stall:
it takes engine A from 192 per-line renders a frame to one batch, and a
batched engine A uses the deferred join rather than the per-line pre-join.

**Measured, five scenes, three reps, alternating:**

| scene | Δ median | Δ mean | Δ p99 | join base -> split |
|---|---|---|---|---|
| gsdd | +0.076 | +0.039 | -0.396 | **2.898 -> 2.880** |
| mlbis | -0.011 | +0.007 | -0.723 | 0.023 -> 0.035 |
| dbori | +0.040 | +0.020 | -0.619 | 0.324 -> 0.353 |
| sm64 | +0.008 | +0.011 | +1.124 | 0.010 -> 0.023 |
| etody | -0.027 | -0.016 | +0.728 | 0.036 -> 0.020 |

**Nothing, on any scene.** And the join column says why: on Golden Sun it does
not move. The split never acts at all.

**It was made inert the day after it was written.** `8bdcd13` ("stop arming the
lazy trap on frames that never batch") landed on 2026-09-01, on by default,
against the same Golden Sun behaviour, and took the title from 23.5 to 22.2 ms
median with the dma stage 6.87 -> 2.96 and trap hits 4800 -> 128 a frame. Its
rule sets `lazy_frame_ = false` once a scene is futile — so **the frame does
not batch at all**, and the split only decides *which engine leaves a batch
that never starts*. Re-probe frames come one per `lazy_probe_period_`, which
doubles to 1024. The counters confirm the state on the current tree: 232 lazy
frames against 1569 skipped, 129 trap hits.

So the split's headline number was not merely measured against a superseded
baseline — the commit that superseded it removed the conditions the split needs
to do anything.

**Recommendation: delete it rather than ship it.** It returns nothing on five
scenes; it carries a real hazard that its own follow-up recorded (`31a7ab6`:
lifting the trap while the other engine still batches *"would let a store into
that engine's vram land unseen before its batch renders. Golden Sun hit it 13
times a frame and produced correct frames only because it writes nothing to
engine A — a title that streams to both would have dropped a frame"*); and it
is another per-subsystem knob, which SS2 counts as a cost in itself. The five
scenes pass only because none of them streams to both engines, so the gate we
have cannot retire that risk.

*Picture, both arms, host, against the golden reference:* all eight scenes pass
with the split on and off, and the differences are identical in both arms
(etody 1772/1800 exact with 223 realigned, artacd 1799, st-intro ~1948) — they
are pre-existing, verified by running the control rather than assumed. gsdd is
2400/2400 exact with the split and 2399/2400 without. **Zero cadence movement
on any counter on any scene, in either arm.**

*Also recorded in `31a7ab6`, so it is not re-tried:* arming the trap per engine
measured **+0.9 %** against the split's -5.1 % and was rejected — a page is
2 KB, so each toggle rewrites 8192 page-table entries and the 8-line bursts
toggle it ~31 times a frame.

### 3.23 The gate could not name the failure the bar is set on

The user's acceptance bar, stated plainly: **blatantly incorrect rendering
(missing geometry, poor textures), and DraStic's threaded-3D failure on
capture-heavy titles — screens flipping rapidly, or a mistaken latch or delay
rendering onto the opposite one.** Everything else is negotiable.

Checked rather than assumed, and the `swapped` detector could not see half of
it. `--fault-persist` requires **4 consecutive frames**, and runs are collapsed
per kind per screen, so a fault that *alternates* — one frame wrong, one right
— has every run at length 1 and never reaches the threshold however long it
continues. Eight of twenty-four frames with the screens exchanged reports as
`(transient, under --fault-persist: swapped 8 frame(s))` and nothing more.

**It was not passing such a build, and the first reading of this said it was.**
With floors applied it fails `ssim_min`: a swapped frame scores ~0.49 against
floors of 0.90-0.94. But it fails *unnamed* — three ssim rows and no statement
of what is wrong — and *incidentally*, because the floors exist to catch
gradual degradation and `floors.json` says outright that they are to be tuned
per scene as phases land. sm64's `ssim_min` is already 0.900 against a measured
0.9146. A structural fault should not rest on a number under that kind of
pressure.

`--fault-total` (`53c5e48`) counts frames of a kind anywhere in the run
regardless of adjacency: swapped 3, others 8. The tight swapped budget is
affordable because **all eight golden scenes produce zero swapped marks**, so
it only has to cover a genuine POWCNT screen swap landing a frame early. The
self-test gains the flapping case and its converse — two scattered swaps must
still be forgiven — and the existing transient cases still pass, so the
persistence rule is intact. gsdd, etody, artacd and st-intro pass unchanged.

**This is the fourth gate in two days found to be reporting rather than
judging**, after the cadence counters (SS3.17), the sweep's status column
(SS3.17) and `GX_RUN`/`W2D_JOIN` (SS3.18). The remaining unaudited one is the
self-consistency runner, and it should be audited before Phase 3 leans on it.

### 3.24 Spirit Tracks joins as hard as Golden Sun, by a completely different route

The join census extended past the four A/B scenes, because Spirit Tracks was
expected to look like Golden Sun -- another per-line streamer, with edge
marking on top. It does not. It joins just as badly and almost nothing about
the mechanism is the same.

| scene | frame median | join typ | join p99 | join tail | lazy frames | dominant site |
|---|---|---|---|---|---|---|
| gsdd | **20.96** | **2.947** | **15.689** | **+12.742** | 232 / 1801 | `render_ranges` pre, 99.8 % |
| **st-intro** | **19.08** | **0.872** | **9.649** | **+8.777** | **2401 / 2401** | **vram remap, 99.85 %** |
| meteos | 7.79 | 0.004 | 0.353 | +0.349 | 1801 / 1801 | — |

**Two things this settles and one it opens.**

**1. Spirit Tracks is also outside the budget, and was never in the
before-line.** 19.08 ms against 16.74. `fixtures/device/baseline-phase3.jsonl`
covers gsdd, mlbis, dbori, sm64 and etody, so SS3.17's and SS3.22's "Golden
Sun is the only scene outside the budget" is true of the scenes measured and
false of the scene set. **Two scenes miss, not one**, and Phase 3's framing
must carry both. (`docs/frame-profile-2026-09-16.md` had Spirit Tracks as the
worst scene of its four at 21.3 ms; it was dropped from the device work when
it became a boot scene, and the omission was never noticed.)

**2. Batching is not a defence against the join.** Spirit Tracks batches
*perfectly* -- 2401 lazy frames of 2401, no futile frames, no lag frames --
and still spends 0.87 ms a typical frame and 9.65 ms at p99 waiting for the
worker, which is the largest single contributor to its tail exactly as on
Golden Sun. The two scenes are the opposite of each other in every respect
except the outcome:

* **Golden Sun** never batches (232 lazy frames of 1801), so `render_ranges`
  hands off per line and pays the pre-join every line -- **many cheap waits**.
* **Spirit Tracks** batches every frame, and a VRAMCNT remap then forces the
  whole pending frame out. `Gpu::vram_remap_begin()` calls `catch_up(3)` --
  both engines, everything pending -- and joins, unconditionally. **2485 join
  calls in the run against Golden Sun's hundreds of thousands, at roughly
  700 us each**: about one flushed batch per frame.

So the lever that would fix one does nothing for the other, and neither is the
deeper-queue change SS3.20 proposed. A deeper queue helps Golden Sun's
per-line hand-off and is irrelevant to a remap that must flush by definition.

**3. What to look at for Spirit Tracks.** `vram_remap_begin` catches up *both*
engines and joins for *any* mapping change. The proposal here was to narrow
that to the engines whose views actually move. **Censused in SS3.25 and it is
the wrong lever** -- 99.9 % of remaps do move one. The right one is that the
catch-up was never the cost (0.4 % of remaps have lines pending); the
unconditional `join_worker()` after it is, and a job in flight should render
against the mapping it was handed rather than the live one.

*Coverage gap found while doing this:* Art Academy is in `games-bugtest` on
the host but **is not on the device at all**, so `artacd` has never been part
of any device measurement in this rework. It is the most 2D-heavy title in the
set and the likeliest third scene to have a join problem. Copy the ROM over
before the next census.

### 3.25 The remap census: the obvious lever is not there, and the right one is a snapshot

SS3.24 proposed narrowing `vram_remap_begin`'s catch-up to the engines whose
views a remap actually moves -- the test the 3D path beside it already makes
(*"whether the band workers must be joined depends on whether the two views
they index actually move, and most VRAMCNT traffic ... leaves them alone"*).
Censused, 2400 frames:

| | remaps | a 2D view moved | none moved | **lines pending** |
|---|---|---|---|---|
| st-intro | 5951 | **5945 (99.9 %)** | 6 | **24 (0.4 %)** |
| mlbis | 6194 | 6163 (99.5 %) | 31 | 44 (0.7 %) |
| gsdd | 411 | 405 | 6 | 1 |

**The lever is not there.** Where the 3D views mostly stay put, the 2D views
almost always move -- 99.9 % on Spirit Tracks -- so a test on "did an engine's
view move" would skip essentially nothing. That idea is dead.

**But the last column is the finding.** Only **24 of 5951** remaps had any
lines pending, so `catch_up(3)` returns immediately 99.6 % of the time. The
catch-up was never the cost. **`join_worker()` is called unconditionally
afterwards**, and *that* is what waits: ~2488 joins that actually blocked, 1.75 s
between them, about **700 us each and one per frame** -- the whole of a batched
frame's 2D render.

So the sequence on Spirit Tracks is:

1. Line 191: engine A's frame batches and is handed to the worker as a
   *deferred* job -- the one hand-off designed **not** to block, so the render
   overlaps the next frame's emulation.
2. VBlank: the game rewrites VRAMCNT, as games do.
3. `vram_remap_begin` joins the worker immediately, and the deferral buys
   nothing at all.

**The lever is to give the worker its own view, not to skip the join.** A
remap moves the *mapping*; it does not move the banks, whose storage is fixed
(A..I) and shared either way. A job in flight should render against the
`VramMap` as it stood when it was handed off, which is exactly what those
lines are supposed to read -- and then a remap has nothing to wait for. The
copy is affordable and already exists: `Bus::update_vram` builds a whole
`VramMap` copy on *every* remap and the comment calls it *"a few KB of plain
arrays"*.

This is the same insight lazy 2D started from, applied one level up. That
design says *"VRAM cannot be journaled, so it is trapped"* -- true of VRAM's
**contents**, and false of its **mapping**, which is nine VRAMCNT bytes.
Registers, palette and OAM are journaled; the mapping can be too.

**The hazard, stated before anything is written.** A snapshot keeps pointing at
banks the guest may now write through a *new* mapping, and the write trap
covers the pages the engines read *now*, not the ones a job in flight still
needs. So the trap's coverage has to follow the job, not the live map. That is
the real work in this change, it is the same class of bug `31a7ab6` had to fix
for the per-engine split, and it is the reason this is a Phase 3 item with a
compatibility sweep rather than an afternoon's patch.

*Bounding it first:* count the remaps that both move a view a job in flight is
reading **and** are followed by a write into the moved bank before that job
lands. If that is zero on the scene set, the snapshot is nearly free and the
trap work is insurance; if it is common, this is a much larger change.

### 3.26 The snapshot's value and its danger are the same scenes

Before implementing the per-job `VramMap` snapshot (SS3.25), the question asked
was whether games that swap their engines every frame would be at risk, and
whether the capture scenes cover them. Censused, all eight scenes:

| scene | frames | alternating phase | capture on | **remaps in flight** | join calls |
|---|---|---|---|---|---|
| mlbis | 1801 | 0 | 0 | 973 | 2993 |
| meteos | 1801 | 0 | 0 | 26 | 2448 |
| sm64 | 1801 | 0 | 3 | 148 | 14809 |
| artacd | 1801 | 0 | 0 | 77 | 11150 |
| etody | 1801 | 783 (43 %) | 825 | 874 | 2448 |
| dbori | 1801 | 914 (51 %) | 922 | 943 | 1913 |
| gsdd | 2401 | 1269 (53 %) | 1276 | 27 | 418981 |
| **st-intro** | 2401 | **2163 (90 %)** | **2170 (90 %)** | **2179 (91 %)** | 2488 |

**"Remaps in flight" is the population the snapshot changes** — the remaps that
block today and would not with it.

**The alternation is the capture.** Phase-alternating frames and capture frames
track each other almost exactly on every scene that has either (783/825,
914/922, 1269/1276, 2163/2170). `gpu.h` predicted this — *"a capture can
alternate between two destination banks the same way"* — so the engine swap
the question was about is, in this scene set, capture double-buffering rather
than a bare POWCNT1 flip.

**And that is the problem.** Spirit Tracks is **90 % capture, 90 % alternating
phase, and 91 % of its frames take a remap while a job is in flight**. The
scene the snapshot exists to fix is the scene most exposed to it. The four
scenes where the change would be unambiguously safe — mlbis, meteos, sm64,
artacd, no capture and no alternation between them — have 1224 in-flight
remaps combined against st-intro's 2179, and none of them has a join problem
worth fixing.

**So the safe version of this change is worth nothing, and the valuable
version lands exactly on the failure mode the bar names** — a mis-latch
putting a frame on the wrong screen, on capture-heavy titles. Gating the
snapshot on `!capture_on_ && phase_period_ == 1` would be provably safe and
would return nothing on st-intro or gsdd.

**Refined once the right thing was measured.** The table above counts frames
with capture on; the question that matters is whether the *job in flight* is a
capture job, and `capture_on_` is cleared at VBlank before these remaps land,
so it is trivially false at that point and says nothing. `capture_render_`,
latched when the job was handed over, is the signal:

| in-flight remaps | total | on an alternating frame | **in-flight job is a capture** | clean |
|---|---|---|---|---|
| st-intro | 2179 | 2162 (99.2 %) | **2169 (99.5 %)** | 10 |
| dbori | 943 | 913 | **920 (97.6 %)** | 23 |
| etody | 874 | 779 | **821 (94 %)** | 53 |
| gsdd | 27 | 0 | 0 | 27 |

**The jobs do not alternate with the captures** — the job in flight at remap
time *is* the capture job, 99.5 % of the time on Spirit Tracks. Gating on
"not alternating" keeps 10 of 2179 there, so it is worth nothing on the scene
that needs it, and the same for capture.

**But most of what that implies is already defended.** A capture job in flight
writes an LCDC bank the guest might read before the join, and `render_ranges`
already sets an LCDC *read* trap on exactly that bank when it hands the job
over — and `vram_remap_begin` does not touch it, so **the guard survives the
remap**. It is cleared only in `finish_a()`, at the frame's end, and in
`prepare_load()`. So the loudest-sounding hazard, a guest read of a
half-written capture bank, has machinery for it already.

That leaves the residual hazard much narrower than "capture is dangerous":

* What the job reads to composite — the BG/OBJ views a remap moves — is what
  the snapshot **fixes**, since those lines are meant to read the old mapping.
* What the guest *writes* into a bank the snapshot still references, where that
  bank has left the engines' current view and is therefore no longer trapped,
  is the one real hole. **That is the SS3.25 hazard, unchanged: the write
  trap's coverage has to follow the job rather than the live map.**

**This is not a refusal, it is a scoping result.** Three ways forward, in the
order they should be tried:

1. **Narrow the join instead of removing it.** `moved_2d` already says *which*
   engine's views a remap moves, and it is computed and thrown away. If most
   remaps move only one engine's views, a per-engine job and a per-engine join
   would halve the waits with none of the snapshot's exposure. **Census this
   first: it is one counter split three ways and one run.** (`join_worker()`
   joins the whole worker today, so this needs the jobs separable — which is
   a real change, but a local and checkable one.)
2. **Snapshot, gated on capture being off**, shipped and swept, and only then
   extended to capture frames with its own sweep. This gets the mechanism
   proven on the scenes where it is safe before it is pointed at the scenes
   where it matters.
3. **Snapshot everywhere**, with the compatibility sweep as the gate and the
   flapping-swap detector (SS3.23) as the specific instrument. Note what this
   rests on: the sweep runs 46 titles for 3600 frames *with no input*, and the
   failure being guarded against is a wrong-screen latch — which SS3.23's
   `swapped` detector now catches, but only against a reference dump, which
   the sweep does not produce. **The sweep as it stands could not see this
   bug.** That gap should be closed before route 3 is taken, not after.

### 3.27 Why the join is really there: the worker reads the live `VramMap`

The remap join has been treated throughout SS3.24-SS3.26 as a *semantic*
guard — make the pending lines land before the mapping they read changes.
Reading the code it is mostly a *synchronisation*, and that reframes the whole
change.

**The deferred 2D job reads `Bus::vram_map_` live, on the worker thread.**
`Engine2D::vram()` is `return nds_.bus.vram_map();` — the live object, not a
copy — and `Gpu::capture()`, which runs inside the job, opens with
`const VramMap& vm = nds_.bus.vram_map();` and consults `vm.lcdc_mask` and
`vm.bank()` at render time. Meanwhile `Bus::update_vram` assigns
`vram_map_ = next` on the emulation thread. **The join in `vram_remap_begin`
is what stops those two overlapping.** Remove it naively and the worker is
reading a `VramMap` while it is being rebuilt underneath it.

So the cost is not the price of correctness in the emulated machine. It is the
price of the worker and the emulation thread **sharing one mutable object**,
paid once a frame at about 700 us on Spirit Tracks.

**Two things make the snapshot cheaper than it first looked.**

1. **Bank storage never moves.** `Bus::vram_bank(i)` is `vram.get() + off`
   over fixed `VRAM_BANK_SIZES`, and `VramMap::rebuild` copies those same
   pointers into `banks_` every time. So `vm.bank(i)` is **remap-invariant**;
   a remap moves the *views* and `lcdc_mask`, never the storage. A job's
   snapshot therefore does not have to own memory, only a description.
2. **`Gpu::capture()`'s only remap-sensitive reads are `lcdc_mask` and the
   source-bank choice** — its destination pointer is `vm.bank(dst_bank)`,
   which by (1) is stable. Latching a couple of words at hand-off would take
   capture off the live map entirely.

**The 3D side already solved its half of this the narrow way.** `Renderer3D`
also takes `vm_ = &nds_.bus.vram_map()`, and `Bus::update_vram` calls
`sync_raster()` only when the texture or texture-palette views actually move —
*"whether the band workers must be joined depends on whether the two views
they index actually move, and most VRAMCNT traffic ... leaves them alone"*.
The 2D side cannot use that test, because SS3.25 measured that its views move
on 99.9 % of remaps. **It has to go the other way: stop sharing the object.**

**So the change to make is one thing, and it is smaller than SS3.25 implied:**
give a dispatched job the mapping description it should render against, and
`vram_remap_begin`'s join goes with it. That is also the right answer on the
merits — a job rendering lines 0..191 *should* read the mapping those lines
were displayed under, not whatever the guest has installed since.

**What still has to be handled, unchanged from SS3.25/SS3.26:** a guest write
into a bank the snapshot still references, where that bank has left the
engines' current view and is no longer write-trapped. The capture *read* side
is already covered (SS3.26: the LCDC read trap is set at hand-off and survives
the remap). This is the one hole, it is on the write side, and it is a
question about what the trap covers rather than about the snapshot.

*Recommended order:* latch capture's two words first — it is self-contained,
testable on its own, and takes the hottest reader off the live map — then the
job-wide snapshot, then the trap-follows-the-job work, each with the gate and
the sweep. And close the sweep's blind spot first (SS3.26 route 3): it runs
with no input and no reference dump, so the `swapped` detector cannot run
against it, and a wrong-screen latch is what all of this risks.

### 3.28 The remap join removed: Spirit Tracks -0.44 ms, and a regression not explained

The per-job snapshot of SS3.27, built and measured. `Engine2D::vram()`,
`Gpu::capture()`, `output_line()`'s VRAM-display case and the split/scale line
key all read `Gpu::vram_render_` — a `VramMap` copy taken at dispatch, and only
when `generation()` has moved — so **nothing on the worker touches
`Bus::vram_map_` any more** and `vram_remap_begin()`'s join is gone.

The whole 2D side turned on one accessor: all 28 readers in `engine2d.cpp` go
through `Engine2D::vram()`, so pointing that at the snapshot moved them
together.

**Device, five scenes, three reps, alternating; both arms carry the snapshot
and differ only in the join:**

| scene | Δ median | Δ mean | Δ p99 | join base -> none | verdict |
|---|---|---|---|---|---|
| **st-intro** | **-0.442** | -0.257 | -0.732 | **0.935 -> 0.003** | **clean win** |
| dbori | -0.061 | -0.174 | -0.512 | 0.303 -> 0.012 | clean win |
| gsdd | +0.026 | -0.010 | -1.036 | 2.916 -> 2.966 | noise |
| etody | -0.085 | -0.076 | +0.089 | 0.030 -> 0.000 | noise |
| **mlbis** | **+0.328** | +0.349 | -0.082 | 0.016 -> 0.027 | **clean regression** |

"Clean" means every rep of one arm falls outside every rep of the other.

**The join is gone where it existed** — st-intro 0.935 -> 0.003, dbori 0.303 ->
0.012 — and Spirit Tracks drops 18.99 -> 18.55 ms. gsdd is untouched exactly as
predicted: its join is `render_ranges`' pre-join, a different problem that this
does not address.

**A race this found, and a hypothesis it killed.** mlbis regressed in the first
run too, on a scene with essentially no remap join to remove, which made no
sense as contention. Chasing it found that `vram_remap_begin` also called
`Engine2D::vram_remapped()`, clearing `extpal_checked_`/`objext_checked_` —
**Engine2D members the worker reads.** The join had been protecting those as
well as the `VramMap`, and mlbis has 6194 remaps, the most of any scene, so it
fired into a live worker ~3.4 times a frame. That is now deferred to the next
dispatch, with the snapshot, which is also the right moment: a job rendering
against the old mapping should keep the validation that belongs to it.

**It was not the cause.** With the deferral in, mlbis reads +0.328 against the
first run's +0.271 — unchanged within the session-to-session spread. The race
was real and worth fixing on its own merits; it was not the regression.

**So the regression is unexplained, and is recorded as such.** The leading
hypothesis is contention: mlbis is the scene where the 2D worker earns most
(SS3.20 — inline is 1.76 ms worse), so its line worker is busy, and with the
join gone five threads contend for four A55s where the join used to park one
3.4 times a frame. That is a guess. It has not been measured and should not be
repeated as though it had been.

**The trade, stated plainly:** -0.44 ms on a scene 1.8 ms over budget, against
+0.33 ms on a scene 4.4 ms inside it. On Phase 3's own target — gsdd and
st-intro inside 16.74, no regression elsewhere — this buys the first half and
breaks the second. It is a good trade for the goal and a real cost, and the
right call is not obvious without knowing why mlbis moves.

*Gates:* the picture and cadence gates pass on all eight scenes at every stage
— snapshot alone, join removed, revalidation deferred — and match the control
where they are not byte-exact. **This means less than it looks:** the golden
configuration is serial (`DS_R3D_THREADS=0 DS_2D_THREAD=0`), so a gate run
cannot exercise the worker at all and could not have caught the `vram_remapped`
race. The race was found by a performance regression, not by a gate.

*Still open, from SS3.25 and unaddressed here:* a guest write into a bank the
snapshot still references, once that bank has left the engines' current view
and is therefore no longer write-trapped. The trap's coverage does not follow
the job. Nothing above tests for it, and the clean gates should not be read as
covering it.

### Phase 3 — DMA, SPU and scheduler granularity (week 4-5) — target **per scene, see below**

**Rescoped 2026-09-21 against SS3.17's before-line, the way SS3.3 rescoped
Phase 2.** The original shape — three rows, one -2.5 ms number, DMA 1.5 → 0.2
and SPU 0.9 → 0.1 — was arithmetic from the 2026-09-16 NSMB profile, taken in
the SDL frontend with `--dual-window` on a tree two phases old, on a scene that
has never been device-measured in this rework. Measured on the device, the
rows do not contain that much time on four of the five scenes.

**Phase 3 is the Golden Sun phase** — and, since SS3.24, the Spirit Tracks
phase with it. Both miss the budget; the table below is the five scenes that
were in the before-line, and Spirit Tracks at 19.08 ms is a sixth that was
not:

| | gsdd | mlbis | dbori | sm64 | etody |
|---|---|---|---|---|---|
| frame median | **20.67** | 12.30 | 11.33 | 9.26 | 5.05 |
| over/under 16.74 | **+3.93** | -4.44 | -5.41 | -7.48 | -11.69 |
| Phase 3 surface | **4.64** | 1.23 | 1.00 | 1.36 | 1.36 |

Four of the five are already inside the budget and hold 1.0-1.4 ms each across
DMA, SPU and the scheduler *combined*; there is no -2.5 ms in them to find.
Golden Sun misses by 3.93 ms and holds 4.64 ms of surface; Spirit Tracks
misses by 2.34 ms and has never had a stage baseline taken. **So the target
is: gsdd and st-intro inside 16.74 ms, and no regression anywhere else** --
and the first thing Phase 3 owes itself is a before-line for the scene it
forgot. Hitting it means taking
about 85 % of gsdd's DMA, SPU and scheduler time, which is a harder ask than
the -2.5 ms it replaces, and it is the honest one.

The ordering below is by evidence per unit of risk, and it deliberately does
not follow the subsystem order in the phase's title.

#### 3b — Census before commit, on Golden Sun

Every premise in the original bullets is now old enough to re-check, and one
is provably stale (3d). The Phase 2 rescope is the precedent: *"a `perf` share
says where the time is, not why"*, and the plan committed weeks to a row it
had not profiled once.

* **`perf` gsdd on the device.** The only device census in
  `fixtures/device/README.md` covers dbori and mlbis. Golden Sun — the scene
  carrying 48 % of Phase 3's surface and the only one missing the budget — has
  never been profiled. Do this first and let it choose between 3d, 3e and 3f.
* **Split `join_worker()` out of the DMA scope.** `prof::Scope` is plain
  wall-clock with no nesting subtraction (`profile.h`), and the DMA scope in
  the slice loop wraps `dma.run()`, which calls `gpu.vram_store_trap()`, which
  can join the line worker and burst-render 2D lines. The 2D rendering is
  double-counted into `bg_draw` as well and is therefore bounded (gsdd's 2D
  rows total 0.64 ms), but **the join's own wait has no scope at all** and
  lands entirely in `dma`. Golden Sun is the trap's worst scene, ~3.5 k VRAM
  stores a frame. **Until that is split, "gsdd spends 2.54 ms in DMA" is not
  known to be true**, and neither is SS3.6's basis for dropping Phase 5.
* **The counters already exist** for the DMA half: `C_DMA_RUN_W` / `RUN_H` /
  `SLOW_W` / `SLOW_H` / `RUN_SEGS` / `LOOP` / `VRAM_TRAP` / `GXF_*`. The split
  between bulk-copied words, per-word bus words and trap hits decides 3d
  without writing any of it.

*Exit:* a per-stage attribution for gsdd that survives the scope overlap, and
a named lever for each of 3d-3f. Expected gain: 0 ms.

#### 3c — The cart, first

**The largest identified non-JIT cost on the emulation thread, and Phase 2
never harvested it.** SS3.3 measured `Io::cart_catch_up_slow` as the hottest
C++ symbol on both censused scenes — ~24 % of dbori's emulation thread, ~11 %
of mlbis's — named it "Phase 2's first candidate", and Phase 2 then spent
itself on JIT emitters instead. It is not a DraStic checklist row, which is
why the plan never had it; it is in Phase 3 because **the cart is DMA**, and
because the lever is the same whole-transfer granularity the DMA row is about.

The lever is already written. `DS_CART_BULK` (`io.cpp`, `cart_receive_word`):
with a DMA taking the words, nothing observes their cadence — DRQ is consumed
by the channel, ROMCTRL's busy bit holds to the end either way, and words the
DMA has not read are not in RAM on hardware either. So the words are produced
as the DMA reads them, and the transfer keeps **one** event at the nominal
time of the last word. Per 512-byte block that is 1 scheduler event and 1
slice instead of 128 of each, and a loading screen streams tens of blocks a
frame. The code comment says it: *"Opt-in until swept on the device."*

Three things to do, in order: measure it as an A/B on the device (it is an
environment variable, so this costs one run, not a patch); sweep it, because
cart timing is where loaders live and the title sweep is the real gate here,
not the five scenes; and if it holds, **make it the default and delete the
flag** — which also pre-pays Phase 7's `--fast-load` deletion, the frontend
wiring `set_cart_bulk()` being the only thing that flag does to the core.

*Risk:* this is the change most likely to alter what a title observes, and the
one whose failure mode is a loader that hangs rather than a picture that
differs. It is first because the evidence is strongest, not because it is safe.

#### 3d — DMA, with the lever chosen by 3b

**The original bullet's premise is stale.** It says *"every word is charged,
stall-checked and budget-bounded against a per-unit burst model, even inside a
direct-mapped run... ~30 instructions per word"*. `dma.cpp` already does
whole-run `memcpy` between direct-mapped pages, closed-form cyclic burst costs
(`RunCost::bulk`), a binary-searched budget cut, one page-table walk per end
per run, a once-per-run VRAM trap instead of once per word, and a bulk GXFIFO
feed. The per-word path is the fallback now, not the model. Rows 4.8/4.9 in
the form the checklist describes them are substantially done.

So do not re-derive the lever from the checklist. Take it from 3b's counter
split: per-word bus words (`SLOW_W`/`SLOW_H`) point at the bus path; many
`RUN_SEGS` carrying few words each point at per-transfer and per-run overhead
— `start()`, the page walks, the trap probe, the budget loop — which is where
the genuine whole-transfer row still lives; and a large `VRAM_TRAP` count
points at lazy 2D rather than at DMA, which is a different phase.

Still unclaimed and still worth having whichever way that falls: the code-page
invalidation over the destination becoming the coarse (64 KB) / fine (2 KB)
bitmap OR (4.10).

#### 3e — The scheduler, and the FF3 freeze together

SS3.6 already relocated the freeze here: *"not a JIT defect. It is interleave
granularity, and it belongs WITH the Phase 3 scheduler row"*. Two things have
changed since it was written, and both help.

* **The freeze is reachable sooner.** SS3.17's sweep has FF3 freezing 43 %
  earlier than the Phase 2 sweep did — 95 distinct frames against 167, the
  same 95 Phase 6's table reads for both recompilers at quantum 2048. A defect
  that arrives at frame ~95 of a boot bisects faster than one at ~167.
* **The `EVENT_BOUND_QUANTUM` justification has already expired once.** The
  reason given for pinning it at 2048 — "SM64DS boots at 2048 and does not at
  2560" — is no longer true on this tree, and 1792 fixes FF3 at no measured
  cost. **That is a mitigation, not the fix**: it moves a cliff without
  explaining it, and the explanation is a cycle-accounting difference between
  the two engines that this row should find. Re-verify the SM64DS boot
  constraint before relying on either number.

The row's own work is unchanged: remove `LOCKSTEP_QUANTUM` and the dual-mode
interleave, let both-halted slices run to the deadline, and let the event count
fall out of 3c and 3f rather than being attacked directly.

#### 3f — SPU, last and split

The one bullet whose premise **is** intact: `run_channel` really does step a
16-bit timer per sample per channel, and `mix()` really does run per sample.
It is last anyway, and the reasons are worth stating because they are not
about the design being wrong.

It is the largest piece of new code in the phase; it is worth at most 1.13 ms
and only on gsdd (0.14-0.59 elsewhere); it carries the highest compatibility
risk in the rework, since a title that paces itself on the SPU's write timing
has no gate that can catch it; and **its exit gate is the weakest in this
document** — "by ear and by a spectral comparison", which is the only gate here
that cannot be run unattended.

So split it, and let 3b decide whether the second half happens at all:

* **The cheap half**, independent of the model: resolve each channel's source
  to a host pointer at key-on, decode ADPCM a word (8 samples) at a time into
  a ring, PSG as duty tables, noise as a precomputed 32 K LFSR table. These
  shrink the per-sample cost without changing when anything is observed.
* **The model change** — mix once per frame at VBlank with a catch-up on the
  ARM7 audio timer, resampling folded into playback with a 32.32 cursor — only
  if 3b shows the row is per-sample loop cost rather than per-event cost. The
  note that this "would change what Rhythm Heaven's just-in-time stream writer
  sees" remains a compatibility-sweep question rather than a veto, but Rhythm
  Heaven is in `games-bench` and should be checked by ear specifically.

#### The stop rule

**Phase 3 ends when gsdd is inside 16.74 ms, or when 3b's census says every
remaining row is under ~0.3 ms — whichever comes first.** Not when all the
rows have been done.

SS4 already says the project lands inside budget even if Phase 2 underdelivers,
and it did underdeliver; four of the five scenes now clear the budget with
4.4-11.7 ms of margin. Spending weeks on the SPU for 0.14 ms on dbori would be
risk 4 — "the most code for the least median gain" — in a new costume, and the
phase that is genuinely short of evidence for its remaining time is not this
one.

*Exit gate:* the title sweep (3c makes this mandatory, not optional), diffed on
`distinct` and `frozen` as well as `status` (SS3.17); the picture and cadence
gates; and audio by ear and by spectral comparison for 3f only. Every step
reports missed frames and dry audio queues beside the median, as Phase 6
requires.

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

**The FF3 both-recompiler freeze is not a JIT defect. It is interleave
granularity, and it belongs WITH the Phase 3 scheduler row, not after it.**
Measured 2026-09-21, off-device under qemu-aarch64, 6000 frames from a direct
boot, judged by `tools/frame_health.py` (the frame counter keeps advancing
through the freeze, so `--frames` alone shows nothing; `DS_WATCHDOG` catches a
stalled pipeline, not a game that has stopped):

| configuration | distinct frames / 6000 | frozen |
|---|---|---|
| `--interp`, quantum 2048 | 1107 | 0.8137 |
| `--jit9`, quantum 2048 | 1107 | 0.8137 |
| `--jit7`, quantum 2048 | 1107 | 0.8137 |
| **both recompilers, quantum 2048** | **95** | **0.9823** |
| both recompilers, quantum 1024 / 1536 / 1792 | 1107 | 0.8137 |

`--quantum 0` IS 2048 (`set_quantum` maps a non-positive value to
`EVENT_BOUND_QUANTUM`), and both spellings freeze identically -- so the old
note's "frozen with both recompilers" was really "frozen at the event-bound
quantum with both recompilers". Two readings are ruled out by the table. It is
not the recompiler miscompiling anything: at the SAME quantum the interpreter
is healthy. It is not the quantum alone: at the same quantum one recompiler is
healthy. What is left is the two CPUs' relative progress inside one slice,
which only differs when both run under the recompiler's cycle accounting --
and only bites once the slice is 2048 cycles wide. The cliff is sharp: 1792
is clean, 2048 freezes, with nothing in between.

**A candidate mitigation, and a constraint that has expired.** The reason
given elsewhere in this document for pinning `EVENT_BOUND_QUANTUM` at 2048 is
that "SM64DS boots at 2048 and does not at 2560". That is no longer true on
this tree: SM64DS boots identically at 1792, 2048 and 2560 (2093 distinct
frames of 2400, blank 0.039, in all three). So lowering the constant to 1792
fixes FF3 at no measured cost -- FF3 at 1792 stays healthy out to 9000 frames
(1703 distinct). Treat that as a mitigation to keep in the pocket, not the
fix: it moves a cliff without explaining it, and the explanation is a cycle
accounting difference between the two engines that Phase 3's scheduler work
should find. Re-verify the SM64DS boot constraint before relying on either
number.

mGBA's "Holy grail bugs II" describes an FF3 intro freeze with the same
timing-sensitive character ("the slower memory timings were, the longer it
would play for"), fixed there by using the KEY1 gap parameters for KEY2
accesses. That is NOT this: `cart_write_romctrl` already applies the gap1
field (ROMCTRL bits 0-12) in every command mode, and gap2 per 512-byte block.
Worth re-reading if the scheduler line of enquiry runs out.

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


### Phase 8 — The scaffolding out (week 10)

Phase 7 deletes the knobs a *player* can reach. This deletes the ones only we
ever reach, and it is a separate phase because the justification is different:
those were an accuracy-for-speed trade offered to the user, these are
experiments that have finished.

**The tree carries 146 distinct `DS_*` environment knobs.** Most arrived to
settle one question, settled it, and stayed. `DS_2D_SPLIT` is the worked
example (SS3.22): written to fix a real problem, superseded by a different fix
the following day, inert ever since, still a branch and still a supported
configuration that anything claiming to be exhaustive has to cover.

**The line to draw is not "experiment versus product" but what the knob costs
when it is off.**

* **A knob that selects between two code paths is the dual-path tax SS2
  names** — a branch in a hot path, a state the tests must cover, a
  combination that can deadlock, and a reason a bug report is not
  reproducible. These go. `DS_2D_SPLIT`, `DS_JIT_FASTCOST` (measured *slower*,
  SS3.2), `DS_R3D_ADAPT` and its `_BUSY`/`_SHIFT` (Phase 4 deletes the adaptive
  controller anyway), `DS_2D_LAZY` / `DS_2D_THREAD` / `DS_2D_LAG` /
  `DS_2D_DEFER` / `DS_SPU_BATCH` / `DS_IDLE_SKIP` once each has one settled
  value, and `DS_CART_BULK` once 3c defaults it.
* **A knob that only turns on reporting costs one `getenv` at construction and
  nothing after.** `DS_DEBUG_*`, the `_LOG` and `_TRACE` families, `DS_PROFILE`
  and its friends. **These stay** — they are how every number in this document
  was produced, and deleting them would be deleting the instruments while the
  lesson of SS3.11, SS3.12, SS3.18 and SS3.19 is that we need *more* of them
  and better ones.
* **A census knob that sits on a per-access path is the awkward middle.**
  `profile.h` already records that `PageTable::write_ptr`'s counters cost a
  global load and a branch on every store even when disabled, which is why
  `DSPERATE_CENSUS` is a compile-time switch. Anything in that class either
  moves behind the compile-time switch or goes.

**Also in scope, and the reason this is a phase rather than a chore:**

* **Dead profile stages.** `GX_RUN` and `GX_JOIN` sat declared and siteless
  through two phases while `gx_geom` read 0.00 and was twice written up as work
  that had been deleted (SS3.18). A stage with no site is worse than no stage:
  it reports a confident zero. Assert at startup that every stage has a site,
  or drop the enumerator.
* **Knobs referenced only by a `tools/` script that no longer runs**, and
  scripts whose arms name binaries that no longer exist.
* **The `ab*.sh` family on the device**, which is nine copies of one script
  whose header comment is wrong in eight of them (SS3.16). One parameterised
  runner.

*Exit gate:* the knob count, stated. Every removal is a deletion of a
*configuration*, so the compatibility sweep and the four gates run once at the
end — not per removal. Nothing here should change a measurement, and a removal
that does is a finding, not a regression to paper over.

*Expected gain: 0 ms, and that is the point.* This phase buys reproducibility,
a smaller test surface, and the ability to say what the emulator does without
qualifying it.

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
from measurements.

**Superseded, 2026-09-21 — keep this table as the plan's opening bet and do
not quote it as a forecast.** Every row in it has since been measured on the
device, and the three the table leaned on hardest did not hold:

* The geometry claim it cites as evidence (`--timing-oc` worth 3-4 ms) is
  wrong on the device (SS3.4); the phase delivered -0.30 to -1.49 ms (SS3.5)
  and the row is now **0.00 ms on every scene** (SS3.17) — the only row that
  beat its target, by being deleted rather than shrunk.
* Phase 2 returned -0.02 to -1.13 ms against 3.5 (SS3.16), which SS4 itself
  said to treat as success rather than as a reason to extend the phase. It was.
* The DMA and SPU rows cited as measured come from the checklist's censuses and
  the 2026-09-16 NSMB profile, not from this device. Measured here they are
  1.0-1.4 ms of *combined* Phase 3 surface on four of the five scenes (SS3.17),
  so the -2.5 ms this table assumes is not in them.

**The conclusion survives the arithmetic that produced it.** Four of the five
device scenes are inside 16.74 ms today with 4.4-11.7 ms of margin, which is
what the table was predicting for the end of Phase 5. NSMB, the scene the
column is about, has never been a device scene in this rework; if the number
matters, record it as one rather than carrying this table forward.

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
