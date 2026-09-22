#include <vector>
// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/engine2d.h"
#include "core/gpu/line_worker.h"
#include "core/gpu/render3d.h"
#include "core/profile.h"

#include <array>

namespace ds { struct NDS; }

namespace ds::gpu {

// Display timing, the two 2D engines and the output stage. 3D plugs in via
// Engine2D::set_3d_line().
//
// Lazy 2D: engines aren't rendered every HBlank. Writes are journaled per
// engine with the display line they first affect; the frame renders in one
// batch at the last line's HBlank, replaying the journal per line. A trapped
// VRAM store, VRAMCNT remap, or captured LCDC bank catches lines up and
// forces a per-line burst before re-batching. DS_2D_LAZY=0 forces per-line
// rendering always; must produce identical frames.
//
// Framebuffers: 256x192 u32 per screen, 0xAARRGGBB, 8-bit channels expanded
// from the hardware's 6 bits.
class Gpu {
public:
  explicit Gpu(NDS& nds);
  void reset();

  // Scheduler callbacks.
  void on_scanline_start();   // VCOUNT advance, VBlank/VCount flags
  void on_hblank();           // render (lazily), HBlank flag
  void on_display_fifo(u32 x);
  void begin_frame();
  void async_probe_start();
  void async_probe_check(bool at_line0);
  bool probe_enabled_ = std::getenv("DS_ASYNC_PROBE") != nullptr;
  bool probe_open_ = false;
  u64  probe_hash_ = 0, probe_vramcnt_at_l0_ = 0, probe_vramcnt_base_ = 0;   // latches POWCNT/FIFO/capture state; runs at line 0
  bool frame_begun() const { return frame_begun_; }
  bool at_line_start() const { return !hblank_done_; }

  // Save states, taken at line 0. quiesce() joins the worker and drains
  // journals without changing guest state; prepare_load() also lifts the VRAM
  // trap; after_load() re-takes the lazy/trap decision as begin_frame() did.
  void quiesce();
  void prepare_load();
  template <class S> void sync_state(S& s);
  void after_load();

  // 2D register file (0x04000000-0x0400006F, 0x04001000-0x0400106F) minus
  // DISPSTAT/VCOUNT, which stay with the interrupt logic in Io.
  static bool owns_reg(u32 addr) {
    const u32 r = addr - 0x04000000;
    if (r < 0x70) return (r >= 8 || r < 4) && (r < 0x60 || r >= 0x64);   // 0x60 is DISP3DCNT
    return r >= 0x1000 && r < 0x1070 && (r < 0x1004 || r >= 0x1008);
  }
  u32  reg_read(u32 addr, u32 width);
  void reg_write(u32 addr, u32 width, u32 value);
  void set_powcnt(u16 value);

  // Journal stamp for a write to engine `e`: twice the first display line it
  // can affect, plus one if that line's scanline start already passed.
  // NO_STAMP means the write applies at once (nothing in flight, frame done).
  static constexpr u32 NO_STAMP = 0xFFFF;
  u32 journal_stamp(int e) const {
    const u32 l = hblank_done_ ? line_ + 1u : line_;
    return (l < SCREEN_H || inflight_[e]) ? l * 2 + (hblank_done_ ? 0 : 1) : NO_STAMP;
  }
  // Slow-path stores (Bus): a VRAM store on a trapped page catches the render up.
  void palette_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void oam_store(Cpu cpu, u32 addr, u32 width, u32 value);
  void vram_store_trap(Cpu cpu, u32 addr);
  bool vram_remap_begin(u32 moved_2d);  // before a VRAMCNT remap: catch up, lift the trap; returns whether it was set
  void vram_remap_end(bool trapped);  // after: re-arm it
  void set_lazy(bool on) { lazy_enabled_ = on; }   // DS_2D_LAZY

  const u32* framebuffer(int screen) const { return fb_[screen].data(); }   // 0 = top, 1 = bottom

  // A screen the frontend does not show. Its driving engine skips drawing but
  // keeps journal/latches/windows/lazy-2D bookkeeping running so it's exact
  // once shown again. Engine A never skips. Set between frames only.
  void set_screen_visible(int screen, bool on) { screen_visible_[screen] = on; }

  // Frameskip (frontend policy). A skipped frame runs unchanged, only
  // omitting line rendering/output and the 3D raster feeding it. Decided one
  // frame ahead, at line 215. Never skips a frame that captures or feeds the FIFO.
  void set_frame_skip(bool on) { skip_req_ = on; }
  // Skip frames that display-capture too (INEXACT): a game reading captured
  // pixels back with the CPU sees an older frame than hardware.
  void set_frameskip_capture(bool on) { skip_capture_ok_ = on; }

  // Frames until the display setup repeats. Drawing at a multiple of this
  // period freezes one screen on its off-frame, so keep the drawn cadence off
  // a multiple of it (1 = no alternation).
  u8 display_phase_period() const { return phase_period_; }
  bool lines_in_flight() const { return inflight_[0] || inflight_[1] || scale_inflight_; }
  bool lag_active() const { return lag_frame_ && par_2d_ && !lazy_frame_; }
  // Time spent in join_worker since the last take (ns); read by the 3D shape controller at VBlank.
  u64 take_join_wait_ns() { const u64 v = join_wait_ns_; join_wait_ns_ = 0; return v; }
  // Bus::vram_read on an LCDC page under the capture read trap: join in-flight lines if reading the bank capture writes.
  bool lcdc_read_trapped() const { return read_trap_bank_ >= 0; }
  void lcdc_read_hit(u32 addr) {
    if (static_cast<int>((addr >> 17) & 7) == read_trap_bank_) { prof::add(prof::C_2D_A_JOIN_READS, 1); join_worker(JoinSite::Trap); }
  }
  bool will_skip_frame() const { return skip_frame_; }

  // A frontend-owned, panel-sized destination for one screen. When set, the
  // output stage scales each line into it as produced instead of filling fb_.
  // Chunky with a cell of P panel pixels: cells_x * cells_y cells, each an
  // area-weighted box of the DS pixels it covers. Per axis, cell i takes taps
  // first[i]..first[i]+n[i]-1 with weights w[i][..] summing to 256.
  static constexpr u32 CELL_TAPS = 8;
  struct CellAxis {
    u32 cells = 0, cell_px = 0;      // count, and panel pixels per cell
    std::vector<u16> first;          // per cell
    std::vector<u8>  n;
    std::vector<u16> w;              // cells * CELL_TAPS
  };
  struct CellMap { CellAxis x, y; };
  // Fills `a` for `cells` cells over `src_n` source pixels, `cell_px` panel
  // pixels each. False if a cell would need more than CELL_TAPS taps.
  static bool build_cell_axis(u32 src_n, u32 cells, u32 cell_px, CellAxis& a);

  struct ScaleTarget {
    u32* px = nullptr;          // top-left of this screen's rect in the frontend's buffer
    u32 pitch = 0;              // destination pitch, in u32
    u32 h = 0;                  // destination rect height, in pixels
    const u16* xrun = nullptr;  // 257 entries; see kern::scale_row
    u32 grid = 256;             // LCD grid: brightness kept on the grid lines, 0..256 (256 = no grid)
    u8 chunky = 0;              // 0 off; else 2x2 block -> one cell: 1 top-left, 2 mean, 3 dominant colour,
                                // 4 darkest, 5 brightest, 6 farthest-from-mean luma beyond chunky_thresh (else mean)
    u32 chunky_thresh = 180 * 256;  // luma units (0..255 * 256); mode 6 only
    u8 blend = 0;               // box-filter seams: 1 blend in sRGB, 2 in linear light
    const u8* seam_w = nullptr; // 256 entries: weight (0..255 = 0..1) of pixel s+1 in run s's last pixel -- the part of
                                // that pixel past the s|s+1 boundary, 1 - frac((s+1)*w/256); 0 = no straddle
    const CellMap* cells = nullptr; // chunky with a panel-sized cell (see CellMap); null = the 2x2 pair path
    // Bilinear: blends the 2x2 source pixels around each sample point; takes
    // precedence over grid/seams/chunky. Column x samples between lin_sx[x]
    // and lin_sx[x]+1 at weight lin_wx[x]/256 (lin_sx <= 254).
    bool bilinear = false;
    const u16* lin_sx = nullptr;
    const u8* lin_wx = nullptr;
    // Rows of the rect, [y_lo, y_hi) (y_hi 0 = h: no crop); px points at row y_lo.
    u32 y_lo = 0, y_hi = 0;
  };
  // Composite's per-line input beside Engine2D's planes: line[y] = bldcnt |
  // eva<<16 | evb<<21 | evy<<26; mbright[y] = MASTER_BRIGHT | 1<<31 when the
  // line's planes were exported. Engine A only.
  struct LayerExport { u32* top = nullptr; u32* second = nullptr; u32* meta = nullptr; u32* win = nullptr; u32* line = nullptr; u32* mbright = nullptr; };
  void set_layer_export(const LayerExport& e) { layer_ = e; engine[0].set_layer_export(e.top, e.second, e.meta, e.win); }
  // Hi-res 3D layer the just-finished frame's lines read (0 if CPU raster
  // drew it). `screen`: which screen engine A displayed. `edge`: smooth filter's edge plane (0 if none).
  u64 frame_hires(size_t* bytes, u32* scale, int* screen = nullptr, u64* edge = nullptr) const { if (bytes) *bytes = shown_hires_bytes_; if (scale) *scale = shown_scale_; if (screen) *screen = layer_screen_; if (edge) *edge = shown_edge_; return shown_hires_; }
  // Both screens or neither: pass a null `px` to go back to fb_.
  void set_scale_target(int screen, const ScaleTarget& t) { scale_[screen] = t; if (scale_[screen].y_hi == 0) scale_[screen].y_hi = t.h; update_shape(); }
  // Edge shaping shows only through the scanline scaler's runs, so it's held
  // off on a tier that can't patch cells (bilinear, chunky, own scaler, downscale).
  bool shape_usable() const {
    for (int s = 0; s < 2; ++s) { const ScaleTarget& t = scale_[s];
      if (t.px && !t.bilinear && !t.chunky && t.h >= SCREEN_H && t.xrun[SCREEN_W] >= SCREEN_W) return true; }
    return false;
  }
  // Runs a whole DS-resolution image (e.g. a frontend pause menu) through the
  // scanline scaler with the game's grid/chunky/seam treatment. No-op without a scale target.
  void scale_image(int screen, const u32* src);
  bool scaling() const { return scale_[0].px && scale_[1].px; }
  // Sub-pixel polygon edges: the 3D raster's split map cuts an edge pixel's
  // panel cell at the polygon's real boundary. Only nearest/grid/seam scaler
  // paths show it, only where 3D is the pixel. Every edge drawn (AA fill rule).
  void set_subpixel(bool on);
  bool subpixel() const { return subpixel_; }
  void set_shape(bool on);                 // edge shaping on top of the hardware picture (Renderer3D::shape_frame); not with set_subpixel
  bool shape() const { return shape_; }
  u16 line() const { return line_; }

  Engine2D engine[2];

  // Both screens forced white by MASTER_BRIGHT (mode 1, full factor). Reading
  // the register beats scanning fb_ (pixels go white a frame later there).
  bool screens_forced_white() const {
    for (u16 mb : master_bright_g_)
      if ((mb >> 14) != 1 || (mb & 0x1F) < 16) return false;
    return true;
  }

private:
  NDS& nds_;
  u16 line_ = 0;
  bool hblank_done_ = false;  // this line's HBlank event has run (its render, if any, is behind us)
  bool frame_begun_ = false;
  bool screens_on_ = false;   // POWCNT1 bit 0, latched at frame start
  bool screen_visible_[2] = {true, true};
  bool skipped_[2] = {false, false};   // this engine's last line was skipped: its next drawn line re-renders its sprites
  bool skip_req_ = false;     // frameskip: the frontend's request, taken at line 215
  bool skip_next_ = false;    // taken there: the next frame's display lines are skipped
  bool skip_frame_ = false;   // latched in begin_frame from skip_next_, gated by skippable()
  // Never skip a frame the guest reads back; capture_recent_ extends that a
  // few frames after the last capture too (3D raster skips a frame ahead of the display it feeds).
  static constexpr u8 CAPTURE_STICKY = 8;
  u8 capture_recent_ = 0;
  bool skip_capture_ok_ = false;
  bool skippable() const { return !run_fifo_ && (skip_capture_ok_ || (!capture_on_ && !capture_recent_)); }
  // Display-phase detection: signature of the last PHASE_HISTORY frames, newest last.
  static constexpr u32 PHASE_HISTORY = 8, PHASE_MAX = 4;
  u32 phase_sig_[PHASE_HISTORY] = {};
  u32 phase_seen_ = 0;
  u8 phase_period_ = 1;
  void update_phase();
  u16 master_bright_g_[2] = {0, 0};   // guest-visible; the engines hold the render-side value
  u32 capcnt_ = 0;
  bool capture_on_ = false;
public:
  // video.gpu_defer: show the GPU's frame one later than drawn, so the compositor never waits for it.
  void set_defer_3d(bool on) { defer_3d_ = on; }
  bool defer_3d() const { return defer_3d_; }
private:
  bool defer_3d_ = false;
  std::array<u16, 16> fifo_{};
  u8 fifo_rd_ = 0, fifo_wr_ = 0;
  alignas(16) std::array<u16, 256> fifo_line_{};
  bool run_fifo_ = false;
  std::array<std::array<u32, SCREEN_W * SCREEN_H>, 2> fb_{};
  ScaleTarget scale_[2];
  static constexpr u32 SCALED_ROW_MAX = 4096;
  alignas(16) u32 chunk_even_[2][SCREEN_W];   // chunky: the even line, held until the odd one completes the block
  alignas(16) u32 seam_prev_[2][SCREEN_W];    // blend: the previous source row, for the straddling row
  u32 seam_prev_line_[2] = {~0u, ~0u};
  // Cell chunky: last CELL_TAPS source lines, a ring by line number.
  alignas(16) u32 cell_lines_[2][CELL_TAPS][SCREEN_W];
  u32 cell_row_[2] = {0, 0};        // the cell row being gathered
  void emit_cells(int screen, u32 line, const u32* src);
  void emit_row_straddle(const ScaleTarget& t, const u32* src, u32* dst);
  // Bilinear: previous and current source lines widened horizontally, per
  // screen; a destination row between them is a lerp of the pair.
  alignas(16) u32 lin_row_[2][2][SCALED_ROW_MAX];
  u32 lin_cur_[2] = {0, 0};              // which of lin_row_[screen] holds the current line
  u32 lin_prev_line_[2] = {~0u, ~0u};
  void emit_bilinear(int screen, u32 line, const u32* src);
  void blend_rows(const ScaleTarget& t, const u32* a, const u32* b, u32 w, u32* out);
  // output_line writes here instead of fb_ when scaling; scale_row reads it back hot.
  alignas(16) std::array<std::array<u32, SCREEN_W>, 2> line_out_{};
  // Scaled row staged here before the target (scanout memory, uncached CMA):
  // duplicating straight out of it would read that memory back.
  alignas(16) u32 row_scratch_[2][SCALED_ROW_MAX];
  // Sub-pixel edges (emit_splits). A cut whose uncovered part takes the line
  // below waits in sp_pend_ until that line is composed.
  struct SplitRec { u8 x, side, unc; s8 slope, src; };   // side: SPLIT_* | 0x80 weak | 0x40 spill | 0x20 edge-marked (unc = position+32) | 0x10 reach from line above; unc 0..32; src: see Renderer3D::extract_splits
  bool subpixel_ = false;
  bool shape_ = false;
  bool shape_live_ = false;                // shape_ and a screen that can show it: what the renderer was told
  void update_shape();                     // renderer runs the pass only while a screen can show it (shape_usable)
  bool splits_on() const { return subpixel_ || shape_live_; }   // a split map comes with the 3D lines
  u8 sp_prev_own_[2][256]{};               // per screen: which pixels of the line before were the 3D layer's, unblended
  const u32* split3d_ = nullptr;          // split map of line3d_ (null: none this frame)
  SplitRec sp_pend_[2][4 * SCREEN_W];
  u32 sp_pend_n_[2] = {0, 0}, sp_pend_line_[2] = {~0u, ~0u};
  alignas(16) u32 sp_prev_[2][SCREEN_W];  // the previous composed line, for cuts that take the line above
  u32 sp_prev_line_[2] = {~0u, ~0u};
  void emit_splits(int screen, u32 line, const u32* dst, const Pixel* composed, bool shown, u64 key);
  // Carry through display capture: per frame generation, a content-keyed table of split lines.
  static constexpr u32 kSplitGens = 4;
  struct SplitGen {
    struct Slot { u64 key = 0; u32 off = 0; u16 n = 0; };
    u64 frame = ~u64{0};
    u32 used = 0;
    std::array<Slot, 512> slot{};
    std::array<SplitRec, 16384> recs{};
  };
  std::vector<SplitGen> sp_gen_;          // kSplitGens of them once the mode is on (set_subpixel)
  u64 sp_last_store_ = 0;
  static bool split_admit(const SplitRec& r, const u32* own, const u32* other, u32 ox, bool horiz);
  void emit_splits_b(int screen, u32 line, const u32* dst, u64 key);
  void carry_store(u64 key, const SplitRec* recs, u32 n);
  u32 carry_find(u64 key, const SplitRec** recs) const;
  void patch_cell(const ScaleTarget& t, u32 line, const SplitRec& r, u32 nb);
  const u32* line3d_ = nullptr;   // 3D output for the line being drawn (whichever thread draws engine A)

  // Lazy-2D state for the frame in progress.
  bool lazy_enabled_ = true;      // DS_2D_LAZY != 0
  bool lazy_frame_ = false;       // this frame may batch
  // Per engine, kept in lockstep: a trapped store takes BOTH engines out of batched mode.
  bool per_line_[2] = {false, false};
  bool per_line_prev_[2] = {false, false};
  u32  render_next_[2] = {SCREEN_H, SCREEN_H};
  bool frame_finished_ = false;   // frame_done() called for both engines
  bool trap_armed_ = false, trap_lcdc_ = false, trap_a_only_ = false;
  // Engines an address can change: bit 0 A, bit 1 B; LCDC only A (and only when trapped); else both.
  u32 reach_engines(u32 addr) const {
    if ((addr >> 24) != 0x06) return 3;
    if (addr >= 0x06800000) return trap_lcdc_ ? 1 : 0;
    return ((addr >> 21) & 1) ? 2 : 1;
  }
  // Capture frames batch too (DS_2D_LAZY_CAPTURE=0 forces per line). A
  // trapped store renders LAZY_BURST_LINES per line, then re-arms; past
  // LAZY_BURST_LIMIT bursts the frame stays per line.
  bool lazy_capture_ = true;
  bool burst_[2] = {false, false};   // per-line for the current burst of stores
  u32  burst_left_[2] = {0, 0};      // display lines left before re-batching
  u32  lazy_bursts_[2] = {0, 0};
  static constexpr u32 LAZY_BURST_LIMIT = 16, LAZY_BURST_LINES = 8;
  // After LAZY_FUTILE_LIMIT frames in a row where the trap paid for itself
  // and got nothing, it stops arming; one frame in lazy_probe_period_ re-arms
  // it to retry. Failing probes double the period up to LAZY_PROBE_MAX,
  // resetting to LAZY_PROBE_PERIOD on success or a VRAM remap.
  static constexpr u32 LAZY_FUTILE_LIMIT = 4, LAZY_PROBE_PERIOD = 64, LAZY_PROBE_MAX = 1024;
  u32  lazy_futile_ = 0;
  u32  lazy_probe_period_ = LAZY_PROBE_PERIOD, lazy_probe_in_ = LAZY_PROBE_PERIOD;   // frames until the next probe
  bool lazy_tried_ = false;
  // A probe frame gets a smaller budget (LAZY_PROBE_BURSTS) and falls both engines back at once when it runs out.
  static constexpr u32 LAZY_PROBE_BURSTS = 8;
  bool lazy_limit_hit_ = false, lazy_probe_ = false;
  u32  frontier() const { return hblank_done_ ? line_ + 1u : line_; }   // first line a write now can still affect
  void catch_up(u32 mask);             // render the masked engines' lines below the frontier
  void fall_back_per_line(u32 mask);   // catch up and render the rest of the frame per line
  void arm_trap();
  void disarm_trap();
  // Per-engine ranges; first > last means "nothing pending for that engine".
  // Engine B's range goes to the worker and overlaps engine A's here.
  void render_ranges(u32 af, u32 al, u32 bf, u32 bl);
  // Which engines a VRAM store can change: bit 0 engine A, bit 1 engine B.
  // Only the fixed BG/OBJ address ranges are attributed; anything else (LCDC,
  // an unmapped alias) is charged to both, so the split can only ever be more
  // conservative than the address map.
  void step_engine(int e, u32 line);        // one engine's display line: replay, latches, render, output

  // One worker thread beside the emulation thread, drawing a run of one
  // engine's display lines (worker_job); which engine depends on the frame.
  //
  // A batched frame hands engine A's 192 lines over at the last line's
  // HBlank without waiting (A carries the 3D composite, capture and half of
  // frontend scaling; B's batch draws here). Join is deferred to line 0.
  // Anything the guest does meanwhile that in-flight lines could observe
  // joins earlier: a trapped VRAM store, a VRAMCNT remap, a read of a
  // batched-capture bank, a full journal, a save state. VBlank writes and
  // latches go through the journal, applied in order at the join.
  //
  // Per-line frames (capture per line, display FIFO, a VRAM trap) run engine
  // A here and hand engine B's lines to the worker, as before.
  //
  // The two engines share no mutable state, so a line sees exactly the state
  // the sequential order would. DS_2D_THREAD=0 forces sequential for comparison.
  LineWorker worker_;
public:
  // DS_WATCHDOG: where the display pipeline stands when a frame stalls.
  void debug_dump(FILE* f);
private:
  // The job: per engine, a run of lines (first > last = nothing for it).
  u32  job_first_[2] = {1, 1}, job_last_[2] = {0, 0};
  bool par_2d_ = false;
  bool inflight_[2] = {false, false};   // that engine's lines are on the worker
  bool a_deferred_ = false;             // engine A's whole-frame batch is in flight past line 191 (finish_a at the join)
  // Frame-level capture state latched at each render_ranges: DISPCAPCNT's
  // enable bit clears at line 192 while a batched engine A is still drawing.
  u32  capcnt_render_ = 0;
  bool capture_render_ = false;
  // Gpu::capture() runs on the WORKER; vram_remap_begin() joins before a
  // remap changes vram_map_, so latching lcdc_mask_render_ here keeps capture off the shared object.
  u32  lcdc_mask_render_ = 0;
  int  read_trap_bank_ = -1;            // LCDC bank under the capture read trap, or -1
  // Engine B's scaling, handed to the worker with engine A's lagged lines:
  // the join is only for buffer reuse and frame end. Lines drawn here in lag
  // mode stash their output (output_engine); the worker scales it after engine A's lines.
  struct StashedLine { u32 line; int screen; u64 key; alignas(16) u32 px[SCREEN_W]; };   // key: see emit_splits
  LayerExport layer_;
  int layer_screen_ = 0;   // engine A's screen on the frame's first line (frame_hires)
  u64 shown_hires_ = 0; size_t shown_hires_bytes_ = 0; u32 shown_scale_ = 1; u64 shown_edge_ = 0;   // the layer of the frame whose lines were just output (begin_frame latches it)
  StashedLine bscale_[SCREEN_H];
  u32  bscale_n_ = 0;                   // lines stashed for the job being built / in flight
  bool bscale_defer_ = false;           // output_engine stashes engine B's line instead of scaling it
  bool scale_inflight_ = false;         // the worker holds a stash to scale
  bool defer_join_ = true;              // DS_2D_DEFER=0: join engine A's batch at once (bisecting tool)
  Renderer3D::FrameRef ref3d_;          // the 3D frame these display lines read (begin_frame)
  // Lag mode (DS_2D_LAG): per-line frames hand BOTH engines' line to the
  // worker at its HBlank without waiting, joining at that line's HBlank or
  // earlier if observed. The last display line always joins. Past
  // LAG_TRAP_LIMIT joining stores in a frame, lag is dropped and the trap
  // lifted. Display FIFO frames stay on this thread.
  bool lag_enabled_ = true;       // DS_2D_LAG=0 disables
  bool lag_frame_ = false;        // this frame's per-line lines may stay in flight
  u32  lag_trap_hits_ = 0;
  static constexpr u32 LAG_TRAP_LIMIT = 4096;
  // Wait for whatever is on the worker. Engine A's deferred batch also ends
  // its frame here (finish_a), as render_ranges does for a frame finished on this thread.
  enum class JoinSite { CatchUp, Trap, Journal, Line0, Remap, RangesPre, RangesPost, Other };   // where a join was taken from, to attribute its wait
  void join_worker(JoinSite site = JoinSite::Other);
  u64 join_wait_ns_ = 0;
  void finish_a();
public:
  void journal_full() { join_worker(JoinSite::Journal); }   // Engine2D::queue on a full journal
private:
  static void worker_job(void* self);

  void output_engine(int e, u32 line);
  void emit_scaled(int screen, u32 line, const u32* src);
  void output_a(u32 line, u32* dst);
  void output_b(u32* dst);
  void capture(u32 line);
  void apply_master_brightness(u16 reg, u32* dst);
  void expand_colours(u32* dst);
  bool uses_fifo() const;
  void sample_fifo(u32 offset, u32 count);
};

constexpr u32 HBLANK_START = (48 + 256 * 6) * 2;   // system cycles into the line (melonDS), in ARM9 cycles

} // namespace ds::gpu
