// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The GPU present stage (docs/gpu-path-scoping.md, P1). The scanout tier's
// CMA buffers are imported into Vulkan as LINEAR images (libmali cannot
// export, but imports, and the display sees the GPU's writes with no sync
// ioctl -- P0.1), the core writes its two 256x192 frames as on the plain
// framebuffer path, and one compute dispatch per frame lays them out,
// scales, rotates and blends the inset straight into the buffer the tier
// then presents unchanged. What it removes is the CPU scanline scaler:
// on the RG DS Plus two 1024x768 panels are 6 MB of writes a frame, split
// between the emulation thread and the LineWorker, and the worker's share
// was measured on the critical path through the line-0 join (P1a).
//
// Frames are pipelined one deep: present() submits frame N and returns;
// the wait for its fence, and the tier's end_frame(), happen at the start
// of frame N+1 (or in flush()). So the emulation thread never waits for
// the GPU unless it is more than a frame behind, at the price of the
// tier's own buffer queue being one deeper -- and a stale frame is never
// shown, because the buffer is committed only after its fence.
#pragma once

#include "core/types.h"
#include "frontend/sdl/scanout.h"

#include <SDL.h>
#include <memory>
#include <string>

namespace ds::sdl {

class GpuPresent {
public:
  // Imports every buffer of `out`; null (with a reason) when there is no
  // Vulkan, no dma-buf import, or the tier has no dma-buf.
  static std::unique_ptr<GpuPresent> open(ScanoutOut& out, std::string* why);
  ~GpuPresent();
  GpuPresent(const GpuPresent&) = delete;
  GpuPresent& operator=(const GpuPresent&) = delete;

  // After the tier reopened at a new size: drop and re-import its buffers.
  bool reimport(ScanoutOut& out);

  struct View { int screen; SDL_Rect rect; bool shown; bool blends; };
  // One frame: take a buffer from the tier, upload fb[0..1], dispatch into
  // that buffer, submit. `lw` x `lh` is the logical (unrotated) frame the
  // rects are laid out in; `rot` maps it onto the presented buffer. False
  // when the tier had no buffer or the submit failed (nothing was shown).
  // `hires`/`hires_bytes`/`scale`: the frame's hi-res 3D layer (an opaque
  // VkBuffer from the core's raster, Gpu::frame_hires) -- when non-zero and
  // the planes are registered, screen 0 is composited here at S x from the
  // exported planes and that layer (shaders/composite.comp) instead of
  // taken from fb[0].
  bool present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha,
               u64 hires = 0, size_t hires_bytes = 0, u32 scale = 1);
  // The planes the core's 2D engine writes for the composite (Gpu::LayerExport
  // takes exactly these); one set for the process, allocated by the first
  // stage opened. Null pointers when there is no stage.
  struct LayerPtrs { u32* top = nullptr; u32* second = nullptr; u32* meta = nullptr; u32* win = nullptr; u32* line = nullptr; u32* mbright = nullptr; };
  static LayerPtrs layer_ptrs();
  // Wait for the frame in flight and hand its buffer to the tier. Called by
  // present() for the previous frame; call it directly before a pause.
  void flush(ScanoutOut& out);

  const std::string& device_name() const;

private:
  GpuPresent() = default;
  struct Impl;
  std::unique_ptr<Impl> d_;
};

} // namespace ds::sdl
