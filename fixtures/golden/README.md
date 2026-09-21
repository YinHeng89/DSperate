# The golden fixtures

What `tools/gate.sh check` judges a candidate build against. Eight scenes:
`mlbis meteos sm64 etody dbori artacd gsdd st-intro`, 1800 frames each except
the two boot scenes (`gsdd`, `st-intro`) at 2400.

**The two halves have different references, and that is deliberate.** Read
this before concluding a fixture is wrong.

| | recorded from | recorded how |
|---|---|---|
| `<scene>.hashes` | `exact-reference` (see `REFERENCE`) | `tools/golden_dump.sh`: `--interp`, `--quantum 128`, `DS_R3D_THREADS=0`, `DS_2D_THREAD=0`, `DS_2D_LAZY=0`, `DS_SPU_BATCH=1` |
| `<scene>.cadence` | the current tree | `tools/cadence_census.sh`: the binary **as played** -- `--quantum 0`, threads on, whatever engine it was built with |

The picture is compared against a *frozen* reference that never has to build
again, through a pinned configuration that makes it reproducible on any host.
That is the whole point of `exact-reference` and it does not move.

The cadence counters cannot work that way. They exist to answer "did a timing
change make this game draw fewer frames?", which is a question about the model
*as it ships* -- at the event-bound quantum, with the threads on. Pinning them
to the interpreter at `--quantum 128` would measure a configuration nobody
plays and would never see, for instance, the FF3 freeze that only appears at
2048 with both recompilers. So the cadence half is re-baselined at the end of
a phase that intentionally changed the timing model, and what moved is
recorded below.

`floors.json` holds the per-scene PSNR/SSIM floors for `perceptual_gate.py`.
`evidence/` holds the gate's self-test material.

## The cadence counters are not equal, and only three of them gate

Since 2026-09-21 (see the note in `tools/gate.sh`):

* **`3d frames kept`, `gx swap_buffers`, `gx vblanks with no swap` fail the
  gate.** These are what the census is for, and they do not depend on which
  engine produced them -- the same tree reads 850 / 1417 / 386 on sm64 from
  the x86 interpreter and from the AArch64 recompiler.
* **`gx reads of GXSTAT` is reported, never fatal.** It is a poll count: it
  moves with how often the guest looked, which depends on where the two
  engines' slices fall. The same tree reads 24,772 on the host against 24,372
  on AArch64 -- 1.6 % apart with nothing wrong.

Before this, a cadence diff was printed and the gate passed anyway, so none of
the four gated at all. Three of them do now.

## Re-baselined 2026-09-21, after Phases 1 and 2

Recorded from the x86 host build of `4faaff7`+ (the hard counters are
engine-independent, so the choice of build does not affect what gates). The
picture hashes were **not** touched and remain `exact-reference`'s.

| scene | kept | swaps | no-swap vbl | GXSTAT |
|---|---|---|---|---|
| mlbis | 760 = | 1482 = | 318 = | 12422 = |
| meteos | 545 = | 1601 = | 199 = | 14 = |
| sm64 | 851 -> 850 | 1416 -> **1417** | 384 -> 386 | 29720 -> 24772 |
| etody | 551 = | 1658 -> **1659** | 142 = | 37995 = |
| dbori | 902 = | 1093 = | 707 = | 1551829 -> 1550732 |
| artacd | 1681 = | 1062 = | 738 = | 7443 = |
| gsdd | 1012 = | 1846 = | 554 = | 424601 -> 398285 |
| st-intro | 227 = | 2255 = | 145 = | 76291 -> 72502 |

**No scene draws fewer frames.** Swap counts are unchanged on six of eight and
rise by one on the other two, which is the invariant the census was added to
protect. Only sm64 moves a hard counter at all, by one frame of alignment in
each direction.

**The GXSTAT falls are Phase 1 working as designed.** The no-FIFO geometry
model means a game polling GXSTAT for FIFO room stops finding a reason to spin:
sm64 -17 %, gsdd -6 %, st-intro -5 %. Four scenes do not move, and dbori's
1.55 M polls a run shift by 0.07 %.

These were the diffs SS3.13 of the plan recorded as "predating this change" and
had to spend a verification run attributing to earlier Phase 2 work rather than
to the emitter under test. That run is the cost the counter split now avoids.
