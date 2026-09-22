#version 450
// The ordered tail in the AA pass: tri_tail.frag's rules on the two-deep
// pixel stack. A pass against the top blends into the top and, on an edge
// pixel, into the slot underneath too; a fail against the top of an edge
// pixel may still blend underneath alone. Shadows stay on top.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#define DS_AA_PASS 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
#include "tri_aa_common.glsl"

// Blend, id rule, attribute merge, optional depth write on one slot; false if the equal-id rule skips it.
bool blend_slot(inout uint dcol, inout uint dattr, inout uint dz, GpuPoly p, Frag f) {
  uint attr = (f.polyattr & 0xE0F0u) | ((f.polyattr >> 8) & 0xFF0000u) | (1u << 22) | (dattr & 0xFF001F0Fu);
  bool same;
  if ((p.flags & DS_PF_SHADOW) != 0u)
    same = ((dattr & (1u << 22)) != 0u) ? ((dattr & 0x007F0000u) == (attr & 0x007F0000u)) : ((dattr & 0x3F000000u) == (f.polyattr & 0x3F000000u));
  else
    same = (dattr & 0x007F0000u) == (attr & 0x007F0000u);
  if (same) return false;
  if ((dattr & (1u << 15)) == 0u) attr &= ~(1u << 15);
  dcol = alpha_blend(pc.f.dispcnt, f.src, dcol, f.alpha);
  dattr = attr;
  if ((p.attr & 0x800u) != 0u) dz = f.depth;
  return true;
}

void main() {
  GpuPoly p = polys[v_poly];
  if ((p.flags & DS_PF_SHADOW) != 0u && subpassLoad(in_sh).r != (pc.f.flags >> DS_FF_RUN_SHIFT)) discard;
  int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);
  uint cov;
  uint edge = aa_edge(p, x, y, cov);
  if (!aa_inside) discard;
  Frag f = shade_row(p, aa_row, x);
  if (f.alpha <= pc.f.alpha_ref) discard;
  Stack s = stack_load();
  if (f.alpha == 31u) {
    if (!stack_opaque(s, p, f, edge, cov)) discard;
    stack_store(s);
    return;
  }
  bool pass_top = ds_depth_pass(p, f.depth, s.tz, s.tattr);
  bool under_ok = (s.tattr & 0xFu) != 0u && (p.flags & DS_PF_SHADOW) == 0u;
  bool drawn = false;
  if (pass_top) {
    drawn = blend_slot(s.tcol, s.tattr, s.tz, p, f);
    if (under_ok) drawn = blend_slot(s.ucol, s.uattr, s.uz, p, f) || drawn;
  } else if (under_ok && ds_depth_pass(p, f.depth, s.uz, s.uattr)) {
    drawn = blend_slot(s.ucol, s.uattr, s.uz, p, f);
  }
  if (!drawn) discard;
  stack_store(s);
}
