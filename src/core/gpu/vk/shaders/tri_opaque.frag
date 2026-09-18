#version 450
// The order-free prefix: opaque-list polygons. Their alpha is 0 or 31, so
// the fragment either fails the alpha test or replaces the pixel whole; the
// hardware depth test in submission order is the DS's ownership rule.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
void main() {
  GpuPoly p = polys[v_poly];
  Frag f = shade_fragment(p);
  if (f.alpha <= pc.f.alpha_ref) discard;
  o_col = f.src;
  o_attr = f.polyattr;
  o_z = f.depth;
}
