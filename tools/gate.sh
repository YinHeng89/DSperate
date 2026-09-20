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
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SCRATCH=${GATE_SCRATCH:-${TMPDIR:-/tmp}/dsperate-gate.$$}
mkdir -p "$SCRATCH"; trap 'rm -rf "$SCRATCH"' EXIT

# scene -> frames. The replays are 1800 frames and measuring fewer measures
# boot and title screens (scenes/README.md); the two state scenes are shorter
# because that is what they were recorded for.
frames_for() {
  case $1 in
    gsdd) echo 300;; st-intro) echo 600;; *) echo 1800;;
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

MODE=${1:?usage: tools/gate.sh baseline|check ...}; shift

if [ "$MODE" = baseline ]; then
  REF=${1:?ref headless binary}; OUT=${2:?fixtures dir}; shift 2
  mkdir -p "$OUT"
  for s in $ALL_SCENES; do
    n=$(frames_for "$s"); d="$SCRATCH/$s.bin"
    if ! "$HERE/golden_dump.sh" "$REF" "$s" "$n" "$d" >"$SCRATCH/$s.log" 2>&1; then
      echo "skip $s (see $SCRATCH/$s.log)"; sed -n '$p' "$SCRATCH/$s.log"; continue
    fi
    hash_dump "$d" > "$OUT/$s.hashes"
    "$HERE/cadence_census.sh" "$REF" "$s" "$n" "$OUT/$s.cadence" >/dev/null 2>&1 || true
    echo "$s: $(wc -l < "$OUT/$s.hashes") frames hashed"
    rm -f "$d"
  done
  printf '%s\n' "$(git -C "$HERE/.." describe --tags --always 2>/dev/null)" > "$OUT/REFERENCE"
  echo "baseline in $OUT (reference $(cat "$OUT/REFERENCE"))"
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
    if ! diff -q "$FIX/$s.cadence" "$SCRATCH/$s.cadence" >/dev/null 2>&1; then
      echo "  CADENCE changed:"; diff "$FIX/$s.cadence" "$SCRATCH/$s.cadence" | sed 's/^/    /'
    fi
  fi
  rm -f "$g" "$c"
done
exit $rc
