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
  if ((pc.f.flags & DS_FF_IDCOLOUR) != 0u) o_col = 0x1F000000u | (v_poly & 63u) | (((v_poly >> 6) & 63u) << 8) | (((v_poly >> 12) & 63u) << 16);
  // Bits 0-3 are the DS's edge flags (left, right, top, bottom pixel of a
  // span); the final pass marks only flagged pixels whose neighbour has a
  // different polygon id and lies behind. The hardware rasteriser does not
  // say which pixels are a polygon's outline, so every opaque pixel is a
  // candidate and the neighbour test alone decides -- an interior pixel has
  // no differently-owned neighbour in front of nothing, so in practice only
  // the outline marks. melonDS's GL renderer makes the same approximation.
  // (Anti-aliasing reads these bits as coverage; the AA gate keeps such
  // frames off this path.)
  uint edge = 0xFu;
  if ((pc.f.flags & DS_FF_ROWS) != 0u) {
    // The span table is here (edge marking or anti-aliasing on): the DS's
    // own edge flags, and its coverage for the fast anti-aliasing.
    uint cov; bool inside; GpuRow r;
    int S = int(pc.f.scale);
    edge = row_edge(p, int(gl_FragCoord.x) / S, int(gl_FragCoord.y) / S, cov, inside, r);
    if (edge != 0u && (pc.f.dispcnt & 16u) != 0u) edge |= (cov << 8);
  }
  o_attr = f.polyattr | edge;
  o_z = f.depth;
}
