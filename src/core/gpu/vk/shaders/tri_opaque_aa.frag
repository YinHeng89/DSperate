#version 450
// The order-free prefix in the AA pass: the pixel's span row gives its
// coverage, attributes and depth; the hardware only decides which pixels
// to visit (tri.vert grows the polygon by a pixel so every pixel of the DS
// span is visited, and the rest are cut).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#define DS_AA_PASS 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
#include "tri_aa_common.glsl"
void main() {
  GpuPoly p = polys[v_poly];
  int x = int(gl_FragCoord.x), y = int(gl_FragCoord.y);
  uint cov;
  uint edge = aa_edge(p, x, y, cov);
  if (!aa_inside) discard;
  Frag f = shade_row(p, aa_row, x);
  if (f.alpha <= pc.f.alpha_ref) discard;
  Stack s = stack_load();
  if (!stack_opaque(s, p, f, edge, cov)) discard;
  stack_store(s);
}
