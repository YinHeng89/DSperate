#!/bin/sh
# Build the Vulkan compute probe for the RG DS Plus, ship it, run it.
#   tools/vk_probe.sh [ssh-host]   (default: rgdsplus)
#
#   VK_SUITE=3d   dispatch floor and shading cost at 1x/2x/4x   (default)
#   VK_SUITE=2d   the 2D composite, and what a split frame costs
#   VK_SCALES, VK_WORKS, ITERS trim the grid; the heavy corner (4x at a high
#   work count) is minutes per configuration on a G52.
# The device has glslc but no C compiler; the host has a cross gcc and the
# Vulkan headers but no glslc. So: compile C here, compile SPIR-V there.
set -e
HOST=${1:-rgdsplus}
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/vk_probe

aarch64-linux-gnu-gcc -O2 -std=gnu11 -o "$OUT" "$DIR/vk_probe.c" -ldl
ssh "$HOST" 'pkill -9 vk_probe; rm -f /tmp/vk_probe' 2>/dev/null || true
scp -q "$OUT" "$DIR"/vk_probe_trivial.comp "$DIR"/vk_probe_3draster.comp "$DIR"/vk_probe_2d.comp "$HOST:/tmp/"
SUITE=${VK_SUITE:-3d}; SCALES=${VK_SCALES:-1,2,4}; WORKS=${VK_WORKS:-0,16,64}; N=${ITERS:-200}
ssh "$HOST" "cd /tmp \
  && glslc -O vk_probe_trivial.comp -o trivial.spv \
  && glslc -O vk_probe_3draster.comp -o fill.spv \
  && glslc -O vk_probe_2d.comp      -o composite2d.spv \
  && chmod +x vk_probe \
  && VK_SUITE=$SUITE VK_SCALES=$SCALES VK_WORKS=$WORKS ./vk_probe trivial.spv fill.spv $N composite2d.spv" 2>&1 \
  | grep -v "^arm_release_ver\|module param file"
