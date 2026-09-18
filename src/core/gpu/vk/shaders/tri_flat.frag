#version 450
// Attribution only (DS_VK_TRI_FLAT=1): the opaque draw with a constant
// colour and no texel, to price the real fragment stage against it.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
#include "tri_frag_common.glsl"
void main() {
  bool front = (polys[v_poly].flags & DS_PF_FRONTFACING) != 0u;
  o_col = 0x1F00003Fu | (uint(v_rgb.r) << 8);
  o_attr = (polys[v_poly].attr & 0x3F008000u) | (front ? 0u : (1u << 4));
  o_z = uint(clamp(v_z, 0.0, 16777215.0));
}
