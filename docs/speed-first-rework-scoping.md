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

### Phase 1 — Geometry as a replayed log (weeks 1-2) — target **-3.0 ms**

The largest single win, and the one already measured: `--timing-oc` alone is
worth 3-4 ms a frame on every 3D scene, and DraStic's full model
(04 §6, checklist 4.17/4.18) goes further than `--timing-oc` does.

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

### Phase 2 — The ARM9 at 6.7 ms (weeks 2-4) — target **revised, see §3.2**

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

### Phase 5 — The 2D engines (week 7-8) — target **-0.4 ms + worker time**

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
