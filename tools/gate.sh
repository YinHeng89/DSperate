#!/bin/bash
# The speed-first gate: regenerate the golden reference, then judge a
# candidate build against it by picture and by cadence.
#
#   tools/gate.sh baseline <ref-headless> <fixtures-dir>
#   tools/gate.sh check    <ref-headless> <cand-headless> <fixtures-dir> [scene ...] [-- extra cand args]
#
# The golden dumps are NOT stored. A 1800-frame dump is 708 MB and the golden
# configuration reproduces it in about six seconds on a desktop host, so the
# baseline keeps only what is small and durable -- per-frame hashes, the
# cadence counters -- and the raw frames are regenerated into a scratch
# directory whenever a comparison needs them. `baseline` records; `check`
# regenerates and compares.
#
# The reference binary should be built from the `exact-reference` tag, not
# from the working tree. See docs/speed-first-rework-scoping.md §3.
#
# THE CANDIDATE SIDE IS NOT DETERMINISTIC, and a comparison must be read with
# that in mind. The golden side runs --interp --quantum 128 with every thread
# off, so it reproduces exactly; the candidate runs as the game is played --
# event-bound, threads on (run_candidate.sh) -- which is the point, because a
# gate against a configuration nobody ships proves little. The cost is
# run-to-run variation on the long scenes: gsdd measured 2399 / 2400 / 2399
# exact frames over three runs of the SAME pair of binaries (2026-09-20), with
# ssim min 0.9996 / 1.0000 / 0.9990.
#
# So: a scene one or two frames off byte-exact is noise, not evidence, and
# "byte-exact" is not a usable invariant for the boot scenes. What IS evidence
# is the floors, the structural faults, the swap counts, and -- for a change
# that should be behaviour-preserving -- the deterministic scenes (sm64,
# dbori, mlbis, meteos) coming back digit for digit.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SCRATCH=${GATE_SCRATCH:-${TMPDIR:-/tmp}/dsperate-gate.$$}
mkdir -p "$SCRATCH"; trap 'rm -rf "$SCRATCH"' EXIT

# scene -> frames. The replays are 1800 frames and measuring fewer measures
# boot and title screens (scenes/README.md); the two boot scenes are longer
# because they include that boot.
frames_for() {
  case $1 in
    # The two boot scenes run longer than the replays: they start at power-on
    # rather than at a recorded moment, so they have a title sequence to get
    # through before the part worth judging, and 2400 leaves room for it.
    gsdd|st-intro) echo 2400;; *) echo 1800;;
  esac
}
ALL_SCENES="mlbis meteos sm64 etody dbori artacd gsdd st-intro"

hash_dump() {   # <dump> -> per-frame sha1 on stdout
  python3 - "$1" <<'PY'
import sys, hashlib
FR = 256 * 192 * 4 * 2
with open(sys.argv[1], 'rb') as f:
    n = 0
    while True:
        d = f.read(FR)
        if len(d) < FR: break
        print(n, hashlib.sha1(d).hexdigest()); n += 1
PY
}

# The cadence counters do not all carry the same weight -- and until 2026-09-21
# none of them carried any: a diff was printed and the gate passed regardless.
#
# Frames kept, swap counts and no-swap vblanks are what the census exists for
# ("did the game draw fewer frames?"), and they do not depend on which engine
# produced them: the same tree reads 850 / 1417 / 386 on sm64 from the x86
# interpreter and from the AArch64 recompiler. These fail the gate.
#
# GXSTAT reads are a POLL count. They move with how often the guest looked,
# which depends on where the two engines' slices fall, and the same tree reads
# 24,772 on the host against 24,372 on AArch64 -- 1.6 % apart with nothing
# wrong. Reported, never fatal. SS3.13 spent a whole verification run proving
# one such diff was not its emitter's doing; that is the cost this avoids.
cadence_cmp() {   # <fixture> <candidate> -> prints, nonzero if a hard counter moved
  python3 - "$1" "$2" <<'CADPY'
import sys
HARD = ('3d frames kept', 'gx swap_buffers', 'gx vblanks with no swap')
SOFT = ('gx reads of GXSTAT',)
def read(p):
    try: lines = open(p).read().splitlines()
    except OSError: return None
    d = {}
    for l in lines:
        for k in HARD + SOFT:
            if k in l:
                try: d[k] = int(l.split()[-1])
                except ValueError: pass
    return d
a, b = read(sys.argv[1]), read(sys.argv[2])
if not a or not b:
    print('  CADENCE: census missing or empty -- not judged'); sys.exit(0)
for k in SOFT:
    if k in a and k in b and a[k] != b[k]:
        print(f'  cadence note: {k} {a[k]} -> {b[k]} (poll count, engine-dependent; not gated)')
bad = [(k, a[k], b[k]) for k in HARD if k in a and k in b and a[k] != b[k]]
for k, x, y in bad:
    print(f'  CADENCE FAIL: {k} {x} -> {y}')
sys.exit(1 if bad else 0)
CADPY
}

MODE=${1:?usage: tools/gate.sh baseline|check ...}; shift

if [ "$MODE" = baseline ]; then
  REF=${1:?ref headless binary}; OUT=${2:?fixtures dir}; shift 2
  mkdir -p "$OUT"
  # Named scenes re-baseline only those, leaving the rest of the fixtures
  # alone; with none named, every scene is recorded.
  for s in ${*:-$ALL_SCENES}; do
    n=$(frames_for "$s"); d="$SCRATCH/$s.bin"
    if ! "$HERE/golden_dump.sh" "$REF" "$s" "$n" "$d" >"$SCRATCH/$s.log" 2>&1; then
      echo "skip $s (see $SCRATCH/$s.log)"; sed -n '$p' "$SCRATCH/$s.log"; continue
    fi
    hash_dump "$d" > "$OUT/$s.hashes"
    "$HERE/cadence_census.sh" "$REF" "$s" "$n" "$OUT/$s.cadence" >/dev/null 2>&1 || true
    echo "$s: $(wc -l < "$OUT/$s.hashes") frames hashed"
    rm -f "$d"
  done
  # Only a full baseline may claim what the whole directory was recorded
  # against. `git describe` reads the WORKING TREE, not the reference build,
  # so a partial re-baseline run from a later commit would otherwise stamp
  # that commit over a directory recorded from the tag (it did, once).
  if [ $# -eq 0 ]; then
    printf '%s\n' "$(git -C "$HERE/.." describe --tags --always 2>/dev/null)" > "$OUT/REFERENCE"
  fi
  echo "baseline in $OUT (reference $(cat "$OUT/REFERENCE" 2>/dev/null || echo unknown))"
  exit 0
fi

[ "$MODE" = check ] || { echo "unknown mode $MODE" >&2; exit 2; }
REF=${1:?ref headless}; CAND=${2:?candidate headless}; FIX=${3:?fixtures dir}; shift 3
SCENES=""; EXTRA=()
while [ $# -gt 0 ]; do
  [ "$1" = "--" ] && { shift; EXTRA=("$@"); break; }
  SCENES="$SCENES $1"; shift
done
[ -n "${SCENES// /}" ] || SCENES=$ALL_SCENES

rc=0
for s in $SCENES; do
  n=$(frames_for "$s")
  g="$SCRATCH/$s.golden.bin"; c="$SCRATCH/$s.cand.bin"
  if ! "$HERE/golden_dump.sh" "$REF" "$s" "$n" "$g" >"$SCRATCH/$s.g.log" 2>&1; then
    echo "skip $s (no ROM?)"; continue
  fi
  # The candidate runs as it is meant to be played: event-bound, threads on.
  if ! "$HERE/run_candidate.sh" "$CAND" "$s" "$n" "$c" "${EXTRA[@]+"${EXTRA[@]}"}" >"$SCRATCH/$s.c.log" 2>&1; then
    echo "FAIL $s: candidate did not run (see $SCRATCH/$s.c.log)"; rc=1; continue
  fi
  # GATE_FLOORS overrides; otherwise the baseline's own floors file gates when
  # it is there, and the run only reports when it is not.
  floors=${GATE_FLOORS:-$FIX/floors.json}
  python3 "$HERE/perceptual_gate.py" "$g" "$c" --scene "$s" \
      ${floors:+$([ -f "$floors" ] && echo --floors "$floors")} || rc=1
  if [ -f "$FIX/$s.cadence" ]; then
    "$HERE/cadence_census.sh" "$CAND" "$s" "$n" "$SCRATCH/$s.cadence" "${EXTRA[@]+"${EXTRA[@]}"}" >/dev/null 2>&1 || true
    cadence_cmp "$FIX/$s.cadence" "$SCRATCH/$s.cadence" || rc=1
  fi
  rm -f "$g" "$c"
done
exit $rc
