#version 450
// The depth prepass of the opaque prefix (DS_VK_TRI_PREPASS): depth only, for
// the polygons whose texture has no transparent texel (DS_FF_ONLY_PLAIN in
// tri.vert keeps the others out), so the shaded pass that follows at EQUAL
// depth pays the fragment stage once per pixel. The alpha-tested polygons
// are drawn after that with the normal test and their own overdraw.
// A polygon alpha at or under the alpha test's reference fails everywhere
// (opaque-list polygons are alpha 31; the reference can be 31).
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
void main() {
  GpuPoly p = polys[v_poly];
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  if (polyalpha == 0u) polyalpha = 31u;   // wireframe draws as opaque here
  if (polyalpha <= pc.f.alpha_ref) discard;
}
