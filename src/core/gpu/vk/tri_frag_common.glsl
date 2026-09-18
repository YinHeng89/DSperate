// Shared by tri_opaque.frag and tri_tail.frag: the per-fragment shading of
// the triangle path, through ds_shade.glsl's arithmetic.
layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
layout(std430, binding = 3) readonly buffer Texels { uint texels[]; };
layout(std430, binding = 5) readonly buffer Post   { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(location = 0) flat in uint v_poly;
layout(location = 1) in vec3 v_rgb;
layout(location = 2) in vec2 v_st;
layout(location = 3) noperspective in float v_z;
layout(location = 4) in float v_w;
layout(location = 5) in float v_zp;
layout(location = 0) out uint o_col;     // the 3D layer record: RGB666 + alpha5 << 24
layout(location = 1) out uint o_attr;    // the attribute plane the final pass reads
layout(location = 2) out uint o_z;       // the depth plane (DS z, or w in W-buffer mode)
#include "ds_shade.glsl"
struct Frag { uint src; uint alpha; uint polyattr; uint depth; };
Frag shade_fragment(GpuPoly p) {
  Frag o;
  uint blendmode = (p.attr >> 4) & 3u;
  uint polyalpha = (p.attr >> 16) & 0x1Fu;
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
  o.src = shade_pixel(p, blendmode, polyalpha, textured,
                      int(round(v_rgb.r)), int(round(v_rgb.g)), int(round(v_rgb.b)),
                      int(floor(v_st.x + 0.01)), int(floor(v_st.y + 0.01)));
  // Colours round (measured closer to the DS's fixed-point interpolation);
  // texture coordinates TRUNCATE, as the DS's 12.4 >> 4 does -- a quad that
  // stretches one texel column across a hundred pixels (Etrian's menu panels,
  // s 22.0 -> 23.0) otherwise flips to the edge texel columns early. The
  // 0.01 (a hundredth of a 1/16 texel) absorbs float error at exact texel
  // boundaries: the same panels step t by exactly 16.0 a row, and 15.9999
  // floored onto the row above -- the one-row shift in the menu text.
  o.alpha = o.src >> 24;
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  o.polyattr = (p.attr & 0x3F008000u) | (front ? 0u : (1u << 4));
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  o.depth = uint(clamp(round(wbuf ? v_zp : v_z), 0.0, 16777215.0));   // W-depth is perspective-correct on the DS, z linear
  return o;
}
