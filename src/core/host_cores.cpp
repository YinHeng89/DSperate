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

#if defined(__linux__)
// "0,3" or "0-3,6": the kernel's online list. 0 when unreadable.
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

// The process's affinity as first read -- always before a DS_PIN_THREADS pin
// narrows the calling thread's own, since pinning reads the usable set first.
// A taskset or cpuset is a standing restriction.
u64 read_affinity() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof set, &set) != 0) return ~u64{0};
  u64 m = 0;
  for (u32 c = 0; c < MAX_CPUS; ++c) if (CPU_ISSET(c, &set)) m |= u64{1} << c;
  return m ? m : ~u64{0};
}
std::atomic<u64> g_startup_affinity{0};   // re-read by avoid_cpus()
u64 startup_affinity() {
  u64 m = g_startup_affinity.load(std::memory_order_relaxed);
  if (!m) { m = read_affinity(); g_startup_affinity.store(m, std::memory_order_relaxed); }
  return m;
}

// One small integer from a sysfs file, or `fallback`.
long read_long(const char* path, long fallback) {
  FILE* f = std::fopen(path, "r");
  if (!f) return fallback;
  long v = fallback;
  if (std::fscanf(f, "%ld", &v) != 1) v = fallback;
  std::fclose(f);
  return v;
}

// A CPU's capacity for the "equal or greater" rule: the scheduler's
// cpu_capacity where the kernel exports it, else the maximum frequency.
long cpu_capacity(u32 cpu) {
  char path[96];
  std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpu_capacity", cpu);
  long v = read_long(path, -1);
  if (v >= 0) return v;
  std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);
  return read_long(path, 0);
}

// "0" / "0-3" / "1,3" -> bit mask.
u64 parse_cpu_list(const char* s) {
  u64 mask = 0;
  for (const char* p = s; *p;) {
    char* end;
    const unsigned long lo = std::strtoul(p, &end, 10);
    if (end == p) break;
    unsigned long hi = lo;
    if (*end == '-') hi = std::strtoul(end + 1, &end, 10);
    for (unsigned long c = lo; c <= hi && c < MAX_CPUS; ++c) mask |= u64{1} << c;
    p = *end == ',' ? end + 1 : end;
    if (*p == '\n' || *p == ' ') break;
  }
  return mask;
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

u64 gpu_irq_cpus() {
#if defined(__linux__)
  FILE* f = std::fopen("/proc/interrupts", "r");
  if (!f) return 0;
  u64 cpus = 0;
  char line[1024];
  while (std::fgets(line, sizeof line, f)) {
    // "81:  1268499  0  0  0  GICv3 72 Level  fde60000.gpu": the device name
    // is the last word; the rows of interest name a gpu or mali device.
    char* colon = std::strchr(line, ':');
    if (!colon) continue;
    char* name = line + std::strlen(line);
    while (name > line && (name[-1] == '\n' || name[-1] == ' ')) *--name = 0;
    while (name > colon && name[-1] != ' ') --name;
    std::string dev(name);
    for (auto& c : dev) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (dev.find("gpu") == std::string::npos && dev.find("mali") == std::string::npos) continue;
    *colon = 0;
    const long irq = std::strtol(line, nullptr, 10);
    char path[96], buf[128];
    std::snprintf(path, sizeof path, "/proc/irq/%ld/effective_affinity_list", irq);
    FILE* a = std::fopen(path, "r");
    if (!a) { std::snprintf(path, sizeof path, "/proc/irq/%ld/smp_affinity_list", irq); a = std::fopen(path, "r"); }
    if (!a) continue;
    if (std::fgets(buf, sizeof buf, a)) cpus |= parse_cpu_list(buf);
    std::fclose(a);
  }
  std::fclose(f);
  return cpus;
#else
  return 0;
#endif
}

bool avoid_cpus(u64 cpus, std::string* note) {
#if defined(__linux__)
  auto list = [](u64 m) { std::string s; for (u32 c = 0; c < MAX_CPUS; ++c) if ((m >> c) & 1) { if (!s.empty()) s += ','; s += std::to_string(c); } return s.empty() ? std::string("none") : s; };
  const u64 have = read_affinity() & (read_online_mask() ? read_online_mask() : ~u64{0});
  const u64 drop = have & cpus;
  const u64 keep = have & ~cpus;
  if (!drop) { if (note) *note = "GPU irq cpus " + list(cpus) + " not in this process's set"; return false; }
  if (__builtin_popcountll(keep) < 2) { if (note) *note = "too few cpus would remain (" + list(keep) + ")"; return false; }
  long best_keep = 0;
  for (u32 c = 0; c < MAX_CPUS; ++c) if ((keep >> c) & 1) best_keep = std::max(best_keep, cpu_capacity(c));
  for (u32 c = 0; c < MAX_CPUS; ++c)
    if (((drop >> c) & 1) && cpu_capacity(c) > best_keep) { if (note) *note = "cpu " + std::to_string(c) + " has no equal in the rest (capacity " + std::to_string(cpu_capacity(c)) + " > " + std::to_string(best_keep) + ")"; return false; }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (u32 c = 0; c < MAX_CPUS; ++c) if ((keep >> c) & 1) CPU_SET(static_cast<int>(c), &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) { if (note) *note = "sched_setaffinity refused"; return false; }
  g_startup_affinity.store(keep, std::memory_order_relaxed);
  if (note) *note = "off cpu " + list(drop) + " (GPU interrupts), on " + list(keep);
  return true;
#else
  (void)cpus; if (note) *note = "not Linux"; return false;
#endif
}

u32 host_cores() {
  if (forced_cores() > 0) return static_cast<u32>(forced_cores());
#if defined(__linux__)
  const u64 m = usable_mask();
  if (m != ~u64{0}) return static_cast<u32>(__builtin_popcountll(m));
#endif
  const u32 hc = std::thread::hardware_concurrency();
  return hc ? hc : 4u;
}

bool pin_threads() {
  static const bool on = [] { const char* e = std::getenv("DS_PIN_THREADS"); return e && std::atoi(e) != 0; }();
  return on;
}

void pin_current_thread(u32 k) {
#if defined(__linux__)
  // The k-th usable CPU, wrapping: the online set need not be contiguous
  // (spruce's powersave on the A30 leaves cpus 0 and 3).
  const u64 m = usable_mask();
  const u32 count = m == ~u64{0} ? 0 : static_cast<u32>(__builtin_popcountll(m));
  if (!count) return;
  u32 want = k % count, cpu = 0;
  for (; cpu < MAX_CPUS; ++cpu) if ((m >> cpu) & 1) { if (want == 0) break; --want; }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(cpu), &set);
  sched_setaffinity(0, sizeof set, &set);   // a hint: failure leaves the thread where it was
#else
  (void)k;
#endif
}

void name_current_thread(const char* name) {
#if defined(__linux__)
  prctl(PR_SET_NAME, name, 0, 0, 0);
#else
  (void)name;
#endif
}

} // namespace ds
