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

// Calling thread's name (15 chars max on Linux), for profilers/debuggers.
void name_current_thread(const char* name);

} // namespace ds
