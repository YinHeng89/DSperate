#version 450
// The triangle path: polygons as fans through the hardware rasteriser. One
// instance per polygon, eight triangles at most; integer DS vertices sit at
// pixel centres. Coverage is the GPU's, not the DS span rule (the inexact path, by decision).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 0) readonly buffer Polys { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;
layout(location = 1) out vec3 v_rgb;                 // 9-bit vertex colour, perspective-correct
layout(location = 2) out vec2 v_st;                  // 12.4 texture coordinates, perspective-correct
layout(location = 3) noperspective out float v_z;    // DS z, linear across the screen (Z-buffer mode)
layout(location = 5) out float v_zp;                 // DS depth again, perspective-correct: the W-buffer record
void main() {
  uint pi = uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  // Opaque prefix drawn twice: back faces (LESS) then front (LESS_OR_EQUAL),
  // since the DS lets front-facing take an opaque back-facing pixel on a tie.
  uint face = pc.f.flags & (DS_FF_FACE_BACK | DS_FF_FACE_FRONT);
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  if (tri + 2u >= p.nverts || (face == DS_FF_FACE_BACK && front) || (face == DS_FF_FACE_FRONT && !front)) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_zp = 0.0; return; }
  uint vi = k == 0u ? 0u : tri + k;
  GpuVert vt = verts[p.first_vert + vi];
  float W = 256.0 * float(pc.f.scale), H = 192.0 * float(pc.f.scale);
  float w = max(float(vt.w), 1.0);
  // DS span covers [xstart, xend] INCLUSIVE, rows [ytop, ybot) exclusive. A
  // left vertex sits on its pixel's left edge; a right one pushes out a
  // pixel so xend's centre is inside too. Rows need no push.
  float cx = 0.5 * float(p.xmin + p.xmax);
  float sxf = float(vt.sx), syf = float(vt.sy);
  float ox = sxf > cx ? 0.501 : 0.5;   // right side a hair past centre so the inclusive last column is covered
  float oy = 0.5;
  // A zero-width/height polygon is a line to the DS but zero area to a
  // triangle rasteriser: give it the pixel via the second/third vertex.
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.001 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (sxf + ox) / W * 2.0 - 1.0, y = (syf + oy) / H * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z/w after the divide is z/2^24, linear in screen space.
  // W-buffer: vt.z is ALREADY the depth the DS compares (normalised per
  // polygon, not vt.w's interpolation w); a constant numerator over vt.z
  // reproduces the DS's perspective-correct interpolation, test runs GREATER.
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  float zc = wbuf ? (1.0 / max(z, 1.0)) * w : (z / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi;
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_z = z;
  v_zp = z;
}
