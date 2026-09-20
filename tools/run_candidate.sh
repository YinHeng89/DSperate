#!/bin/bash
# Run a scene through a candidate build the way it is meant to be played --
# event-bound interleave, threads and the recompiler as the frontends use them
# -- and dump its frames. The counterpart to golden_dump.sh, which runs the
# slow exact configuration.
#
#   tools/run_candidate.sh <headless> <scene> <frames> <out.bin> [extra args]
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
  NAME="$ROM"; ROM=""; IFS=':' read -ra DIRS <<< "$DS_ROMS"
  for d in "${DIRS[@]}"; do [ -f "$d/$NAME" ] && { ROM="$d/$NAME"; break; }; done
  [ -n "$ROM" ] || { echo "no ROM \"$NAME\" under $DS_ROMS" >&2; exit 3; }
fi
ENTRY=()
if   [ "$SCENE" = gsdd ];               then ENTRY=(--load-state "$HERE/scenes/gsdd-phase2.dss")
elif [ -f "$HERE/scenes/$SCENE.dsin" ]; then ENTRY=(--replay "$HERE/scenes/$SCENE.dsin")
elif [ -f "$HERE/scenes/$SCENE.dss" ];  then ENTRY=(--load-state "$HERE/scenes/$SCENE.dss")
else echo "no scene file for $SCENE" >&2; exit 4; fi
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")
exec "$BIN" --direct --quantum 0 \
  --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
  "${ENTRY[@]}" "${SAVE[@]}" --frames "$FRAMES" --dump-frames "$OUT" "$@" "$ROM"
