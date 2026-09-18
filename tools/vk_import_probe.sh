#!/bin/sh
# Build the dma-heap import probe (P0.1 of the GPU path), ship it, run it.
#   tools/vk_import_probe.sh [ssh-host] [W H]     (default: rgdsplus 640 480)
# The device has glslc but no C compiler: C compiles here, SPIR-V there.
set -e
HOST=${1:-rgdsplus}; W=${2:-640}; H=${3:-480}
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/vk_import_probe
aarch64-linux-gnu-gcc -O2 -std=gnu11 -o "$OUT" "$DIR/vk_import_probe.c" -ldl
scp -q "$OUT" "$DIR"/vk_import_fill.comp "$DIR"/vk_import_img.comp "$HOST:/tmp/"
ssh "$HOST" "cd /tmp && glslc -O vk_import_fill.comp -o import_fill.spv && glslc -O vk_import_img.comp -o import_img.spv \
  && chmod +x vk_import_probe && DS_DMA_HEAP=\$DS_DMA_HEAP ./vk_import_probe import_fill.spv import_img.spv $W $H" 2>&1 \
  | grep -v "^arm_release_ver\|module param file"
