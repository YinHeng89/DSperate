// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_SHADERS_H
#define DS_VK_SHADERS_H
#include "core/types.h"
#include <cstddef>

// The GPU raster's SPIR-V, linked in from the tracked blobs beside the .comp
// sources (see vk_shaders.cpp and tools/gen_shaders.sh).

namespace ds::gpu::vk {

struct Spirv {
  const u32* code = nullptr;
  size_t     bytes = 0;      // vkCreateShaderModule wants a byte count
};

Spirv shader_bin();      // binning pass: polygons -> per-tile lists
Spirv shader_span();     // span pass: the Y stage, once per (polygon, scanline)
Spirv shader_raster();   // raster pass: one workgroup per tile
Spirv shader_post();     // final pass: edge marking then fog
Spirv shader_vis();      // visibility pass: the order-free prefix, one fragment at a time
Spirv shader_raster_vis();   // raster pass seeded from the visibility pass (needs shaderInt64)
Spirv shader_tri_vert();     // the triangle path: polygons as fans through the hardware rasteriser
Spirv shader_tri_opaque();   // ... its opaque-prefix fragment stage
Spirv shader_tri_tail();     // ... and the ordered translucent tail

} // namespace ds::gpu::vk

#endif // DS_VK_SHADERS_H
