// The anti-aliasing pass of the triangle path (1x only): the DS's two-deep
// pixel stack. Attachments 4-6 hold the pixel UNDERNEATH (colour, attribute,
// depth record); an opaque edge pixel pushes what it covers down there, a
// polygon that loses the depth test against an edge pixel may still land
// underneath, and the final pass (post.comp, DS_FF_AA) blends the two by the
// edge pixel's coverage. Edge flags and coverage come from the span table
// (the span pass runs for these frames), exactly as Renderer3D::resolve_span
// derives them. No hardware depth buffer: the test is the DS's, in here.
layout(std430, binding = 1) readonly buffer Verts { GpuVert verts[]; };
#include "ds_span.glsl"
layout(input_attachment_index = 0, set = 1, binding = 0) uniform usubpassInput in_col;
layout(input_attachment_index = 1, set = 1, binding = 1) uniform usubpassInput in_attr;
layout(input_attachment_index = 2, set = 1, binding = 2) uniform usubpassInput in_z;
layout(input_attachment_index = 3, set = 1, binding = 3) uniform usubpassInput in_sh;
layout(input_attachment_index = 4, set = 1, binding = 4) uniform usubpassInput in_ucol;
layout(input_attachment_index = 5, set = 1, binding = 5) uniform usubpassInput in_uattr;
layout(input_attachment_index = 6, set = 1, binding = 6) uniform usubpassInput in_uz;
layout(location = 4) out uint o_ucol;
layout(location = 5) out uint o_uattr;
layout(location = 6) out uint o_uz;

// Renderer3D::depth_pass: the mode from the polygon (equal-depth with its
// tolerance, else LESS -- with equal passing for a front-facing polygon over
// an opaque back-facing pixel).
bool ds_depth_pass(GpuPoly p, uint z, uint dz, uint dattr) {
  if ((p.attr & (1u << 14)) != 0u) { uint tol = (pc.f.flags & DS_FF_WBUFFER) != 0u ? 0xFFu : 0x200u; return (dz > z ? dz - z : z - dz) <= tol; }
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  return (front && (dattr & 0x00400010u) == 0x00000010u) ? z <= dz : z < dz;
}

// This fragment's edge flags (1 left run, 2 right run, 4 top row, 8 bottom
// row) and coverage, from its polygon's span on this scanline; 0 flags for a
// pixel the DS span does not reach (the hardware rasteriser's coverage
// differs a little from the DS's).
bool aa_inside = true;   // whether the pixel is in its polygon's DS span (aa_edge sets it)
GpuRow aa_row;           // the row aa_edge found, for shade_row
uint aa_edge(GpuPoly p, int x, int y, out uint cov) {
  GpuRow r;
  uint e = row_edge(p, x, y, cov, aa_inside, r);
  if (aa_inside) aa_row = r;
  return e;
}

// The pixel from its span row: the DS's own X-stage interpolation of depth,
// colour and texture coordinates (raster_body.glsl does the same), not the
// hardware's varyings of a polygon that tri.vert grew by a pixel.
Frag shade_row(GpuPoly p, GpuRow r, int x) {
  Interp ix = row_interp(r);
  interp_set_x(ix, x);
  int z = interp_z(ix, r.zl, r.zr);
  int vr9 = interp_val(ix, unpack_r(r.lrgb), unpack_r(r.rrgb));
  int vg9 = interp_val(ix, unpack_g(r.lrgb), unpack_g(r.rrgb));
  int vb9 = interp_val(ix, unpack_b(r.lrgb), unpack_b(r.rrgb));
  bool textured = (p.flags & DS_PF_TEXTURED) != 0u && (pc.f.flags & DS_FF_NOTEX) == 0u;
  int sc = 0, tc = 0;
  if (textured) {
    sc = interp_val(ix, unpack_s(r.lst), unpack_s(r.rst));
    tc = interp_val(ix, unpack_t(r.lst), unpack_t(r.rst));
  }
  Frag o;
  o.src = shade_pixel(p, (p.attr >> 4) & 3u, (p.attr >> 16) & 0x1Fu, textured, vr9, vg9, vb9, sc, tc);
  o.alpha = o.src >> 24;
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
  o.polyattr = (p.attr & 0x3F008000u) | (front ? 0u : (1u << 4));
  o.depth = uint(clamp(z, 0, 16777215));
  return o;
}

// The opaque write with the pixel stack: the new pixel on top, the old top
// pushed underneath when the new one is an edge pixel; a pixel that fails
// against the top of an edge pixel may take the slot underneath.
struct Stack { uint tcol, tattr, tz, ucol, uattr, uz; };
Stack stack_load() {
  Stack s;
  s.tcol = subpassLoad(in_col).r; s.tattr = subpassLoad(in_attr).r; s.tz = subpassLoad(in_z).r;
  s.ucol = subpassLoad(in_ucol).r; s.uattr = subpassLoad(in_uattr).r; s.uz = subpassLoad(in_uz).r;
  return s;
}
void stack_store(Stack s) {
  o_col = s.tcol; o_attr = s.tattr; o_z = s.tz;
  o_ucol = s.ucol; o_uattr = s.uattr; o_uz = s.uz;
}
// Returns false when the pixel takes neither slot (the caller discards).
bool stack_opaque(inout Stack s, GpuPoly p, Frag f, uint edge, uint cov) {
  uint attr = f.polyattr | edge;
  bool push = edge != 0u;
  if (push) attr |= (cov << 8);
  if (ds_depth_pass(p, f.depth, s.tz, s.tattr)) {
    if (push) { s.ucol = s.tcol; s.uattr = s.tattr; s.uz = s.tz; }
    s.tcol = f.src; s.tattr = attr; s.tz = f.depth;
    return true;
  }
  if ((s.tattr & 0xFu) != 0u && ds_depth_pass(p, f.depth, s.uz, s.uattr)) {
    s.ucol = f.src; s.uattr = attr; s.uz = f.depth;
    return true;
  }
  return false;
}
