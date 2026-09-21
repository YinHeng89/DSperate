#!/bin/bash
# The JIT's correctness gate: the interpreter against the recompiler, judged
# by result rather than by bytes.
#
#   tools/jit_gate.sh <jit-capable-headless> <fixtures-dir> [scene ...]
#
# Why this exists. The JIT README offers "--interp vs default with
# --dump-frames (DS_JIT_STRICT=1 for byte equality)", and taken as a BYTE
# comparison it fails: on Golden Sun the two dumps differ in 11,347,831 bytes,
# 3.6 %, on an unmodified tree. Judged perceptually the same pair reads ssim
# mean 0.9999, p01 0.9986, min 0.9983, no structural faults -- 359 of 800
# frames merely realigned. The difference is timing phase, not incorrectness:
# the two engines run at slightly different speeds and frames land at
# different moments. Bytes were the wrong standard.
#
# No new comparison is needed for this. golden_dump.sh already forces --interp
# on the reference side and run_candidate.sh runs the candidate as it is
# played, so handing gate.sh the SAME JIT-capable binary twice compares the
# interpreter against the recompiler, with the floors, the structural faults
# and the cadence counters all applying.
#
# It cannot run on the x86 host: the JIT has AArch64 and ARM32 backends only,
# so the host build is interpreter-only and `tools/title_sweep.sh` -- which
# Phase 2's exit gate calls the real gate -- cannot exercise the recompiler
# there either. Run this under qemu-aarch64 or on the device.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${1:?a JIT-capable headless binary (AArch64)}; FIX=${2:?fixtures dir}; shift 2
exec "$HERE/gate.sh" check "$BIN" "$BIN" "$FIX" "$@"
