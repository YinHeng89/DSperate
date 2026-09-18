// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include <functional>
#include <cstdio>
#include <cstdlib>
#include "core/types.h"
#include "core/gpu/texcache.h"

#include <unordered_map>

#include <array>
#include <atomic>
#include <thread>
#include <memory>
#include <string>
#include <vector>

namespace ds { struct NDS; }

namespace ds::gpu {

namespace vk { class Device; class Raster; }

class Gpu3D;
struct Polygon;
struct Vertex;
struct RenderState;
class VramMap;
struct VramView;

// Software rasteriser for the 3D engine's polygon list.
//
// Scanline renderer with the hardware's fixed-point edge stepping (18-bit
// slope fraction computed as x * (1/y)), its two-stage perspective-correct
// interpolation (a 9-bit factor along Y, 8-bit along X, then linear
// interpolation of the attributes by that factor), its edge fill rules, a
// two-deep pixel stack for anti-aliasing and the final pass (edge marking,
// fog, AA blend). The behaviour follows the documentation and software
// renderer of the melonDS project (GPLv3); output is checked against it
// pixel for pixel.
//
// The working buffers are a ring of four 258-pixel lines (a one-pixel
// border each side so edge marking never tests outside them): line y is
// rendered, then the final pass of line y-1 reads lines y-2..y, and the
// finished line is copied to the output buffer. The whole working set
// (colour, depth, attributes, two pixels deep) stays in L1.
// The render registers latched at a swap (Gpu3D::vblank); the raster works
// from a copy (Renderer3D::rs_frame_).
struct RenderState {
  u32 dispcnt = 0;
  u8  alpha_ref = 0;
  std::array<u16, 32> toon{};
  std::array<u16, 8> edge{};
  u32 fog_color = 0, fog_offset = 0, fog_shift = 0;
  std::array<u8, 34> fog_density{};
  u32 clear_attr1 = 0x3F000000, clear_attr2 = 0x00007FFF;
};

class Renderer3D {
public:
  explicit Renderer3D(NDS& nds);
  ~Renderer3D();
  void reset();
  // Save states: the rendered output the display is reading this frame (the
  // frame was rasterised from VRAM as it was at line 215, which may since
  // have changed, so it cannot be re-rendered). Loading resets everything
  // else, the texture cache included.
  template <class S> void sync_output(S& s);

  // Rasterise the frame latched by the geometry engine.
  void render(const Gpu3D& gx);

  // Output lines are RGB666 in bits 0-21, 5-bit alpha in bits 24-28, read
  // through a FrameRef (below): the buffer is double-buffered so the raster
  // can move on to the next frame at line 215 while the display of this one
  // is still being composited.

  // Portable pixel-pipeline pieces, exposed for the unit tests.
  static u32 alpha_blend(u32 dispcnt, u32 src, u32 dst, u32 alpha);

  // Anti-aliasing (DISP3DCNT bit 4) honoured or not. Off, the raster treats
  // the bit as clear for the whole frame -- no coverage, no pixel-stack push,
  // no under-layer depth test, no AA blend in the final pass -- which is not
  // what the hardware draws, so the core default is on; the SDL frontend
  // makes it opt-in (video.aa), the headless frontend has --no-aa for measurement.
  void set_aa(bool on) { aa_ = on; }
  bool aa() const { return aa_; }

  // The GPU 3D raster (docs/gpu-raster-scoping.md, P1). Off unless a frontend
  // asks for it, and it may decline: no libvulkan, no device, no memory type
  // with the cacheability the composite needs, or -- today -- a pixel
  // pipeline that is not finished. Returns whether it is now on; `why` takes
  // the reason when it is not.
  //
  // Only ever called on the coordinator. The band workers are Renderer3D
  // instances too (bands_), and they must not each open a device.
  bool set_gpu_raster(bool on, std::string* why = nullptr);
  bool gpu_raster_active() const;

  // A/B mode (--gpu-ab): draw every frame BOTH ways and compare, in the same
  // call, from the same polygon list and the same resolved textures. That is
  // the whole point of doing it in-process rather than diffing two runs --
  // there is no timing divergence to rule out first, so a difference is the
  // GPU's and nothing else's. The CPU's output is still what the display
  // gets, so A/B is a measurement mode, not a render mode.
  void set_gpu_ab(bool on) { gpu_ab_ = on; }
  // Why a frame went to the CPU instead. Reported per reason rather than as a
  // total, because the total only says the gate is narrow and the breakdown
  // says which shader feature would widen it most.
  enum GpuReject : u32 {
    GpuOk = 0, GpuAA, GpuEdgeMark, GpuFog, GpuRearBitmap,
    GpuShadow, GpuToon, GpuDepthEqual, GpuWireframe, GpuRejectCount
  };
  static const char* gpu_reject_name(u32 r);

  struct GpuStats {
    u64 frames = 0;       // frames the GPU drew (or, in A/B, drew alongside)
    u64 eligible = 0;     // frames the gate would accept, whether or not one was drawn
    u64 rejected = 0;     // frames the feature gate sent to the CPU
    u64 failed = 0;       // frames the GPU accepted but could not dispatch
    const char* last_fail = nullptr;   // and why the most recent one did not
    // Refusals by reason: the gpu_fail_ strings are literals, so a pointer
    // identifies one. Eight is more than there are reasons.
    struct Fail { const char* why = nullptr; u64 n = 0; } fails[8];
    u64 kept = 0;             // frames the identical-frame path kept, drawing nothing
    // Where the frame's GPU time goes. `upload` is the CPU converting the
    // polygon list into mapped memory and submitting; `wait` is whatever the
    // emulation or compositing thread then blocks for on the fence. The two
    // answer different questions -- whether driving the GPU is cheap (it
    // should be ~0.03 ms) and whether the GPU is keeping up.
    // Split by WHERE the wait is paid, because the three mean different
    // things. `wait_line` is the compositor catching up to a frame the GPU is
    // still drawing -- the expected, overlappable kind. `wait_frame` is the
    // next frame's dispatch finding the previous one unfinished: the GPU did
    // not keep up with a whole frame. `wait_forced` is everything else --
    // chiefly a texture VRAM bank moving under the raster (Bus::update_vram
    // calls sync_raster), which lands at an arbitrary point in the frame and
    // so exposes the whole remaining latency with no slack to hide it in.
    u64 upload_ns = 0, wait_line_ns = 0, wait_frame_ns = 0, wait_forced_ns = 0;
    u64 submit_ns = 0;        // of upload_ns: Raster::submit itself (command recording and the queue submits)
    u64 wait_forced_n = 0, texel_words = 0;
    u64 span_rows = 0;        // rows the span table held, summed
    // The order-free prefix (vk_layout.h GpuFrame::first_ordered): how much
    // of the list it takes, and how often a shadow mask or shadow polygon
    // inside the opaque group cut it short rather than the first translucent.
    u64 opaque_polys = 0, tail_polys = 0, opaque_rows = 0, prefix_cut_by_shadow = 0;
    u64 tail_area = 0;        // the tail's bounding-box area in pixels, summed: what the ordered loop walks at most
    u32 span_rows_max = 0;    // and the most any one frame needed
    u64 deferred = 0;         // display frames shown a frame late, and so never waited for
    u64 undeferred = 0;       // and those that could not be, so paid the fence
    u64 defer_no_caller = 0;  // ... because the caller refused (capture, or the knob is off)
    u64 defer_no_prev = 0;    // ... because the frame before was not GPU-drawn
    u64 reject[GpuRejectCount] = {};   // rejections by first reason found
    u64 ab_checked = 0;   // frames compared
    u64 ab_bad = 0;       // of those, frames with any differing pixel
    u64 ab_pixels = 0;    // differing pixels, summed
    u32 ab_worst = 0;     // most differing pixels in one frame
    s32 ab_first_frame = -1;  // first frame that differed
  };
  const GpuStats& gpu_stats() const { return gpu_stats_; }
  // GPU time per pass (DS_VK_TIMING=1): ns[] summed over `frames`; false
  // when the backend is not timing.
  bool gpu_pass_times(u64 ns[5], u64* frames) const;
  u32 gpu_bands() const;   // completion checkpoints the GPU frame is cut into

  // --gpu-coverage: does the DS's span ever fall OUTSIDE the polygon as a
  // graphics pipeline would rasterise it?
  //
  // The question decides whether the raster could move to vertex/fragment
  // shaders at all. A fragment shader only runs where the hardware generates
  // a fragment, and the hardware generates one where a pixel CENTRE is inside
  // the triangle. The DS's span is not that: it comes from 18-bit slope
  // stepping with its own fill rules, X-major edge runs and an xmin/xmax
  // clamp, and where those put a pixel the geometry does not cover, no
  // fragment shader can recover it.
  //
  // So this counts, over real frames, every pixel the software raster draws
  // and asks whether a pixel centre at that position lies inside the polygon.
  // It needs no GPU and answers the question before any is written.
  void set_coverage_probe(bool on) { coverage_probe_ = on; }
  struct CoverageStats {
    u64 drawn = 0;        // pixels the raster drew
    u64 outside = 0;      // of those, pixels a pixel-centre test says are not in the polygon
    u64 spans = 0;        // scanline spans examined
    u64 spans_bad = 0;    // spans with at least one such pixel
    u32 worst_run = 0;    // longest run of them in one span -- how far a primitive would need expanding
    // Spans by how many of their pixels fell outside. The shape of this is
    // the answer: a tail of ones and twos at span ends is a rounding effect a
    // slightly expanded primitive would cover, and anything else is not.
    u64 bucket[6] = {};   // 1, 2, 3-4, 5-8, 9-16, >16
    u64 spans_all_out = 0;   // spans with NO pixel inside the polygon at all
  };
  static const CoverageStats& coverage_stats();

  // Run the feature gate over every frame and count what it would take,
  // without a GPU and without drawing anything differently. Worth having
  // separately from the raster: it answers "is this whitelist wide enough to
  // be worth finishing the shader for, and on which games" on any machine,
  // now, rather than after the pixel pipeline exists. The gate is pure -- it
  // reads the polygon list and the render state and writes nothing -- so a
  // dry run cannot change a frame.
  void set_gpu_gate_dryrun(bool on) { gpu_gate_dryrun_ = on; }

  // Where a DIFFERING A/B frame goes, so that the difference can be looked
  // at rather than only counted. Two streams in exactly the format
  // tools/compare_frames.py already reads -- 0xAARRGGBB, top screen then
  // bottom -- with the 3D layer expanded into the top screen and the bottom
  // left blank. That is a deliberate choice of format over convenience: the
  // diff images, the bounding box and the PNG dump all come for free, and
  // frame N of one file is frame N of the other because only differing
  // frames are written and both are written together.
  void set_gpu_ab_dump(const char* ref_path, const char* cand_path);

private:
  NDS& nds_;
  // The ring holds a whole chunk of scanlines at once, not just the line
  // being drawn: polygons are rasterised chunk at a time in list order
  // (render_chunk), so every line of the chunk must be writable while any
  // polygon in it is being drawn. CHUNK lines, plus one border line either
  // side for the final pass, rounded up to a power of two for the masking.
  //
  // RING is a build-time knob (-DDS_R3D_RING=): the live tile is
  // 3 * W * RING * 2 words of colour/depth/attr plus a 256-byte stencil row
  // per line, which at RING 16 is 99 KB -- three times the 32-36 KB DraStic
  // sizes its bins to, and well past the A55's 32 KB L1D. RING 8 halves it.
  // Must be a power of two (row_of masks with it) and at least CHUNK + 2.
  //
  // 8 is the default because it measured faster on every scene: -0.30 % to
  // -1.64 % of frame time, mlbis over-budget frames -9.03 %, output
  // byte-identical. 16 remains selectable for comparison.
#ifndef DS_R3D_RING
#define DS_R3D_RING 8
#endif
  static constexpr int W = 258, RING = DS_R3D_RING, CHUNK = RING - 2, RSIZE = W * RING;
  static_assert((RING & (RING - 1)) == 0, "RING must be a power of two");
  static_assert(CHUNK >= 2, "CHUNK must leave room for the final pass lag");
  // Ring row of frame line y (-1 and 192 are the border rows); the pixel
  // address of (x, y) is row_of(y) + 1 + x, the pixel underneath RSIZE on.
  static constexpr u32 row_of(s32 y) { return static_cast<u32>((y + 1) & (RING - 1)) * W; }

  // Colour: R 0-5, G 8-13, B 16-21, A 24-28.
  // Attr: bits 0-3 edge flags (L/R/T/B), bit 4 back-facing, bits 8-12 AA
  // coverage, bit 15 fog, bits 16-21 translucent polygon id, bit 22
  // translucent, bits 24-29 opaque polygon id.
  // The second half of each buffer holds the pixel underneath (for AA).
  // Eight words of slack: the resolve works eight lanes at a time from any
  // span start, and a group that begins near x = 255 of the last ring row
  // loads and writes back (unchanged) lanes past the under plane's end.
  std::array<u32, RSIZE * 2 + 8> color_{}, depth_{}, attr_{};
  // Finished lines, two frames' worth. render() draws into the buffer the
  // display is not reading (display_ ^ 1) and then makes it the displayed
  // one; a FrameRef taken before that keeps naming the old buffer, which is
  // what lets the compositor of frame N run on past line 215 of frame N.
  std::array<u32, 256 * 192> out_[2]{};
  u32 display_ = 0;
  std::array<u8, 256 * RING> stencil_{};   // one row per ring line: see render_chunk
  // "the polygon drawn immediately before this one on THIS line was a shadow
  // mask", which is what decides whether the stencil row is cleared or added
  // to. Per ring line, not global: render_chunk draws a polygon's whole run of
  // lines before moving to the next polygon, so a single flag would carry one
  // line's state onto the next.
  std::array<bool, RING> prev_shadow_mask_{};

  // Perspective-correct interpolation factor between two endpoints.
  template <int dir> struct Interp {
    s32 x0 = 0, x1 = 0, xdiff = 0, x = 0;
    int shift = 0; bool linear = false, wbuffer = false;
    s32 xrecip_z = 0; s32 w0n = 0, w0d = 0, w1d = 0; u32 yfactor = 0;
    u32 recip = 0;   // ceil(2^32 / xdiff) for xdiff >= 2: exact linear division by one multiply and a fix-up
    void setup(s32 x0_, s32 x1_, s32 w0, s32 w1, bool wbuf);
    void set_x(s32 xv);
    s32 interpolate(s32 y0, s32 y1) const;
    s32 interpolate_z(s32 z0, s32 z1) const;
  };

  // Everything the per-pixel work needs from the polygon and the render
  // state, decoded once per polygon.
  struct SpanBuf;
public:
  struct Shade;
private:
  struct SpanJob;
  // Resolves a whole batch: the kernel loops jobs_ itself, so its prologue
  // and the setup that depends only on the Shade are paid once per batch
  // rather than 2.4-2.9 times per span.
  using ResolveFn = void (Renderer3D::*)(const Shade&, const SpanJob*, u32);
public:
  // Everything a span needs from its polygon, decoded once (the NEON gather
  // helpers in render3d.cpp take it, hence public).
  struct Shade {
    u32 blendmode, polyalpha, polyattr;
    bool highlight, textured, wireframe, shadow, polyattr_z;   // polyattr_z: translucent pixels update depth
    u32 dispcnt, alpha_ref;
    const u16* toon;
    // Texture: format, VRAM base, size, wrap/flip, transparent-colour-0 alpha, palette base.
    u32 fmt, base, texpal, alpha0;
    s32 width, height;
    bool srep, sflip, trep, tflip;
    // Direct host pointers when the whole texture / palette lies in directly
    // mapped, host-contiguous VRAM (nullptr: go through the views).
    const u8* tex_ptr;
    const u16* pal_ptr;
    // Views for the formats that address VRAM per texel (the compressed one).
    const VramView* texv; const VramView* palv; const VramMap* vm;
    // Decoded texels from the texture cache (width*height words, colour16 |
    // alpha << 16), or nullptr when the cache is off.
    const u32* texels;
    // NEON builds: the four-texel gather specialised for (format, S wrap,
    // T wrap), or nullptr for the per-lane sampler (render3d.cpp).
    const void* gather4;
    // The resolve kernel, the depth mode and whether the vector path applies,
    // all bound once here instead of re-derived on every flush -- which for a
    // polygon that does not batch is once per scanline. Same shape as
    // gather4 above; DraStic makes the equivalent choice at bin time and it
    // is the reason its flush has no indirect calls at all
    // (docs/techniques/02 s2).
    ResolveFn resolve;
    int mode;      // pick_depth_mode
    bool vec;      // the NEON resolve applies (no shadow / wireframe / blend 2)
    // Every record alpha the shading pass can emit for this polygon is 0 or
    // 31, and the 0-alpha lanes never reach the resolve (span_shade clears
    // their pass bits), so the resolve's translucent machinery compiles out.
    bool opaque;
    // Edges all fill when AA, edge marking, blended translucency or wireframe
    // is on. Built from dispcnt, polyalpha and wireframe -- all fixed for the
    // polygon -- and so decided here rather than on every scanline.
    bool always_fill;
    // Attributes uniform across the polygon, determined once during setup.
    // This avoids rechecking the same endpoints on every scanline.
    bool attrs_constant, rgb_constant;
  };
private:

  // One polygon edge walked down the scanlines.
  template <int side> struct Slope {
    s32 increment = 0; bool negative = false, xmajor = false;
    Interp<1> interp;
    s32 x0 = 0, xmin = 0, xmax = 0, xlen = 0, ylen = 0, dx = 0, y = 0, xcov_incr = 0;
    s32 setup_dummy(s32 x0_, bool wbuf);
    s32 setup(s32 x0_, s32 x1_, s32 y0, s32 y1, s32 w0, s32 w1, s32 y_, bool wbuf);
    s32 step();
    s32 xval() const;
    // `aa`: compute the coverage at all (a divide per X-major edge per line);
    // off, it is 0 and nothing reads it (the resolve's AA path is off too).
    template <bool swapped> void edge_params(bool aa, s32* length, s32* coverage) const;
  };

  struct Edge {
    const Polygon* poly;
    Slope<0> left; Slope<1> right;
    s32 xl, xr;
    u32 cur_vl, cur_vr, next_vl, next_vr;
    Shade sh;
    // Everything the per-scanline path needs that only changes when an edge
    // is set up. Slope::step advances dx and y and nothing else, so the slope
    // shape (negative / xmajor / increment) and the current vertex pair hold
    // for a whole run of scanlines -- refresh_edge_state recomputes these at
    // the vertex boundaries instead. This is DraStic's "decide it at bin
    // time" applied to the fill rules (docs/techniques/02 s2); the vertex
    // pointers in particular were two indirections per scanline.
    const Vertex *vcl, *vnl, *vcr, *vnr;   // vertex(vtx[cur_vl]) and friends
    s32 wcl, wnl, wcr, wnr;                // p.w[cur_vl] and friends
    s32 zcl, znl, zcr, znr;                // p.z[cur_vl] and friends
    bool nx_l, nx_r;        // negative || !xmajor
    bool px_l, px_r;        // !negative && xmajor
    bool lneg_xm;           // left.negative && left.xmajor
    bool lxm, rxm;          // xmajor
    bool same_incr;         // left.increment == right.increment
    bool l_incr0, r_incr0;  // increment == 0
    bool next_sx_differ;    // vnl->sx != vnr->sx (symmetric, so swap-safe)
  };
  std::array<Edge, 2048> edges_{};
  // Edges are built on first use, not for the whole list: with the frame cut
  // into bins each instance draws a fraction of the lines, and a full
  // setup_polygon per polygon per instance (~500 B of Shade, slopes and
  // cursors) was most of a megabyte of stores a frame that touched nothing.
  // build_edges records the polygon and its list index (the recorded
  // texture-cache pointers are per list index); built_edge does the rest.
  std::array<u16, 2048> edge_list_{};    // edge index -> polygon list index
  std::array<u64, 32> edge_built_{};     // one bit per edge
  bool edge_is_built(u32 i) const { return (edge_built_[i >> 6] >> (i & 63)) & 1; }
  Edge& built_edge(u32 i) {
    if (!edge_is_built(i)) { edge_built_[i >> 6] |= u64{1} << (i & 63); setup_poly_ = edge_list_[i]; setup_polygon(edges_[i], *edges_[i].poly); }
    return edges_[i];
  }
  // Per-line active polygon set: polygons enter at their top line (buckets
  // by ytop, in list order) and leave after their last line; the active list
  // is kept in list order, which the blending rules depend on.
  std::array<u16, 2048> order_{};        // edge indices sorted by ytop, stable
  std::array<u16, 194> bucket_{};        // order_ offset where ytop == y starts (193 = end)
  std::array<u16, 2048> active_buf_[2]{};
  std::array<u16, 2048> enter_{};   // chunk's entering polygons, re-sorted into list order
  // Dense bin-local membership for large entry slices. Materialising in edge
  // order avoids the O(n log n) sort when a tile starts many polygons.
  std::array<u64, 32> enter_bits_{};
  u16* active_ = nullptr; u16* active_next_ = nullptr;
  u32 active_count_ = 0;
  std::array<bool, 192> line_touched_{};   // a polygon was active on the line (final pass needed)

  const Gpu3D* gx_ = nullptr;
  const RenderState* rs_ = nullptr;
  bool aa_ = true, aa_rendered_ = true;   // aa_rendered_: the setting the kept frame was drawn with
  // DISP3DCNT as the raster sees it this frame: rs_->dispcnt with bit 4
  // cleared when AA is off. Every AA decision in the raster reads this, so
  // one place decides. With the bit clear the whole under layer (the second
  // half of the pixel buffers) is dead for the frame: the only reads of it
  // that reach the output are the AA blend and, through the pushed copy, the
  // under-layer depth test, translucent plot and fog -- all gated on this
  // bit -- and every AA read is of a pixel pushed in the same frame (edge
  // flags on a top pixel only ever come from an opaque write that pushed).
  // A shadow polygon's stencil bit 2 also reads it, but that bit only steers
  // under-layer writes (the edge-flag clear it makes is local to the
  // resolve). So the depth pre-pass, the resolve, the shadow stencil and the
  // fog pass all skip their under-layer work when the bit is clear, and
  // nothing can tell -- with or without shadow polygons.
  u32 dispcnt_ = 0;
  // The 32-entry toon table as three 6-bit byte planes, expanded once per
  // frame from rs_->toon for the vector toon / highlight stages (flush_batch).
  alignas(16) u8 toon6_[3][32] = {};
  // The eight edge-marking colours as three 6-bit byte planes (final_pass).
  alignas(16) u8 edge6_[3][16] = {};
  void expand_toon();
  const VramMap* vm_ = nullptr;
  mutable TextureCache texcache_;
  bool rendered_once_ = false;   // the colour buffer holds a rendered frame
  const VramView* texv_ = nullptr;
  const VramView* palv_ = nullptr;

  u8  tex8(u32 addr) const;
  u16 tex16(u32 addr) const;
  u16 pal16(u32 addr) const;
  // A batch is up to BATCH_PX pixels and one more span (which may itself be a
  // full 256-pixel scanline) can always be staged before the flush, so the
  // buffers hold both plus the vector slack.
  static constexpr u32 BATCH_PX = 256;
  static constexpr u32 BATCH_CAP = BATCH_PX + 256 + 16;
  struct SpanBuf {
    s32 x0;
    // Every array carries sixteen entries of slack: the stages round the span
    // length up to their vector width (four, eight or sixteen pixels) and write
    // whole vectors, so the tail of a short span runs past `n`.
    alignas(16) u32 fac[BATCH_CAP];
    alignas(16) s32 z[BATCH_CAP];
    // The pixel stages read colour as a 6-bit channel and texture coordinates
    // as s16, so the span keeps them in those widths (a third of the bytes and
    // eight pixels a vector instead of four).
    alignas(16) u8  vr[BATCH_CAP], vg[BATCH_CAP], vb[BATCH_CAP];
    alignas(16) s16 sc[BATCH_CAP], tc[BATCH_CAP];
    // Which pixels can still draw: bit 0 the top layer, bit 1 the pixel
    // underneath. depth_candidates sets it from the depth test alone; on the
    // vector path span_shade then clears the lanes the alpha test kills, so
    // by the time the resolve reads it the plane means "will draw", not
    // "passed depth", and a group of eight zeroes is skipped whole.
    alignas(16) u8 pass[BATCH_CAP];
    alignas(16) u32 tcol[BATCH_CAP];     // texels for the span (textured polygons), colour15 and
    alignas(16) u32 talp[BATCH_CAP];     // 5-bit alpha, gathered once per span
    alignas(16) u32 col[BATCH_CAP];      // shaded pixel records (18-bit colour, alpha 24-28)
  };

  // One buffer per renderer, not one per call: a batch is staged into it
  // across several calls before the pixel stages run over the whole thing.
  SpanBuf spanbuf_;
  // One staged span of the polygon currently being batched. The pixel stages
  // (texel gather and shading) run once over the whole batch instead of once
  // per span, which is what DraStic's 256-pixel flush buys
  // (docs/techniques/02 s3). Everything here is what the resolve still needs
  // per span: its scanline, its candidate range, where it sits in the batch
  // buffers, and the three-part edge/fill decisions.
  struct SpanJob {
    s32 y, ca, cb; u32 off;
    s32 xdraw, lim0, lim1, lim2;
    s32 l_cov, r_cov;
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  std::array<SpanJob, 256> jobs_{};
  u32 njobs_ = 0, batch_px_ = 0;

  // One scanline's geometry and endpoint attributes: everything the
  // per-scanline path needs that is a function of the polygon and y alone,
  // with no framebuffer state in it.
  //
  // precompute_lines derives a whole run of scanlines at once, so the edge
  // walk, the fourteen interpolations, the swapped-edge decision and the fill
  // rules are one loop over the run instead of a fresh derivation inside the
  // per-scanline path. That is DraStic's structure -- its edge and span setup
  // is all per-polygon (render_polygon_setup_spans_asm_1x,
  // render_polygon_interpolate_edges, render_polygon_edge_interpolate_*) and
  // its scanline loop is pointer arithmetic against the array those produce.
  struct LineSpan {
    s32 xstart, xend;        // span endpoints, the right-edge push-left applied
    s32 wl, wr, zl, zr;      // endpoint w / z, swapped-edge order applied
    s32 al[5], ar[5];        // endpoint r g b s t, swapped-edge order applied
    s32 l_len, r_len, l_cov, r_cov;
    s32 xa, xb;              // the clipped screen range [xa, xb)
    int yedge;
    bool l_fill, r_fill, wf_skip;
  };
  // render_band rasterises at most CHUNK scanlines per render_chunk call, and
  // a polygon's run inside one chunk is bounded by that.
  std::array<LineSpan, CHUNK> lines_{};
  // Walk the edges over scanlines [y0, y1) filling lines_[0 .. y1-y0), and
  // leave the edge cursors stepped past y1-1 exactly as the per-scanline path
  // left them.
  void precompute_lines(Edge& e, s32 y0, s32 y1);
#if DSPERATE_NEON && defined(__arm__)
  static bool edge_values_vec(const Interp<1>& in, s32 w0, s32 w1, const Vertex& vc, const Vertex& vn, s32* w, s32* a);
#endif
  // The half of the old render_polygon_line that framebuffer state reaches:
  // the depth pre-pass, the attribute staging and the batch job.
  void stage_line(Edge& e, s32 y, const LineSpan& ls);
  u32  texture_sample(const Shade& sh, s32 s, s32 t, u32* alpha) const;
  template <bool textured> u32 shade_pixel(const Shade& sh, u32 vr, u32 vg, u32 vb, s32 s, s32 t) const;
  void plot_translucent(u32 addr, u32 color, u32 z, u32 polyattr, bool shadow);
  template <int mode> bool depth_pass(u32 addr, s32 z, u32 dstattr) const;
  // One contiguous range of one span. Called only from the batch kernels and
  // from the vector kernel's scalar fallback, so it inlines into them and the
  // Shade-dependent setup lifts out of the job loop on its own.
  template <int mode, bool textured, bool aa, bool shadow> void resolve_span(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa, bool shadow> void resolve_batch(const Shade& sh, const SpanJob* jobs, u32 n);
#if DSPERATE_NEON
  // Four pixels per step; same results as resolve_span (the specification),
  // for polygons without shadow / wireframe / toon shading.
  template <int mode, bool textured, bool aa, bool opq> [[gnu::always_inline]] inline void resolve_span_vec(const Shade& sh, const SpanBuf& sb, s32 y, s32 xa, s32 xb, int part, int edge, s32 l_cov, s32 r_cov, s32& xcov);
  template <int mode, bool textured, bool aa, bool opq> void resolve_batch_vec(const Shade& sh, const SpanJob* jobs, u32 n);
  // Census only (DS_PROFILE): how uniform the resolve's kind decision is, per
  // eight-pixel group and per batch. `kinds` is one kind-code byte per lane,
  // zero where the lane draws nothing; a group is uniform when every drawing
  // lane carries the same code. rk_or_ / rk_mixed_ carry that up to the batch.
  void rk_census(u64 kinds, u64 p8);
  u32 rk_or_ = 0, rk_groups_ = 0;
  bool rk_mixed_ = false;
  void texture_gather4(const Shade& sh, const s16* sa, const s16* ta, u32* colour, u32* alpha) const;
  // A textured span's texels, gathered once into the span buffer before the
  // resolve loop reads them (removes an indirect call per four pixels).
  void span_texels(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;
  // The span's shaded colours (texture blend and the packed 18-bit record),
  // computed before the resolve loop so the blend mode is decided once.
  template <bool textured> void span_shade(const Shade& sh, SpanBuf& sb, s32 ca, s32 cb) const;
  static const void* select_gather4(const Shade& sh);
#endif
  static u32 fac_bound(s32 xdiff, s32 wl, s32 wr);
  // Returns whether the perspective factor was staged for the whole span
  // (only a w-buffered depth needs it there); span_attrs takes that as fac_ready.
  bool span_stage(SpanBuf& sb, s32 xstart, s32 xend, s32 xa, s32 xb, s32 wl, s32 wr, s32 zl, s32 zr, bool wbuffer,
                  const s32* al, const s32* ar, bool with_attrs, u32 off) const;
  void span_attrs(SpanBuf& sb, s32 xstart, s32 xend, s32 ca, s32 cb, s32 wl, s32 wr, const s32* al, const s32* ar, bool attrs_constant, bool rgb_constant, bool fac_ready) const;
  void setup_left_edge(Edge& e, s32 y) const;
  void setup_right_edge(Edge& e, s32 y) const;
  void setup_polygon(Edge& e, const Polygon& p);
  void rewind_edge(Edge& e);
  // Recompute Edge's cached per-edge-segment state. Called from the two edge
  // setups (so it cannot be missed) and once per polygon for the flat case.
  void refresh_edge_state(Edge& e) const;
  void setup_shade(Shade& sh, const Polygon& p);
  // The texture part of setup_shade; true when the polygon reads the decoded
  // cache (resolved on the emulation thread only, see render()).
  bool texture_fields(Shade& sh, const Polygon& p) const;
  // The resolve kernel for a decoded Shade (the dispatch tables live with
  // flush_batch in render3d.cpp).
  static ResolveFn select_resolve(const Shade& sh);
  // One span's three-part edge/fill walk (left edge run, interior, right
  // edge run), clipped to the range the depth pre-pass left alive. `range`
  // draws one clipped run; the batch kernels pass their own inlined body.
  //
  // always_inline, and not negotiable: left to itself GCC emits this as a
  // real call with a 112-byte frame and five register-pair saves, paid once
  // per span. Measured, that call cost 93-176 cycles a span -- 1.5-3.1 % of
  // frame across four scenes.
  template <typename Range> [[gnu::always_inline]] inline void walk_span(const SpanJob& j, Range&& range);
  void render_shadow_mask_line(Edge& e, s32 y);
  void flush_batch(const Shade& sh);
  // Rasterise lines [ya, yb) polygon at a time rather than line at a time:
  // the active set for the whole chunk is merged once, then each polygon
  // draws every line it covers inside the chunk before the next one starts.
  // Per pixel the order is unchanged -- a pixel belongs to one line, and the
  // polygons still reach it in list order -- which is what the blend and
  // stencil rules depend on.
  void render_chunk(s32 ya, s32 yb);

  // ---- banded parallel rasterising ---------------------------------------
  //
  // The screen is split into horizontal bands, one worker per band. A band
  // owns its line ring, its active polygon set and its edge cursors, and
  // writes only its own output lines, so the workers share nothing mutable:
  // the only shared state is the decoded-texture cache, which is resolved to
  // plain pointers on the calling thread before any worker starts (the cache
  // itself is not thread-safe). Everything else -- the edge setup included --
  // is a worker's own, built on its pool thread.
  //
  // Slope::setup takes the line to position at and computes the edge state
  // directly from it, so a band can enter a polygon that began above its
  // first line without walking the lines in between. The final pass of a
  // line reads its two neighbours, so a band rasterises one line above the
  // range it emits; the line below comes from its own loop.
  //
  // Band boundaries are fixed for a given band count, so the split is
  // deterministic and the output does not depend on thread scheduling.
  void build_edges();   // from list_polys_ / list_count_, latched by render() or prepare_worker
  void seed_active(s32 y);
  void render_band(s32 y0, s32 y1, u32* dst);
  void prepare_worker(const Gpu3D& gx, const Polygon* const* polys, u32 npoly, const std::vector<const u32*>* texels, const RenderState* rs);
  // The coordinator's copy of the render registers for the frame: the
  // engine's own are rewritten at the next VBlank while the raster may
  // still be running (see Gpu3D::raster_bank_).
  RenderState rs_frame_;
  static u32 band_count(u32 polygons);
  // Bin cut points. The frame is split into more bins than there are workers
  // and each worker takes the next unclaimed one, so a bin that turns out
  // heavy is absorbed by the others finishing theirs early. A static split
  // cannot do that: measured per band on the device the three came out
  // 2.47 / 5.75 / 6.96 ms, a 2.8x spread, and the emulation thread waits for
  // the slowest -- 6.96 ms against the 5.06 ms an even three-way split of the
  // same 15.19 ms would have cost.
  static constexpr u32 MAX_BINS = 32;
  // How the bins are sized. Ascending is the deadline shape (small first bin,
  // large last) and is right while bins == workers, when they all start at
  // once; the others are for the binned regime, where they do not.
  // DS_R3D_SPLIT=stair|even|desc|taper.
  enum class Split { Ascending, Even, Descending, Taper };
  static Split split_mode();
  void compute_bins(u32 nbins, u32 workers);
  static u32 bin_count(u32 workers);
  u32 adaptive_workers(u32 max_workers);
  static bool threads_forced();
  static bool adapt_enabled();
  u32 workers_now_ = 0, quiet_frames_ = 0;
  s64 wait_ema_ = 0;             // averaged block time, the regime signal
  std::atomic<u64> wait_ns_{0};  // time blocked on the raster this frame (any thread: the compositor waits too)
  std::array<s32, MAX_BINS + 1> bin_y_{};
  u32 nbins_ = 0;

public:
  // The raster runs on the workers while the emulation thread carries on.
  // sync_line waits for the one band that owns a display line; sync_all waits
  // for all of them and is the escape hatch for anything that would change
  // what the workers read.
  //
  // What the display reads for one frame: which output buffer, and which
  // pool generation's bands to wait on before a line of it is read. Taken on
  // the emulation thread at the start of the display frame (Gpu::begin_frame)
  // and handed to whichever thread composites it -- the compositor keeps
  // reading frame N's buffer and waiting on frame N's bands after render()
  // has dispatched frame N+1 into the other buffer at line 215. A frame with
  // nothing outstanding (rendered inline, kept, or ablated) has nbins 0.
  //
  // A GPU-drawn frame carries `gpu` instead of a band cut: `out` names the
  // raster's own buffer (the composite reads Vulkan memory directly -- see
  // vk_raster.h) and what sync_line waits for is one fence, not a band. The
  // granularity is the whole frame either way, because a tile raster has no
  // per-scanline completion to report.
  struct FrameRef {
    const u32* out = nullptr;
    u64 gen = 0;
    u32 nbins = 0;
    vk::Raster* gpu = nullptr;
    std::array<s32, MAX_BINS + 1> bin_y{};
    const u32* line(u32 y) const { return out + y * 256; }
  };
  // `allow_defer`: the caller is willing to show the frame BEFORE the one
  // just drawn. That hands the GPU a whole extra frame to finish in, which
  // removes the compositor's wait entirely -- it is by far the largest term
  // left. The cost is a frame of visual latency, and it is unsafe for a frame
  // that display-captures, because capture writes the composited result into
  // VRAM the guest reads back: stale there is wrong emulation, not lag. The
  // caller owns that decision (Gpu::begin_frame).
  //
  // Only a GPU frame defers, and only when the frame before it was also
  // GPU-drawn; anything else returns the ordinary ref and waits as before.
  FrameRef frame_ref(bool allow_defer = false) const;
  void sync_line(const FrameRef& f, s32 y);   // any thread; each call waits for one band at most
  void sync_all();
  bool raster_pending() const { return pending_bands_ != 0; }
  // Serial raster cost of the frame most recently synced (sum over bands, ns).
  u64 last_band_sum_ns() const { return band_sum_ns_[0]; }
  // Time the dispatching thread spent on the raster path (waiting, or drawing
  // a stolen bin) since the last take. The shape controller's input.
  u64 take_owner_wait_ns() { return owner_wait_ns_.exchange(0, std::memory_order_relaxed); }
  // Band workers for the next dispatch (0 = band_count's default); set by the
  // shape controller, overridden by DS_R3D_THREADS.
  void set_bands_next(u32 n) { bands_next_ = n; }
private:
  std::function<void(u32)> job_fn_;   // outlives the dispatch, unlike a local
  u32 pending_bands_ = 0;             // bins in flight (0 = nothing running)
  u64 gen_ = 0;                       // pool generation of the bands in flight
  bool async_ = std::getenv("DS_R3D_SYNC") == nullptr;

  // The GPU raster and the device it runs on, owned by the coordinator only.
  // Held as unique_ptrs to incomplete types: the destructor is out of line,
  // so render3d.h stays free of the Vulkan headers and of DSPERATE_VULKAN.
  std::unique_ptr<vk::Device>  vk_dev_;
  std::unique_ptr<vk::Raster>  vk_raster_;
  bool gpu_frame_ = false;    // the last render() went to the GPU: out_[] is not where the picture is
  bool gpu_live_ = false;     // the backend can draw: latched once per frame, before the texture resolve reads it
  bool gpu_frame_prev_ = false;   // the render before the last one also went to the GPU
  bool gpu_defer_ok_ = false;     // ... and so the previous GPU buffer is the frame before this one
  bool gpu_sync_is_frame_ = false;   // the sync_all at the top of render(), as against a forced one
  bool gpu_ab_ = false;
  mutable GpuStats gpu_stats_{};   // mutable: frame_ref is const and counts deferrals
  // Whether the GPU raster implements everything this frame needs. Whole
  // frames, never part of one: the pixel stack, the edge-marking pass and
  // the translucent polygon ids are frame-global, so a frame split across
  // the two rasters would be wrong in ways neither of them is alone.
  u32 gpu_supported(const Polygon* const* polys, u32 npoly) const;
  bool gpu_gate_dryrun_ = false;
  bool coverage_probe_ = false;
  void probe_coverage(const Polygon& p, const SpanJob& j) const;
  // Convert this frame's polygon list into the shader's layout, straight into
  // mapped GPU memory, and submit it. False if it could not be -- a texture
  // the cache does not hold, a tile with more polygons than the bins take, a
  // list past the buffer sizes -- and then the CPU draws the frame.
  bool gpu_dispatch(const Polygon* const* polys, u32 npoly);
  // Why the last refusal, for the report. A string rather than an enum
  // because every one of them is a distinct one-off condition and the only
  // consumer is a human reading a line of output.
  const char* gpu_fail_ = nullptr;
  std::vector<u32> gpu_tile_use_;         // per-tile polygon count, for the overflow check
  std::vector<u8>  gpu_line_mask_;        // "the last polygon on this line was a shadow mask", while uploading
  std::vector<u32> gpu_line_run_;         // and which mask run that line is up to
  // The GPU's texel arena is PERSISTENT across frames: a decoded texture is
  // copied in once and stays, keyed by the cache entry's id and decode
  // version, so a frame copies only what the cache re-decoded. The arena is
  // bump-allocated; when it fills, the frame is refused (CPU raster) and the
  // arena starts over at the next one.
  struct GpuResident { u32 off = 0, words = 0, version = 0; };
  std::unordered_map<u32, GpuResident> gpu_resident_;
  u32 gpu_arena_top_ = 0;
  bool gpu_arena_reset_ = false;          // start over at the next frame (the arena filled)
  std::vector<TextureCache::Ref> poly_texref_;   // per polygon, the GPU path's view of its texture
  void gpu_compare(const u32* cpu, const u32* gpu);
  void gpu_ab_write(FILE* f, const u32* layer);
  FILE* ab_ref_ = nullptr;
  FILE* ab_cand_ = nullptr;
  void gpu_snapshot_output();   // bring a GPU frame into out_[] for a save state

  u32  edge_count_ = 0;
  u32* out_dst_ = nullptr;                              // where final_pass writes
  const std::vector<const u32*>* texels_in_ = nullptr;  // decoded textures per polygon (render() records them)
  // The polygon list this frame draws, latched once by the coordinator after
  // sync_all and handed to every band. Workers never read it off the engine:
  // in the no-FIFO model the SWAP command finalises the next list while this
  // raster is still in flight, and a late-starting band reading the engine's
  // live list drew the new list's polygons against the old bank (etody
  // segfault in texture_sample under --timing-oc, 2026-09-07).
  const Polygon* const* list_polys_ = nullptr;
  u32 list_count_ = 0;
  s32  rendered_upto_ = 0;    // lines this instance has already rasterised this frame
  u32  setup_poly_ = 0;                                 // polygon index during build_edges
  std::vector<const u32*> poly_texels_;
  std::vector<std::unique_ptr<Renderer3D>> bands_;      // workers 1..n-1 (band 0 is this)
  u64 band_ns_[8] = {};                                 // last frame's per-band wall time (workers write their own slot)
  u64 band_sum_ns_[2] = {0, 0};                         // summed band time (serial raster cost) of the last two frames
  u32 last_nb_ = 0;                                     // workers given the last frame (slots of band_ns_ that are live)
  static constexpr u64 kLagBandThresholdNs = 40'000'000; // serial raster cost two workers can still hide; below it a hot compositor gets the third core (DS_R3D_LAG_NS overrides; 40 ms puts Golden Sun's title on two, which wins unclocked and is even with three under --cpu-oc)
  struct Pool;
public:
  void debug_dump(FILE* f);   // DS_WATCHDOG: band hand-off state
private:
  std::unique_ptr<Pool> pool_;
  // Steal-on-wait. When a thread would block for a band (sync_line on the
  // compositing thread, sync_all on the emulation thread) and an unclaimed
  // bin exists, it renders that bin itself instead: a bin nobody has taken
  // would only start when a worker frees up, so drawing it now is never later
  // than waiting for it, and the waiting core does the work. Any thread may
  // steal: what a bin needs is snapshotted per dispatch (ctx_, two slots by
  // generation parity -- the compositor of frame N may still be reading N's
  // while the emulation thread dispatches N+1), each thief draws through a
  // band of its own, and wait_idle waits for thieves as it does for workers,
  // so a VRAM remap never overtakes a stolen bin.
  struct DispatchCtx {
    const Gpu3D* gx = nullptr;
    const Polygon* const* polys = nullptr;
    u32 npoly = 0;
    std::vector<const u32*> texels;
    RenderState rs;
    std::array<s32, MAX_BINS + 1> bin_y{};
    u32 nbins = 0;
    u32* dst = nullptr;
    bool aa = false;
  };
  DispatchCtx ctx_[2];
  struct StealBand { std::unique_ptr<Renderer3D> band; std::atomic<bool> busy{false}; u64 gen = ~u64{0}; };
  StealBand steal_[2];
  std::thread::id owner_;                // the thread render() dispatched from
  std::atomic<u64> owner_wait_ns_{0};
  u32 bands_next_ = 0;
  bool steal_bins(u64 gen, u32 upto);    // claim and draw unclaimed bins until bin `upto` is done; true if it is
  // DS_R3D_STEAL: 0 off, 1 (default) the dispatching thread only, 2 any
  // thread. The compositor stealing measured flat-to-worse on Golden Sun (RG
  // DS, 2026-09-07): it is the critical path there, and a bin it takes is two
  // ahead of the one it needs, so a short wait became a full render.
  static int steal_mode();

  u32  fog_density(u32 addr) const;
  void final_pass(s32 y);
  void final_pass_ref(s32 y);
public:
  // tests/gpu3d_test.cpp: final_pass against final_pass_ref on random buffers; 0 when identical.
  u32  selftest_final_pass(u32 seed, u32 dispcnt);
private:
  void clear_border(s32 y);
  void clear_line(s32 y);
};

} // namespace ds::gpu
