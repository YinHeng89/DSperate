#!/bin/sh
# Build and run the two capability probes on the device, as SEPARATE binaries.
#   tools/gles_probe.sh [ssh-host]      (default: rgdsplus)
#
# Separate on purpose: the EGL entry point and the Vulkan loader pick their
# vendors independently, so one process linking both cannot say which answer
# belongs to which stack. Both link DYNAMICALLY so the loader resolves exactly
# what an application would get -- no dlopen of a path we chose, which is how
# a wrong answer gets recorded (see the speed-first scoping doc, SS3.34: a
# `nm`-based probe on a busybox box with no `nm` returned all false negatives).
#
# WHAT THE LINK LINE IS ABOUT. On ROCKNIX /usr/lib/libEGL.so.1 exports no egl*
# symbols at all: it is a shim with NEEDED libmali-hook.so.1, and that hook
# overrides four entry points and dlsyms the rest out of libmali.so.1 -- the
# Mali blob. So -lEGL does NOT reach Mesa here, whatever
# /usr/share/glvnd/egl_vendor.d/50_mesa.json suggests. The shim is linked
# first so the binary's DT_NEEDED is the path an application really takes; the
# blob is linked after it only to satisfy the symbols at link time.
#
# The device has no C compiler, and the cross sysroot has no EGL/GLES/Vulkan
# headers or libraries: host headers (API-generic), device libraries.
set -e
HOST=${1:-rgdsplus}
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${TMPDIR:-/tmp}/dsperate-probe.$$
mkdir -p "$OUT/lib"
trap 'rm -rf "$OUT"' EXIT

for f in libEGL.so.1 libGLESv2.so.2 libvulkan.so.1 libmali-hook.so.1; do
  scp -q "$HOST:/usr/lib/$f" "$OUT/lib/" || { echo "cannot fetch $f from $HOST" >&2; exit 1; }
done

# libmali.so.1 is ~60 MB; cache it between runs.
CACHE=${DSPERATE_PROBE_CACHE:-${TMPDIR:-/tmp}/dsperate-probe-libmali}
if [ ! -f "$CACHE/libmali.so.1" ]; then
  mkdir -p "$CACHE"
  echo "fetching libmali.so.1 (~60 MB, cached in $CACHE)..." >&2
  scp -q "$HOST:/usr/lib/libmali.so.1" "$CACHE/" || { echo "cannot fetch libmali.so.1" >&2; exit 1; }
fi
cp "$CACHE/libmali.so.1" "$OUT/lib/"

CC=${CC:-aarch64-linux-gnu-gcc}
$CC -O1 -std=gnu11 -o "$OUT/gles_probe" "$DIR/gles_probe.c" \
    -I/usr/include -L"$OUT/lib" \
    -l:libEGL.so.1 -l:libGLESv2.so.2 -l:libmali-hook.so.1 -l:libmali.so.1 \
    -Wl,-rpath-link,"$OUT/lib"
$CC -O1 -std=gnu11 -o "$OUT/vk_caps_probe" "$DIR/vk_caps_probe.c" \
    -I/usr/include -L"$OUT/lib" -l:libvulkan.so.1 -Wl,-rpath-link,"$OUT/lib"

scp -q "$OUT/gles_probe" "$OUT/vk_caps_probe" "$HOST:/tmp/"
echo "================ GLES / EGL ================"
ssh "$HOST" 'chmod +x /tmp/gles_probe && /tmp/gles_probe; echo "exit=$?"' 2>&1 | grep -v '^arm_release_ver'
echo
echo "================ VULKAN ===================="
ssh "$HOST" 'chmod +x /tmp/vk_caps_probe && /tmp/vk_caps_probe; echo "exit=$?"' 2>&1 | grep -v '^arm_release_ver'
