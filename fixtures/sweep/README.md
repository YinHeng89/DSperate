# Compatibility baseline — `exact-reference`, 2026-09-20

46 retail titles (`games-bench`, `games-bugtest`, `games-sweep`; the NSMB
duplicate is skipped), 3600 frames each, no input. Produced by
`tools/title_sweep.sh`; regenerate with the same command and diff the status
column after every phase.

| | ok | static | fail |
|---|---|---|---|
| `a64-jit.tsv` — AArch64 build, JIT+NEON, under qemu | 39 | 7 | 0 |
| `host-interp.tsv` — x86 build, interpreter, portable renderer | 40 | 6 | 0 |

**No title crashed, hung, timed out or drew a blank screen on either build.**

`static` means nothing changed at all over the last quarter of the run. With
no input that is ambiguous by construction — a title waiting on a motionless
"press start" screen is indistinguishable from one that died after booting —
so these are listed apart from failures and need a human once. Six are static
on both builds (Art Academy, Puppy Palace, Spider-Man Shattered Dimensions,
ZhuZhu Babies, GTA Chinatown Wars, Spore Creatures) and are most likely input
gates rather than bugs.

## The one real finding: Final Fantasy III

`static` under the JIT build, `ok` under the interpreter — the only status
difference between the two builds, and it is a genuine defect that exists on
`exact-reference`, i.e. before this rework changed anything.

Bisected on the same AArch64 binary under the same qemu, so neither the NEON
kernels nor the host architecture are involved — only the CPU engine changes:

| | distinct | late | verdict |
|---|---|---|---|
| `--interp` | 645 | 181/0 changes 180 | alive |
| `--jit9` (ARM9 recompiled only) | 645 | 181/180 | alive |
| `--jit7` (ARM7 recompiled only) | 645 | 181/180 | alive |
| both recompiled (default) | 167 | **1/0** | frozen |

The picture's **last change is frame 1211** and it is static to 3599; the
first frame differing from the interpreter is 1216. The interpreter is still
drawing at 3598.

**Neither recompiler alone reproduces it — it needs both.** That points at an
interaction rather than a mistranslated instruction: something timing- or
interleave-sensitive between the two CPUs that only appears when both run at
recompiled granularity. `jit_test.cpp` checks each translator against the
interpreter instruction by instruction and would not catch this.

**Confirmed on hardware, 2026-09-20.** The qemu caveat that was here — that
qemu models self-modifying code and icache maintenance differently from a real
A55, which is what a recompiler leans on — is resolved. The same four runs on
the RG DS Plus (ROCKNIX 7.0.2, Cortex-A55, glibc 2.41) reproduce the table
above *digit for digit*: 645 distinct and 181/180 late for `--interp`,
`--jit9` and `--jit7`; 167 distinct and 1/0 late with both. This is a real
defect, and qemu reproduces it faithfully, so it can be bisected off-device.

## After Phases 1 and 2 (`a64-jit-phase2.tsv`, 2026-09-20)

The same command, the same 46 titles, the same 3600 frames, the AArch64
JIT+NEON build under qemu — re-run after the geometry rework (Phase 1) and
the JIT and scheduler work (Phase 2).

| | ok | static | fail |
|---|---|---|---|
| `a64-jit.tsv` — `exact-reference` | 39 | 7 | 0 |
| `a64-jit-phase2.tsv` — after Phases 1+2 | 39 | 7 | 0 |

**No title changed status.** Joining the two on the title column produces no
differing rows, so nothing regressed and nothing was fixed. The seven static
titles are the same seven, still needing the one-off human look recorded above.

This is the gate Phase 2's exit criteria call the real one — "the sweep, not
the four scenes" — and it cannot run on the host: the JIT has AArch64 and
ARM32 backends only, so the x86 build is interpreter-only. Run it under qemu
(as here, ~25 s a title) or on the device.

## After Phase 2 closed (`a64-jit-phase3a.tsv`, 2026-09-21)

The same command, titles and frame count, run because the Phase 2 sweep above
was produced at `1f82522` and **three changes landed after it** -- the
mode-switch restore (`58fca58`), the exception return (`f72421e`) and the
user-bank work (`83898bf`). All three touch banked registers or mode
switching, whose failure mode is a corrupted interrupted context rather than a
clean gate failure, so the earlier sweep did not cover them.

| | ok | static | fail |
|---|---|---|---|
| `a64-jit-phase2.tsv` | 39 | 7 | 0 |
| `a64-jit-phase3a.tsv` | 39 | 7 | 0 |

**No title changed status**, and the seven static titles are the same seven.
Phase 2's exit gate is met.

### But the status column is too coarse, and three titles moved underneath it

Joining on `distinct` rather than on `status`:

| title | phase2 | 3a | |
|---|---|---|---|
| Final Fantasy III | 167 | **95** | **-43 %** |
| Animal Crossing - Wild World | 2318 | 2724 | +17.5 % |
| Advance Wars - Days of Ruin | 774 | 791 | +2.2 % |

**Final Fantasy III now freezes earlier.** It was already the one real finding
in this directory -- `static` under the recompiler, `ok` under the
interpreter, a defect that predates the rework -- and Phase 2 moved the freeze
forward by 43 % of the frames it used to draw. Its `frozen` fraction rises
0.9506 -> 0.9706. The status column cannot see this, because the title was
already `static`.

That number matches the plan's Phase 6 table exactly -- 95 distinct for "both
recompilers, quantum 2048", measured on the current tree over 6000 frames,
against 167 here at 3600 on the older one. Distinct frames stop accumulating
at the freeze, so the count matching across two different run lengths means
both runs froze at the same point. This is the known interleave defect, not a
new one -- but it is now reachable sooner, which makes it a better bisect
target and a worse thing to hit while playing.

The lesson is the same one the cadence counters just taught: **a pass/fail
column is not the whole gate.** Diff `distinct` and `frozen` as well as
`status` after every phase; the numbers are in the TSV already.
