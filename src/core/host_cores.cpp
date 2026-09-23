// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/host_cores.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#if defined(__linux__)
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace ds {

namespace {

constexpr u32 MAX_CPUS = 64;

int forced_cores() {
  static const int n = [] { const char* e = std::getenv("DS_HOST_CORES"); return e ? std::atoi(e) : 0; }();
  return n;
}
std::atomic<u32> g_configured_cores{0};

#if defined(__linux__)
// "0,3" or "0-3,6" -> mask. 0 when unreadable.
u64 read_online_mask() {
  FILE* f = std::fopen("/sys/devices/system/cpu/online", "r");
  if (!f) return 0;
  char buf[256];
  const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  buf[n] = 0;
  u64 mask = 0;
  for (char* p = buf; *p;) {
    char* end;
    const unsigned long lo = std::strtoul(p, &end, 10);
    if (end == p) break;
    unsigned long hi = lo;
    if (*end == '-') hi = std::strtoul(end + 1, &end, 10);
    for (unsigned long c = lo; c <= hi && c < MAX_CPUS; ++c) mask |= u64{1} << c;
    p = *end == ',' ? end + 1 : end;
    if (*p == '\n') break;
  }
  return mask;
}

// Process affinity as first read.
u64 read_affinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return ~u64{0};
  u64 m = 0;
  for (u32 c = 0; c < MAX_CPUS; ++c) if (CPU_ISSET(c, &set)) m |= u64{1} << c;
  return m ? m : ~u64{0};
}
std::atomic<u64> g_startup_affinity{0};
u64 startup_affinity() {
  u64 m = g_startup_affinity.load(std::memory_order_relaxed);
  if (!m) { m = read_affinity(); g_startup_affinity.store(m, std::memory_order_relaxed); }
  return m;
}

// The usable set, re-read at most once a second.
u64 usable_mask() {
  static std::atomic<u64> cached{0};
  static std::atomic<s64> read_at{0};
  const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  u64 m = cached.load(std::memory_order_relaxed);
  if (!m || now - read_at.load(std::memory_order_relaxed) >= 1000) {
    const u64 aff = startup_affinity();
    const u64 online = read_online_mask();
    m = online ? (online & aff) : aff;
    if (!m) m = online ? online : 1;
    cached.store(m, std::memory_order_relaxed);
    read_at.store(now, std::memory_order_relaxed);
  }
  return m;
}
#endif

} // namespace

void set_host_cores(u32 n) { g_configured_cores.store(n, std::memory_order_relaxed); }

u32 host_cores() {
  if (forced_cores() > 0) return static_cast<u32>(forced_cores());
  if (const u32 n = g_configured_cores.load(std::memory_order_relaxed)) return n;
#if defined(__linux__)
  const u64 m = usable_mask();
  if (m != ~u64{0}) return static_cast<u32>(__builtin_popcountll(m));
#endif
  const u32 hc = std::thread::hardware_concurrency();
  return hc ? hc : 4u;
}

void name_current_thread(const char* name) {
#if defined(__linux__)
  prctl(PR_SET_NAME, name, 0, 0, 0);
#else
  (void)name;
#endif
}

} // namespace ds
