// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/gpu.h"
#include <chrono>
#include "core/state/state.h"
#include "core/gpu/vram_map.h"
#include "core/gpu/kernels.h"
#include "core/nds.h"
#include "core/profile.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>

// pow() for the gamma LUT below; pins a glibc symbol version for a low aarch64 runtime floor.
extern "C" double ds_pow_compat(double x, double y);

namespace ds::gpu {

// Knobs read once in the constructor.
namespace {
bool g_dbg_join = false, g_dbg_gpu = false, g_dbg_vramnz = false, g_dbg_skip = false;
bool g_no_lazy = false, g_no_lag = false;
const char* g_dump_frame = nullptr;
void read_knobs() {
  static bool done = false;
  if (done) return;
  done = true;
  g_dbg_join = std::getenv("DS_DEBUG_JOIN") != nullptr;
  g_dbg_gpu = std::getenv("DS_DEBUG_GPU") != nullptr;
  g_dbg_vramnz = std::getenv("DS_DEBUG_VRAMNZ") != nullptr;
  g_dbg_skip = std::getenv("DS_DEBUG_SKIP") != nullptr;
  g_dump_frame = std::getenv("DS_DEBUG_DUMP_FRAME");
  g_no_lazy = std::getenv("DS_DEBUG_NOLAZY") != nullptr;   // every frame per-line (the lazy batch's reference)
  g_no_lag = std::getenv("DS_DEBUG_NOLAG") != nullptr;     // per-line lines never stay in flight past their HBlank
}
}  // namespace

static void ev_scanline(NDS& nds, u32) { nds.gpu.on_scanline_start(); }
static void ev_hblank(NDS& nds, u32)   { nds.gpu.on_hblank(); }
static void ev_fifo(NDS& nds, u32 x)   { nds.gpu.on_display_fifo(x); }

Gpu::Gpu(NDS& nds) : engine{Engine2D(nds, 0), Engine2D(nds, 1)}, nds_(nds) {
  read_knobs();
  worker_.start(&Gpu::worker_job, this);
}

void Gpu::reset() {
  join_worker();
  disarm_trap();
  line_ = 0; hblank_done_ = false;
  lazy_frame_ = false; per_line_[0] = per_line_[1] = false;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = false;
  frame_begun_ = false; screens_on_ = false;
  master_bright_g_[0] = master_bright_g_[1] = 0;
  capcnt_ = 0; capture_on_ = false;
  fifo_.fill(0); fifo_rd_ = fifo_wr_ = 0; fifo_line_.fill(0); run_fifo_ = false;
  for (auto& fb : fb_) fb.fill(0);
  engine[0].reset(); engine[1].reset();
  set_powcnt(nds_.io.powcnt1);
  nds_.io.set_vcount(0);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.now() + HBLANK_START, ev_hblank);
}

// ---- registers --------------------------------------------------------------

u32 Gpu::reg_read(u32 addr, u32 width) {
  const u32 r = addr - 0x04000000;
  if (r >= 0x64 && r < 0x70) {
    auto rd16 = [&](u32 a) -> u32 {
      switch (a) {
      case 0x64: return capcnt_ & 0xFFFF;
      case 0x66: return capcnt_ >> 16;
      case 0x6C: return master_bright_g_[0];
      default: return 0;
      }
    };
    if (width == 32) return rd16(r) | (rd16(r + 2) << 16);
    if (width == 16) return rd16(r);
    return (rd16(r & ~1u) >> ((r & 1) * 8)) & 0xFF;
  }
  if (r >= 0x1064 && r < 0x1070) {
    const u32 v = (r & ~1u) == 0x106C ? master_bright_g_[1] : 0;
    return width == 8 ? (v >> ((r & 1) * 8)) & 0xFF : v;
  }
  return engine[r >= 0x1000].read(addr, width);
}

void Gpu::reg_write(u32 addr, u32 width, u32 value) {
  const u32 r = addr - 0x04000000;
  // MASTER_BRIGHT: guest copy here, render side via the journal.
  auto mb = [&](int e, u16 v) { master_bright_g_[e] = v; engine[e].master_bright_write(v); };
  if (r >= 0x64 && r < 0x70) {
    if (width == 32) {
      switch (r) {
      case 0x64: capcnt_ = value & 0xEF3F1F1F; return;
      case 0x68: fifo_[fifo_wr_] = value & 0xFFFF; fifo_[fifo_wr_ + 1] = value >> 16; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    if (width == 16) {
      switch (r) {
      case 0x64: capcnt_ = (capcnt_ & 0xFFFF0000) | (value & 0x1F1F); return;
      case 0x66: capcnt_ = (capcnt_ & 0x0000FFFF) | ((value & 0xEF3F) << 16); return;
      case 0x68: fifo_[fifo_wr_] = value; return;
      case 0x6A: fifo_[fifo_wr_ + 1] = value; fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;   // the write pointer advances on the high half
      case 0x6C: mb(0, value & 0xC01F); return;
      default: return;
      }
    }
    switch (r) {
    case 0x64: capcnt_ = (capcnt_ & 0xFFFFFF00) | (value & 0x1F); return;
    case 0x65: capcnt_ = (capcnt_ & 0xFFFF00FF) | ((value & 0x1F) << 8); return;
    case 0x66: capcnt_ = (capcnt_ & 0xFF00FFFF) | ((value & 0x3F) << 16); return;
    case 0x67: capcnt_ = (capcnt_ & 0x00FFFFFF) | ((value & 0xEF) << 24); return;
    case 0x68: fifo_[fifo_wr_] = static_cast<u16>(value * 0x0101); return;
    case 0x6A: fifo_[fifo_wr_ + 1] = static_cast<u16>(value * 0x0101); return;
    case 0x6B: fifo_wr_ = (fifo_wr_ + 2) & 0xF; return;
    case 0x6C: mb(0, (master_bright_g_[0] & 0xFF00) | (value & 0x1F)); return;
    case 0x6D: mb(0, (master_bright_g_[0] & 0x00FF) | ((value & 0xC0) << 8)); return;
    default: return;
    }
  }
  if (r >= 0x1064 && r < 0x1070) {
    if (r == 0x106C && width >= 16) mb(1, value & 0xC01F);
    else if (r == 0x106C) mb(1, (master_bright_g_[1] & 0xFF00) | (value & 0x1F));
    else if (r == 0x106D) mb(1, (master_bright_g_[1] & 0x00FF) | ((value & 0xC0) << 8));
    return;
  }
  const int e = r >= 0x1000;
  engine[e].write(addr, width, value);
  // Engine A switching to VRAM display mid-frame starts reading an LCDC bank
  // the trap does not cover: render the rest of the frame per line.
  if (e == 0 && (r & 0xFFF) < 4 && trap_armed_ && !trap_lcdc_ && ((engine[0].read(0x04000000, 32) >> 16) & 3) == 2) fall_back_per_line(3);
}

void Gpu::set_powcnt(u16 value) {
  engine[0].powcnt_write(value);
  engine[1].powcnt_write(value);
  nds_.gpu3d.set_powcnt(value);
}

// ---- slow-path stores and the VRAM trap ---------------------------------------

// Guest bytes change now, engine copy via the journal. A store of the value already there is not journaled.
void Gpu::palette_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.palette.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].palette_written(off & 0x3FF, width, value);
}
void Gpu::oam_store(Cpu cpu, u32 addr, u32 width, u32 value) {
  const u32 off = addr & 0x7FF, n = width / 8;
  u8* host = nds_.bus.oam.get() + off;
  if (std::memcmp(host, &value, n) == 0) return;
  if (nds_.cpu(cpu).page_table.entry(addr) & mem::TAG_CODE) mem::store_code(host, &value, n); else std::memcpy(host, &value, n);
  engine[off >> 10].oam_written(off & 0x3FF, width, value);
}

// A store into VRAM the engines can see, before it lands. Only ARM9 reaches
// the engine windows; the LCDC window matters only while it is displayed.
void Gpu::vram_store_trap(Cpu cpu, u32 addr) {
  if (!trap_armed_ || cpu != Cpu::ARM9) return;
  if (addr >= 0x06800000 && !trap_lcdc_) return;
  // A's deferred batch: a store reaching what it reads joins it, finishing
  // the frame and lifting the trap. B's windows cannot trigger this.
  if (a_deferred_) {
    if (reach_engines(addr) & 1) { prof::add(prof::C_2D_A_JOIN_STORES, 1); join_worker(JoinSite::Trap); }
    return;
  }
  // A store is charged to both engines, so both per-line means the whole frame is.
  if (per_line_[0] && per_line_[1]) {
    // Lag mode: the store may land on a line engine B is still drawing.
    prof::add(prof::C_2D_LAG_STORES, 1);
    const u32 reach = reach_engines(addr);
    const bool b_joined = ((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1]);
    if (b_joined) { prof::add(prof::C_2D_LAG_STORE_JOINS, 1); join_worker(JoinSite::Trap); }
    // Past either limit, drop lag and lift the trap unless the other engine is still
    // batching. Nothing may stay in flight once the trap is gone.
    const bool over = (b_joined && ++lag_trap_hits_ >= LAG_TRAP_LIMIT) || ++lag_trap_stores_ >= LAG_STORE_LIMIT;
    if (over && lag_frame_) {
      if (!b_joined) join_worker(JoinSite::Trap);
      lag_frame_ = false;
      if (per_line_[0] && per_line_[1]) disarm_trap();
      prof::add(prof::C_2D_LAG_DROPPED, 1);
    }
    return;
  }
  prof::add(prof::C_2D_TRAP_HITS, 1);
  // One engine per-line with a lag line in flight, the other batching: the
  // catch-up below only joins if the batching engine has lines due, so a store
  // reaching the in-flight line waits for it here.
  if (inflight_[0] || inflight_[1]) {
    const u32 reach = reach_engines(addr);
    if (((reach & 1) && inflight_[0]) || ((reach & 2) && inflight_[1])) join_worker(JoinSite::Trap);
  }
  // Budget stays per engine, even though a store is charged to both.
  u32 burst_mask = 0;
  const u32 limit = lazy_probe_ ? LAZY_PROBE_BURSTS : LAZY_BURST_LIMIT;
  for (int e = 0; e < 2; ++e)
    if (!per_line_[e] && ++lazy_bursts_[e] < limit) burst_mask |= 1u << e;
  if (burst_mask) {
    catch_up(burst_mask);
    for (int e = 0; e < 2; ++e)
      if (burst_mask & (1u << e)) { per_line_[e] = true; burst_[e] = true; burst_left_[e] = LAZY_BURST_LINES; }
    // As in fall_back_per_line: a lag frame's in-flight line still needs the trap.
    if (!lag_frame_ && per_line_[0] && per_line_[1]) disarm_trap();
    return;
  }
  lazy_limit_hit_ = true;
  fall_back_per_line(3);
}

// `moved_2d`: engines whose read views this remap actually moves. CENSUS
// ONLY for now -- the catch-up below is still unconditional.
bool Gpu::vram_remap_begin(u32 moved_2d) {
  if (prof::enabled) {
    prof::add(prof::C_VRAM_REMAP, 1);
    prof::add(moved_2d ? prof::C_VRAM_REMAP_2D_MOVED : prof::C_VRAM_REMAP_2D_STILL, 1);
    if (lines_in_flight()) {
      prof::add(prof::C_VRAM_REMAP_INFLIGHT, 1);
      // capture_on_ is cleared at VBlank before remaps land, so it says nothing here;
      // what matters is whether the in-flight job is rendering a capture (capture_render_).
      const bool alt = phase_period_ > 1, cap = capture_render_;
      if (alt) prof::add(prof::C_VRAM_REMAP_INFLIGHT_ALT, 1);
      if (cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CAP, 1);
      if (!alt && !cap) prof::add(prof::C_VRAM_REMAP_INFLIGHT_CLEAN, 1);
    }
    const u32 f = frontier();
    if (f && ((render_next_[0] < f && render_next_[0] < SCREEN_H) ||
              (render_next_[1] < f && render_next_[1] < SCREEN_H)))
      prof::add(prof::C_VRAM_REMAP_PENDING, 1);
  }
  lazy_probe_period_ = LAZY_PROBE_PERIOD;   // remap = scene change: probe soon
  if (lazy_probe_in_ > LAZY_PROBE_PERIOD) lazy_probe_in_ = LAZY_PROBE_PERIOD;
  catch_up(3);
  join_worker(JoinSite::Remap);
  engine[0].vram_remapped(); engine[1].vram_remapped();
  const bool was = trap_armed_;
  if (was) disarm_trap();
  return was;
}
void Gpu::vram_remap_end(bool trapped) { if (trapped) arm_trap(); }

void Gpu::arm_trap() {
  // LCDC banks trapped when engine A displays one, or a capture writes one.
  trap_lcdc_ = ((engine[0].dispcnt() >> 16) & 3) == 2 || capture_on_;
  // Armed for the whole VRAM window: arming only the batching engine's
  // windows costs more in page-table toggling than it saves. Lag frames that
  // aren't batching only need A's windows guarded (B may stream via per-scanline HDMA).
  trap_a_only_ = lag_frame_ && !lazy_frame_;
  nds_.bus.set_vram_trap(true, trap_lcdc_, trap_a_only_);
  trap_armed_ = true;
}
void Gpu::disarm_trap() {
  if (!trap_armed_) return;
  nds_.bus.set_vram_trap(false, trap_lcdc_, trap_a_only_);
  trap_armed_ = false;
}


void Gpu::catch_up(u32 mask) {
  const u32 f = frontier();
  if (f == 0) return;
  const u32 last = (f < SCREEN_H ? f : SCREEN_H) - 1;
  const bool a = (mask & 1) && render_next_[0] <= last && render_next_[0] < SCREEN_H;
  const bool b = (mask & 2) && render_next_[1] <= last && render_next_[1] < SCREEN_H;
  if (!a && !b) return;
  render_ranges(a ? render_next_[0] : 1, a ? last : 0,
                b ? render_next_[1] : 1, b ? last : 0);
  join_worker(JoinSite::CatchUp);
}
void Gpu::fall_back_per_line(u32 mask) {
  catch_up(mask);
  for (int e = 0; e < 2; ++e) if (mask & (1u << e)) { per_line_[e] = true; burst_[e] = false; }
  // Trap now guards the in-flight line, not the batch.
  if (!lag_frame_ && per_line_[0] && per_line_[1]) disarm_trap();
}

// ---- timing -----------------------------------------------------------------

void Gpu::on_hblank() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(true);
  const bool frame_reset = line_ == 262;
  if (line_ < SCREEN_H) {
    // Rendered now per-line, or all together at the last one. Both engines
    // are always in the same mode.
    u32 f[2], l[2];
    for (int e = 0; e < 2; ++e) {
      const bool batch = lazy_frame_ && !per_line_[e];
      if (batch && line_ != SCREEN_H - 1) { f[e] = 1; l[e] = 0; continue; }   // nothing yet
      f[e] = batch ? render_next_[e] : line_;
      l[e] = batch ? SCREEN_H - 1 : line_;
      if (!batch && render_next_[e] < line_) f[e] = render_next_[e];          // catch up anything skipped
    }
    // Draws account for themselves; subtracted from this hook's time. The
    // worker join in render_ranges has no scope of its own, so it's added back rather than subtracted.
    const auto t_draw0 = prof::enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const u64 join0 = prof::enabled ? join_wait_ns_ : 0;
    render_ranges(f[0], l[0], f[1], l[1]);
    if (prof::enabled)
      prof::add_ns(prof::GPU_LINE, static_cast<u64>(-(std::chrono::steady_clock::now() - t_draw0).count()) + (join_wait_ns_ - join0));
    // End of a burst window: the lines after this one batch again, trapped.
    for (int e = 0; e < 2; ++e)
      if (burst_[e] && --burst_left_[e] == 0 && line_ < SCREEN_H - 1) {
        burst_[e] = false; per_line_[e] = false; render_next_[e] = line_ + 1; arm_trap();
      }
    hblank_done_ = true;
    nds_.dma.check(Cpu::ARM9, dma::MODE9_HBLANK);
  } else {
    hblank_done_ = true;
    engine[0].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_PREDRAW, line_, frame_reset);
    if (line_ == 215) {
      // 3D flushed at VBlank rasterises now, ahead of the next frame's display
      // lines, so frameskip is decided here for begin_frame to latch.
      skip_next_ = skip_req_ && skippable();
      const auto t_r0 = prof::enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      if (!skip_next_) nds_.gpu3d.render_frame(); else nds_.gpu3d.note_raster_skipped();
      if (prof::enabled) prof::add_ns(prof::GPU_LINE, static_cast<u64>(-(std::chrono::steady_clock::now() - t_r0).count()));
      if (probe_enabled_) async_probe_start();
    } else if (line_ == 262) {
      engine[0].latch(Engine2D::L_SPRITES, 0, false); engine[1].latch(Engine2D::L_SPRITES, 0, false);
      // Engine A's skip flag resets at the join while its lines are in flight (finish_a).
      if (!inflight_[0]) skipped_[0] = false;
      skipped_[1] = false;
    }
    engine[0].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
    engine[1].latch(Engine2D::L_POSTDRAW, line_, frame_reset);
  }
  nds_.sched.schedule(EventId::VBlank_Scanline, nds_.sched.event_time() + (CYCLES_PER_SCANLINE - HBLANK_START), ev_scanline);
}

// ---- async-raster probe (DS_ASYNC_PROBE=1, measurement only) ----------------
// Hashes texture/texture-palette VRAM at line 215 and at two join deadlines
// (line 0 next frame, line 192 swap): a change means an async raster worker
// would have read bytes the CPU was writing.
namespace {
u64 hash_view(const VramView& v) {
  u64 h = 0xcbf29ce484222325ull;
  for (u32 b = 0; b < v.blocks(); ++b) {
    const u8* p = v.ptr[b];
    if (!p) { h = (h ^ 0x9e37) * 0x100000001b3ull; continue; }
    for (u32 o = 0; o < VramView::BLOCK; o += 8) {
      u64 w; std::memcpy(&w, p + o, 8);
      h = (h ^ w) * 0x100000001b3ull;
    }
  }
  return h;
}
u64 hash_tex_vram(const VramMap& vm) { return hash_view(vm.texture) * 31 + hash_view(vm.texpal); }
} // namespace

void Gpu::async_probe_start() {
  const VramMap& vm = nds_.bus.vram_map();
  probe_hash_ = hash_tex_vram(vm);
  probe_open_ = true;
  probe_vramcnt_at_l0_ = 0;
  prof::async_window = true;
  prof::add(prof::C_ASYNC_FRAMES, 1);
}

void Gpu::async_probe_check(bool at_line0) {
  if (!probe_open_) return;
  const bool dirty = hash_tex_vram(nds_.bus.vram_map()) != probe_hash_;
  if (at_line0) {
    if (dirty) prof::add(prof::C_ASYNC_DIRTY_L0, 1);
    probe_vramcnt_at_l0_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
    if (probe_vramcnt_at_l0_ != probe_vramcnt_base_) prof::add(prof::C_ASYNC_VRAMCNT_L0, 1);
    return;
  }
  if (dirty) prof::add(prof::C_ASYNC_DIRTY_SWAP, 1);
  probe_open_ = false;
  prof::async_window = false;
  probe_vramcnt_base_ = prof::count(prof::C_ASYNC_VRAMCNT_SWAP);
}

void Gpu::on_scanline_start() {
  prof::Scope hook(prof::GPU_LINE);
  nds_.io.set_hblank(false);
  line_ = static_cast<u16>((line_ + 1) % SCANLINES_PER_FRAME);
  hblank_done_ = false;

  // Display lines evaluate window edges in step_engine; the rest go through the latch.
  if (line_ >= SCREEN_H) { engine[0].latch(Engine2D::L_WINDOWS, line_, false); engine[1].latch(Engine2D::L_WINDOWS, line_, false); }
  if (line_ == 0) {
    // All display lines must be drawn before the frontend reads them and before begin_frame runs.
    { prof::Scope j(prof::JOIN0); join_worker(JoinSite::Line0); }
    if (probe_enabled_) async_probe_check(true);
    { prof::Scope b(prof::BEGIN_FRAME); begin_frame(); }
    nds_.frame_ready = true;
  } else if (line_ == 192) {
    if (probe_enabled_) async_probe_check(false);
    nds_.io.set_vblank(true);
    fifo_rd_ = fifo_wr_ = 0;
    nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.dma.check(Cpu::ARM9, dma::MODE9_VBLANK);
    nds_.dma.check(Cpu::ARM7, dma::MODE7_VBLANK);
    { prof::Scope v(prof::GX_VBLANK); nds_.gpu3d.vblank(); }
    if (capture_on_) { capcnt_ &= ~(1u << 31); capture_on_ = false; }
  } else if (line_ == 262) nds_.io.set_vblank(false);
  if (line_ >= 2 && line_ < 194) nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  else if (line_ == 194) nds_.dma.stop(Cpu::ARM9, dma::MODE9_DISPLAY_START);
  if (line_ < 192 && run_fifo_) nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 32 * 2, ev_fifo, 0);
  nds_.io.set_vcount(line_);
  nds_.sched.schedule(EventId::HBlank, nds_.sched.event_time() + HBLANK_START, ev_hblank);
}

// Signature of only structural display choices (driving engine, display
// mode/bank, capture destination); per-frame changes like fades/scroll are excluded.
void Gpu::update_phase() {
  const u32 a = engine[0].dispcnt(), b = engine[1].dispcnt();
  const u32 sig = ((nds_.io.powcnt1 >> 15) & 1)
                | (((a >> 16) & 3) << 1) | (((a >> 18) & 3) << 3)
                | (((b >> 16) & 3) << 5)
                | ((capture_on_ ? 1u + ((capcnt_ >> 16) & 0xF) : 0u) << 7);
  for (u32 i = 0; i + 1 < PHASE_HISTORY; ++i) phase_sig_[i] = phase_sig_[i + 1];
  phase_sig_[PHASE_HISTORY - 1] = sig;
  if (phase_seen_ < PHASE_HISTORY) { ++phase_seen_; phase_period_ = 1; return; }
  // Smallest period that explains the whole window; none found means treat as 1.
  for (u32 p = 1; p <= PHASE_MAX; ++p) {
    bool ok = true;
    for (u32 i = p; i < PHASE_HISTORY && ok; ++i) ok = phase_sig_[i] == phase_sig_[i - p];
    if (ok) { phase_period_ = static_cast<u8>(p); return; }
  }
  phase_period_ = 1;
}

void Gpu::begin_frame() {
  frame_begun_ = true;
  if (g_dbg_gpu)
    std::fprintf(stderr, "[gpu] frame %llu powcnt %04x dispcntA %08x dispcntB %08x mb %04x/%04x cap %08x vramcnt %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 static_cast<unsigned long long>(nds_.frame_count), nds_.io.powcnt1, engine[0].dispcnt(), engine[1].dispcnt(), master_bright_g_[0], master_bright_g_[1], capcnt_,
                 nds_.io.vramcnt[0], nds_.io.vramcnt[1], nds_.io.vramcnt[2], nds_.io.vramcnt[3], nds_.io.vramcnt[4], nds_.io.vramcnt[5], nds_.io.vramcnt[6], nds_.io.vramcnt[7], nds_.io.vramcnt[8]);
  if (g_dbg_vramnz) {
    std::fprintf(stderr, "[vram] frame %llu nz:", static_cast<unsigned long long>(nds_.frame_count));
    for (int i = 0; i < 9; ++i) { u32 n = 0; const u8* b = nds_.bus.vram_bank(i); for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) n += b[k] != 0; std::fprintf(stderr, " %c=%u", 'A' + i, n); }
    for (int i : {5, 7}) { const u8* b = nds_.bus.vram_bank(i); u32 lo = ~0u, hi = 0; for (u32 k = 0; k < mem::Bus::VRAM_BANK_SIZES[i]; ++k) if (b[k]) { if (k < lo) lo = k; hi = k; } std::fprintf(stderr, " %c[%x..%x]", 'A' + i, lo, hi); }
    std::fputc('\n', stderr);
  }
  if (const char* f = g_dump_frame) {   // DS_DEBUG_DUMP_LINE=L: engine state and one rendered line
    if (nds_.frame_count == static_cast<u64>(std::atoi(f))) {
      const char* l = std::getenv("DS_DEBUG_DUMP_LINE"); const u32 line = l ? std::atoi(l) : 96;
      engine[0].debug_dump(line); engine[1].debug_dump(line);
    }
  }
  screens_on_ = nds_.io.powcnt1 & 1;
  // FIFO only needs clocking when something displays/captures from it, or a DMA channel awaits it.
  run_fifo_ = uses_fifo() || nds_.dma.in_mode(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
  if (capcnt_ & (1u << 31)) capture_on_ = true;
  if (capture_on_) capture_recent_ = CAPTURE_STICKY; else if (capture_recent_) --capture_recent_;
  // 3D frame this display frame reads, latched here since the raster moves on
  // at line 215 while the compositor may still be reading it.
  ref3d_ = nds_.gpu3d.frame_ref();
  update_phase();
  if (prof::enabled) {
    prof::add(prof::C_FRAMES_TOTAL, 1);
    if (phase_period_ > 1) prof::add(prof::C_FRAMES_PHASE_ALT, 1);
    if (capture_on_) prof::add(prof::C_FRAMES_CAPTURE, 1);
  }
  // Frameskip: what line 215 assumed, re-checked now the capture bit is known.
  skip_frame_ = skip_next_ && skippable();
  if (g_dbg_skip)
    std::fprintf(stderr, "[skip] frame %llu req %d raster %d capture %d/%u fifo %d period %u -> %s\n",
                 static_cast<unsigned long long>(nds_.frame_count), skip_req_ ? 1 : 0, skip_next_ ? 1 : 0,
                 capture_on_ ? 1 : 0, capture_recent_, run_fifo_ ? 1 : 0, phase_period_, skip_frame_ ? "skipped" : "drawn");
  render_next_[0] = render_next_[1] = 0;
  per_line_prev_[0] = per_line_[0]; per_line_prev_[1] = per_line_[1];
  per_line_[0] = per_line_[1] = false; frame_finished_ = false;
  // Was last frame's trap worth arming? Both per-line at the end means no batch survived.
  if (lazy_tried_) {
    const bool wasted = (per_line_prev_[0] && per_line_prev_[1]) || lazy_limit_hit_;
    if (wasted) {
      ++lazy_futile_;
      if (lazy_probe_ && lazy_probe_period_ < LAZY_PROBE_MAX) lazy_probe_period_ *= 2;
    } else { lazy_futile_ = 0; lazy_probe_period_ = LAZY_PROBE_PERIOD; }
  }
  lazy_limit_hit_ = false;
  const bool skipping = lazy_futile_ >= LAZY_FUTILE_LIMIT;
  lazy_probe_ = false;
  if (skipping && --lazy_probe_in_ == 0) { lazy_probe_ = true; lazy_probe_in_ = lazy_probe_period_; if (g_dbg_skip) std::fprintf(stderr, "[lazy] probe at frame %llu period %u futile %u\n", (unsigned long long)nds_.frame_count, lazy_probe_period_, lazy_futile_); }
  else if (!skipping) lazy_probe_in_ = lazy_probe_period_;
  const bool futile = skipping && !lazy_probe_;
  lazy_frame_ = lazy_enabled_ && !g_no_lazy && !run_fifo_ && !futile;
  lazy_tried_ = lazy_frame_;
  if (futile) prof::add(prof::C_2D_LAZY_SKIPPED, 1);
  lazy_bursts_[0] = lazy_bursts_[1] = 0; burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0;
  lag_frame_ = !run_fifo_ && !g_no_lag;
  lag_trap_hits_ = 0; lag_trap_stores_ = 0;
  if (lazy_frame_ || lag_frame_) arm_trap();
  if (lazy_frame_) prof::add(prof::C_2D_LAZY_FRAMES, 1);
  if (lag_frame_ && !lazy_frame_) prof::add(prof::C_2D_LAG_FRAMES, 1);
  if (!lazy_frame_) per_line_[0] = per_line_[1] = true;
}

// ---- main-memory display FIFO -----------------------------------------------

bool Gpu::uses_fifo() const {
  if (((engine[0].dispcnt() >> 16) & 3) == 3) return true;
  return (capcnt_ & (1 << 25)) && ((capcnt_ >> 29) & 3) != 0;
}

void Gpu::sample_fifo(u32 offset, u32 count) {
  for (u32 i = 0; i < count; ++i) { fifo_line_[offset + i] = fifo_[fifo_rd_]; fifo_rd_ = (fifo_rd_ + 1) & 0xF; }
}

// FIFO read out in 8-pixel steps starting ~3 pixels before the visible line
// (offset from the 8-pixel DMA grid). Each step requests the next DMA transfer (mode 4).
void Gpu::on_display_fifo(u32 x) {
  if (x > 0) { if (x == 8) sample_fifo(0, 5); else sample_fifo(x - 11, 8); }
  if (x < 256) {
    nds_.dma.check(Cpu::ARM9, dma::MODE9_DISPLAY_FIFO);
    nds_.sched.schedule(EventId::DisplayFifo, nds_.sched.event_time() + 6 * 8 * 2, ev_fifo, x + 8);
  } else sample_fifo(253, 3);
}

// ---- rendering and the output stage ------------------------------------------

// One engine's run of display lines on the worker; engine state is disjoint.
void Gpu::worker_job(void* self) {
  Gpu& g = *static_cast<Gpu*>(self);
  for (int e = 0; e < 2; ++e)
    for (u32 l = g.job_first_[e]; l <= g.job_last_[e]; ++l) g.step_engine(e, l);
  for (u32 i = 0; i < g.bscale_n_; ++i) {
    const StashedLine& st = g.bscale_[i];
    g.emit_scaled(st.screen, st.line, st.px);
    if (g.splits_on()) g.emit_splits_b(st.screen, st.line, st.px, st.key);
  }
}

void Gpu::join_worker(JoinSite site) {
  if (!inflight_[0] && !inflight_[1] && !scale_inflight_) return;
  const bool dbg = g_dbg_join;
  if (dbg) std::fprintf(stderr, "[join] frame %llu line %u hblank %d a %u..%u b %u..%u deferred %d\n", (unsigned long long)nds_.frame_count, line_, hblank_done_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], a_deferred_ ? 1 : 0);
  { const auto t0 = std::chrono::steady_clock::now(); worker_.wait();
    const u64 dt = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
    join_wait_ns_ += dt; prof::add_ns(prof::W2D_JOIN, dt);
    if (prof::enabled) {
      prof::add(prof::C_W2D_JOIN_CALLS, 1);
      static constexpr prof::Counter kBySite[] = {
        prof::C_W2D_JOIN_NS_CATCHUP, prof::C_W2D_JOIN_NS_TRAP, prof::C_W2D_JOIN_NS_JOURNAL,
        prof::C_W2D_JOIN_NS_LINE0, prof::C_W2D_JOIN_NS_REMAP,
        prof::C_W2D_JOIN_NS_RPRE, prof::C_W2D_JOIN_NS_RPOST, prof::C_W2D_JOIN_NS_OTHER };
      prof::add(kBySite[static_cast<int>(site)], dt);
    } }
  inflight_[0] = inflight_[1] = false; scale_inflight_ = false; bscale_n_ = 0;
  if (a_deferred_) { a_deferred_ = false; finish_a(); }
}

// Engine A's deferred batch has been joined: what render_ranges does at frame
// end, done now (read trap, then journal, then frame end).
void Gpu::finish_a() {
  if (read_trap_bank_ >= 0) { nds_.bus.set_lcdc_read_trap(read_trap_bank_, false); read_trap_bank_ = -1; }
  if (line_ == 0 || (line_ == 262 && hblank_done_)) skipped_[0] = false;
  engine[0].apply_pending();
  if (frame_finished_) { engine[0].frame_done(); disarm_trap(); }
}

void Gpu::debug_dump(FILE* f) {
  std::fprintf(f, "  gpu: line %u hblank_done %d render_next %u/%u lazy %d per_line %d/%d trap %d job a %u..%u b %u..%u inflight %d/%d deferred %d read_trap %d lag %d frame_ready %d\n", line_, hblank_done_ ? 1 : 0, render_next_[0], render_next_[1],
               lazy_frame_ ? 1 : 0, per_line_[0] ? 1 : 0, per_line_[1] ? 1 : 0, trap_armed_ ? 1 : 0, job_first_[0], job_last_[0], job_first_[1], job_last_[1], inflight_[0] ? 1 : 0, inflight_[1] ? 1 : 0, a_deferred_ ? 1 : 0, read_trap_bank_, lag_frame_ ? 1 : 0, nds_.frame_ready ? 1 : 0);
  worker_.debug_dump(f);
  nds_.gpu3d.debug_dump(f);
}

// Render display lines of both engines: one hand-off to the worker, the other
// engine's run here. An empty range (first > last) means nothing due.
void Gpu::render_ranges(u32 af, u32 al, u32 bf, u32 bl) {
  const bool a_has = af <= al, b_has = bf <= bl;
  if (!a_has && !b_has) return;
  join_worker(JoinSite::RangesPre);
  capcnt_render_ = capcnt_; capture_render_ = capture_on_;
  lcdc_mask_render_ = nds_.bus.vram_map().lcdc_mask;   // latched so the job doesn't consult a live map that may be rebuilding
  const bool dbg = g_dbg_join;
  auto hand = [&](bool a, bool b) {
    if (dbg) std::fprintf(stderr, "[hand] frame %llu line %u a %u..%u b %u..%u lag %d stash %u\n", (unsigned long long)nds_.frame_count, line_, a ? af : 1, a ? al : 0, b ? bf : 1, b ? bl : 0, lag_frame_ ? 1 : 0, bscale_n_);
    job_first_[0] = a ? af : 1; job_last_[0] = a ? al : 0;
    job_first_[1] = b ? bf : 1; job_last_[1] = b ? bl : 0;
    scale_inflight_ = bscale_n_ > 0;
    worker_.dispatch();
    inflight_[0] = a; inflight_[1] = b;
    // A capture in flight writes an LCDC bank the guest may read before the join: trap it.
    if (a && capture_render_ && read_trap_bank_ < 0) {
      const int bank = static_cast<int>((capcnt_render_ >> 16) & 3);
      nds_.bus.set_lcdc_read_trap(bank, true);
      read_trap_bank_ = bank;
    }
  };
  const u32 a_len = a_has ? al - af + 1 : 0, b_len = b_has ? bl - bf + 1 : 0;
  const u32 last = a_has && b_has ? (al > bl ? al : bl) : a_has ? al : bl;
  bool a_handed = false, b_handed = false;
  // Read once: parked() can change under separate branches.
  const bool parked_at_decision = prof::enabled ? worker_.parked() : false;
  if (prof::enabled) {
    prof::add(prof::C_RR_CALLS, 1);
    prof::add(parked_at_decision ? prof::C_RR_PARKED : prof::C_RR_AWAKE, 1);
    if (b_has && b_len < 24 && !parked_at_decision) prof::add(prof::C_RR_SHORT_HANDED_AWAKE, 1);
    if (b_has && b_len < 24 && parked_at_decision) prof::add(prof::C_RR_SHORT_INLINE_PARKED, 1);
  }
  if (a_has && al == SCREEN_H - 1 && (a_len >= 24 || (!worker_.parked() && a_len >= b_len))) {
    // Run to the last display line: deferred join, engine B drawn here meanwhile.
    prof::add(prof::C_RR_DEFER_A, 1);
    hand(true, false);
    a_handed = true; a_deferred_ = true;
  } else if (lag_frame_ && (a_has || (b_has && scaling()))) {
    // Lag: A's lines stay in flight until next HBlank unless this is the
    // last one. B is drawn here (its window streams per-line, joining lag on
    // every store), but its scaling is stashed with the job (bscale_).
    if (b_has && scaling()) {
      bscale_defer_ = true;
      for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
      bscale_defer_ = false;
      b_handed = true;                        // drawn, its scaling in flight
    }
    prof::add(prof::C_RR_LAG, 1);
    hand(a_has, false);
    a_handed = a_has;
  } else if (b_has && (b_len >= 24 || !worker_.parked())) {
    // Short run against a parked worker drawn here: wake-up costs more than the lines do.
    prof::add(prof::C_RR_HAND_B, 1);
    hand(false, true);
    b_handed = true;
  } else {
    prof::add(prof::C_RR_NONE, 1);
  }
  if (a_has) prof::add(prof::C_2D_RANGE_A, 1);
  if (b_has) prof::add(prof::C_2D_RANGE_B, 1);
  if (prof::enabled) {
    prof::add(prof::C_RR_LINES_HANDED, (a_handed ? a_len : 0) + (b_handed ? b_len : 0));
    prof::add(prof::C_RR_LINES_INLINE, (a_has && !a_handed ? a_len : 0) + (b_has && !b_handed ? b_len : 0));
  }
  if (a_has && !a_handed) for (u32 x = af; x <= al; ++x) step_engine(0, x);
  if (b_has && !b_handed) for (u32 x = bf; x <= bl; ++x) step_engine(1, x);
  // A's deferred batch stays in flight until line 0; a lagged run until the
  // next line. The last display line always joins.
  if (!a_deferred_) {
    if ((a_handed || b_handed || scale_inflight_) && lag_frame_ && last < SCREEN_H - 1) prof::add(prof::C_2D_LAG_LINES, 1);
    else join_worker(JoinSite::RangesPost);
  }
  if (a_has) render_next_[0] = al + 1;
  if (b_has) render_next_[1] = bl + 1;
  if (!frame_finished_ && render_next_[0] >= SCREEN_H && render_next_[1] >= SCREEN_H) {
    frame_finished_ = true;
    if (a_deferred_) engine[1].frame_done();   // engine A's end (and traps) happen at the join in finish_a
    else {
      join_worker(JoinSite::RangesPost);
      if (read_trap_bank_ >= 0) { nds_.bus.set_lcdc_read_trap(read_trap_bank_, false); read_trap_bank_ = -1; }
      engine[0].frame_done(); engine[1].frame_done(); disarm_trap();
    }
  }
}

// One engine's display line, in hardware order: pre-scanline writes, window
// edges, the line's own writes, latches, the line, its output, next line's sprites.
void Gpu::step_engine(int e, u32 line) {
  Engine2D& en = engine[e];
  {
    prof::Scope jn(prof::JOURNAL);
    en.replay_to(line * 2);
    en.update_windows(line);
    en.replay_to(line * 2 + 1);
    en.pre_draw(line, false);
  }
  // Engine B on a hidden screen draws nothing; the line it comes back on re-renders its sprites.
  const bool draw = !skip_frame_ && !(e == 1 && !screen_visible_[en.screen()]);
  if (!draw) skipped_[e] = true; else if (skipped_[e]) { skipped_[e] = false; en.render_sprites(line); }
  // Reading the 3D line joins the raster bands: skip it when the raster never ran.
  if (e == 0 && (draw || capture_render_)) { prof::Scope l3(prof::R3D_LINE); line3d_ = nds_.gpu3d.line(ref3d_, line); en.set_3d_line(line3d_); split3d_ = splits_on() ? nds_.gpu3d.split_line(ref3d_, line) : nullptr; }
  if (draw) { en.render_line(line); output_engine(e, line); }
  if (e == 0 && capture_render_ && !skip_frame_) { DS_PROF(CAPTURE); capture(line); }
  // Sprites are rendered one line ahead of the backgrounds.
  if (draw && line < SCREEN_H - 1) {
    prof::Scope sc(prof::OBJ_DRAW, e == 0);
    en.render_sprites(line + 1);
  }
  { prof::Scope jn(prof::JOURNAL); en.post_draw(false); }
}

// A line's identity for the carry: its colours at the 15 bits a capture keeps.
static inline u64 key_finish(u64 h) { h ^= h >> 29; h *= 0xFF51AFD7ED558CCDull; return (h ^ (h >> 32)) | 1; }   // 0 = empty slot
static u64 line_key(const u32* px) {
  u64 h = 0x9E3779B97F4A7C15ull;
  for (u32 i = 0; i < SCREEN_W; i += 2) {
    const u64 v = (px[i] & 0x3E3E3Eu) | (static_cast<u64>(px[i + 1] & 0x3E3E3Eu) << 32);
    h = ((h << 7) | (h >> 57)) ^ v;
  }
  return key_finish(h);
}
static u64 line_key15(const u16* px) {
  // Same value line_key gives once RGB666: 5 bits/channel at bits 1, 9, 17.
  auto c = [](u32 v) -> u32 { return ((v & 0x1Fu) << 1) | ((v & 0x3E0u) << 4) | ((v & 0x7C00u) << 7); };
  u64 h = 0x9E3779B97F4A7C15ull;
  for (u32 i = 0; i < SCREEN_W; i += 2) {
    const u64 v = c(px[i]) | (static_cast<u64>(c(px[i + 1])) << 32);
    h = ((h << 7) | (h >> 57)) ^ v;
  }
  return key_finish(h);
}

// The output stage for one engine's line: display mode, master brightness,
// 6->8 bit expansion, into the screen POWCNT1 bit 15 gives it (or scaled
// straight into the frontend's buffer).
void Gpu::output_engine(int e, u32 line) {
  prof::Scope sc(prof::OUTPUT, e == 0);
  const Engine2D& en = engine[e];
  const int screen = en.screen();
  const bool scaled = scaling();
  u32* dst = scaled ? line_out_[e].data() : fb_[screen].data() + line * SCREEN_W;
  if (screens_on_) {
    if (e == 0) {
      const u32 mode = (en.dispcnt() >> 16) & 3;
      if (mode == 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else if (mode >= 2) output_a(line, dst);      // VRAM / FIFO display: expanded inside
      else { output_a(line, dst); expand_colours(dst); }
    } else {
      if ((en.dispcnt() >> 16) & 1) kern::active::output_line(en.output(), en.master_bright(), dst);
      else { output_b(dst); expand_colours(dst); }
    }
  } else { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF000000; }
  if (scaled) {
    if (e == 1 && bscale_defer_ && bscale_n_ < SCREEN_H) {
      StashedLine& st = bscale_[bscale_n_++];
      st.line = line; st.screen = screen;
      st.key = splits_on() && screens_on_ && ((en.dispcnt() >> 16) & 1) && sp_last_store_ + kSplitGens > nds_.frame_count ? line_key(en.output()) : 0;
      std::memcpy(st.px, dst, sizeof st.px);
    } else {
      emit_scaled(screen, line, dst);
      if (splits_on()) {
        const u32 mode = e == 0 ? (en.dispcnt() >> 16) & 3 : ((en.dispcnt() >> 16) & 1);
        const bool a_3d = e == 0 && split3d_ && (mode == 1 || capture_render_);
        u64 key = 0;
        if (!(a_3d && mode == 1) && screens_on_ && sp_last_store_ + kSplitGens > nds_.frame_count) {
          if (mode == 1) key = line_key(en.output());
          else if (e == 0 && mode == 2) {
            const u32 bank = (en.dispcnt() >> 18) & 3;
            const VramMap& vm = nds_.bus.vram_map();   // bank pointers are remap-invariant
            if (lcdc_mask_render_ & (1u << bank)) key = line_key15(reinterpret_cast<const u16*>(vm.bank(bank)) + line * 256);
          }
        }
        emit_splits(screen, line, dst, a_3d ? en.output() : nullptr, mode == 1, key);
      }
    }
  }
}

static inline bool row_kept(const Gpu::ScaleTarget& t, u32 y) { return y >= t.y_lo && y < t.y_hi; }
static inline u32* row_at(const Gpu::ScaleTarget& t, u32 y) { return t.px + (static_cast<size_t>(y) - t.y_lo) * t.pitch; }

// ---- sub-pixel edges ------------------------------------------------------------
// After a line's rows are out, split pixels' cells are patched in place: the
// uncovered part takes the composed colour of the neighbour on that side.
void Gpu::set_shape(bool on) {
  if (on && sp_gen_.empty()) sp_gen_.resize(kSplitGens);
  shape_ = on;
  update_shape();
  for (int s = 0; s < 2; ++s) { sp_pend_n_[s] = 0; sp_pend_line_[s] = sp_prev_line_[s] = ~0u; }
}

void Gpu::update_shape() {
  const bool live = shape_ && shape_usable();
  if (live == shape_live_) return;
  shape_live_ = live;
  nds_.gpu3d.renderer().set_shape(live);
}

void Gpu::set_subpixel(bool on) {
  if (on && sp_gen_.empty()) sp_gen_.resize(kSplitGens);
  subpixel_ = on;
  nds_.gpu3d.renderer().set_subpixel(on);
  for (int s = 0; s < 2; ++s) { sp_pend_n_[s] = 0; sp_pend_line_[s] = sp_prev_line_[s] = ~0u; }
}

static inline u32 rgb_dist(u32 a, u32 b) {
  auto d = [](u32 x, u32 y) { return x > y ? x - y : y - x; };
  return d(a & 0xFF, b & 0xFF) + d((a >> 8) & 0xFF, (b >> 8) & 0xFF) + d((a >> 16) & 0xFF, (b >> 16) & 0xFF);
}
constexpr u32 kSplitContrast = 24;   // summed RGB888 distance below which a cut would not show
constexpr u32 kSplitStep = 60;       // and what a weak record's step must reach (see split_admit)
constexpr u32 kSplitBold = 120;      // a step this big is a boundary whatever runs along it

// The carry: a game showing its 3D via display capture never shows split
// lines directly, so a captured line's splits are remembered under the
// line's content and reattached wherever that content is later shown.
// Generations by frame: the one being written is never read.
void Gpu::carry_store(u64 key, const SplitRec* recs, u32 n) {
  const u64 frame = nds_.frame_count;
  SplitGen& g = sp_gen_[frame % kSplitGens];
  if (g.frame != frame) { g.frame = frame; g.used = 0; for (auto& sl : g.slot) sl.key = 0; }
  if (g.used + n > g.recs.size()) return;
  for (u32 probe = 0; probe < 8; ++probe) {
    auto& sl = g.slot[((key >> 8) + probe) & (g.slot.size() - 1)];
    if (sl.key && sl.key != key) continue;
    sl.key = key; sl.off = g.used; sl.n = static_cast<u16>(n);
    std::memcpy(&g.recs[g.used], recs, n * sizeof(SplitRec));
    g.used += n;
    sp_last_store_ = frame;
    return;
  }
}

u32 Gpu::carry_find(u64 key, const SplitRec** recs) const {
  const u64 frame = nds_.frame_count;
  for (u32 k = 1; k < kSplitGens; ++k) {
    const SplitGen& g = sp_gen_[(frame + kSplitGens - k) % kSplitGens];
    if (g.frame + kSplitGens <= frame || g.frame >= frame) continue;     // stale, or the one being written
    for (u32 probe = 0; probe < 8; ++probe) {
      const auto& sl = g.slot[((key >> 8) + probe) & (g.slot.size() - 1)];
      if (!sl.key) break;
      if (sl.key == key) { *recs = &g.recs[sl.off]; return sl.n; }
    }
  }
  return 0;
}

// Whether a cut shows and should. `own` is the cell's line, `other` the line
// the test may also look at, `ox` the neighbour column. Strong records need
// only contrast; weak ones must be a step, not texture (colour across the cut
// must clear how much either side varies along the edge).
bool Gpu::split_admit(const SplitRec& r, const u32* own, const u32* other, u32 ox, bool horiz) {
  const u32 x = r.x;
  const u32 p = own[x], o = horiz ? other[ox] : own[ox];
  const u32 across = rgb_dist(p, o);
  if (across < kSplitContrast) return false;
  if (!(r.side & 0x80)) return true;
  if (across < kSplitStep) return false;
  if (across >= kSplitBold) return true;
  auto nearest = [](u32 c, const u32* row, u32 cx, bool self) {
    u32 best = ~0u;
    for (s32 d = -1; d <= 1; ++d) {
      if (d == 0 && !self) continue;
      const s32 xi = static_cast<s32>(cx) + d;
      if (xi < 0 || xi >= static_cast<s32>(SCREEN_W)) continue;
      best = std::min(best, rgb_dist(c, row[xi]));
    }
    return best;
  };
  u32 noise;
  if (horiz) noise = std::max(nearest(p, own, x, false), nearest(o, other, ox, false));   // along = the same lines, x +- 1
  else {
    if (!other) return false;                                                             // along = the line above
    noise = std::max(nearest(p, other, x, true), nearest(o, other, ox, true));
  }
  return across > 3 * noise;
}

void Gpu::emit_splits_b(int screen, u32 line, const u32* dst, u64 key) {
  emit_splits(screen, line, dst, nullptr, true, key);   // no composed line: engine A's split map is not read
}

// `composed`: A's composed line when shown/captured; `key`: shown line's content key when not A's own 3D (0 = none).
void Gpu::emit_splits(int screen, u32 line, const u32* dst, const Pixel* composed, bool shown, u64 key) {
  const ScaleTarget& t = scale_[screen];
  const bool usable = !t.bilinear && !t.chunky && t.h >= SCREEN_H && t.xrun[SCREEN_W] >= SCREEN_W;
  const u32* const l3_now = composed ? line3d_ : nullptr;
  // The 3D layer is what shows at x, unblended (a record's colour source must be).
  auto own3d = [&](u32 x) { return !composed || !l3_now || (x < SCREEN_W && !((composed[x] ^ l3_now[x]) & 0x3F3F3F) && ((l3_now[x] >> 24) & 0x1F) == 31); };
  // Cuts of the line above that were waiting for this one.
  if (sp_pend_n_[screen]) {
    if (usable && sp_pend_line_[screen] + 1 == line)
      for (u32 i = 0; i < sp_pend_n_[screen]; ++i) {
        const SplitRec& r = sp_pend_[screen][i];
        // An outline's band reaching down into this line: this cell's upper part from the line above.
        if (r.side & 0x10) { patch_cell(t, line, r, sp_prev_[screen][r.x]); continue; }
        if ((r.side & 0x08) && !own3d(r.x)) continue;   // a grown triangle's pixel must be the front's own
        if (split_admit(r, sp_prev_[screen], dst, r.x, true)) patch_cell(t, line - 1, r, dst[r.x]);
      }
    sp_pend_n_[screen] = 0;
  }
  // This line's splits: engine A's own 3D, or a remembered line's.
  SplitRec own[2 * SCREEN_W]; u32 n = 0;
  const SplitRec* recs = own;
  const u32* const sm = composed ? split3d_ : nullptr;   // engine A's thread only
  const u32* const l3 = composed ? line3d_ : nullptr;
  if (composed && sm && l3) {
    for (u32 i = 0; i < 2 * SCREEN_W && n < 2 * SCREEN_W; ++i) {   // two slots a pixel: its own cut, then a spill into it
      const u32 v = sm[i], x = i >> 1;
      if (!v) continue;
      // The 3D layer must be what shows, unblended, at the polygon's pixel (or for a spill the source pixel).
      const s32 src = static_cast<s32>(static_cast<s8>(((v >> 16) & 7) << 5) >> 5);
      const bool grow = v & Renderer3D::SPLIT_GROW;
      if (grow) {
        // Edge shaping: cell is the back's (may be 2D); the front's cell the colour comes from
        // must be the 3D layer's -- beside it, on the line above (remembered), or the next line.
        const u32 s3 = v & 7;
        if (s3 <= 2 ? !own3d(x + static_cast<u32>(src)) : (s3 == 3 && !(sp_prev_line_[screen] + 1 == line && sp_prev_own_[screen][x]))) continue;
      } else {
      const u32 px = (v & Renderer3D::SPLIT_SPILL) ? x + static_cast<u32>(src) : x;
      if (px >= SCREEN_W || ((composed[px] ^ l3[px]) & 0x3F3F3F) || ((l3[px] >> 24) & 0x1F) != 31) continue;
      }
      const bool marked = v & Renderer3D::SPLIT_MARKED;
      const u8 side = static_cast<u8>((v & 7) | ((v & Renderer3D::SPLIT_WEAK) ? 0x80 : 0) | ((v & Renderer3D::SPLIT_SPILL) ? 0x40 : 0) | (marked ? 0x20 : 0) | (grow ? 0x08 : 0));
      own[n++] = SplitRec{static_cast<u8>(x), side, static_cast<u8>(marked ? (v >> 19) & 0x7F : (v >> 3) & 0x3F), static_cast<s8>(static_cast<s8>(((v >> 9) & 0x7F) << 1) >> 1), static_cast<s8>(src)};
    }
    // A plain capture of this picture: remember the line as the bank will hold it.
    if (n && capture_render_ && ((capcnt_render_ >> 29) & 3) == 0 && !(capcnt_render_ & (1u << 24)) && ((capcnt_render_ >> 20) & 3) == 3)
      carry_store(line_key(composed), own, n);
    if (!shown) n = 0;
  }
  if (!n && key && sp_last_store_ + kSplitGens > nds_.frame_count) n = carry_find(key, &recs);
  if (usable && n) {
    const bool have_prev = sp_prev_line_[screen] + 1 == line;
    for (u32 i = 0; i < n; ++i) {
      const SplitRec& r = recs[i];
      const u32 x = r.x;
      const u32* const prev = have_prev ? sp_prev_[screen] : nullptr;
      u32 nb; bool ok;
      if (r.side & 0x20) {
        // An edge-marked pixel. The outline stays a pixel wide: the band starts at the edge,
        // `pos` in from this pixel's outer boundary (-32..64, see Renderer3D::note_edge_part),
        // reaching as far into the next pixel in as it left this one. All half-plane patches of whole cells.
        const s32 pos = static_cast<s32>(r.unc) - 32;
        const u32 s3 = r.side & 7;
        const bool low = s3 == 1 || s3 == 3;
        const s32 sg = low ? -1 : 1;
        const u8 outer = static_cast<u8>(s3), inner = static_cast<u8>(s3 == 1 ? 2 : s3 == 2 ? 1 : s3 == 3 ? 4 : 3);
        auto half = [&](u32 cx, u32 cline, u8 sd, s32 amount, u32 colour) {
          if (cx >= SCREEN_W || amount <= 0) return;
          SplitRec h{static_cast<u8>(cx), sd, static_cast<u8>(std::min(32, amount)), amount >= 32 ? s8{0} : r.slope, 0};
          patch_cell(t, cline, h, colour);
        };
        if (s3 <= 2) {
          const u32 xo = x + static_cast<u32>(sg), xi = x - static_cast<u32>(sg), xii = x - static_cast<u32>(2 * sg);
          if (xo >= SCREEN_W || xi >= SCREEN_W) continue;
          const u32 O = dst[xo], C = dst[x], I = dst[xi];
          if (!split_admit(r, dst, prev, xo, false)) continue;
          if (pos < 0) { half(xo, line, inner, -pos, C); half(x, line, inner, -pos, I); }        // the band starts in the pixel beyond
          else if (pos <= 32) { half(x, line, outer, pos, O); half(xi, line, outer, pos, C); }
          else { half(x, line, outer, 32, O); half(xi, line, outer, 32, C); half(xi, line, outer, pos - 32, O); half(xii, line, outer, pos - 32, C); }
        } else if (pos > 0 && pos <= 32) {
          // Horizontal cuts never saturate; the band's reach into the line further in is the extra.
          if (s3 == 3) {                     // uncovered above: outside from the line above, band into the line below
            if (prev && split_admit(r, dst, prev, x, true)) {
              patch_cell(t, line, SplitRec{r.x, 3, static_cast<u8>(pos), r.slope, -1}, prev[x]);
              if (line + 1 < SCREEN_H) sp_pend_[screen][sp_pend_n_[screen]++] = SplitRec{r.x, static_cast<u8>(3 | 0x10), static_cast<u8>(pos), r.slope, -1};
            }
          } else {                           // uncovered below: band into the line above now, outside when the next line comes
            if (line + 1 < SCREEN_H) sp_pend_[screen][sp_pend_n_[screen]++] = SplitRec{r.x, static_cast<u8>(4 | (r.side & 0x80)), static_cast<u8>(pos), r.slope, 1};
            if (line > 0 && prev) patch_cell(t, line - 1, SplitRec{r.x, 4, static_cast<u8>(pos), r.slope, 1}, dst[x]);
          }
        }
        (void)inner;
        continue;
      }
      switch (r.side & 7) {
      case 1: case 2: {
        const u32 sx = x + static_cast<u32>(static_cast<s32>(r.src));
        if (sx >= SCREEN_W) continue;
        nb = dst[sx]; ok = split_admit(r, dst, prev, sx, false);
        break;
      }
      case 3: if (!prev) continue; nb = prev[x]; ok = split_admit(r, dst, prev, x, true); break;
      case 4: if (line + 1 < SCREEN_H) sp_pend_[screen][sp_pend_n_[screen]++] = r; continue;
      default: continue;
      }
      if (ok) patch_cell(t, line, r, nb);
    }
    sp_pend_line_[screen] = line;
  }
  std::memcpy(sp_prev_[screen], dst, sizeof sp_prev_[screen]);
  if (shape_) for (u32 x = 0; x < SCREEN_W; ++x) sp_prev_own_[screen][x] = own3d(x);
  sp_prev_line_[screen] = line;
}

// The cell of DS pixel r.x on `line`: panel columns [xrun[x], xrun[x+1]) by the
// line's rows. Along the cut's axis the uncovered part is the low side (left,
// up) up to `bound`, or the high side from it; across the axis the bound moves
// by the slope. All in 1/32 of a cell, sampled at panel pixel centres.
void Gpu::patch_cell(const ScaleTarget& t, u32 line, const SplitRec& r, u32 nb) {
  const u32 xa = t.xrun[r.x], xb = t.xrun[r.x + 1];
  const u32 ya = (line * t.h + SCREEN_H - 1) / SCREEN_H, yb = ((line + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (xa >= xb || ya >= yb) return;
  const s32 cw = static_cast<s32>(xb - xa), ch = static_cast<s32>(yb - ya);
  const s32 unc = r.unc;
  const u32 side = r.side & 7;
  const bool low = side == 1 || side == 3, horiz = side >= 3;
  const s32 bound0 = low ? unc : 32 - unc;
  // The grid's seams: leading column/row of a wide enough cell.
  const bool grid = t.grid < 256 && !t.blend;
  u32 nbd = nb;
  bool seam_col = false, seam_row = false;
  if (grid) {
    const u32 w = t.xrun[SCREEN_W];
    const u32 min_run = std::max<u32>(2, (w + SCREEN_W - 1) / SCREEN_W), min_rows = std::max<u32>(2, (t.h + SCREEN_H - 1) / SCREEN_H);
    seam_col = static_cast<u32>(cw) >= min_run && r.x % (w == 2 * SCREEN_W ? 2u : 1u) == 0;
    seam_row = static_cast<u32>(ch) >= min_rows && line % (t.h == 2 * SCREEN_H ? 2u : 1u) == 0;
    const u32 f = t.grid;
    nbd = f ? ((nb & 0xFF000000u) | (((nb & 0x00FF00FFu) * f >> 8) & 0x00FF00FFu) | (((nb & 0x0000FF00u) * f >> 8) & 0x0000FF00u)) : 0xFF000000u;
  }
  // Pixel centres in 1/32 of the cell. Cells are a few pixels each way; anything
  // larger takes the first kMax (no panel scales a DS pixel past that).
  constexpr s32 kMax = 16;
  const s32 nw = std::min(cw, kMax), nh = std::min(ch, kMax);
  s32 cx[kMax], cy[kMax];
  for (s32 i = 0; i < nw; ++i) cx[i] = ((2 * i + 1) * 32) / (2 * cw);
  for (s32 j = 0; j < nh; ++j) cy[j] = ((2 * j + 1) * 32) / (2 * ch);
  const s32 sl = r.slope;
  if (!horiz) {
    // A vertical cut: each row is a run from one end of the cell.
    for (s32 j = 0; j < nh; ++j) {
      const u32 y = ya + static_cast<u32>(j);
      if (!row_kept(t, y)) continue;
      const s32 bound = bound0 + (sl * (cy[j] - 16)) / 32;
      s32 k = 0;
      while (k < nw && cx[k] < bound) ++k;                 // pixels [0, k) are before the cut
      u32* const row = row_at(t, y) + xa;
      const bool srow = seam_row && j == 0;
      for (s32 i = low ? 0 : k, e = low ? k : nw; i < e; ++i) row[i] = (srow || (seam_col && i == 0)) ? nbd : nb;
    }
  } else {
    s32 bnd[kMax];
    for (s32 i = 0; i < nw; ++i) bnd[i] = bound0 + (sl * (cx[i] - 16)) / 32;
    for (s32 j = 0; j < nh; ++j) {
      const u32 y = ya + static_cast<u32>(j);
      if (!row_kept(t, y)) continue;
      u32* const row = row_at(t, y) + xa;
      const bool srow = seam_row && j == 0;
      const s32 f = cy[j];
      for (s32 i = 0; i < nw; ++i)
        if (low ? f < bnd[i] : f >= bnd[i]) row[i] = (srow || (seam_col && i == 0)) ? nbd : nb;
    }
  }
}

// Mean of four 0xAARRGGBB pixels, per channel, alpha forced opaque.
static inline u32 mean4(u32 a, u32 b, u32 c, u32 d) {
  const u32 rb = (((a & 0xFF00FFu) + (b & 0xFF00FFu) + (c & 0xFF00FFu) + (d & 0xFF00FFu) + 0x020002u) >> 2) & 0xFF00FFu;
  const u32 g  = (((a & 0xFF00u) + (b & 0xFF00u) + (c & 0xFF00u) + (d & 0xFF00u) + 0x0200u) >> 2) & 0xFF00u;
  return 0xFF000000u | rb | g;
}
// The colour at least two of the four share (ties to the earlier pixel), or their mean when all differ.
static inline u32 mode4(u32 a, u32 b, u32 c, u32 d) {
  if (a == b || a == c || a == d) return a;
  if (b == c || b == d) return b;
  if (c == d) return c;
  return mean4(a, b, c, d);
}

// Rec.601-ish luma, 0..255*256.
static inline u32 luma(u32 c) { return ((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29; }
static inline u32 min4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l < bl) { best = p; bl = l; } }
  return best;
}
static inline u32 max4(u32 a, u32 b, u32 c, u32 d) {
  u32 best = a, bl = luma(a);
  for (u32 p : {b, c, d}) { const u32 l = luma(p); if (l > bl) { best = p; bl = l; } }
  return best;
}
// Darkest/brightest of the four when it stands farther than `thr` from the
// block's mean luma (keeps a thin stroke while dithers/gradients average); mean otherwise.
static inline u32 extreme4(u32 a, u32 b, u32 c, u32 d, u32 thr) {
  const u32 lo = min4(a, b, c, d), hi = max4(a, b, c, d);
  const u32 m = (luma(a) + luma(b) + luma(c) + luma(d)) / 4;
  const u32 dlo = m - luma(lo), dhi = luma(hi) - m;
  if (dlo <= thr && dhi <= thr) return mean4(a, b, c, d);
  return dlo > dhi ? lo : dhi > dlo ? hi : mean4(a, b, c, d);
}

// sRGB <-> linear for the linear-light blend: 8-bit sRGB to 12-bit linear and back, built once.
namespace {
struct GammaLut {
  u16 to_lin[256];
  u8 from_lin[4096];
  GammaLut() {
    for (u32 i = 0; i < 256; ++i) {
      const double c = i / 255.0;
      const double l = c <= 0.04045 ? c / 12.92 : ds_pow_compat((c + 0.055) / 1.055, 2.4);
      to_lin[i] = static_cast<u16>(std::lround(l * 4095.0));
    }
    for (u32 i = 0; i < 4096; ++i) {
      const double l = i / 4095.0;
      const double c = l <= 0.0031308 ? l * 12.92 : 1.055 * ds_pow_compat(l, 1.0 / 2.4) - 0.055;
      from_lin[i] = static_cast<u8>(std::lround(c * 255.0));
    }
  }
};
const GammaLut& gamma_lut() { static const GammaLut lut; return lut; }

// Per-pixel weighted blend in linear light; w per pixel as blend_line_w.
void blend_line_linear(const u32* a, const u32* b, const u8* w, u32* out) {
  const GammaLut& g = gamma_lut();
  for (u32 i = 0; i < 256; ++i) {
    const u32 f = w[i];
    if (!f) { out[i] = a[i]; continue; }
    u32 r = 0xFF000000u;
    for (u32 sh = 0; sh < 24; sh += 8) {
      const u32 la = g.to_lin[(a[i] >> sh) & 255], lb = g.to_lin[(b[i] >> sh) & 255];
      r |= static_cast<u32>(g.from_lin[(la * (256 - f) + lb * f + 128) >> 8]) << sh;
    }
    out[i] = r;
  }
}
} // namespace

void Gpu::blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out) {
  alignas(16) u8 wt[SCREEN_W];
  std::memset(wt, static_cast<int>(w), sizeof wt);
  if (t.blend == 2) blend_line_linear(a, b, wt, out); else kern::active::blend_line_w(a, b, wt, out);
}

// One row with box-filter seams: the last pixel of a run that straddles two
// source pixels is their area-weighted blend.
void Gpu::emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst) {
  alignas(16) u32 next[SCREEN_W], seam[SCREEN_W];
  std::memcpy(next, src + 1, (SCREEN_W - 1) * sizeof(u32));
  next[SCREEN_W - 1] = src[SCREEN_W - 1];
  if (t.blend == 2) blend_line_linear(src, next, t.seam_w, seam); else kern::active::blend_line_w(src, next, t.seam_w, seam);
  kern::active::scale_row_straddle(src, seam, t.seam_w, t.xrun, dst);
}

bool Gpu::build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a) {
  a.cells = cells; a.cell_px = cell_px;
  a.first.assign(cells, 0); a.n.assign(cells, 0); a.w.assign(static_cast<size_t>(cells) * CELL_TAPS, 0);
  for (u32 i = 0; i < cells; ++i) {
    // Cell i covers source [i*src_n/cells, (i+1)*src_n/cells); tap weights are the overlap, 1/256 units.
    const u32 lo = i * src_n, hi = (i + 1) * src_n;         // in 1/cells units
    const u32 s0 = lo / cells, s1 = (hi + cells - 1) / cells; // taps [s0, s1)
    if (s1 - s0 > CELL_TAPS) return false;
    a.first[i] = static_cast<u16>(s0); a.n[i] = static_cast<u8>(s1 - s0);
    u32 sum = 0;
    for (u32 s = s0; s < s1; ++s) {
      const u32 olo = std::max(lo, s * cells), ohi = std::min(hi, (s + 1) * cells);
      u32 w = ((ohi - olo) * 256 + src_n / 2) / src_n;
      if (s + 1 == s1) w = 256 - sum;
      sum += w;
      a.w[static_cast<size_t>(i) * CELL_TAPS + (s - s0)] = static_cast<u16>(w);
    }
  }
  return true;
}

// One cell row: every cell's colour from its taps in the held lines, then the
// row of cells drawn as `cell_px` panel rows (the first the seam row when the
// grid is on) through the grid kernel with the cells' xrun.
void Gpu::emit_cells(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const CellMap& m = *t.cells;
  const u32 j = cell_row_[screen];
  if (j >= m.y.cells) return;
  const u32 l0 = m.y.first[j], ln = m.y.n[j];
  std::memcpy(cell_lines_[screen][line % CELL_TAPS], src, SCREEN_W * sizeof(u32));
  if (line + 1 < l0 + ln) return;
  auto held = [&](u32 v) { return cell_lines_[screen][(l0 + v) % CELL_TAPS]; };
  // The row is complete.
  alignas(16) u32 cells[SCREEN_W];
  const bool linear = t.blend == 2;
  const GammaLut& g = gamma_lut();
  const u16* wy = &m.y.w[static_cast<size_t>(j) * CELL_TAPS];
  for (u32 i = 0; i < m.x.cells; ++i) {
    const u32 s0 = m.x.first[i], sn = m.x.n[i];
    const u16* wx = &m.x.w[static_cast<size_t>(i) * CELL_TAPS];
    // Mean: the 2D box (weights in 1/65536), in sRGB or linear light.
    u32 acc[3] = {0, 0, 0};
    for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
      const u32 c = held(v)[s0 + u], wt = wx[u] * wy[v];
      for (u32 ch = 0; ch < 3; ++ch) { const u32 b = (c >> (8 * ch)) & 255; acc[ch] += (linear ? g.to_lin[b] : b) * wt; }
    }
    u32 mean = 0xFF000000u;
    for (u32 ch = 0; ch < 3; ++ch) {
      const u32 v = (acc[ch] + 32768) >> 16;
      mean |= static_cast<u32>(linear ? g.from_lin[std::min<u32>(v, 4095)] : v) << (8 * ch);
    }
    u32 out = mean;
    if (t.chunky != 2) {
      // Other modes use pixels >= half covered on both axes; largest-coverage pixel stands in for "top-left".
      u32 cand[CELL_TAPS * CELL_TAPS]; u32 nc = 0;
      u32 best = 0, bestw = 0;
      for (u32 v = 0; v < ln; ++v) for (u32 u = 0; u < sn; ++u) {
        const u32 wt = wx[u] * wy[v];
        if (wt > bestw) { bestw = wt; best = held(v)[s0 + u]; }
        if (wx[u] >= 128 && wy[v] >= 128) cand[nc++] = held(v)[s0 + u];
      }
      if (nc == 0) cand[nc++] = best;
      switch (t.chunky) {
      case 1: out = best; break;
      case 3: {   // dominant: the most repeated candidate, ties to the earlier; the mean when all differ
        u32 bc = 0; out = mean;
        for (u32 a = 0; a < nc; ++a) { u32 cnt = 0; for (u32 b = 0; b < nc; ++b) cnt += cand[b] == cand[a]; if (cnt > bc && cnt >= 2) { bc = cnt; out = cand[a]; } }
        break;
      }
      case 4: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) < luma(out)) out = cand[a]; break; }
      case 5: { out = cand[0]; for (u32 a = 1; a < nc; ++a) if (luma(cand[a]) > luma(out)) out = cand[a]; break; }
      default: {  // extreme: the darkest or brightest candidate, if farther than the threshold from the mean
        u32 lo = cand[0], hi = cand[0];
        for (u32 a = 1; a < nc; ++a) { if (luma(cand[a]) < luma(lo)) lo = cand[a]; if (luma(cand[a]) > luma(hi)) hi = cand[a]; }
        const u32 lm = luma(mean), dlo = lm > luma(lo) ? lm - luma(lo) : 0, dhi = luma(hi) > lm ? luma(hi) - lm : 0;
        out = (dlo <= t.chunky_thresh && dhi <= t.chunky_thresh) ? mean : dlo > dhi ? lo : dhi > dlo ? hi : mean;
      }
      }
    }
    cells[i] = out;
  }
  for (u32 i = m.x.cells; i < SCREEN_W; ++i) cells[i] = 0;
  // Draw: rows [j*P, (j+1)*P), the first the seam row.
  const u32 P = m.y.cell_px;
  const u32 y0 = j * P;
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  const bool grid = t.grid < 256;
  if (t.xrun[SCREEN_W] > SCALED_ROW_MAX) return;
  u32* row = row_scratch_[screen];
  if (grid) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, false, row);
  else      kern::active::scale_row(cells, t.xrun, row);
  for (u32 y = y0 + (grid ? 1 : 0); y < y0 + P; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (grid && row_kept(t, y0)) kern::active::scale_row_grid(cells, t.xrun, t.grid, 2, 1, true, row_at(t, y0));
  cell_row_[screen] = j + 1;
}

void Gpu::scale_image(int screen, const u32* src) {
  if (!scale_[screen].px) return;
  for (u32 line = 0; line < SCREEN_H; ++line) emit_scaled(screen, line, src + line * SCREEN_W);
}

// Bilinear. Destination row y samples v = (y+0.5)*192/h - 0.5 in source
// lines; rows blending L-1 and L are written when L arrives. Rows above line
// 0's centre / below line 191's are that line alone (clamped). Source line L
// owns rows [ystart(L), ystart(L+1)); every row claimed exactly once.
void Gpu::emit_bilinear(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  const u32 w = t.xrun[SCREEN_W];
  if (w > SCALED_ROW_MAX || w == 0 || t.h == 0) return;
  const u32 h = t.h;
  auto ystart = [h](u32 l) -> u32 {
    if (l == 0) return 0;
    const u32 k = ((2 * l - 1) * h + SCREEN_H - 1) / SCREEN_H;   // ceil((2L-1)h/192)
    return std::min(h, k / 2);
  };
  // Widen this line; previous line's row kept if it is line-1.
  const bool have_prev = line > 0 && lin_prev_line_[screen] + 1 == line;
  const u32 cur = have_prev ? lin_cur_[screen] ^ 1u : 0u;
  u32* const hcur = lin_row_[screen][cur];
  const u32* const hprev = lin_row_[screen][cur ^ 1u];
  kern::active::lerp_row_gather(src, t.lin_sx, t.lin_wx, w, hcur);
  lin_cur_[screen] = cur;
  lin_prev_line_[screen] = line;
  const size_t bytes = static_cast<size_t>(w) * sizeof(u32);
  auto out_row = [&](u32 y) -> u32* { return row_kept(t, y) ? row_at(t, y) : nullptr; };   // null: cropped away
  if (line == 0) {
    for (u32 y = 0; y < ystart(1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
    if (line + 1 < SCREEN_H) return;
  }
  if (have_prev) {
    const u32 y0 = ystart(line), y1 = ystart(line + 1);
    for (u32 y = y0; y < y1; ++y) {
      // Weight of this line: v - (line-1), in 1/256.
      const s32 wf = static_cast<s32>(((2 * y + 1) * SCREEN_H * 128) / h) - 128 - static_cast<s32>((line - 1) * 256);
      const u32 wy = static_cast<u32>(std::min(255, std::max(0, wf)));
      u32* const o = out_row(y);
      if (!o) continue;
      if (wy == 0) std::memcpy(o, hprev, bytes);
      else kern::active::lerp_rows(hprev, hcur, wy, w, o);
    }
  } else {
    // A gap in the lines (hidden span, or first line after reset): this line stands alone.
    for (u32 y = ystart(line); y < ystart(line + 1); ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
  }
  if (line + 1 == SCREEN_H)
    for (u32 y = ystart(SCREEN_H); y < h; ++y) if (u32* o = out_row(y)) std::memcpy(o, hcur, bytes);
}

void Gpu::emit_scaled(int screen, u32 line, const u32* src) {
  const ScaleTarget& t = scale_[screen];
  if (t.bilinear && t.lin_sx && t.lin_wx) { emit_bilinear(screen, line, src); return; }
  if (t.chunky && t.cells) {
    if (line == 0) cell_row_[screen] = 0;
    emit_cells(screen, line, src);
    return;
  }
  u32 first = line, last = line;
  alignas(16) u32 block[SCREEN_W];
  if (t.chunky) {
    // Each 2x2 block is one cell (xrun merges pixel pairs; line pair merged
    // here). Top-left draws from the even line; other modes resolve on the odd one.
    if (t.chunky == 1) { if (line & 1) return; last = line + 1; }
    else {
      if (!(line & 1)) { std::memcpy(chunk_even_[screen], src, sizeof block); return; }
      first = line - 1;
      const u32* up = chunk_even_[screen];
      switch (t.chunky) {
      case 2:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mean4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 3:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = mode4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 4:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = min4(up[s], up[s + 1], src[s], src[s + 1]); break;
      case 5:  for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = max4(up[s], up[s + 1], src[s], src[s + 1]); break;
      default: for (u32 s = 0; s < SCREEN_W; s += 2) block[s] = extreme4(up[s], up[s + 1], src[s], src[s + 1], t.chunky_thresh); break;
      }
      src = block;
    }
  }
  const u32 y0 = (first * t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 y1 = ((last + 1) * t.h + SCREEN_H - 1) / SCREEN_H;
  if (y0 >= y1) return;                      // downscale: this line is dropped
  const size_t bytes = static_cast<size_t>(t.xrun[SCREEN_W]) * sizeof(u32);
  // Rows built in cached scratch and copied out, never read back from the
  // target. A row wider than the scratch goes direct, which a cropped view cannot.
  const bool stage = t.xrun[SCREEN_W] <= SCALED_ROW_MAX;
  const bool cropped = t.y_lo != 0 || t.y_hi != t.h;
  if (!stage && cropped) return;
  u32* row = stage ? row_scratch_[screen] : row_at(t, y0);
  const bool blend = t.blend && t.seam_w;   // --seam blend: box-filter seams stand in for the grid
  if (blend && t.h >= SCREEN_H) {
    // Box-filter seams: a straddling panel pixel/row is an area-weighted
    // blend, others are nearest. The straddling row of this span is its last
    // and needs the next line, so it's written when that arrives. Upscales
    // only: a downscale drops lines and never returns for the row they left.
    const u32 hb = (last + 1) * t.h;                 // this span's lower boundary, in 1/192 rows
    const bool straddle_below = (hb % SCREEN_H) != 0 && last + 1 < SCREEN_H;
    const u32 ycrisp_end = straddle_below ? y1 - 1 : y1;
    if (ycrisp_end > y0) {
      emit_row_straddle(t, src, row);
      for (u32 y = stage ? y0 : y0 + 1; y < ycrisp_end; ++y)
        if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    }
    // Row above straddles the previous line and this one: boundary falls
    // frac/192 down it, previous line owns that much, this line the rest.
    if (first > 0 && seam_prev_line_[screen] + 1 == first) {
      const u32 tb = first * t.h;                    // this span's upper boundary
      const u32 frac = tb % SCREEN_H;
      if (frac) {
        alignas(16) u32 mid[SCREEN_W];
        blend_rows(t, seam_prev_[screen], src, ((SCREEN_H - frac) * 256) / SCREEN_H, mid);   // weight of this line
        if (row_kept(t, y0 - 1)) emit_row_straddle(t, mid, row_at(t, y0 - 1));
      }
    }
    std::memcpy(seam_prev_[screen], src, sizeof seam_prev_[screen]);
    seam_prev_line_[screen] = last;
    return;
  }
  // Plain nearest: no grid, or seam blend on a view too small for its seams.
  if (t.grid >= 256 || blend) {
    kern::active::scale_row(src, t.xrun, row);
    for (u32 y = stage ? y0 : y0 + 1; y < y1; ++y)
      if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
    return;
  }
  // LCD grid: last output column/row of every source pixel's run/span is
  // dimmed, so each DS pixel shows a lit cell with a dark seam right/below.
  // A run of one pixel is left alone. Only runs the fractional scale widened
  // carry a seam (min_run = ceil(scale)); the seam leads its run. At exactly
  // 2x a seam per pixel reads as a wash, so it goes on every other pixel instead (4-pixel pitch).
  const u32 w = t.xrun[SCREEN_W];
  const u32 min_run = (w + SCREEN_W - 1) / SCREEN_W, min_rows = (t.h + SCREEN_H - 1) / SCREEN_H;
  const u32 pitch_x = w == 2 * SCREEN_W ? 2 : 1, pitch_y = t.h == 2 * SCREEN_H ? 2 : 1;
  const bool seam = y1 - y0 >= std::max<u32>(2, min_rows) && first % pitch_y == 0;
  const u32 yfirst = seam ? y0 + 1 : y0;
  row = stage ? row_scratch_[screen] : row_at(t, yfirst);
  kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, false, row);
  for (u32 y = stage ? yfirst : yfirst + 1; y < y1; ++y)
    if (row_kept(t, y)) std::memcpy(row_at(t, y), row, bytes);
  if (seam && row_kept(t, y0)) kern::active::scale_row_grid(src, t.xrun, t.grid, min_run, pitch_x, true, row_at(t, y0));
}

void Gpu::output_a(u32 line, u32* dst) {
  const u32 dispcnt = engine[0].dispcnt();
  switch ((dispcnt >> 16) & 3) {
  case 0: for (u32 i = 0; i < 256; ++i) dst[i] = 0x3F3F3F; return;          // display off: white
  case 1: { const Pixel* src = engine[0].output(); for (u32 i = 0; i < 256; ++i) dst[i] = src[i]; break; }
  case 2: {                                                                 // VRAM display (LCDC bank)
    const u32 bank = (dispcnt >> 18) & 3;
    const VramMap& vm = nds_.bus.vram_map();   // bank pointers are remap-invariant
    static constexpr u16 kZeroLine[256] = {};
    const u16* src = (lcdc_mask_render_ & (1u << bank)) ? reinterpret_cast<const u16*>(vm.bank(bank)) + line * 256 : kZeroLine;
    kern::active::output_vram_line(src, engine[0].master_bright(), dst);
    return;
  }
  case 3: kern::active::output_vram_line(fifo_line_.data(), engine[0].master_bright(), dst); return;
  }
  apply_master_brightness(engine[0].master_bright(), dst);
}

void Gpu::output_b(u32* dst) {
  if (!((engine[1].dispcnt() >> 16) & 1)) { for (u32 i = 0; i < 256; ++i) dst[i] = 0xFF3F3F3F; return; }
  const Pixel* src = engine[1].output();
  for (u32 i = 0; i < 256; ++i) dst[i] = src[i];
  apply_master_brightness(engine[1].master_bright(), dst);
}

// Display capture: blends source A (engine A composite or the 3D layer) with
// source B (VRAM or the display FIFO) into an LCDC-mapped bank as BGR555.
void Gpu::capture(u32 line) {
  const u32 cnt = capcnt_render_;
  const u32 size = (cnt >> 20) & 3;
  const u32 width = size == 0 ? 128 : 256, height = size == 0 ? 128 : 64 * size;
  if (line >= height) return;
  const u32 dst_bank = (cnt >> 16) & 3;
  // Bank pointers are remap-invariant so the live map is safe here; lcdc_mask is not (latched at hand-off).
  const VramMap& vm = nds_.bus.vram_map();
  const u32 lcdc = lcdc_mask_render_;
  if (!(lcdc & (1u << dst_bank))) return;
  u16* dst = reinterpret_cast<u16*>(vm.bank(dst_bank)) + (((((cnt >> 18) & 3) << 14) + line * width) & 0xFFFF);

  const Pixel* src_a = (cnt & (1 << 24)) ? line3d_ : engine[0].output();
  const u16* src_b = nullptr;
  if (cnt & (1 << 25)) src_b = fifo_line_.data();
  else {
    const u32 dispcnt = engine[0].dispcnt();
    const u32 src_bank = (dispcnt >> 18) & 3;
    if (lcdc & (1u << src_bank)) {
      u32 off = line * 256;
      if (((dispcnt >> 16) & 3) != 2) off += ((cnt >> 26) & 3) << 14;
      src_b = reinterpret_cast<const u16*>(vm.bank(src_bank)) + (off & 0xFFFF);
    }
  }

  switch ((cnt >> 29) & 3) {
  case 0:
    kern::active::capture_a15(src_a, width, dst);
    break;
  case 1:
    if (src_b) std::memcpy(dst, src_b, width * sizeof(u16));
    else std::memset(dst, 0, width * sizeof(u16));
    break;
  default: {
    u32 eva = cnt & 0x1F, evb = (cnt >> 8) & 0x1F;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    // A missing B source (unmapped LCDC bank) reads as zero either way.
    static constexpr u16 kZeroLine[256] = {};
    kern::active::capture_blend(src_a, src_b ? src_b : kZeroLine, width, eva, evb, dst);
    break;
  }
  }
}


void Gpu::apply_master_brightness(u16 reg, u32* dst) { kern::active::master_brightness(reg, dst); }

// 6-bit RGB666 records -> 8-bit 0xAARRGGBB (top two bits replicated into the low two).
void Gpu::expand_colours(u32* dst) { kern::active::expand_colours(dst); }


void Gpu::quiesce() {
  join_worker();
  engine[0].apply_pending(); engine[1].apply_pending();
}

void Gpu::prepare_load() {
  join_worker();
  if (read_trap_bank_ >= 0) { nds_.bus.set_lcdc_read_trap(read_trap_bank_, false); read_trap_bank_ = -1; }
  disarm_trap();
  lazy_frame_ = false; per_line_[0] = per_line_[1] = true;
  render_next_[0] = render_next_[1] = SCREEN_H; frame_finished_ = true;
  burst_[0] = burst_[1] = false; burst_left_[0] = burst_left_[1] = 0; lag_frame_ = false;
}

template <class S> void Gpu::sync_state(S& s) {
  s.begin("GPU ");
  s.fields(line_, hblank_done_, frame_begun_, screens_on_, master_bright_g_, capcnt_, capture_on_, fifo_, fifo_rd_, fifo_wr_, fifo_line_, run_fifo_);
  s.fields(fb_);   // what the display shows until the next frame (and the thumbnail)
  s.end();
  engine[0].sync_state(s);
  engine[1].sync_state(s);
  if constexpr (S::reading) {
    nds_.sched.rebind(EventId::HBlank, ev_hblank);
    nds_.sched.rebind(EventId::VBlank_Scanline, ev_scanline);
    nds_.sched.rebind(EventId::DisplayFifo, ev_fifo);
  }
}
template void Gpu::sync_state<state::Writer>(state::Writer&);
template void Gpu::sync_state<state::Reader>(state::Reader&);

void Gpu::after_load() {
  // State was taken right after line 0's begin_frame(); its decisions depend
  // only on restored registers/DMA state, so retaking them re-arms the trap.
  if (frame_begun_ && line_ == 0 && !hblank_done_) begin_frame();
}

} // namespace ds::gpu
