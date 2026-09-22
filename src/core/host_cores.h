// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// How many cores the emulator's threads can share (tunes the 3D band pool).
#pragma once
#include "core/types.h"
#include <string>

namespace ds {

// DS_HOST_CORES=N overrides, then set_host_cores(). Else the CPUs online now, re-read at
// most once a second (the online set can move under a running game).
u32 host_cores();
// Frontend setting (emu.host_cores); 0 = detect.
void set_host_cores(u32 n);

// CPUs servicing the GPU's interrupts, as a bit mask; 0 if unknown. A tiler page fault is
// serviced on the interrupting CPU, which a SCHED_RR emulator thread there can starve.
u64 gpu_irq_cpus();

// Takes `cpus` out of the process's affinity, only if at least two CPUs remain and every CPU
// removed has an equal-or-greater-capacity one remaining (big.LITTLE keeps its big cores).
// Call before any thread is created. Returns whether it changed anything; `note` says why not.
bool avoid_cpus(u64 cpus, std::string* note);

// Calling thread's name (15 chars max on Linux), for profilers/debuggers.
void name_current_thread(const char* name);

} // namespace ds
