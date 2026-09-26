#version 450
// 4x MSAA resolve, on the tile: the four samples of each pixel into the 1x
// planes the final pass and the compositor read. Colour is averaged
// weighted by each sample's 5-bit alpha (a sample with no polygon, alpha 0,
// adds nothing but lowers the pixel's alpha), so a partly covered edge
// pixel blends into whatever is under the 3D layer. Attribute and depth
// are sample 0's: edge marking and fog see one polygon per pixel.
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInputMS in_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform usubpassInputMS in_attr;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform usubpassInputMS in_z;
layout(location = 0) out uint o_col;
layout(location = 1) out uint o_attr;
layout(location = 2) out uint o_z;
void main() {
  uvec3 sum = uvec3(0u);
  uint asum = 0u;
  uint c0 = subpassLoad(in_col, 0).r;
  for (int s = 0; s < 4; ++s) {
    uint c = subpassLoad(in_col, s).r;
    uint a = (c >> 24) & 0x1Fu;
    sum += uvec3(c & 0x3Fu, (c >> 8) & 0x3Fu, (c >> 16) & 0x3Fu) * a;
    asum += a;
  }
  if (asum == 0u) o_col = c0;
  else {
    uvec3 rgb = (sum + uvec3(asum >> 1)) / asum;
    o_col = rgb.r | (rgb.g << 8) | (rgb.b << 16) | (((asum + 2u) >> 2) << 24);
  }
  o_attr = subpassLoad(in_attr, 0).r;
  o_z = subpassLoad(in_z, 0).r;
}
