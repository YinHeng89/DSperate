#version 450
// Order-free prefix: opaque-list polygons. Alpha is always 0 or 31, so a
// fragment either fails the alpha test or replaces the pixel whole.
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
  // Bits 0-3: DS edge flags (left/right/top/bottom pixel of a span). Every
  // opaque pixel is a candidate; the final pass's neighbour test decides, so
  // in practice only the outline marks. (AA reads these bits as coverage; the AA gate keeps such frames off this path.)
  uint edge = 0xFu;
  if ((pc.f.flags & DS_FF_ROWS) != 0u) {
    uint cov; bool inside; GpuRow r;
    int S = int(pc.f.scale);
    edge = row_edge(p, int(gl_FragCoord.x) / S, int(gl_FragCoord.y) / S, cov, inside, r);
    if (edge != 0u && (pc.f.dispcnt & 16u) != 0u) {
      edge |= (cov << 8);
      // Smooth filter: is this run's edge X-major (coverage then a vertical
      // fraction), and which half of the pixel the polygon covers.
      if ((edge & 3u) != 0u) {
        bool left = (edge & 1u) != 0u;
        bool xmajor = ((left ? r.lcov : r.rcov) & 0x80000000u) != 0u;
        bool neg = (r.fl & (left ? 2048u : 4096u)) != 0u;
        if (xmajor) edge |= 0x2000u | ((left != neg) ? 0x4000u : 0u);
      }
    }
    if ((pc.f.dispcnt & 16u) == 0u) edge = 0xFu;   // edge marking alone: back to every pixel a candidate
  }
  o_attr = f.polyattr | edge;
  o_z = f.depth;
}
