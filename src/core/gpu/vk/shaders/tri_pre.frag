#version 450
// Depth prepass of the opaque prefix (DS_VK_TRI_PREPASS): depth only, for
// polygons with no transparent texel (DS_FF_ONLY_PLAIN keeps others out), so
// the shaded pass that follows at EQUAL depth pays the fragment stage once
// per pixel. Alpha-tested polygons draw after with the normal test.
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
