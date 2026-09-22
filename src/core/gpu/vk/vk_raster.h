// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_layout.h"
#include "core/types.h"

#include <memory>

// GPU 3D raster: replaces Renderer3D's band workers only. Geometry engine
// and 2D compositing stay on the CPU/NEON as before.
//
// output() memory is Access::CpuRead (HOST_CACHED), not HOST_COHERENT, which
// would make the NEON composite read far slower. A frame is ONE submit and
// ONE fence regardless of dispatch count.

namespace ds::gpu::vk {

class Raster {
public:
  // Null when the device cannot run it; the caller keeps the software path.
  // scale: internal resolution 1..4 (triangle path only; otherwise 1).
  static std::unique_ptr<Raster> create(Device& dev, u32 scale = 1, std::string* why = nullptr);
  ~Raster();

  Raster(const Raster&) = delete;
  Raster& operator=(const Raster&) = delete;

  // False if the pipelines did not build; caller falls back to band workers.
  bool ready() const { return ready_; }

  // Internal resolution multiplier; downstream composite must agree.
  u32 scale() const { return scale_; }
  // Hi-res layer (256S x 192S) of the current frame, as a VkBuffer handle
  // for the present stage. Same buffer as output() when S == 1.
  u64 output_hires_handle() const;
  size_t output_hires_bytes() const;
  // Smooth filter's edge plane (0 if the frame wrote none), and whether on.
  u64 output_edge_handle() const;
  bool smooth() const;
  void set_smooth(bool on);   // live; forced off at S >= 2

  // Whether the order-free prefix uses the visibility pass (vis.comp /
  // resolve.comp) instead of the ordered loop. Needs 64-bit buffer atomics;
  // DS_VK_VIS=0 disables (compute mode). Either path draws the same picture.
  bool visibility() const { return vis_; }

  // Caller writes the frame directly into mapped GPU memory (no CPU-side
  // copy). Valid only between frames.
  GpuPoly* poly_buffer();                 // DS_MAX_POLYS entries
  GpuVert* vert_buffer();                 // DS_MAX_VERTS entries
  u32*     texel_buffer(u32* capacity);   // decoded texels; capacity in words
  GpuPost* post_buffer();                 // fog, edge and toon tables
  u32*     shadow_run_buffer();           // DS_MAX_POLYS * DS_SHRUN_LINES: see vk_layout.h
  u32*     rowpoly_buffer();              // DS_MAX_SPAN_ROWS: span row -> polygon index

  // Submit the frame: clear bin counts, bin, then rasterise in BANDS of
  // scanlines, each with its own completion, so sync_line only waits on the
  // band owning the line it reads (matches the software raster's shape).
  // One band per tile row (one in all on the triangle path). A display-capture frame (DISPCAPCNT bit
  // 24) cannot be composited a frame behind, gated on Gpu::capture_render_.
  bool submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& f);

  // Completion bands; band_line(bands()) is the height.
  u32 bands() const { return bands_; }
  // Bands the in-flight frame reports; 1 if it has a final pass (edge
  // marking reads across band boundaries, so nothing is final till all is).
  u32 frame_bands() const;
  // Triangle path drawing (DS_VK_MODE=tri or default where supported):
  // hardware rasteriser instead of compute passes. One band.
  bool tri() const;
  bool tri_ordered() const;   // ... with ordered attachment access (one draw per tail run)
  s32 band_line(u32 b) const;

  // Wait for bands 0..b and make output CPU-readable. Idempotent, any thread.
  void wait_band(u32 b);
  void wait() { wait_band(bands_ - 1); }

  // Finished 3D layer: 256*192 words at scale 1, RGB666 in bits 0-21, 5-bit
  // alpha in bits 24-28.
  //
  // output() is the last frame drawn; output_prev() the one before. A third
  // buffer exists because a deferred composite at display line 0 of frame N
  // reads frame N-2 while N-1 is still in flight and N is not yet submitted.
  const u32* output() const;
  const u32* output_prev() const;
  // Whether DISP3DCNT anti-aliasing can run here (triangle path).
  bool aa_supported() const;

  // GPU time per pass, summed since the last read, when DS_VK_TIMING=1 and
  // the queue has timestamps. Read after wait().
  enum Pass : u32 { P_SPAN = 0, P_BIN, P_VIS, P_RASTER, P_POST, P_COUNT };
  struct PassTimes { u64 ns[P_COUNT] = {}; u64 frames = 0; };
  const PassTimes& pass_times() const;
  bool timing() const;

  // Bind the decoded texture cache for the shader to sample in place
  // (VK_EXT_external_memory_host). Unused while each entry is a vector.
  bool bind_texture_arena(void* ptr, size_t size);

private:
  Raster() = default;
  struct Impl;
  bool tri_setup(Impl& d, Device& dev, std::string* why);   // triangle path's resources
  std::unique_ptr<Impl> d_;
  bool ready_ = false;
  bool vis_ = false;
  u32  scale_ = 1;
  u32  bands_ = 1;
};

} // namespace ds::gpu::vk
