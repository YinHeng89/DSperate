#version 450
// The lean path's resolve: the four samples of a pixel into the 3D layer
// record the compositor reads (RGB666 in bits 0-21, 5-bit alpha in 24-28),
// with the DS's fog applied first, per sample by the sample's depth (the DS
// fogs the finished pixel once, after blending; the polygon's fog bit rides
// in bit 0 of the sample's alpha byte). Colour is the mean of the drawn
// samples only, so an edge over an undrawn rear plane is not tinted by the
// clear colour; alpha is the coverage-weighted mean, so such an edge pixel
// carries its coverage for the compositor.
#extension GL_GOOGLE_include_directive : require
#define DS_GLSL 1
#include "vk_layout.h"
layout(std430, binding = 3) readonly buffer Post { GpuPost ps; };
layout(push_constant) uniform PC { GpuFrame f; } pc;
layout(input_attachment_index = 0, set = 1, binding = 0) uniform subpassInputMS ms_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform subpassInputMS ms_depth;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform subpassInputMS ms_fog;
layout(location = 0) out uint o_word;
uint decode_alpha(uint ab) { return ab < 4u ? 0u : min(31u, (ab + 4u) / 8u - 1u); }   // (a + 1) * 8
uint c15_to_18(uint c, uint shift) { uint v = ((shift == 0u ? (c << 1) : (c >> shift)) & 0x3Eu); return v != 0u ? v + 1u : v; }
uint fog_density(uint z) {
  uint id, frac;
  if (z < ps.fog_offset) { id = 0u; frac = 0u; }
  else {
    z -= ps.fog_offset;
    z = (z >> 2) << ps.fog_shift;
    id = z >> 17;
    if (id >= 32u) { id = 32u; frac = 0u; } else frac = z & 0x1FFFFu;
  }
  uint d = ((ps.density[id] * (0x20000u - frac)) + (ps.density[id + 1u] * frac)) >> 17;
  return d >= 127u ? 128u : d;
}
void main() {
  bool wbuf = (pc.f.flags & DS_FF_WBUFFER) != 0u;
  bool fog_on = (pc.f.dispcnt & 128u) != 0u, fog_colour = (pc.f.dispcnt & 64u) == 0u;
  uint fc = ps.fog_color;
  uint fr = c15_to_18(fc, 0u), fg = c15_to_18(fc, 4u), fb = c15_to_18(fc, 9u), fa = (fc >> 16) & 0x1Fu;
  uvec3 acc = uvec3(0u);
  uint asum = 0u, n = 0u;
  for (int s = 0; s < 4; ++s) {
    vec4 c = subpassLoad(ms_col, s);
    uint ab = uint(round(c.a * 255.0));
    if (ab == 0u) continue;
    uvec3 rgb = uvec3(round(c.rgb * 255.0));
    uint a5 = decode_alpha(ab);
    if (fog_on && subpassLoad(ms_fog, s).r > 0.5) {
      float dz = subpassLoad(ms_depth, s).r;
      uint z = wbuf ? uint(clamp(1.0 / max(dz, 5.96e-8), 0.0, 16777215.0)) : uint(clamp(dz * 16777215.0, 0.0, 16777215.0));
      uint d = fog_density(z);
      if (fog_colour) rgb = (uvec3(fr, fg, fb) * d + rgb * (128u - d)) >> 7;
      a5 = (fa * d + a5 * (128u - d)) >> 7;
    }
    acc += rgb; asum += a5; ++n;
  }
  if (n == 0u) { o_word = 0u; return; }
  uvec3 m = (acc + n / 2u) / n;
  uint r = min(m.r, 63u), g = min(m.g, 63u), b = min(m.b, 63u);
  uint a5 = min((asum + 2u) / 4u, 31u);
  o_word = r | (g << 8) | (b << 16) | (a5 << 24);
}
