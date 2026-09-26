// Shadow-mask polygon: where its depth test FAILS is inside the volume, and
// the run's shadow polygons test it. No stencil attachment: the mask writes
// the run's id into a colour plane instead (input attachments for depth/attr,
// ordered by rasterization_order_attachment_access or a per-draw barrier).
// Plane clears to 0 once a frame; a new run just uses a new id.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
#include "tri_inputs.glsl"
layout(input_attachment_index = 1, set = 1, binding = 1) uniform DS_INPUT in_attr;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform DS_INPUT in_z;
layout(location = 3) out uint o_sh;
void main() {
  GpuPoly p = polys[v_poly];
  uint alpha = (p.attr >> 16) & 0x1Fu;
  if (alpha == 0u) alpha = 31u;   // wireframe: the DS fills a mask polygon regardless
  if (alpha <= pc.f.alpha_ref) discard;
  uint z = f_depth();
  uint dz = DS_LOAD(in_z).r;
  uint dattr = DS_LOAD(in_attr).r;
  // Mode 1: front-facing over opaque back-facing pixel passes on equal depth; mode 0 doesn't.
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  bool pass = (front && (dattr & 0x00400010u) == 0x00000010u) ? z <= dz : z < dz;
  if (pass) discard;
  o_sh = pc.f.flags >> DS_FF_RUN_SHIFT;
}
