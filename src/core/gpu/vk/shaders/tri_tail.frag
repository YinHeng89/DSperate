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
layout(input_attachment_index = 2, set = 1, binding = 2) uniform usubpassInput in_z;
layout(input_attachment_index = 3, set = 1, binding = 3) uniform usubpassInput in_sh;
void main() {
  GpuPoly p = polys[v_poly];
  // A shadow polygon draws only inside the volume its run's masks marked
  // (tri_mask.frag): the plane holds that run's id there.
  if ((p.flags & DS_PF_SHADOW) != 0u && subpassLoad(in_sh).r != (pc.f.flags >> DS_FF_RUN_SHIFT)) discard;
  Frag f = shade_fragment(p);
  if (f.alpha <= pc.f.alpha_ref) discard;
  if ((pc.f.flags & DS_FF_TAIL1X) != 0u) {
    // No depth buffer in the native tail pass: the DS test against the depth
    // record, with its rules -- equal passes for a front-facing polygon over
    // an opaque back-facing pixel (mode 1); the equal-depth mode (attribute
    // bit 14) passes within its tolerance (+-0x200 in Z mode, +-0xFF in W).
    uint dz = subpassLoad(in_z).r, da = subpassLoad(in_attr).r;
    bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
    bool pass;
    if ((p.attr & (1u << 14)) != 0u) { uint tol = (pc.f.flags & DS_FF_WBUFFER) != 0u ? 0xFFu : 0x200u; pass = (dz > f.depth ? dz - f.depth : f.depth - dz) <= tol; }
    else pass = (front && (da & 0x00400010u) == 0x00000010u) ? f.depth <= dz : f.depth < dz;
    if (!pass) discard;
    o_touch = 1u;
  }
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
  // The DS depth record moves only when the polygon writes depth (attribute
  // bit 11); the fixed-function depth buffer already obeys that, and this
  // plane must too or the final pass fogs a translucent pixel at ITS depth
  // rather than at the depth of what it was blended over (Golden Sun's
  // mist over fogged terrain: dark, hard-edged).
  o_z = (p.attr & 0x800u) != 0u ? f.depth : subpassLoad(in_z).r;
}
