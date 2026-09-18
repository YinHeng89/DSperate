// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// How many cores the emulator's threads can share, for the thread shapes that
// were tuned on 4-core handhelds (the 3D band pool, see Renderer3D::band_count).
#pragma once
#include "core/types.h"
#include <string>

namespace ds {

// DS_HOST_CORES=N overrides. Otherwise the CPUs online now that the process
// may run on, re-read at most once a second: the online set moves under a
// running game -- spruce keeps two of the A30's four cores online in its
// powersave mode and applies an NDS game's overclock mode 33 s after launch --
// and a taskset or cpuset restricts it.
u32 host_cores();

// DS_PIN_THREADS=1: pin the emulation thread to the first usable core and
// band worker i to the (1 + i)-th, wrapping. Off by default -- a device A/B,
// not a tuned shape. Threads started after the emulation thread pins itself
// inherit its core only; the band pool pins its own.
bool pin_threads();
void pin_current_thread(u32 k);

// The CPUs that service the GPU's interrupts (Linux: /proc/interrupts rows
// naming a gpu/mali device, each irq's effective affinity), as a bit mask;
// 0 when unknown. The Mali kernel driver services a tiler page fault on the
// CPU that took the interrupt, in a worker that a SCHED_RR emulator thread
// on that core starves until the RT bandwidth cap opens -- a GPU frame then
// completes 0.2-0.95 s late (docs/gpu-path-scoping.md, "GPU-side stalls").
u64 gpu_irq_cpus();

// Take `cpus` out of the process's affinity, when what remains still has at
// least two CPUs and, for every CPU removed, one of equal or greater capacity
// (cpu_capacity, else cpuinfo_max_freq: a big.LITTLE device keeps its big
// cores). Before any thread is created. Returns whether it changed anything;
// `note` says what it did or why not.
bool avoid_cpus(u64 cpus, std::string* note);

// The calling thread's name (15 characters at most on Linux), for profiles
// and debuggers: the sampling profiler's thread column reads it.
void name_current_thread(const char* name);

} // namespace ds
