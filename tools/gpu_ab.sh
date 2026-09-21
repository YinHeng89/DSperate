#!/bin/bash
# The GPU raster's exactness gate: draw every frame of a scene BOTH ways and
# compare, in one process.
#
#   tools/gpu_ab.sh <dsperate-headless> <scene> <frames> [extra args]
#
# Why in one process rather than two runs diffed afterwards: the two rasters
# see the same polygon list, the same render state and the same decoded
# textures, in the same call, so there is no timing divergence to rule out
# first and a difference is the GPU's and nothing else's. Two runs would have
# to agree on emulation timing before they could say anything about pixels.
#
# What it reports:
#   * how many frames the GPU drew, and how many the feature gate sent to the
#     CPU instead (a gate that refuses everything is a pass by vacuum, so the
#     script fails when nothing was compared)
#   * how many frames differed, by how much, and which was first
#   * PNGs of the differing frames, via tools/compare_frames.py
#
# Environment: DS_ROMS and DS_BIOS as tools/scene_hashes.sh wants them.
set -u
BIN=$1; SCENE=$2; FRAMES=$3; shift 3
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${DS_ROMS:?set DS_ROMS to the ROM directory}"
: "${DS_BIOS:?set DS_BIOS to the BIOS/firmware directory}"
case $SCENE in
  mlbis)  ROM="$DS_ROMS/Mario & Luigi - Bowser's Inside Story.nds";;
  meteos) ROM="$DS_ROMS/Meteos.nds";;
  sm64)   ROM="$DS_ROMS/Super Mario 64 DS.nds";;
  etody)  ROM="$DS_ROMS/Etrian Odyssey.nds";;
  dbori)  ROM="$DS_ROMS/Dragon Ball - Origins.nds";;
  artacd) ROM="$DS_ROMS/Art Academy.nds";;
  *) echo "unknown scene $SCENE" >&2; exit 2;;
esac
OUT=${DS_GPU_AB_OUT:-gpu-ab-$SCENE}
mkdir -p "$OUT"
SAVE=(); [ -f "$HERE/scenes/$SCENE.sav" ] && SAVE=(--save "$HERE/scenes/$SCENE.sav")

"$BIN" --direct --quantum 0 \
       --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin" \
       "${SAVE[@]}" --replay "$HERE/scenes/$SCENE.dsin" --frames "$FRAMES" \
       --gpu-ab-dump "$OUT/frame" "$@" "$ROM" 2>&1 | tee "$OUT/run.log"

echo
grep -E "^gpu (raster|gate|A/B):" "$OUT/run.log"

# A gate that refused every frame proves nothing, so say so rather than
# reporting a pass.
if grep -q "nothing compared" "$OUT/run.log"; then
  echo "gpu A/B: FAIL -- no frame reached the GPU raster (feature gate, or the backend is not ready)"
  exit 1
fi
if grep -q "all identical to the software raster" "$OUT/run.log"; then
  echo "gpu A/B: PASS"
  exit 0
fi

# Something differed: show where.
if [ -s "$OUT/frame-cpu.bin" ]; then
  python3 "$HERE/tools/compare_frames.py" "$OUT/frame-cpu.bin" "$OUT/frame-gpu.bin" --png "$OUT/png" --max-png 8
  echo "gpu A/B: FAIL -- see $OUT/png (cpu = reference, gpu = candidate)"
fi
exit 1
