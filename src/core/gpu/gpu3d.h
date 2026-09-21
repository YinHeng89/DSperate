// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "core/gpu/render3d.h"

#include <array>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace ds { struct NDS; }

namespace ds::gpu {

// 3D geometry engine: command FIFO, matrix stacks, lighting, clipping,
// viewport transform and the double-buffered vertex/polygon RAM the
// rasteriser consumes. Fixed-point integer math throughout (20.12 matrices,
// 4.12 vertex components), with the hardware's truncation points as
// documented by the melonDS project (GPLv3), whose software implementation
// this engine is verified against frame for frame.
//
// Timing: there is none. Commands are appended to a log as they arrive and
// replayed in one pass -- at VBlank, when a read observes the engine, or when
// the log fills. Nothing is priced in cycles, the FIFO has no level and never
// stalls the ARM9, and GXSTAT is synthesised (see read()) rather than derived
// from an execution clock. Phase 1 of the speed-first rework; the exact
// per-command cycle model and the geometry worker it coexisted with were
// deleted in 1a, measured in docs/speed-first-rework-scoping.md §3.4.

// Parameter count per command. A command with zero parameters is one byte in
// the command log and nothing in the parameter log; the replay advances
// through par_log_ by this table, so it is the contract between the two.
inline constexpr u8 CMD_PARAMS[256] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 1, 1, 1, 0, 16, 12, 16, 12, 9, 3, 3, 0, 0, 0,
  1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
  1, 1, 1, 1, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  // 0x80-0xFF: none
};

struct Vertex {
  s32 pos[4];          // clip space, 20.12
  s32 col[3];          // 5-bit colour with 12 fractional bits (kept through clipping)
  s16 tex[2];          // 12.4 texture coordinates
  bool clipped;
  u8  oc;              // frustum outcode of pos (bits: +x -x +y -y +z -z), set at transform time
  s32 sx, sy;          // screen position after the viewport transform
  s32 fcol[3];         // final 9-bit colour used by the rasteriser
};

struct Polygon {
  u16 vtx[10];         // indices into the engine's vertex RAM
  u32 nverts;
  s32 z[10], w[10];    // per-vertex depth and normalised W
  bool wbuffer;
  u32 attr, texparam, texpal;
  bool degenerate;
  bool facing;         // front-facing as seen by the culling test
  bool translucent;
  bool shadow_mask, shadow;
  u32 vtop, vbot;      // vertex indices (into vtx[]) of the top and bottom points
  s32 ytop, ybot, xtop, xbot;
  u32 sort_key;
};

template <typename T, u32 N>
class Fifo {
public:
  void clear() { rd_ = wr_ = n_ = 0; }
  bool empty() const { return n_ == 0; }
  bool full() const { return n_ == N; }
  u32 level() const { return n_; }
  void push(const T& v) { buf_[wr_] = v; wr_ = (wr_ + 1) % N; ++n_; }
  T pop() { T v = buf_[rd_]; rd_ = (rd_ + 1) % N; --n_; return v; }
private:
  std::array<T, N> buf_{};
  u32 rd_ = 0, wr_ = 0, n_ = 0;
};

class Gpu3D {
public:
  explicit Gpu3D(NDS& nds);
  // The band workers may still be rasterising the last frame: they read the
  // polygon RAM below, which is freed before the renderer (declared first).
  ~Gpu3D();
  void reset();
  template <class S> void sync_state(S& s);   // after sync_raster()

  // Registers: DISP3DCNT (0x60), the 0x320-0x3BF block, the FIFO/command
  // ports and status/results at 0x400-0x6A3.
  static bool owns_reg(u32 addr) {
    const u32 r = addr - 0x04000000;
    return (r >= 0x60 && r < 0x64) || (r >= 0x320 && r < 0x3C0) || (r >= 0x400 && r < 0x6A4);
  }
  u32  read(u32 addr, u32 width);
  void write(u32 addr, u32 width, u32 value);
  // A DMA word landing on GXFIFO, without the bus and I/O dispatch a
  // register write goes through (the same semantics as write(0x04000400, 32)).
  void gxfifo_dma_write(u32 value) { if (geometry_on_) gxfifo_write(value); }
  // A CPU word store to 0x04000400-0x040005CB, exactly the first case of
  // write(): GXFIFO for the 0x400-0x43F window, a direct command port after.
  void gx_port_write(u32 addr, u32 value) {
    const u32 r = addr - 0x04000400;
    if (!geometry_on_) return;
    if (r < 0x40) gxfifo_write(value);
    else log_port(static_cast<u8>((r & 0x1FC) >> 2), value);
  }
  // A run of `n` DMA words fed straight from a direct-mapped source page.
  // See the definition for why the burst is unobservable and what that buys.
  void gxfifo_dma_burst(const u8* src, u32 n);
  // Words a burst may feed without any chance of filling the FIFO: a word
  // carries at most four commands, so this many can never reach FIFO_DEPTH.
  u32 fifo_burst_room() const {
    // A word carries at most four commands, or one parameter. Both logs keep
    // a margin so the caller never has to think about either bound.
    const u32 c = cmd_n_ + 8 < CMD_CAP ? (CMD_CAP - 8 - cmd_n_) >> 2 : 0;
    const u32 q = par_n_ + 8 < PAR_CAP ? PAR_CAP - 8 - par_n_ : 0;
    return c < q ? c : q;
  }

  // POWCNT1 bit 3 (geometry) and bit 2 (rendering).
  void set_powcnt(u16 value);

  // Nothing to replay and no swap parked: the scheduler's idle-skip may
  // advance time without asking the engine anything.
  bool idle() const { return !geometry_on_ || flush_request_ || cmd_n_ == 0; }
  // A swap has been issued and waits for VBlank: the engine changes nothing
  // until then, so a loop polling GXSTAT can be skipped.
  bool swap_pending() const { return flush_request_ != 0 || swap_wait_; }

  // Display timing hooks.
  void vblank();            // VCount 192: latch registers, sort, swap buffers
  void render_frame();      // VCount 215: rasterise the latched frame
  // Frameskip skipped the raster at VCount 215: the picture the renderer
  // holds is not the frame the bookkeeping below describes, so the next
  // frame must be rasterised even if its list and registers are unchanged.
  void note_raster_skipped() { render_stale_ = true; }
  // The frame the display reads now (see Renderer3D::FrameRef): taken on the
  // emulation thread at the start of a display frame, used by whichever
  // thread composites its lines.
  Renderer3D::FrameRef frame_ref() const { return renderer_.frame_ref(); }
  u64 last_raster_ns() const { return renderer_.last_band_sum_ns(); }
  const u32* line(const Renderer3D::FrameRef& f, u32 y);
  const u32* split_line(const Renderer3D::FrameRef& f, u32 y);   // the line's split map, scrolled the same way (after line(); null = none)   // 3D output for display line y, X-scrolled (RGB666 + 5-bit alpha at 24-28)
  // Force the asynchronous raster to finish. Called wherever something is
  // about to change what its workers are reading -- in practice only
  // Bus::update_vram, since texture VRAM is unreachable any other way.
  void sync_raster();
  void debug_dump(FILE* f) { renderer_.debug_dump(f); }
  void set_render_xpos(u16 value, u16 mask);

  // The FIFO IRQ line follows the FIFO level in the two IRQ modes; with the
  // mode off the line is already down (the GXSTAT write handlers call the
  // full check), so the per-command path tests the mode first.
  void check_fifo_irq();
  void check_fifo_irq_fast() { if (gxstat_ >> 30) check_fifo_irq(); }
  void check_fifo_dma();

  u32 dispcnt() const { return dispcnt_; }
  const RenderState& render_state() const { return rstate_; }
  const Vertex& vertex(u32 idx) const { return vram_[idx]; }
  // The finalised list of the bank the raster reads (render_frame sets it).
  const Polygon* const* render_polygons() const { return render_polys_[raster_bank_].data(); }
  u32 render_polygon_count() const { return render_count_[raster_bank_]; }
  // No SWAP_BUFFERS since the last render and the render registers are
  // unchanged: the rasteriser may keep its previous output if the textures
  // it used are unchanged too (it checks those itself).
  bool render_identical() const { return render_identical_; }
  // SWAP_BUFFERS that finalised a list, since reset. Always counted, not
  // behind prof::enabled: the SDL cadence recorder (DS_CADENCE_LOG) needs it
  // on an ordinary build, and it is one increment per swap.
  u64 swap_count() const { return swaps_; }

private:
  NDS& nds_;
  Renderer3D renderer_;
public:
  Renderer3D& renderer() { return renderer_; }   // tests
  // The next render_frame rasterises in full, not the identical-frame shortcut
  // (tools/raster_bench.cpp re-renders one frame to time it).
  void force_full_raster() { render_identical_ = false; }
private:

  // The two logs. A command is one byte in cmd_log_; its parameters are u32
  // words in par_log_, contiguous and in arrival order, so the replay reads a
  // command's parameters straight out of the log instead of accumulating them
  // one dispatch at a time. That is the whole point of 1b: a 16-parameter
  // MTX_LOAD_4x4 used to cost sixteen trips through the drain loop and
  // sixteen calls into an accumulator with its own switch; it now costs one.
  //
  // Sized for a frame rather than for a FIFO: Golden Sun's title pushes ~40 k
  // words a frame. A replay happens at VBlank, on any observing read, and
  // here if either log approaches full, so filling these is the rare path.
  //
  // par_log_ is over-allocated by PAR_SLACK words so that exec_single may
  // read p[0] for a zero-parameter command whose slot is at the very end of
  // the log without running off it.
  static constexpr u32 CMD_CAP = 1u << 16, PAR_CAP = 1u << 18, PAR_SLACK = 64;
  std::unique_ptr<u8[]> cmd_log_ = std::unique_ptr<u8[]>(new u8[CMD_CAP]());
  std::unique_ptr<u32[]> par_log_ = std::unique_ptr<u32[]>(new u32[PAR_CAP + PAR_SLACK]());
  u32 cmd_n_ = 0, par_n_ = 0;
  bool swapped_ = false;         // a SWAP_BUFFERS finalised a list since the last VBlank
  // A SWAP_BUFFERS was issued since the last VBlank. On hardware the engine
  // parks until VBlank and GXSTAT bit 27 reads busy all that time; we have
  // already executed the swap, so the bit is reported from this instead. The
  // hardware engine is still busy for the swap's 325 cycles after the flip,
  // so VBlank turns the flag into a time (ARM9 cycles) the bit reads busy
  // until. This synthesis is the contract Phase 1 must not weaken.
  bool swap_wait_ = false;
  u64 swap_busy_until_ = 0;
  bool list_same_ = false;       // finalise_list: the finished list equals the previous one

  // A command under assembly writes its parameters at par_log_[par_n_ ...]
  // and commits by appending its byte and advancing par_n_. So the two logs
  // are consistent at every point a replay can happen: cmd_log_ describes
  // exactly par_log_[0, par_n_), and an unfinished command is scratch beyond
  // par_n_ that no replay reads and the next command overwrites.
  //
  // Both sources -- the packed GXFIFO and the direct command ports -- assemble
  // here, so they cannot interleave into a corrupt log. A port write arriving
  // mid-FIFO-command abandons that command rather than splicing its
  // parameters, which is the same class of outcome the per-entry accumulator
  // gave and is a guest error either way.
  u32 inflight_n_ = 0;
  u8  inflight_cmd_ = 0xFF;
  struct Sink;                    // the GXFIFO sink; defined in the .cpp beside the walk
  friend struct Sink;
  void log_commit(u8 cmd) { cmd_log_[cmd_n_++] = cmd; par_n_ += inflight_n_; inflight_n_ = 0; inflight_cmd_ = 0xFF; }
  void log_room() { if (cmd_n_ + 8 >= CMD_CAP || par_n_ + 40 >= PAR_CAP) drain_all(); }
  // A direct command port: one parameter of `cmd` per write.
  void log_port(u8 cmd, u32 value) {
    log_room();
    const u32 np = CMD_PARAMS[cmd];
    if (np == 0) { cmd_log_[cmd_n_++] = cmd; return; }
    if (cmd != inflight_cmd_) { inflight_cmd_ = cmd; inflight_n_ = 0; }
    par_log_[par_n_ + inflight_n_] = value;
    if (++inflight_n_ >= np) log_commit(cmd);
  }

  // Command assembly for packed GXFIFO writes.
  // Packed-command parser state. A struct so a DMA burst can walk a whole run
  // with a local copy in registers and write it back once; the single-word
  // port passes the member itself.
  struct GxParse { u32 num_cmds = 0, cur_cmd = 0, param_count = 0, total_params = 0; };
  GxParse parse_;

  // Status. gxstat_ now carries only the two IRQ-mode bits (30-31): the busy
  // bit is synthesised from the swap, the FIFO level is always reported empty,
  // and the matrix-stack and test busy bits are always clear because any read
  // that could observe them replays the log first (see read()).
  u32 gxstat_ = 0;
  // GXSTAT bit 1 (box test result), written by the execute path.
  u32 box_result_ = 0;
  // GXSTAT bit 15 (matrix stack over/underflow), kept apart from gxstat_
  // because the execute path sets it. read() ORs it in.
  u32 stack_err_ = 0;

  void stack_reset();                   // GXSTAT bit 15 written: clear the flag, reset proj/tex stacks
  bool geometry_on_ = false, rendering_on_ = false;
  u32 dispcnt_ = 0;
  u8  alpha_ref_val_ = 0, alpha_ref_ = 0;
  std::array<u16, 32> toon_{};
  std::array<u16, 8> edge_{};
  u32 fog_color_ = 0, fog_offset_ = 0;
  std::array<u8, 32> fog_density_{};
  u32 clear_attr1_ = 0x3F000000, clear_attr2_ = 0x00007FFF;
  u32 zero_dot_w_limit_ = 0xFFFFFF;
  RenderState rstate_;
  // BG0HOFS of engine A, applied from its journal in display-line order (so
  // on the compositor thread); render_on_ mirrors rendering_on_ for that
  // thread's gate.
  u16 render_xpos_ = 0;
  std::atomic<bool> render_on_{false};
  alignas(16) u32 scrolled_[256] = {};
  u32 scrolled_split_[512] = {};

  // Matrices (20.12, row-major: m[row*4+col]).
  u32 matrix_mode_ = 0;
  std::array<s32, 16> proj_, pos_, vec_, tex_, clip_;
  bool clip_dirty_ = true;
  std::array<s32, 16> proj_stack_, tex_stack_;
  std::array<std::array<s32, 16>, 32> pos_stack_, vec_stack_;
  s32 proj_sp_ = 0, pos_sp_ = 0, tex_sp_ = 0;
  std::array<u32, 6> viewport_{};

  // Vertex state.
  u32 poly_mode_ = 0;
  s16 cur_vertex_[3] = {};
  u8  vertex_color_[3] = {};
  s16 texcoords_[2] = {}, raw_texcoords_[2] = {};
  s16 normal_[3] = {};
  s16 light_dir_[4][3] = {};
  s32 spec_recip_[4] = {};
  u8  light_color_[4][3] = {};
  u8  mat_diffuse_[3] = {}, mat_ambient_[3] = {}, mat_specular_[3] = {}, mat_emission_[3] = {};
  bool use_shininess_ = false;
  std::array<u8, 128> shininess_{};
  u32 polygon_attr_ = 0, cur_polygon_attr_ = 0;
  u32 texparam_ = 0, texpal_ = 0;
  s32 pos_test_[4] = {};
  s16 vec_test_[3] = {};

  // Polygon assembly. A strip carries two vertices over into the next polygon;
  // rather than copy 56-byte structs between the slots, vslot_ maps a position
  // in the polygon being assembled to the slot holding it, and the carry-over
  // is a permutation of four bytes. It is always a permutation of 0..3, so
  // every position has a slot of its own to be written into.
  Vertex temp_vtx_[4] = {};
  Vertex* vptr_[4] = {&temp_vtx_[0], &temp_vtx_[1], &temp_vtx_[2], &temp_vtx_[3]};
  void reset_vptr() { for (int i = 0; i < 4; ++i) vptr_[i] = &temp_vtx_[i]; }
  u32 vertex_num_ = 0, vertex_in_poly_ = 0, consecutive_polys_ = 0;
  Polygon* last_strip_poly_ = nullptr;
  u32 num_opaque_ = 0;

  // Vertex/polygon RAM. The hardware has two banks; a third lets the
  // rasteriser keep reading the list it was given while the geometry
  // engine, after a swap, fills the bank the *previous* list occupied:
  // bank_ is being written, render_bank_ holds the finalised list the next
  // render() takes, raster_bank_ the one a raster in flight reads (set at
  // render_frame). A swap retires bank_ into render_bank_ and picks the
  // bank neither of the other two names. Without this vblank() had to wait
  // for the raster before swapping, on the emulation thread, with nothing
  // to hide the wait behind once the display composite moved off it.
  // A fourth bank for the geometry worker: between VBlank (where the list to
  // render is fixed) and VCount 215 (where the raster takes it) the worker
  // may execute the next SWAP, so a finalised-but-unconsumed list, the
  // pending one, the one being rasterised and the one being written can all
  // be live at once. See pending_bank_.
  static constexpr u32 VRAM_BANK = 6144, PRAM_BANK = 2048, BANKS = 4;
  // On the heap: a third bank made an NDS too big for the stack the tests
  // build one on.
  std::vector<Vertex> vram_ = std::vector<Vertex>(VRAM_BANK * BANKS);
  std::vector<Polygon> pram_ = std::vector<Polygon>(PRAM_BANK * BANKS);
  u32 bank_ = 0, render_bank_ = 1, raster_bank_ = 1;
  // The bank VBlank chose for the render at VCount 215 (render_bank_ at the
  // VBlank join). With the worker a SWAP between the two would otherwise move
  // render_bank_ under render_frame; without it the two are the same bank.
  u32 pending_bank_ = 1;
  // Bank roles change on two threads with the worker on: the worker's SWAP
  // (render_bank_, bank_) and the emulation thread's render_frame
  // (raster_bank_) and VBlank (pending_bank_). One uncontended lock per
  // frame each side.
  std::mutex bank_mu_;
  u32 next_write_bank() const { for (u32 b = 0; b < BANKS; ++b) if (b != render_bank_ && b != raster_bank_ && b != pending_bank_) return b; return 0; }
  u32 num_vertices_ = 0, num_polygons_ = 0;
  // The sorted list per bank, written by finalise_list into the bank it
  // finalises. Per bank, not one array: in the no-FIFO model a SWAP command
  // finalises the next list while the raster of the previous one may still be
  // starting its bands (the etody --timing-oc segfault, 2026-09-07).
  std::array<std::array<const Polygon*, PRAM_BANK>, BANKS> render_polys_{};
  std::array<u32, BANKS> render_count_{};
  u64 swaps_ = 0;                  // see swap_count()
  bool list_unconsumed_ = false;   // a finalised list is waiting for a render (see C_GX_LIST_DROPPED)
  bool render_identical_ = false;
  bool render_stale_ = false;      // note_raster_skipped: the last render is older than rstate_ says
  u32 flush_request_ = 0, flush_attr_ = 0;
  u64 census_prev_hash_ = 0;          // DS_CENSUS_GX: hash of the last submitted list
  bool census_have_prev_ = false;
  u32 census_prev_polys_ = 0, census_prev_verts_ = 0;
  bool census_have_prev_counts_ = false;
  u32 prev_swap_polys_ = 0, prev_swap_verts_ = 0;   // DS_R3D_SKIPDUP: the other bank's list size
  bool rendered_before_ = false;

  Vertex* cur_vram() { return &vram_[bank_ * VRAM_BANK]; }
  Polygon* cur_pram() { return &pram_[bank_ * PRAM_BANK]; }
  u32 vram_base() const { return bank_ * VRAM_BANK; }

  // The packed-command walk, shared by the single-word port and the burst.
  // The two differ only in their sink, so the assembly state machine has one
  // copy: a divergence between them would be a silent accuracy bug.
  template <class S> [[gnu::always_inline]] static inline void gxfifo_word(u32 value, GxParse& p, S& sink);
  // Replay everything queued, now.
  void drain_all();
  // The finished polygon list: sort it for the renderer, decide whether it
  // repeats the previous one. At the SWAP command.
  void finalise_list();
  // Put temp_vtx_ back in position order and reset vslot_ to the identity.
  void normalise_temp_vtx();
  void gxfifo_write(u32 value);
  // One call site: the drain_all replay loop. Every command goes through this
  // one switch, reading its parameters from the log in place; there is no
  // accumulator and no second dispatch.
  void exec_single(u8 cmd, const u32* p);


  // Geometry.
  void update_clip_matrix();
  void submit_vertex();
  void submit_polygon();
  // The two survivor legs of submit_polygon (see there) and their shared tail.
  // Out of line so the reject-only entry carries neither their stack frames
  // nor their register pressure.
  __attribute__((noinline)) void emit_polygon_unclipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, bool facing);
  __attribute__((noinline)) void emit_polygon_clipped(const Vertex* const* src, int nverts, int clipstart, const u16* reused_idx, int lastpolyverts, bool facing);
  Polygon* new_polygon(bool facing);
  static u8 outcode(const s32* pos);
  void finish_polygon(Polygon* poly, int nverts);
  void calculate_lighting();
  void box_test(const u32* params);
  void pos_test();
  void vec_test(u32 param);
  void reset_render_state();
};

// Forced-inline members called from the inline code above (q_push, write):
// their bodies must be visible in every translation unit that uses them.
} // namespace ds::gpu
