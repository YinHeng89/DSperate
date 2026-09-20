#!/bin/bash
# The counters that say *when* a game drew, not what it drew.
#
#   tools/cadence_census.sh <dsperate-headless> <scene> <frames> <out.txt> [extra args]
#
# The perceptual gate (tools/perceptual_gate.py) compares pictures, and it is
# blind to the failure mode Phase 1 is most likely to cause: a timing model
# that makes a game draw *fewer frames*, or pace itself differently, while
# every frame it does draw stays correct. docs/frame-profile-2026-09-16.md
# records why this matters -- "a tier can lower the frame time by making the
# game draw fewer frames, and the frame time alone cannot tell that apart from
# a real saving" -- and the swap count is the instrument that tells them apart.
#
# Diff two of these the way scene hashes are diffed. A changed swap count is
# not automatically a failure, but it is never allowed to pass unnoticed.
#
# NOTE, and it is not a small one: this measures the *headless* harness, which
# boots differently from the SDL frontend. Dragon Ball Origins runs its intro
# at 30 Hz headless and 60 Hz in SDL under the same model, so a cadence claim
# about a game as played must be checked in the frontend it is played in.
# This script bounds the regression; it does not settle it.
set -u
BIN=$1; SCENE=$2; FRAMES=$3; OUT=$4; shift 4
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"

case $SCENE in
  mlbis)    ROM="Mario & Luigi - Bowser's Inside Story.nds";;
  meteos)   ROM="Meteos.nds";;
  sm64)     ROM="Super Mario 64 DS.nds";;
  etody)    ROM="Etrian Odyssey.nds";;
  dbori)    ROM="Dragon Ball - Origins.nds";;
  gsdd)     ROM="Golden Sun - Dark Dawn.nds";;
  artacd)   ROM="Art Academy.nds";;
  st-intro) ROM="Legend of Zelda, The - Spirit Tracks.nds";;
  *) echo "unknown scene $SCENE" >&2; exit 2;;
esac
OVERRIDE_VAR="DS_ROM_${SCENE//-/_}"
if [ -n "${!OVERRIDE_VAR:-}" ]; then ROM="${!OVERRIDE_VAR}"; else
  NAME="$ROM"; ROM=""
  IFS=':' read -ra DIRS <<< "$DS_ROMS"
  for d in "${DIRS[@]}"; do [ -f "$d/$NAME" ] && { ROM="$d/$NAME"; break; }; done
  [ -n "$ROM" ] || { echo "no ROM \"$NAME\" under $DS_ROMS" >&2; exit 3; }
fi

# Must match golden_dump.sh: gsdd and st-intro are boot scenes with no file
# of their own (scenes/README.md).
ENTRY=()
if   [ -f "$HERE/scenes/$SCENE.dsin" ];      then ENTRY=(--replay "$HERE/scenes/$SCENE.dsin")
elif [ "$SCENE" = gsdd ] || [ "$SCENE" = st-intro ]; then ENTRY=()
else echo "no scene file for $SCENE, and it is not a boot scene" >&2; exit 4; fi
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")

DS_PROFILE=1 "$BIN" --direct --quantum 0 \
  --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
  "${ENTRY[@]}" "${SAVE[@]}" --frames "$FRAMES" "$@" "$ROM" 2>&1 \
  | grep -E '^\[profile\] (gx swap_buffers|gx vblanks with no swap|3d frames kept|gx reads of GXSTAT)' \
  | sed 's/  */ /g' > "$OUT"
echo "$SCENE -> $OUT"; cat "$OUT"
