// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>

// The GPU 3D raster (docs/gpu-raster-scoping.md, P1).
//
// What this replaces: the band workers in Renderer3D. What it does NOT
// replace: the geometry engine, which still runs on the CPU and produces the
// polygon list this consumes, and the 2D engines, which keep compositing on
// NEON exactly as they do now.
//
// The hand-off is the reason P1 is cheap. The 2D compositor already takes the
// 3D layer as a plain pointer (kernels.h: resolve16_full's `line3d`,
// layer16_3d), so this writes into Access::CpuRead memory and the kernels
// read it unchanged -- engine2d.cpp and the kernel list do not move at all.
//
// Two rules from the measurements are structural here, not tuning:
//
//   * output() memory is Access::CpuRead (HOST_CACHED). On HOST_COHERENT the
//     NEON composite would read at 0.12 GB/s instead of 4.5.
//   * a frame is ONE submit and ONE fence, however many dispatches it needs.
//     192 line-range dispatches behind one fence cost +0.95 ms; 192 submits
//     with their own fences cost +24 ms and 5.6 ms of CPU.

namespace ds::gpu::vk {

class Raster {
public:
  // Null when the device cannot run it; the caller keeps the software path.
  static std::unique_ptr<Raster> create(Device& dev, std::string* why = nullptr);
  ~Raster();

  Raster(const Raster&) = delete;
  Raster& operator=(const Raster&) = delete;

  // Whether the raster can actually draw a frame. False if the pipelines did
  // not build, so a broken backend can never silently produce wrong output --
  // the caller falls back to the band workers.
  bool ready() const { return ready_; }

  // Internal resolution. 1 for P1; P2 raises it, and everything downstream
  // (the composite) has to agree, which is why it is not a free knob.
  u32 scale() const { return scale_; }
  // The hi-res layer (256S x 192S records) of the frame output() belongs to,
  // as an opaque VkBuffer handle for the present stage's composite. At S = 1
  // it is the same buffer output() maps.
  u64 output_hires_handle() const;
  size_t output_hires_bytes() const;
  // The smooth filter's edge plane of that same frame (0 when the frame did
  // not write one), and whether the filter is on (DS_VK_SMOOTH3D at creation).
  u64 output_edge_handle() const;
  bool smooth() const;

  // Whether the order-free prefix goes through the visibility pass
  // (vis.comp / resolve.comp) rather than the ordered loop. Needs 64-bit
  // buffer atomics; DS_VK_VIS=0 turns it off for attribution. When it is
  // off the caller hands the whole list to the ordered loop, which is exactly
  // the raster as it was, so either setting draws the same picture.
  bool visibility() const { return vis_; }

  // ---- the upload ---------------------------------------------------------
  //
  // The caller writes the frame straight into mapped GPU memory rather than
  // handing over a CPU-side list to be copied: the conversion from Polygon /
  // Vertex is the only pass over the data, and it lands where the shader
  // reads it. Renderer3D::gpu_dispatch is the only caller, and these are
  // valid only between frames -- that is, wherever dispatch() may be called.
  GpuPoly* poly_buffer();                 // DS_MAX_POLYS entries
  GpuVert* vert_buffer();                 // DS_MAX_VERTS entries
  u32*     texel_buffer(u32* capacity);   // decoded texels; capacity in words
  GpuPost* post_buffer();                 // fog, edge and toon tables
  u32*     shadow_run_buffer();           // DS_MAX_POLYS * DS_SHRUN_LINES: see vk_layout.h
  u32*     rowpoly_buffer();              // DS_MAX_SPAN_ROWS: each span row's polygon index, for the span pass

  // Submit the frame: clear the bin counts, bin, then rasterise it in BANDS
  // of scanlines, each with its own completion.
  //
  // The bands are not there to divide the work. The work was already divided
  // far finer -- one workgroup per tile, 192 of them in flight at once -- and
  // splitting the dispatch adds no parallelism whatever. They exist for the
  // one property a single fence threw away: partial completion.
  //
  // Measured on the RG DS Plus: with one fence, essentially the whole GPU
  // time showed up as the compositor blocked on line 0 of the 3D layer
  // (9.23 ms of 9.23 on Etrian Odyssey), because a whole-frame fence makes
  // line 0 wait for line 191. The software raster never had this problem --
  // sync_line waits only for the band owning the line it is reading -- and
  // this restores the same shape, so FrameRef and sync_line are one code path
  // for both rasters again.
  //
  // The cost is one submit per band rather than one per frame. The probe
  // measured 192 submits at +24 ms, about 0.125 ms each, so a handful is
  // cheap; DS_VK_BANDS sweeps it, and eight is the measured minimum.
  //
  // IT HELPS MUCH LESS THAN THE REASONING SAID, and the reason is worth
  // keeping. Partial completion only buys what the CONSUMER can overlap, and
  // the 2D composite is batched -- it takes the whole frame's lines in one
  // burst rather than spread across the frame. So the compositor still
  // arrives after the GPU has started and consumes 192 lines in a few
  // milliseconds; the most banding can hide is the burst's own duration.
  // 9.36 ms to 8.42 ms, about a tenth, against the whole GPU time the
  // argument implied.
  //
  // What the split measurement DOES say is that the slack exists: the
  // "next dispatch" wait is zero, so the GPU finishes a frame's work well
  // before the following frame needs to dispatch. Using that slack means
  // compositing a frame behind, which is a user-visible latency decision and
  // is unsafe for a frame that display-captures the 3D layer (DISPCAPCNT bit
  // 24 writes it into VRAM the guest reads back) -- gateable on
  // Gpu::capture_render_, but a decision rather than an optimisation.
  bool submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& f);

  // Completion bands, and the first scanline of each (band_line(bands()) is
  // the height, so the array reads like Renderer3D's bin_y_).
  u32 bands() const { return bands_; }
  // Bands the frame in flight actually reports. A frame with a final pass has
  // one: edge marking reads across band boundaries, so no part of the picture
  // is final until the whole of it is.
  u32 frame_bands() const;
  // The triangle path is drawing (DS_VK_MODE=tri, or the default where the
  // device can): polygons through the hardware rasteriser rather than the
  // compute passes. Same output contract, one band.
  bool tri() const;
  bool tri_ordered() const;   // ... with ordered attachment access (one draw per tail run)
  s32 band_line(u32 b) const;

  // Wait for bands 0..b and make their output readable by the CPU. Idempotent
  // and safe from any thread: the compositor calls it through FrameRef, the
  // emulation thread through sync_all, and whichever gets there first pays.
  void wait_band(u32 b);
  void wait() { wait_band(bands_ - 1); }

  // The finished 3D layer: 256*192 words at scale 1, RGB666 in bits 0-21 and
  // 5-bit alpha in bits 24-28 -- the same record layout render3d.cpp's
  // out_[] carries, because the composite reads it the same way.
  //
  // output() is the frame the last submit drew; output_prev() the one before
  // it, which is already finished and already invalidated. Three buffers,
  // not two, and the third is exactly what the deferred composite needs: at
  // line 0 of display frame N the compositor reads frame N-2's buffer while
  // N-1 is still in flight and N has yet to be submitted.
  const u32* output() const;
  const u32* output_prev() const;
  // At S >= 2 the native plane output() names is built on the CPU, a line at
  // a time as the composite asks for it: the top-left subpixel of each SxS
  // block of the hi-res layer (what downsample.comp did on the GPU). The
  // capture-heavy scenes are the ones with the emulation thread at its
  // limit, and this takes a dispatch and a barrier off the GPU frame they
  // wait for at line 0; the copy itself is 256 strided loads a line. Call
  // after sync_line (the hi-res rows must be finished and invalidated);
  // idempotent, any thread. reduce_all for the whole plane (save states,
  // the A/B). Opt-in (DS_VK_CPU_DOWNSAMPLE=1): on the RG DS Plus it measured
  // a loss -- the gather lands on the reading thread and the dispatch it
  // saves is ~0.15 ms of a 6 ms fence wait (docs/gpu-path-scoping.md).
  void reduce_line(const u32* nat, u32 y);
  // Whether frames with DISP3DCNT anti-aliasing can be drawn here (the
  // triangle path's AA pass, 1x only); else the gate keeps them on the CPU.
  bool aa_supported() const;
  void reduce_all(const u32* nat);

  // GPU time per pass, summed over the frames read back so far, when
  // DS_VK_TIMING=1 and the queue has timestamps. Read after wait(): a
  // frame's stamps are collected when the next frame is submitted.
  enum Pass : u32 { P_SPAN = 0, P_BIN, P_VIS, P_RASTER, P_POST, P_COUNT };
  struct PassTimes { u64 ns[P_COUNT] = {}; u64 frames = 0; };
  const PassTimes& pass_times() const;
  bool timing() const;

  // Bind the decoded texture cache so the shader samples it in place
  // (VK_EXT_external_memory_host). Unused while the cache keeps each entry in
  // its own vector; texel_buffer() copies instead. See vk_raster.cpp.
  bool bind_texture_arena(void* ptr, size_t size);

private:
  Raster() = default;
  struct Impl;
  bool tri_setup(Impl& d, Device& dev, std::string* why);   // the triangle path's resources (vk_raster.cpp)
  std::unique_ptr<Impl> d_;
  bool ready_ = false;
  bool vis_ = false;
  u32  scale_ = 1;
  u32  bands_ = 1;
};

} // namespace ds::gpu::vk
