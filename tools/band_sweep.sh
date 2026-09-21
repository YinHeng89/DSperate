#!/bin/bash
# The band sweep again, on the 1b binary -- the "after" half of the pre-1a
# sweep. Deleting the geometry worker gave a core back, so the question is
# whether three band workers is still the right pin now that the thread
# population has changed.
#
# gsdd is a boot scene now (its save state cannot load under format 4), so it
# runs from power-on and drops the frames before the title.
cd /storage/dsperate-test
B=/storage/roms/bios; N=/storage/roms/nds
OUT=/storage/dsperate-test/bands2.jsonl
rm -f $OUT
run() { # arm scene rom frames statsfrom entry...
  arm=$1; s=$2; rom=$3; fr=$4; sf=$5; shift 5
  lab="$s.$arm.$REP"
  case $arm in def) th="";; *) th="DS_R3D_THREADS=${arm#b}";; esac
  sv=""; [ -f scenes/$s.sav ] && sv="--save scenes/$s.sav"
  env $th DS_PROFILE=1 DS_PROFILE_LINE=$OUT DS_PROFILE_LABEL=$lab \
  ./dsperate-1b --direct --quantum 0 \
    --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin \
    $sv "$@" --frames $fr --stats-from $sf "$N/$rom" >/tmp/$lab.out 2>&1
  echo "  $lab: $(grep -E '^frame ms' /tmp/$lab.out | head -1)"
}
scene() { s=$1; rom=$2; fr=$3; sf=$4; shift 4
  REP=1; for a in def b2 b3 b4; do run $a $s "$rom" $fr $sf "$@"; done
  REP=2; for a in b4 b3 b2 def; do run $a $s "$rom" $fr $sf "$@"; done
  REP=3; for a in b3 def b4 b2; do run $a $s "$rom" $fr $sf "$@"; done
}
scene sm64  "Super Mario 64 DS.nds"                     1800 200 --replay scenes/sm64.dsin
scene dbori "Dragon Ball - Origins.nds"                 1800 200 --replay scenes/dbori.dsin
scene mlbis "Mario & Luigi - Bowser's Inside Story.nds" 1800 200 --replay scenes/mlbis.dsin
scene etody "Etrian Odyssey.nds"                        1800 200 --replay scenes/etody.dsin
scene gsdd  "Golden Sun - Dark Dawn.nds"                1800 700
echo DONE
