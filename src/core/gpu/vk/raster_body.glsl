// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
// Raster pass: one workgroup per tile, one invocation per pixel.
//
// X stage only; the Y stage (edge walk, slopes, endpoint interpolation) was
// computed per (polygon, scanline) by span.comp and arrives as a GpuRow.
// Each invocation owns one pixel for the whole frame, so colour, depth,
// attribute word and shadow stencil live in registers, not a framebuffer.
// Transcription of Renderer3D: same arithmetic, same order.
//
// Walks only the ORDERED TAIL of the list (from the first translucent,
// shadow-mask or shadow polygon on); the order-free opaque prefix was
// decided by vis.comp and each invocation starts from its pixel's winner
// (DS_VIS). See vk_layout.h, GpuFrame::first_ordered.
//
// Two builds: raster_vis.comp defines DS_VIS and reads the keys (needs
// shaderInt64); raster.comp does not, for parts without 64-bit atomics.
//
// AA, wireframe and depth-equal modes are host-gated to the CPU raster, so
// their absence here is not a bug.
#define DS_GLSL 1
#include "vk_layout.h"

layout(local_size_x = DS_TILE_W, local_size_y = DS_TILE_H) in;

layout(std430, binding = 0) readonly buffer Polys  { GpuPoly polys[]; };
layout(std430, binding = 1) readonly buffer Verts  { GpuVert verts[]; };
layout(std430, binding = 2) readonly buffer Tiles  {
  uint count[DS_TILE_COUNT];
  uint overflow;
  uint pad_[3];
  uint list[DS_TILE_COUNT * DS_TILE_POLYS];
  uint bias[DS_TILE_COUNT * DS_TILE_POLYS];
  uint yrng[DS_TILE_COUNT * DS_TILE_POLYS];
} tiles;
layout(std430, binding = 3) readonly  buffer Texels { uint texels[]; };
layout(std430, binding = 5) readonly  buffer Post   { GpuPost ps; };
layout(std430, binding = 8) readonly  buffer ShRun  { uint shrun[]; };
layout(std430, binding = 9) readonly  buffer Rows    { GpuRow rows[]; };
#ifdef DS_VIS
layout(std430, binding = 10) readonly buffer Keys   { uint64_t keys[]; };   // vis.comp's per-pixel owner keys
#endif
layout(std430, binding = 4) writeonly buffer Out    { uint fb[]; };
// Written only when the final pass runs (edge marking, fog).
layout(std430, binding = 6) writeonly buffer Depth  { uint zbuf[]; };
layout(std430, binding = 7) writeonly buffer Attr   { uint abuf[]; };
layout(push_constant) uniform Push { GpuFrame f; } pc;

#include "ds_span.glsl"   // shared with the span pass; only the X-stage half used here
#include "ds_shade.glsl"

// ---- main ------------------------------------------------------------------

void main() {
  // One dispatch covers a band of tile rows: workgroup row is relative to
  // the band, band base via push constant (lets the compositor read the top
  // of the frame before the bottom is drawn).
  uint tyi = gl_WorkGroupID.y + pc.f.tile_y0;
  uint tile = tyi * uint(DS_TILES_X) + gl_WorkGroupID.x;

  int ty0 = int(tyi) * DS_TILE_H;
  uint tbase = tile * uint(DS_TILE_POLYS);
  uint tcount = min(tiles.count[tile], uint(DS_TILE_POLYS));

  int px = int(gl_GlobalInvocationID.x), py = ty0 + int(gl_LocalInvocationID.y);
  int W = 256 * int(pc.f.scale), H = 192 * int(pc.f.scale);
  bool live = px < W && py < H;
  uint idx = uint(py * W + px);

  uint color = pc.f.clear_color;
  uint depth = pc.f.clear_depth;
  uint dattr = pc.f.clear_attr;
#ifdef DS_VIS
  // Order-free prefix's winner for this pixel, shaded once so the ordered
  // tail continues from it. An untouched key is the clear.
  uint64_t key = live ? keys[idx] : ~0ul;
  if (key != ~0ul) {
    uint pi = uint(key) & 0x7FFu;
    GpuPoly wp = polys[pi];
    GpuRow wr = rows[wp.row_base + uint(py - max(wp.ytop, 0))];
    uint wedge = (wr.fl >> 5) & 0xFu;
    if (px < wr.lim0) wedge |= 1u;
    else if (px >= wr.lim1) wedge |= 2u;
    Interp wx = row_interp(wr);
    interp_set_x(wx, px);
    int vr9 = interp_val(wx, unpack_r(wr.lrgb), unpack_r(wr.rrgb));
    int vg9 = interp_val(wx, unpack_g(wr.lrgb), unpack_g(wr.rrgb));
    int vb9 = interp_val(wx, unpack_b(wr.lrgb), unpack_b(wr.rrgb));
    bool wtex = (wp.flags & DS_PF_TEXTURED) != 0u;
    int sc = 0, tc = 0;
    if (wtex) {
      sc = interp_val(wx, unpack_s(wr.lst), unpack_s(wr.rst));
      tc = interp_val(wx, unpack_t(wr.lst), unpack_t(wr.rst));
    }
    color = shade_pixel(wp, (wp.attr >> 4) & 3u, (wp.attr >> 16) & 0x1Fu, wtex, vr9, vg9, vb9, sc, tc);
    depth = uint(key >> 12) ^ 0x80000000u;   // vis.comp flipped sign bit to order z as signed
    dattr = (wp.attr & 0x3F008000u) | (((wp.flags & DS_PF_FRONTFACING) != 0u) ? 0u : (1u << 4)) | wedge;
  }
#endif
  uint sten = 0u;   // shadow stencil, one bit per pixel (a register here, a byte-per-line on the CPU)
  uint cur_run = 0u;   // which mask run this stencil belongs to; runs start at 1
  for (uint k = 0u; k < tcount; ++k) {
      uint yr = tiles.yrng[tbase + k];
      if (!live || py < int(yr & 0xFFu) || py > int((yr >> 8) & 0xFFu)) continue;
      uint ridx = tiles.bias[tbase + k] + uint(py);
      uint fl = rows[ridx].fl;
      if ((fl & 1u) == 0u) continue;
      bool is_mask = (fl & 512u) != 0u;
      // Stencil belongs to one run of mask polygons; a different run clears
      // it (before the span test: the clear is per scanline).
      if (is_mask) {
        uint run = shrun[tiles.list[tbase + k] * uint(DS_SHRUN_LINES) + uint(py)];
        if (run != cur_run) { sten = 0u; cur_run = run; }
      }
      int xstart = rows[ridx].xstart, xend = rows[ridx].xend;
      if (px < xstart || px > xend) continue;
      // Three parts: left edge run, middle, right edge run; middle always draws, ends per fill rule.
      uint edge = (fl >> 5) & 0xFu;      // scanline's top/bottom flags
      if (px < rows[ridx].lim0)       { if ((fl & 2u) == 0u) continue; edge |= 1u; }
      else if (px >= rows[ridx].lim1) { if ((fl & 4u) == 0u) continue; edge |= 2u; }

      GpuRow rw = rows[ridx];
      GpuPoly p = polys[tiles.list[tbase + k]];
    {
      Interp ix = row_interp(rw);   // per-pixel factor only; endpoints came from the Y stage
      interp_set_x(ix, px);

      int z = interp_z(ix, rw.zl, rw.zr);

      // Front-facing is less-or-equal only against an opaque back-facing destination.
      bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
      bool pass;
      if (front) pass = ((dattr & 0x00400010u) == 0x00000010u) ? (z <= int(depth)) : (z < int(depth));
      else       pass = z < int(depth);

      // A mask draws nothing; it marks where its depth test FAILS (the
      // volume's interior), and the following shadow draws only there.
      if (is_mask) {
        if (((p.attr >> 16) & 0x1Fu) > pc.f.alpha_ref && !pass) sten |= 1u;
        continue;
      }
      if ((fl & 1024u) != 0u && (sten & 1u) == 0u) continue;   // shadow confined to the mask's stencil

      if (pass) {
        int vr9 = interp_val(ix, unpack_r(rw.lrgb), unpack_r(rw.rrgb));
        int vg9 = interp_val(ix, unpack_g(rw.lrgb), unpack_g(rw.rrgb));
        int vb9 = interp_val(ix, unpack_b(rw.lrgb), unpack_b(rw.rrgb));
        bool textured = (p.flags & DS_PF_TEXTURED) != 0u;
        int sc = 0, tc = 0;
        if (textured) {
          sc = interp_val(ix, unpack_s(rw.lst), unpack_s(rw.rst));
          tc = interp_val(ix, unpack_t(rw.lst), unpack_t(rw.rst));
        }

        uint blendmode = (p.attr >> 4) & 3u;
        uint polyalpha = (p.attr >> 16) & 0x1Fu;
        uint src = shade_pixel(p, blendmode, polyalpha, textured, vr9, vg9, vb9, sc, tc);
        uint alpha = src >> 24;
        if (alpha > pc.f.alpha_ref) {
          uint polyattr = (p.attr & 0x3F008000u) | (front ? 0u : (1u << 4));
          if (alpha == 31u) {
            depth = uint(z);
            color = src;
            dattr = polyattr | edge;   // only an opaque write sets edge flags
          } else {
            // Equal translucent polygon ids don't blend: keeps a self-overlapping mesh from darkening its own seams.
            uint attr = (polyattr & 0xE0F0u) | ((polyattr >> 8) & 0xFF0000u) | (1u << 22) | (dattr & 0xFF001F0Fu);
            // Shadow compares against whichever id the destination carries
            // (translucent if any, else opaque): stops it darkening its caster.
            bool same;
            if ((fl & 1024u) != 0u) {
              same = ((dattr & (1u << 22)) != 0u)
                   ? ((dattr & 0x007F0000u) == (attr & 0x007F0000u))
                   : ((dattr & 0x3F000000u) == (polyattr & 0x3F000000u));
            } else {
              same = (dattr & 0x007F0000u) == (attr & 0x007F0000u);
            }
            if (!same) {
              if ((dattr & (1u << 15)) == 0u) attr &= ~(1u << 15);
              color = alpha_blend(pc.f.dispcnt, src, color, alpha);
              if ((p.attr & 0x800u) != 0u) depth = uint(z);
              dattr = attr;
            }
          }
        }
      }
    }
  }

  if (live) {
    fb[idx] = color;
    if ((pc.f.dispcnt & ((1u << 5) | (1u << 7))) != 0u) {   // only when the final pass will run
      zbuf[idx] = depth;
      abuf[idx] = dattr;
    }
  }
}
