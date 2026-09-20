#!/bin/bash
# Boot every retail title in the given directories and say which ones are not
# alive. The compatibility half of Phase 0
# (docs/speed-first-rework-scoping.md): the four recorded scenes cannot tell
# you that a phase broke a game nobody recorded, and the user's bar is broad
# *retail* compatibility, so this is the gate that actually holds that bar.
#
#   tools/title_sweep.sh <headless> <out-dir> <frames> <romdir> [romdir ...]
#
# DS_BIOS as elsewhere. One row per title in <out-dir>/results.tsv, the full
# stderr per title in <out-dir>/logs/, and a summary at the end. Re-running
# skips titles already recorded, so an interrupted sweep resumes.
#
# Status is decided from the frame stream (tools/frame_health.py), not from
# the exit code alone -- a title that exits cleanly having drawn nothing is
# the failure this is looking for:
#
#   ok       ran the whole run and the picture moved
#   blank    >98 % of frames are a flat colour on both screens
#   stuck    the picture stopped moving (>98 % repeats, <=3 distinct frames)
#   short    fewer frames than asked for, but exited cleanly
#   hang     the watchdog fired (no frame progress) -- the log has the dump
#   crash    non-zero exit for any other reason
#   timeout  the backstop killed it
set -u
BIN=${1:?headless binary}; OUT=${2:?output dir}; FRAMES=${3:?frames}; shift 3
[ $# -gt 0 ] || { echo "give at least one ROM directory" >&2; exit 2; }
: "${DS_BIOS:?set DS_BIOS}"
HERE=$(cd "$(dirname "$0")" && pwd)
WD=${SWEEP_WATCHDOG:-20}                     # seconds of no frame progress = hang
TO=${SWEEP_TIMEOUT:-600}                     # per-title backstop, seconds
mkdir -p "$OUT/logs"
TSV="$OUT/results.tsv"
[ -f "$TSV" ] || printf 'title\tstatus\tframes\tblank\tfrozen\tdistinct\tfirst_motion\twall_s\n' > "$TSV"

for dir in "$@"; do
  for rom in "$dir"/*.nds; do
    [ -f "$rom" ] || continue
    name=$(basename "$rom" .nds)
    grep -qF "$(printf '%s\t' "$name")" "$TSV" && continue     # already done
    log="$OUT/logs/$name.log"
    fifo=$(mktemp -u "$OUT/.fifo.XXXXXX"); mkfifo "$fifo"
    health="$OUT/logs/$name.health.json"
    python3 "$HERE/frame_health.py" "$fifo" --json "$health" &
    hp=$!
    t0=$(date +%s)
    DS_WATCHDOG=$WD timeout -k 5 "$TO" "$BIN" --direct --quantum 0 \
        --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
        --frames "$FRAMES" --dump-frames "$fifo" "$rom" > "$log" 2>&1
    rc=$?
    wait $hp 2>/dev/null
    t1=$(date +%s); wall=$((t1 - t0))
    rm -f "$fifo"

    read -r n blank frozen distinct motion < <(python3 - "$health" <<'PY'
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except Exception:
    r = {}
print(r.get('frames', 0), r.get('blank', 1.0), r.get('frozen', 1.0),
      r.get('distinct', 0), r.get('first_motion') if r.get('first_motion') is not None else -1)
PY
)
    if   [ $rc -eq 124 ] || [ $rc -eq 137 ];        then st=timeout
    elif grep -q '\[watchdog\]' "$log";             then st=hang
    elif [ $rc -ne 0 ];                             then st=crash
    elif awk "BEGIN{exit !($blank > 0.98)}";        then st=blank
    elif awk "BEGIN{exit !($frozen > 0.98)}" && [ "$distinct" -le 3 ]; then st=stuck
    elif [ "$n" -lt "$FRAMES" ];                    then st=short
    else                                                 st=ok
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$st" "$n" "$blank" "$frozen" "$distinct" "$motion" "$wall" >> "$TSV"
    printf '%-62s %-8s %5s frames  blank %-6s frozen %-6s distinct %-5s %ss\n' "$name" "$st" "$n" "$blank" "$frozen" "$distinct" "$wall"
  done
done

echo
echo "== summary =="
awk -F'\t' 'NR>1{c[$2]++} END{for(k in c) printf "  %-8s %d\n", k, c[k]}' "$TSV" | sort
echo "  total    $(($(wc -l < "$TSV") - 1))"
echo "not ok:"
awk -F'\t' 'NR>1 && $2!="ok"{printf "  %-8s %s\n", $2, $1}' "$TSV" | sort || true
