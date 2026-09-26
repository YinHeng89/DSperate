// Order-free prefix: opaque-list polygons. Alpha is always 0 or 31, so a
// fragment either fails the alpha test or replaces the pixel whole.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
void main() {
  GpuPoly p = polys[v_poly];
#ifdef DS_MSAA
  vec2 gx = dFdx(v_st), gy = dFdy(v_st);
#endif
  Frag f = shade_fragment(p);
#ifdef DS_MSAA
  if ((p.flags & DS_PF_TEX_ALPHA) != 0u) {
    // Alpha to coverage: the alpha test at each sample's own texel, so a
    // cut-out's outline is resolved four times finer than the pixel. The
    // colour is the DS point's, or the first passing sample's.
    const vec2 q[4] = vec2[4](vec2(0.375, 0.125), vec2(0.875, 0.375), vec2(0.125, 0.625), vec2(0.625, 0.875));
    vec2 st0 = f_st();
    vec3 rgb = f_rgb();
    bool have = f.alpha > pc.f.alpha_ref;
    int mask = 0;
    for (int k = 0; k < 4; ++k) {
      Frag g = shade_fragment_st(p, rgb, st_at(st0, gx, gy, q[k]));
      if (g.alpha <= pc.f.alpha_ref) continue;
      mask |= 1 << k;
      if (!have) { f.src = g.src; f.alpha = g.alpha; have = true; }
    }
    mask &= gl_SampleMaskIn[0];
    if (mask == 0) discard;
    gl_SampleMask[0] = mask;
  } else
#endif
  if (f.alpha <= pc.f.alpha_ref) discard;
  o_col = f.src;
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
