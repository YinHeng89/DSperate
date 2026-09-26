// Shared by tri_opaque.frag and tri_tail.frag: per-fragment shading of the
// triangle path, through ds_shade.glsl's arithmetic.
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
// Texel arena as a uniform texel buffer: on Mali a texelFetch goes through
// the texture cache where an SSBO load goes through load/store.
#define DS_TEXEL_BUFFER 1
layout(binding = 3) uniform usamplerBuffer texels_tb;
layout(std430, binding = 5) readonly buffer Post   { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat in uint v_poly;
layout(location = 1) in vec3 v_rgb;
layout(location = 2) in vec2 v_st;
layout(location = 3) noperspective in float v_z;
layout(location = 5) in float v_zp;
// Under MSAA the vertices sit on pixel corners (tri.vert), so a pixel's
// centre is half a pixel past the point the DS evaluates it at (its top-left
// corner): shading reads the varyings there, the same for every sample.
// Where that corner is outside the polygon (the sample nearest it is not
// covered: an edge pixel the DS would not draw) the corner's values are an
// extrapolation that can wrap a texture, so the covered part's are used.
bool msaa() { return (pc.f.flags2 & DS_FF2_MSAA) != 0u; }
#ifdef DS_PER_SAMPLE
bool at_corner() { return true; }   // once per sample, the mask holds only this sample: the DS point throughout
#else
bool at_corner() { return (gl_SampleMaskIn[0] & 1) != 0; }
#endif
vec3 f_rgb() { return !msaa() ? v_rgb : at_corner() ? interpolateAtOffset(v_rgb, vec2(-0.5)) : interpolateAtCentroid(v_rgb); }
vec2 f_st() { return !msaa() ? v_st : at_corner() ? interpolateAtOffset(v_st, vec2(-0.5)) : interpolateAtCentroid(v_st); }
uint f_depth() {
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  float z;
  if (!msaa()) z = wbuf ? v_zp : v_z;
  else if (at_corner()) z = wbuf ? interpolateAtOffset(v_zp, vec2(-0.5)) : interpolateAtOffset(v_z, vec2(-0.5));
  else z = wbuf ? interpolateAtCentroid(v_zp) : interpolateAtCentroid(v_z);
  return uint(clamp(round(z), 0.0, 16777215.0));   // W-depth is perspective-correct on the DS, z linear
}
layout(location = 0) out uint o_col;     // the 3D layer record: RGB666 + alpha5 << 24
layout(location = 1) out uint o_attr;    // the attribute plane the final pass reads
layout(location = 2) out uint o_z;       // the depth plane (DS z, or w in W-buffer mode)
layout(std430, binding = 9) readonly buffer RowsC { GpuRow rows_c[]; };
// Edge flags/coverage from the polygon's span-table row; `inside` says
// whether the DS span reaches the pixel. At S >= 2 caller passes native (x/S, y/S).
uint row_edge(GpuPoly p, int x, int y, out uint cov, out bool inside, out GpuRow row) {
  cov = 31u; inside = false;
  int y0 = max(p.ytop, 0);
  int ylast = min(p.ybot, 191);
  if (y < y0 || y > ylast) return 0u;
  GpuRow r = rows_c[p.row_base + uint(y - y0)];
  row = r;
  if ((r.fl & 1u) == 0u) return 0u;
  uint yedge = (r.fl >> 5) & 0xFu;
  int xa = max(r.xstart, 0);
  if (x < xa || x > r.xend) return 0u;
  inside = true;
  if (x < r.lim0) {
    int c = int(r.lcov);
    if ((c & int(0x80000000u)) != 0) { int xcov = (c >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; xcov += (x - xa) * (c & 0x3FF); cov = uint(min(xcov >> 5, 31)); }
    else cov = uint(c & 0x1F);
    return yedge | 1u;
  }
  if (x >= r.lim1) {
    int c = int(r.rcov);
    if ((c & int(0x80000000u)) != 0) { int xcov = (c >> 12) & 0x3FF; if (xcov == 0x3FF) xcov = 0; xcov += (x - r.lim1) * (c & 0x3FF); cov = uint(max(31 - (xcov >> 5), 0)); }
    else cov = uint(c & 0x1F);
    return yedge | 2u;
  }
  return yedge;
}
#include "ds_shade.glsl"
struct Frag { uint src; uint alpha; uint polyattr; uint depth; };
// The fragment at texture coordinate `st` (colour at the DS point), depth left 0.
Frag shade_fragment_st(GpuPoly p, vec3 rgb, vec2 st) {
  Frag o;
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  o.src = shade_pixel(p, blendmode, polyalpha, textured,
                      int(round(rgb.r)), int(round(rgb.g)), int(round(rgb.b)),
                      int(floor(st.x + 0.01)), int(floor(st.y + 0.01)));
  // Texcoords TRUNCATE as DS 12.4 >> 4 does; +0.01 absorbs float error so an
  // exact-boundary coordinate doesn't floor down a row/column early.
  o.alpha = o.src >> 24;
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  o.polyattr = (p.attr & 0x3F008000u) | (front ? 0u : (1u << 4));
  o.depth = 0u;
  return o;
}
Frag shade_fragment(GpuPoly p) {
  Frag o = shade_fragment_st(p, f_rgb(), f_st());
  o.depth = f_depth();
  return o;
}
// MSAA, a texture with transparent texels: its texture coordinate at a
// sample, with the samples placed around the DS point as they sit around the
// pixel's centre (q: the sample's position in the pixel; gx/gy: the
// coordinate's screen derivatives, taken in uniform control flow).
vec2 st_at(vec2 st0, vec2 gx, vec2 gy, vec2 q) { q -= 0.5; return st0 + gx * q.x + gy * q.y; }
