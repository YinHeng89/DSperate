#!/bin/bash
# Record the SDL frontend's cadence for a game, and compare two builds.
#
#   tools/cadence_sdl.sh <ref-sdl> <cand-sdl> <rom-name> [frames] [-- extra args]
#   tools/cadence_sdl.sh <sdl> - <rom-name> [frames]        record one only
#
# Phase 1's exit gate asks for Dragon Ball Origins' intro checked for cadence
# in the SDL frontend, because no headless gate can settle it (the harnesses
# boot differently and dbori runs its intro at 30 Hz headless and 60 Hz here).
#
# The BIOS directory is COPIED into the scratch dir first, and that is not a
# nicety. The SDL frontend saves firmware settings back to
# <firmware>.ovr beside the firmware it was given -- so pointing two
# recordings at the shared research BIOS lets the first one change what the
# second boots from, and lets a cadence run quietly mutate the same directory
# the golden fixtures are recorded against. (It did, 2026-09-20, before this
# script existed.) Each run here gets its own copy and throws it away.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
REF=${1:?ref sdl binary}; CAND=${2:?candidate sdl binary, or - for one recording}; NAME=${3:?rom name}
FRAMES=${4:-1800}; shift 4 2>/dev/null || shift $#
EXTRA=(); [ "${1:-}" = "--" ] && { shift; EXTRA=("$@"); }

ROM=""
IFS=':' read -ra DIRS <<< "$DS_ROMS"
for d in "${DIRS[@]}"; do [ -f "$d/$NAME" ] && { ROM="$d/$NAME"; break; }; done
[ -n "$ROM" ] || { echo "no ROM \"$NAME\" under $DS_ROMS" >&2; exit 3; }

SCRATCH=${CADENCE_SCRATCH:-${TMPDIR:-/tmp}/dsperate-cadence.$$}
mkdir -p "$SCRATCH"; trap 'rm -rf "$SCRATCH"' EXIT

record() {   # <binary> <out.cad>
  local bin=$1 out=$2 bios="$SCRATCH/bios.$(basename "$2")"
  mkdir -p "$bios"
  cp "$DS_BIOS/bios9.bin" "$DS_BIOS/bios7.bin" "$DS_BIOS/firmware.bin" "$bios/"
  SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy DS_CADENCE_LOG="$out" \
    "$bin" --bios9 "$bios/bios9.bin" --bios7 "$bios/bios7.bin" --firmware "$bios/firmware.bin" \
           --no-vsync --frames "$FRAMES" "${EXTRA[@]+"${EXTRA[@]}"}" "$ROM" >"$out.log" 2>&1
}

# A build without the recorder exits fine and writes nothing, which would
# otherwise surface as a stack trace from the analyser.
check() { [ -s "$1" ] || { echo "no cadence log from $2 -- does that build have DS_CADENCE_LOG? (see $1.log)" >&2; exit 1; }; }

record "$REF" "$SCRATCH/ref.cad" || { echo "ref run failed, see $SCRATCH/ref.cad.log" >&2; exit 1; }
check "$SCRATCH/ref.cad" "$REF"
if [ "$CAND" = "-" ]; then
  python3 "$HERE/cadence_sdl.py" "$SCRATCH/ref.cad" "$@"
  exit $?
fi
record "$CAND" "$SCRATCH/cand.cad" || { echo "candidate run failed, see $SCRATCH/cand.cad.log" >&2; exit 1; }
check "$SCRATCH/cand.cad" "$CAND"
# The first frames are the boot, which is the frontend's and not the game's.
python3 "$HERE/cadence_sdl.py" "$SCRATCH/ref.cad" "$SCRATCH/cand.cad" --skip=200
