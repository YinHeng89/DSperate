#!/bin/bash
# Dump a recorded scene's framebuffers in the *golden* configuration: the
# slowest, most-exact path the tree has, which is the reference every
# perceptual comparison is made against (tools/perceptual_gate.py).
#
#   tools/golden_dump.sh <dsperate-headless> <scene> <frames> <out.bin> [extra args]
#
# Scenes: mlbis meteos sm64 etody dbori artacd (replays) gsdd st-intro (states).
#
# DS_BIOS as for scene_hashes.sh. DS_ROMS is a colon-separated *list* of
# directories, searched in order -- the research tree keeps the scene ROMs in
# binary/games-bench and binary/games-bugtest, so one directory does not cover
# them. Scene names are those in scenes/; both the .dsin replays and the .dss
# state scenes are handled here (the state scenes were special-cased inline in
# all_scene_hashes.sh).
#
# The golden configuration, and why each part of it:
#
#   --interp          both CPUs interpreted -- no recompiler in the reference
#   --quantum 128     LOCKSTEP_QUANTUM: IPC handshakes in melonDS's order,
#                     which is what the interleave was built to be compared
#                     against. Override with GOLDEN_QUANTUM.
#   DS_R3D_THREADS=0  raster inline on the emulation thread, no band pool
#   DS_2D_THREAD=0    engine B inline, no line worker
#   DS_2D_LAZY=0      the per-line 2D renderer, not the journal/trap batch
#                     (verified byte-identical, but the reference should not
#                     depend on that having stayed true)
#   DS_SPU_BATCH=1    one sample per mix event, the pre-batching behaviour
#   (no --no-aa)      hardware anti-aliasing stays ON -- it is the headless
#                     default, and it is the melonDS-comparable picture
#
# Determinism note: this config has no threads and no JIT, so two runs of it
# agree. It is NOT guaranteed that the x86 host and an AArch64 device agree --
# the NEON kernels and kernels_ref.cpp are asserted bit-equal by
# tests/kernels_test.cpp, but that assertion is the only thing standing behind
# it. Generate the reference on the same architecture as the candidates, or
# diff a host dump against a device dump once before trusting a cross-arch
# comparison.
set -u
BIN=$1; SCENE=$2; FRAMES=$3; OUT=$4; shift 4
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS to the ROM directory (colon-separated list allowed)}"
: "${DS_BIOS:?set DS_BIOS to the BIOS/firmware directory}"
QUANTUM=${GOLDEN_QUANTUM:-128}

# scene -> ROM, and how the scene is entered (replay or save state). The first
# six names are the ones scene_hashes.sh and all_scene_hashes.sh already use;
# the last two are guesses at the No-Intro spelling and are not verified here.
# Override any of them with DS_ROM_<scene>, e.g.
#   DS_ROM_st_intro="/roms/Zelda - Spirit Tracks.nds"
# (dashes in the scene name become underscores).
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
if [ -n "${!OVERRIDE_VAR:-}" ]; then
  ROM="${!OVERRIDE_VAR}"
else
  NAME="$ROM"; ROM=""
  IFS=':' read -ra DIRS <<< "$DS_ROMS"
  for d in "${DIRS[@]}"; do [ -f "$d/$NAME" ] && { ROM="$d/$NAME"; break; }; done
  [ -n "$ROM" ] || { echo "no ROM \"$NAME\" for scene $SCENE under $DS_ROMS (set $OVERRIDE_VAR to point at it)" >&2; exit 3; }
fi
[ -f "$ROM" ] || { echo "no ROM for $SCENE at $ROM (set $OVERRIDE_VAR to point at it)" >&2; exit 3; }

# gsdd and st-intro have no entry of their own: both games reach their title
# and attract mode from a direct boot with no input, so the scene IS the boot
# (2400 frames -- see gate.sh's frames_for).
#
# They were save states until 2026-09-20. Phase 1a broke the state format, and
# the states could not be regenerated: a state's frame_count counts frames
# since ITS session's boot, not since a --direct boot, so booting to the
# recorded number lands somewhere else entirely (gsdd's state reads frame 222;
# a direct boot is still black there and does not reach the title until ~700 --
# measured ssim 0.3182 with blank, frozen and region faults). Both states also
# carried a firmware id we no longer have. Running from boot is equivalent for
# a perceptual check, needs no fixture that a chunk layout can invalidate, and
# covers the lighter 2D stretches the states skipped past.
ENTRY=()
if   [ -f "$HERE/scenes/$SCENE.dsin" ]; then ENTRY=(--replay "$HERE/scenes/$SCENE.dsin")
elif [ "$SCENE" = gsdd ] || [ "$SCENE" = st-intro ]; then ENTRY=()
else echo "no scenes/$SCENE.dsin, and $SCENE is not a boot scene" >&2; exit 4; fi
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")

exec env DS_R3D_THREADS=0 DS_2D_THREAD=0 DS_2D_LAZY=0 DS_SPU_BATCH=1 \
  "$BIN" --direct --interp --quantum "$QUANTUM" \
         --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
         "${ENTRY[@]}" "${SAVE[@]}" --frames "$FRAMES" --dump-frames "$OUT" "$@" "$ROM"
