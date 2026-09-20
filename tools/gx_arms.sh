#!/bin/bash
# The geometry worker measured apart from the no-FIFO model and with it.
# Five arms, so the two changes --timing-oc bundles can be told apart:
#   base  inline, FIFO kept, timed          (control)
#   wctl  --gx-worker, DS_GX_THREAD=1       (worker + shape controller, as shipped)
#   walw  --gx-worker, DS_GX_THREAD=2       (worker always on; the controller cannot opt out)
#   tinl  --timing-oc, DS_GX_THREAD=0       (no-FIFO + untimed, inline; bands stay at 3)
#   tall  --timing-oc, DS_GX_THREAD=1       (shipped combo: worker + no-FIFO)
# Arm order rotates between reps so a thermal drift cannot land on one arm.
cd /storage/dsperate-test
B=/storage/roms/bios; N=/storage/roms/nds
OUT=/storage/dsperate-test/gxarms.jsonl
rm -f $OUT
run() { # arm scene rom thread extra...
  arm=$1; s=$2; rom=$3; th=$4; shift 4
  lab="$s.$arm.$REP"
  sv=""; [ -f scenes/$s.sav ] && sv="--save scenes/$s.sav"
  DS_GX_THREAD=$th DS_PROFILE=1 DS_PROFILE_LINE=$OUT DS_PROFILE_LABEL=$lab \
  ./dsperate-headless --direct --quantum 0 \
    --bios9 $B/bios9.bin --bios7 $B/bios7.bin --firmware $B/firmware.bin \
    $sv --replay scenes/$s.dsin --frames 1800 --stats-from 200 "$@" "$N/$rom" >/tmp/$lab.out 2>&1
  echo "  $lab: $(grep -E '^frame ms' /tmp/$lab.out | head -1)"
}
arm() { case $1 in
  base) run base $2 "$3" 1 ;;
  wctl) run wctl $2 "$3" 1 --gx-worker ;;
  walw) run walw $2 "$3" 2 --gx-worker ;;
  tinl) run tinl $2 "$3" 0 --timing-oc ;;
  tall) run tall $2 "$3" 1 --timing-oc ;;
esac }
scene() { s=$1; rom=$2
  REP=1; for a in base wctl walw tinl tall; do arm $a $s "$rom"; done
  REP=2; for a in tall tinl walw wctl base; do arm $a $s "$rom"; done
  REP=3; for a in walw base tall wctl tinl; do arm $a $s "$rom"; done
}
scene sm64  "Super Mario 64 DS.nds"
scene dbori "Dragon Ball - Origins.nds"
scene mlbis "Mario & Luigi - Bowser's Inside Story.nds"
scene etody "Etrian Odyssey.nds"
echo DONE
