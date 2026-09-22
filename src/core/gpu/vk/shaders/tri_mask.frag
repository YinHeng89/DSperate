#version 450
// Shadow-mask polygon: where its depth test FAILS is inside the volume, and
// the run's shadow polygons test it. No stencil attachment: the mask writes
// the run's id into a colour plane instead (input attachments for depth/attr,
// ordered by rasterization_order_attachment_access or a per-draw barrier).
// Plane clears to 0 once a frame; a new run just uses a new id.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
layout(input_attachment_index = 1, set = 1, binding = 1) uniform usubpassInput in_attr;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform usubpassInput in_z;
layout(location = 3) out uint o_sh;
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
#include "ds_span.glsl"
void main() {
  GpuPoly p = polys[v_poly];
  bool row_z = false; int rz = 0;
  if ((pc.f.flags & DS_FF_SPANCULL) != 0u) {
    // AA pass grows polygons by a pixel: keep the DS span only, take depth from the row.
    int S = int(pc.f.scale);   // rows are native
    int x = int(gl_FragCoord.x) / S, y = int(gl_FragCoord.y) / S;
    int y0 = max(p.ytop, 0);
    if (y < y0 || y > min(p.ybot, 191)) discard;
    GpuRow r = rows_c[p.row_base + uint(y - y0)];
    if ((r.fl & 1u) == 0u || x < max(r.xstart, 0) || x > r.xend) discard;
    Interp ix = row_interp(r); interp_set_x(ix, x); rz = interp_z(ix, r.zl, r.zr); row_z = true;
  }
  uint alpha = (p.attr >> 16) & 0x1Fu;
  if (alpha == 0u) alpha = 31u;   // wireframe: the DS fills a mask polygon regardless
  if (alpha <= pc.f.alpha_ref) discard;
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  uint z = row_z ? uint(clamp(rz, 0, 16777215)) : uint(clamp(round(wbuf ? v_zp : v_z), 0.0, 16777215.0));
  uint dz = subpassLoad(in_z).r;
  uint dattr = subpassLoad(in_attr).r;
  // Mode 1: front-facing over opaque back-facing pixel passes on equal depth; mode 0 doesn't.
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  bool pass = (front && (dattr & 0x00400010u) == 0x00000010u) ? z <= dz : z < dz;
  if (pass) discard;
  o_sh = pc.f.flags >> DS_FF_RUN_SHIFT;
}
