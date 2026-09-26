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
DS_INCBIN(ds_vk_tri_mask, "tri_mask_frag.spv");
DS_INCBIN(ds_vk_tri_tail_ms, "tri_tail_ms_frag.spv");
DS_INCBIN(ds_vk_tri_opaque_ms, "tri_opaque_ms_frag.spv");
DS_INCBIN(ds_vk_tri_mask_ms, "tri_mask_ms_frag.spv");
DS_INCBIN(ds_vk_tri_resolve, "tri_resolve_frag.spv");
DS_INCBIN(ds_vk_tri_fs, "tri_fs_vert.spv");
DS_INCBIN(ds_vk_lean_vert, "lean_vert.spv");
DS_INCBIN(ds_vk_lean_frag, "lean_frag.spv");
DS_INCBIN(ds_vk_lean_frag_early, "lean_frag_early.spv");
DS_INCBIN(ds_vk_lean_resolve, "lean_resolve_frag.spv");

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

Spirv shader_tri_mask() { return { reinterpret_cast<const u32*>(ds_vk_tri_mask_data), static_cast<size_t>(ds_vk_tri_mask_end - ds_vk_tri_mask_data) }; }
Spirv shader_tri_opaque_ms() { return { reinterpret_cast<const u32*>(ds_vk_tri_opaque_ms_data), static_cast<size_t>(ds_vk_tri_opaque_ms_end - ds_vk_tri_opaque_ms_data) }; }
Spirv shader_tri_tail_ms() { return { reinterpret_cast<const u32*>(ds_vk_tri_tail_ms_data), static_cast<size_t>(ds_vk_tri_tail_ms_end - ds_vk_tri_tail_ms_data) }; }
Spirv shader_tri_mask_ms() { return { reinterpret_cast<const u32*>(ds_vk_tri_mask_ms_data), static_cast<size_t>(ds_vk_tri_mask_ms_end - ds_vk_tri_mask_ms_data) }; }
Spirv shader_tri_resolve() { return { reinterpret_cast<const u32*>(ds_vk_tri_resolve_data), static_cast<size_t>(ds_vk_tri_resolve_end - ds_vk_tri_resolve_data) }; }
Spirv shader_tri_fs() { return { reinterpret_cast<const u32*>(ds_vk_tri_fs_data), static_cast<size_t>(ds_vk_tri_fs_end - ds_vk_tri_fs_data) }; }

Spirv shader_lean_vert() { return { reinterpret_cast<const u32*>(ds_vk_lean_vert_data), static_cast<size_t>(ds_vk_lean_vert_end - ds_vk_lean_vert_data) }; }
Spirv shader_lean_frag() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_data), static_cast<size_t>(ds_vk_lean_frag_end - ds_vk_lean_frag_data) }; }
Spirv shader_lean_frag_early() { return { reinterpret_cast<const u32*>(ds_vk_lean_frag_early_data), static_cast<size_t>(ds_vk_lean_frag_early_end - ds_vk_lean_frag_early_data) }; }
Spirv shader_lean_resolve() { return { reinterpret_cast<const u32*>(ds_vk_lean_resolve_data), static_cast<size_t>(ds_vk_lean_resolve_end - ds_vk_lean_resolve_data) }; }
} // namespace ds::gpu::vk
