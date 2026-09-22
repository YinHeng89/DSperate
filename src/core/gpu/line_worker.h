// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// One worker thread that runs a fixed job on request, for engine B's share of
// display lines while the calling thread renders engine A's. Spins before
// parking, since per-line dispatch (up to 192/frame) is cheaper than waking
// a condition variable.
#pragma once

#include "core/host_cores.h"
#include "core/types.h"

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace ds::gpu {

class LineWorker {
public:
  LineWorker() = default;
  ~LineWorker() { stop(); }
  LineWorker(const LineWorker&) = delete;
  LineWorker& operator=(const LineWorker&) = delete;

  // `fn`/`arg` is the job; it runs on the worker for every dispatch().
  void start(void (*fn)(void*), void* arg) {
    if (thread_.joinable()) return;
    fn_ = fn; arg_ = arg;
    quit_.store(false, std::memory_order_relaxed);
    req_.store(0, std::memory_order_relaxed);
    ack_.store(0, std::memory_order_relaxed);
    thread_ = std::thread([this] { loop(); });
  }

  void stop() {
    if (!thread_.joinable()) return;
    quit_.store(true, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(m_); }
    cv_.notify_all();
    thread_.join();
  }

  bool running() const { return thread_.joinable(); }
  bool parked() const { return parked_.load(std::memory_order_relaxed); }
  // For a stalled-frame watchdog (DS_WATCHDOG).
  void debug_dump(FILE* f) const {
    std::fprintf(f, "  line worker: running %d req %u ack %u parked %d\n", running() ? 1 : 0,
                 req_.load(std::memory_order_relaxed), ack_.load(std::memory_order_relaxed), parked_.load(std::memory_order_relaxed) ? 1 : 0);
  }

  // Must be paired with wait() before reading the job's output or dispatching again.
  void dispatch() {
    const u32 n = req_.load(std::memory_order_relaxed) + 1;
    // seq_cst: req_/parked_ form a Dekker pair with the worker's park; weaker
    // ordering lets both sides read stale and wait() spins forever.
    req_.store(n, std::memory_order_seq_cst);
    // qemu-user models seq_cst stlr/ldar as plain release/acquire, which
    // could reorder this store past the load below; real hardware doesn't need it.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (parked_.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lk(m_);
      cv_.notify_one();
    }
  }

  void wait() {
    const u32 n = req_.load(std::memory_order_relaxed);
    int spins = 4000;
    while (ack_.load(std::memory_order_acquire) != n) {
      if (--spins > 0) cpu_relax();
      else std::this_thread::yield();
    }
  }

private:
  static void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
  }

  void loop() {
    name_current_thread("line-worker");
    u32 last = 0;
    for (;;) {
      static constexpr int kSpin = 20000;   // spin budget before parking (~line gap)
      int spins = kSpin;
      u32 r = req_.load(std::memory_order_acquire);
      while (r == last) {
        if (quit_.load(std::memory_order_relaxed)) return;
        if (--spins > 0) { cpu_relax(); r = req_.load(std::memory_order_acquire); continue; }
        std::unique_lock<std::mutex> lk(m_);
        parked_.store(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);   // see dispatch()
        // Re-check: catches a dispatch that raced the park and skipped the notify.
        r = req_.load(std::memory_order_seq_cst);
        if (r == last && !quit_.load(std::memory_order_relaxed))
          cv_.wait(lk, [&] {
            r = req_.load(std::memory_order_acquire);
            return r != last || quit_.load(std::memory_order_relaxed);
          });
        parked_.store(false, std::memory_order_seq_cst);
        if (quit_.load(std::memory_order_relaxed)) return;
        spins = kSpin;
      }
      last = r;
      fn_(arg_);
      ack_.store(r, std::memory_order_release);
    }
  }

  std::thread thread_;
  void (*fn_)(void*) = nullptr;
  void* arg_ = nullptr;
  std::atomic<u32> req_{0}, ack_{0};
  std::atomic<bool> quit_{false}, parked_{false};
  std::mutex m_;
  std::condition_variable cv_;
};

} // namespace ds::gpu
