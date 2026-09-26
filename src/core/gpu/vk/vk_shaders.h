// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_SHADERS_H
#define DS_VK_SHADERS_H
#include "core/types.h"
#include <cstddef>

// GPU raster's SPIR-V, linked in from the tracked blobs beside the .comp sources.

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
Spirv shader_tri_vert();     // triangle path: polygons as fans through the hardware rasteriser
Spirv shader_tri_opaque();   // ... its opaque-prefix fragment stage
Spirv shader_tri_tail();     // ... and the ordered translucent tail
Spirv shader_downsample();   // native plane from the hi-res layer (S >= 2)
Spirv shader_tri_mask();     // shadow-mask draw: run id into the shadow plane where the depth test fails
Spirv shader_tri_opaque_ms();   // 4x MSAA: the opaque prefix with alpha to coverage
Spirv shader_tri_tail_ms();  // ... the tail and the mask once per sample
Spirv shader_tri_mask_ms();
Spirv shader_tri_resolve();  // ... and the resolve subpass (four samples into the 1x planes)
Spirv shader_tri_fs();       // its fullscreen triangle
Spirv shader_lean_vert();    // lean path (vk_lean.cpp): fans at 1x, MSAA vertex placement
Spirv shader_lean_frag();    // ... its one fragment stage
Spirv shader_lean_frag_early();   // ... without the alpha test (early depth test) for polygons that cannot fail it
Spirv shader_lean_resolve(); // ... and the resolve into the 3D layer record

} // namespace ds::gpu::vk

#endif // DS_VK_SHADERS_H
