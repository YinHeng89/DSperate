#version 450
// The triangle path (docs/gpu-path-scoping.md P3): polygons as fans through
// the hardware rasteriser. One instance per polygon, eight triangles at
// most; integer DS vertices sit at pixel centres. Coverage is the GPU's,
// not the DS span rule -- the inexact GPU path, by decision.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;
layout(location = 1) out vec3 v_rgb;                 // 9-bit vertex colour, perspective-correct
layout(location = 2) out vec2 v_st;                  // 12.4 texture coordinates, perspective-correct
layout(location = 3) noperspective out float v_z;    // the DS z, linear across the screen (Z-buffer mode)
layout(location = 4) out float v_w;                  // the DS w, perspective-correct (W-buffer mode)
void main() {
  GpuPoly p = polys[uint(gl_InstanceIndex)];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  if (tri + 2u >= p.nverts) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_w = 1.0; return; }
  uint vi = k == 0u ? 0u : tri + k;
  GpuVert vt = verts[p.first_vert + vi];
  float W = 256.0 * float(pc.f.scale), H = 192.0 * float(pc.f.scale);
  float w = max(float(vt.w), 1.0);
  // The DS fill: a span covers [xstart, xend] INCLUSIVE and rows [ytop, ybot)
  // exclusive. A vertex on the polygon's left sits on its pixel's left edge
  // (the centre x+0.5 is inside), one on the right is pushed a pixel out so
  // pixel xend's centre is inside too; rows need no push. Without this the
  // rightmost column of every polygon was missing.
  float cx = 0.5 * float(p.xmin + p.xmax);
  float ox = float(vt.sx) > cx ? 1.0 : 0.0;   // 0 on the left, 1 on the right: the edge lands on the boundary of the pixel it names
  float oy = 0.5;
  // A polygon with no width or no height is a line to the DS -- one column,
  // or one row -- and zero area to a triangle rasteriser. Give it the pixel:
  // the second and third vertices step across, whichever way it winds.
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (float(vt.sx) + ox + 0.001) / W * 2.0 - 1.0, y = (float(vt.sy) + oy) / H * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z / w after the divide is z / 2^24, linear in screen space.
  // W-buffer: a constant numerator gives 1 / w, monotonic in w, so the
  // depth test runs GREATER against it (nearer = larger).
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  float zc = wbuf ? 1.0 : (z / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = uint(gl_InstanceIndex);
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_z = z;
  v_w = float(vt.w);
}
