#version 450
// The ordered tail: translucent polygons, blended in primitive order against
// the colour attachment (read as input attachments; ordered per pixel by
// VK_EXT_rasterization_order_attachment_access, else the host draws it one
// polygon per barrier). Depth test/write are fixed function; equal
// translucent ids don't blend.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform usubpassInput in_attr;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform usubpassInput in_z;
layout(input_attachment_index = 3, set = 1, binding = 3) uniform usubpassInput in_sh;
void main() {
  GpuPoly p = polys[v_poly];
  // Shadow polygon draws only inside the volume its run's masks marked.
  if ((p.flags & DS_PF_SHADOW) != 0u && subpassLoad(in_sh).r != (pc.f.flags >> DS_FF_RUN_SHIFT)) discard;
  Frag f = shade_fragment(p);
  if (f.alpha <= pc.f.alpha_ref) discard;
  if (f.alpha == 31u) { o_col = f.src; o_attr = f.polyattr; o_z = f.depth; return; }
  uint dattr = subpassLoad(in_attr).r;
  uint dcol = subpassLoad(in_col).r;
  uint attr = (f.polyattr & 0xE0F0u) | ((f.polyattr >> 8) & 0xFF0000u) | (1u << 22) | (dattr & 0xFF007F0Fu);   // opaque pixel's edge record stays (bits 0-3, 8-14)
  // Equal ids don't blend. Shadow compares against whichever id the
  // destination carries (translucent if any, else opaque).
  bool same;
  if ((p.flags & DS_PF_SHADOW) != 0u)
    same = ((dattr & (1u << 22)) != 0u) ? ((dattr & 0x007F0000u) == (attr & 0x007F0000u)) : ((dattr & 0x3F000000u) == (f.polyattr & 0x3F000000u));
  else
    same = (dattr & 0x007F0000u) == (attr & 0x007F0000u);
  if (same) discard;
  if ((dattr & (1u << 15)) == 0u) attr &= ~(1u << 15);
  o_col = alpha_blend(pc.f.dispcnt, f.src, dcol, f.alpha);
  o_attr = attr;
  // Moves only when the polygon writes depth (attr bit 11), matching the
  // fixed-function depth buffer, or the final pass would fog at the wrong depth.
  o_z = (p.attr & 0x800u) != 0u ? f.depth : subpassLoad(in_z).r;
}
