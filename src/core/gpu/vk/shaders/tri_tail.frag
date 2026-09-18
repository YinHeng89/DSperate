#version 450
// The ordered tail: translucent polygons, blended in primitive order against
// the colour attachment they write -- read here as input attachments, which
// VK_EXT_rasterization_order_attachment_access makes ordered per pixel. (On a
// driver without it the host draws the tail one polygon per barrier.) The
// depth test and the per-polygon depth write are fixed function; equal
// translucent ids do not blend (Renderer3D::plot_translucent).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform usubpassInput in_attr;
void main() {
  GpuPoly p = polys[v_poly];
  Frag f = shade_fragment(p);
  if (f.alpha <= pc.f.alpha_ref) discard;
  if (f.alpha == 31u) { o_col = f.src; o_attr = f.polyattr; o_z = f.depth; return; }
  uint dattr = subpassLoad(in_attr).r;
  uint dcol = subpassLoad(in_col).r;
  uint attr = (f.polyattr & 0xE0F0u) | ((f.polyattr >> 8) & 0xFF0000u) | (1u << 22) | (dattr & 0xFF001F0Fu);
  // Equal ids do not blend. A shadow compares against whichever id the
  // destination carries -- translucent if it has one, otherwise the opaque
  // one -- which is what stops a shadow darkening the polygon that cast it.
  bool same;
  if ((p.flags & DS_PF_SHADOW) != 0u)
    same = ((dattr & (1u << 22)) != 0u) ? ((dattr & 0x007F0000u) == (attr & 0x007F0000u)) : ((dattr & 0x3F000000u) == (f.polyattr & 0x3F000000u));
  else
    same = (dattr & 0x007F0000u) == (attr & 0x007F0000u);
  if (same) discard;
  if ((dattr & (1u << 15)) == 0u) attr &= ~(1u << 15);
  o_col = alpha_blend(pc.f.dispcnt, f.src, dcol, f.alpha);
  o_attr = attr;
  o_z = f.depth;
}
