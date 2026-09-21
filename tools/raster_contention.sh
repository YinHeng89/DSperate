#!/bin/bash
# Does the rasteriser steal from the emulation thread even though nothing
# waits for it? `3d band wait` measures blocking, not memory-bandwidth or
# cache contention, and the pre-1a sweep found gsdd 1.4 ms FASTER at two band
# workers than three with band wait near zero either way.
#
# The instrument is cpu9 from the profile line -- the emulation thread's own
# ARM9 execution stage, which contains no waiting. If cpu9 climbs as band
# workers are added, the raster is taking time from emulation directly.
cd /storage/dsperate-test
B=/storage/roms/bios; N=/storage/roms/nds
OUT=/storage/dsperate-test/contend.jsonl
rm -f $OUT
run() { th=$1; s=$2; rom=$3; fr=$4; sf=$5; shift 5
  lab="$s.b$th.$REP"
  sv=""; [ -f scenes/$s.sav ] && sv="--save scenes/$s.sav"
  DS_R3D_THREADS=$th DS_PROFILE=1 DS_PROFILE_LINE=$OUT DS_PROFILE_LABEL=$lab \
  ./dsperate-1f --direct --quantum 0 \
    --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin \
    $sv "$@" --frames $fr --stats-from $sf "$N/$rom" >/tmp/$lab.out 2>&1
  echo "  $lab: $(grep -E '^frame ms' /tmp/$lab.out | head -1)"
}
scene() { s=$1; rom=$2; fr=$3; sf=$4; shift 4
  REP=1; for t in 0 1 2 3; do run $t $s "$rom" $fr $sf "$@"; done
  REP=2; for t in 3 2 1 0; do run $t $s "$rom" $fr $sf "$@"; done
  REP=3; for t in 2 0 3 1; do run $t $s "$rom" $fr $sf "$@"; done
}
scene gsdd  "Golden Sun - Dark Dawn.nds"                1800 700
scene mlbis "Mario & Luigi - Bowser's Inside Story.nds" 1800 200 --replay scenes/mlbis.dsin
echo DONE
