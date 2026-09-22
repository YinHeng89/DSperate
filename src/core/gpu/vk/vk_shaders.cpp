// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_shaders.h"

// Compute shaders linked in as SPIR-V via .incbin; no shader compiler needed to build.

#define DS_INCBIN(sym, file)                                     \
  __asm__(".section .rodata\n"                                   \
          ".balign 4\n"                                          \
          ".globl " #sym "_data\n"                               \
          #sym "_data:\n"                                        \
          ".incbin \"" DSPERATE_VK_SHADER_DIR "/" file "\"\n"    \
          ".globl " #sym "_end\n"                                \
          #sym "_end:\n"                                         \
          ".previous\n");                                        \
  extern "C" const unsigned char sym##_data[], sym##_end[]

DS_INCBIN(ds_vk_bin, "bin.spv");
DS_INCBIN(ds_vk_raster, "raster.spv");
DS_INCBIN(ds_vk_post, "post.spv");
DS_INCBIN(ds_vk_span, "span.spv");
DS_INCBIN(ds_vk_vis, "vis.spv");
DS_INCBIN(ds_vk_raster_vis, "raster_vis.spv");
DS_INCBIN(ds_vk_tri_vert, "tri_vert.spv");
DS_INCBIN(ds_vk_tri_opaque, "tri_opaque_frag.spv");
DS_INCBIN(ds_vk_tri_tail, "tri_tail_frag.spv");
DS_INCBIN(ds_vk_downsample, "downsample.spv");
DS_INCBIN(ds_vk_tri_flat, "tri_flat_frag.spv");
DS_INCBIN(ds_vk_tri_mask, "tri_mask_frag.spv");
DS_INCBIN(ds_vk_tri_pre, "tri_pre_frag.spv");
DS_INCBIN(ds_vk_expand, "expand.spv");
DS_INCBIN(ds_vk_tri_opaque_aa, "tri_opaque_aa_frag.spv");
DS_INCBIN(ds_vk_tri_tail_aa, "tri_tail_aa_frag.spv");

namespace ds::gpu::vk {

Spirv shader_bin() {
  return { reinterpret_cast<const u32*>(ds_vk_bin_data),
           static_cast<size_t>(ds_vk_bin_end - ds_vk_bin_data) };
}

Spirv shader_raster() {
  return { reinterpret_cast<const u32*>(ds_vk_raster_data),
           static_cast<size_t>(ds_vk_raster_end - ds_vk_raster_data) };
}

Spirv shader_span() {
  return { reinterpret_cast<const u32*>(ds_vk_span_data),
           static_cast<size_t>(ds_vk_span_end - ds_vk_span_data) };
}

Spirv shader_post() {
  return { reinterpret_cast<const u32*>(ds_vk_post_data),
           static_cast<size_t>(ds_vk_post_end - ds_vk_post_data) };
}

Spirv shader_vis() {
  return { reinterpret_cast<const u32*>(ds_vk_vis_data),
           static_cast<size_t>(ds_vk_vis_end - ds_vk_vis_data) };
}

Spirv shader_raster_vis() {
  return { reinterpret_cast<const u32*>(ds_vk_raster_vis_data),
           static_cast<size_t>(ds_vk_raster_vis_end - ds_vk_raster_vis_data) };
}

Spirv shader_tri_vert()   { return { reinterpret_cast<const u32*>(ds_vk_tri_vert_data),   static_cast<size_t>(ds_vk_tri_vert_end - ds_vk_tri_vert_data) }; }
Spirv shader_tri_opaque() { return { reinterpret_cast<const u32*>(ds_vk_tri_opaque_data), static_cast<size_t>(ds_vk_tri_opaque_end - ds_vk_tri_opaque_data) }; }
Spirv shader_tri_tail()   { return { reinterpret_cast<const u32*>(ds_vk_tri_tail_data),   static_cast<size_t>(ds_vk_tri_tail_end - ds_vk_tri_tail_data) }; }

Spirv shader_downsample() { return { reinterpret_cast<const u32*>(ds_vk_downsample_data), static_cast<size_t>(ds_vk_downsample_end - ds_vk_downsample_data) }; }

Spirv shader_tri_opaque_aa() { return { reinterpret_cast<const u32*>(ds_vk_tri_opaque_aa_data), static_cast<size_t>(ds_vk_tri_opaque_aa_end - ds_vk_tri_opaque_aa_data) }; }
Spirv shader_tri_tail_aa() { return { reinterpret_cast<const u32*>(ds_vk_tri_tail_aa_data), static_cast<size_t>(ds_vk_tri_tail_aa_end - ds_vk_tri_tail_aa_data) }; }
Spirv shader_expand() { return { reinterpret_cast<const u32*>(ds_vk_expand_data), static_cast<size_t>(ds_vk_expand_end - ds_vk_expand_data) }; }
Spirv shader_tri_pre() { return { reinterpret_cast<const u32*>(ds_vk_tri_pre_data), static_cast<size_t>(ds_vk_tri_pre_end - ds_vk_tri_pre_data) }; }
Spirv shader_tri_mask() { return { reinterpret_cast<const u32*>(ds_vk_tri_mask_data), static_cast<size_t>(ds_vk_tri_mask_end - ds_vk_tri_mask_data) }; }
Spirv shader_tri_flat() { return { reinterpret_cast<const u32*>(ds_vk_tri_flat_data), static_cast<size_t>(ds_vk_tri_flat_end - ds_vk_tri_flat_data) }; }

} // namespace ds::gpu::vk
