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
layout(std430, binding = 13) readonly buffer Order { uint order[]; };   // the opaque prefix near to far (DS_FF_SORTED)
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat out uint v_poly;
layout(location = 1) out vec3 v_rgb;                 // 9-bit vertex colour, perspective-correct
layout(location = 2) out vec2 v_st;                  // 12.4 texture coordinates, perspective-correct
layout(location = 3) noperspective out float v_z;    // the DS z, linear across the screen (Z-buffer mode)
layout(location = 4) out float v_w;                  // the DS w, perspective-correct (fog / attribution)
layout(location = 5) out float v_zp;                 // the DS depth again, perspective-correct: the W-buffer record
void main() {
  uint pi = (pc.f.flags & DS_FF_SORTED) != 0u ? order[uint(gl_InstanceIndex)] : uint(gl_InstanceIndex);
  GpuPoly p = polys[pi];
  uint tri = uint(gl_VertexIndex) / 3u, k = uint(gl_VertexIndex) % 3u;
  // The opaque prefix is drawn twice, back faces (LESS) then front faces
  // (LESS_OR_EQUAL): the DS lets a front-facing polygon take an opaque
  // back-facing pixel at equal depth (Renderer3D::depth_pass mode 1), and
  // the column where a side face and a front face share an edge went to
  // whichever was drawn first without it.
  uint face = pc.f.flags & (DS_FF_FACE_BACK | DS_FF_FACE_FRONT);
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  bool talpha = (p.flags & DS_PF_TEX_ALPHA) != 0u;
  bool skip_alpha = ((pc.f.flags & DS_FF_ONLY_PLAIN) != 0u && talpha) || ((pc.f.flags & DS_FF_ONLY_ALPHA) != 0u && !talpha);
  if (tri + 2u >= p.nverts || skip_alpha || (face == DS_FF_FACE_BACK && front) || (face == DS_FF_FACE_FRONT && !front)) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); v_poly = 0u; v_rgb = vec3(0); v_st = vec2(0); v_z = 0.0; v_w = 1.0; v_zp = 0.0; return; }
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
  float ox = float(vt.sx) > cx ? 0.501 : 0.5;   // attributes sampled where the DS samples them (the pixel centre is the DS integer position); the right side a hair past the centre so the inclusive last column is covered
  float oy = 0.5;
  // A polygon with no width or no height is a line to the DS -- one column,
  // or one row -- and zero area to a triangle rasteriser. Give it the pixel:
  // the second and third vertices step across, whichever way it winds.
  if (p.xmax == p.xmin) ox = (vi == 1u || vi == 2u) ? 1.001 : 0.0;
  if (p.ybot == p.ytop) oy = (vi == 1u || vi == 2u) ? 1.0 : 0.0;
  float x = (float(vt.sx) + ox) / W * 2.0 - 1.0, y = (float(vt.sy) + oy) / H * 2.0 - 1.0;
  float z = clamp(float(vt.z), 0.0, 16777215.0);
  // Z-buffer: z / w after the divide is z / 2^24, linear in screen space,
  // as the DS interpolates it.
  // W-buffer: vt.z is ALREADY the depth the DS compares -- w normalised per
  // polygon to its own 16-bit range (gpu3d.cpp wshifted), NOT the
  // interpolation w in vt.w, which polygons of a different w size do not
  // share (1 / vt.w put Spirit Tracks' hills in front of its train). The DS
  // interpolates it perspective-correctly, i.e. its reciprocal is linear on
  // the screen: a constant numerator over vt.z gives the hardware exactly
  // that, and the test runs GREATER (nearer = larger). Shadow volumes sit
  // within a few hundred units of the ground they fall on; a linear z here
  // put their depth-fail region in the wrong place.
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  float zc = wbuf ? (1.0 / max(z, 1.0)) * w : (z / 16777215.0) * w;
  gl_Position = vec4(x * w, y * w, zc, w);
  v_poly = pi;
  v_rgb = vec3(float(vt.r), float(vt.g), float(vt.b));
  v_st = vec2(float(vt.s), float(vt.t));
  v_z = z;
  v_zp = z;
  v_w = float(vt.w);
}
