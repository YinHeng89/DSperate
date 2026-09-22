#!/bin/sh
# DraStic --benchmark from a save state, on the device, hi-res 3D on and off.
#
#   tools/drastic_bench_device.sh "<rom basename>" [frames]
#
# Runs in /storage/.config/drastic. The state must be slot 0 of that ROM
# (savestates/<rom basename>_0.dss, saved in DraStic itself -- it has no CLI
# flag to load one). load.rec presses LOAD_STATE at frame 5 (see the
# measurement-tools notes: .rec = {u32 frame; u32 CONTROL_INDEX mask; u16 touch},
# LOAD_STATE = bit 20). Benchmark mode is unthrottled and skips the screen
# path, so its ms/frame is emulation + raster only; playback never exits on
# its own, hence the timeout. Leaves hires_3d = 1 as it found it.
set -u
ROM=$1; FRAMES=${2:-600}
D=/storage/.config/drastic
cd "$D" || exit 2
[ -f "savestates/${ROM}_0.dss" ] || { echo "no state: savestates/${ROM}_0.dss"; exit 2; }
export XDG_RUNTIME_DIR=/tmp SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
for hires in 1 0; do
  sed -i "s/^hires_3d = .*/hires_3d = $hires/" config/drastic.cfg
  echo "== $ROM hires_3d=$hires, $FRAMES frames"
  timeout 180 ./drastic --benchmark "$FRAMES" --input-playback load.rec "/storage/roms/nds/$ROM.nds" 2>&1 | grep -viE "^$|alsa|pulse|xdg" | tail -12
done
sed -i "s/^hires_3d = .*/hires_3d = 1/" config/drastic.cfg
