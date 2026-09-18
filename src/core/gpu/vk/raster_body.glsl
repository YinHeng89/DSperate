// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
// Raster pass: one workgroup per tile, one invocation per pixel.
//
// The X stage only. The Y stage -- the edge walk, the slopes, the endpoint
// interpolation -- was computed once per (polygon, scanline) by span.comp and
// arrives as a GpuRow; this reads one and shades a pixel from it.
//
// Each invocation owns one pixel for the whole frame, which is why colour,
// depth, the attribute word and the shadow stencil live in REGISTERS here
// rather than in a framebuffer. There is no read-modify-write anywhere in the
// polygon loop, which is the one place this is structurally cheaper than
// either the CPU raster or a fragment shader.
//
// What remains is a transcription of Renderer3D (render3d.cpp): the
// perspective factor, the decal/modulate combine, the alpha blend, the
// translucent polygon-id rule and the shadow stencil are the same arithmetic
// in the same order.
//
// WHAT IT WALKS. Only the ORDERED TAIL of the list -- from the first
// translucent, shadow-mask or shadow polygon to the end. The order-free
// prefix before it (every opaque polygon the hardware sorted first) was
// decided by vis.comp, one fragment at a time with no per-pixel loop at all,
// and each invocation here begins by shading its pixel's winner (DS_VIS).
// The binning pass bins only the tail, so on a tile with no translucency the
// loop below runs zero times. See vk_layout.h, GpuFrame::first_ordered.
//
// Two builds of this body: raster_vis.comp defines DS_VIS and reads the keys
// (64-bit integers, so it needs shaderInt64); raster.comp does not and starts
// every pixel from the clear, which is the raster as it was before the
// visibility pass and what a part without 64-bit atomics runs.
//
// SCOPE. Anti-aliasing, wireframe and the depth-equal modes are host-gated
// (Renderer3D::gpu_supported); such frames go to the CPU, so what is missing
// here is absent rather than wrong.
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
// The visibility pass's per-pixel owner keys (vis.comp): what this pixel
// starts from. Only the DS_VIS build reads them; it needs 64-bit integers.
layout(std430, binding = 10) readonly buffer Keys   { uint64_t keys[]; };
#endif
layout(std430, binding = 4) writeonly buffer Out    { uint fb[]; };
// Written only when the final pass will run (post.comp): edge marking needs
// every pixel's polygon id and depth, and fog needs its depth and fog bit.
layout(std430, binding = 6) writeonly buffer Depth  { uint zbuf[]; };
layout(std430, binding = 7) writeonly buffer Attr   { uint abuf[]; };
layout(push_constant) uniform Push { GpuFrame f; } pc;

// The interpolator and the slope, shared with the span pass that filled the
// rows. The raster needs only the X-stage half of it; compute_row comes along
// unused and compiles out.
#include "ds_span.glsl"



#include "ds_shade.glsl"

// ---- main ------------------------------------------------------------------

void main() {
  // One dispatch covers a band of tile rows, not the whole screen, so the
  // workgroup's own row is relative to the band and the band's base comes in
  // the push constant. See the completion-band note in vk_raster.h: the
  // bands exist to let the compositor read the top of the frame before the
  // bottom is drawn, not to divide the work -- the work was already divided
  // finer than this, one workgroup per tile.
  uint tyi = gl_WorkGroupID.y + pc.f.tile_y0;
  uint tile = tyi * uint(DS_TILES_X) + gl_WorkGroupID.x;

  // Load and order this tile's polygon list. The binning pass appends with an
  // atomic, so the list arrives shuffled; draw order decides translucency and
  // polygon ids on the DS, so it is sorted back into polygon-index order
  // here. One thread, an insertion sort, once per tile rather than per pixel.
  int ty0 = int(tyi) * DS_TILE_H;
  // The list arrives from the binning pass already culled against this tile
  // and already in draw order, so there is nothing to do to it here.
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
  // Where the pixel starts: the order-free prefix's winner, shaded here and
  // now -- once, in the invocation that owns the pixel, so the result never
  // leaves registers before the ordered tail continues from it exactly as
  // the CPU loop would have continued past the same polygon. A key nobody
  // touched is the clear.
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
    depth = uint(key >> 12) ^ 0x80000000u;   // vis.comp flipped the sign bit to order z as signed
    dattr = (wp.attr & 0x3F008000u) | (((wp.flags & DS_PF_FRONTFACING) != 0u) ? 0u : (1u << 4)) | wedge;
  }
#endif
  // The shadow stencil, one bit for this pixel. On the CPU it is a byte per
  // pixel per ring line, shared by the whole scanline; here each invocation
  // owns its own pixel for the whole frame, so it is a register. (Bit 2, the
  // pixel underneath, only ever steers writes to the AA under-layer, which is
  // host-gated off.)
  uint sten = 0u;
  uint cur_run = 0u;   // which mask run that stencil belongs to; runs start at 1
  for (uint k = 0u; k < tcount; ++k) {
      // Read only as far as the pixel gets. Most invocations of most polygons
      // fail the span test, so the row is read a word at a time and the
      // 64-byte polygon record not at all until something will be drawn. The
      // scanline range and the row index come from the binning pass, which
      // had the polygon in hand anyway.
      uint yr = tiles.yrng[tbase + k];
      if (!live || py < int(yr & 0xFFu) || py > int((yr >> 8) & 0xFFu)) continue;
      uint ridx = tiles.bias[tbase + k] + uint(py);
      uint fl = rows[ridx].fl;
      if ((fl & 1u) == 0u) continue;
      bool is_mask = (fl & 512u) != 0u;
      // The stencil belongs to one run of mask polygons; seeing a different
      // run means this one is starting and the stencil is cleared. Before the
      // span test, because the clear is per SCANLINE whatever x it covers,
      // and keyed on the run rather than on a clear flag so that a pixel
      // which never saw the earlier masks of a run still agrees.
      if (is_mask) {
        uint run = shrun[tiles.list[tbase + k] * uint(DS_SHRUN_LINES) + uint(py)];
        if (run != cur_run) { sten = 0u; cur_run = run; }
      }
      int xstart = rows[ridx].xstart, xend = rows[ridx].xend;
      if (px < xstart || px > xend) continue;
      // The span in three parts: the left edge run, the middle, the right
      // edge run. The middle always draws; the two ends draw only when their
      // fill rule says so. (stage_line's lim0 / lim1 / lim2.)
      uint edge = (fl >> 5) & 0xFu;      // the scanline's top/bottom flags
      if (px < rows[ridx].lim0)       { if ((fl & 2u) == 0u) continue; edge |= 1u; }
      else if (px >= rows[ridx].lim1) { if ((fl & 4u) == 0u) continue; edge |= 2u; }

      GpuRow rw = rows[ridx];
      GpuPoly p = polys[tiles.list[tbase + k]];
    {
      // The X stage, between the two endpoints the Y stage produced. Its
      // setup came with the row; only the per-pixel factor is left.
      Interp ix = row_interp(rw);
      interp_set_x(ix, px);

      int z = interp_z(ix, rw.zl, rw.zr);

      // Depth. Mode 1 (front-facing) is less-or-equal only against an opaque
      // back-facing destination; the depth-equal modes are host-gated.
      bool front = (p.flags & DS_PF_FRONTFACING) != 0u;
      bool pass;
      if (front) pass = ((dattr & 0x00400010u) == 0x00000010u) ? (z <= int(depth)) : (z < int(depth));
      else       pass = z < int(depth);

      // A mask polygon draws nothing. It marks where its depth test FAILS --
      // that is the volume's interior -- and the shadow polygon that follows
      // draws only there. The alpha test still gates the marking, and a mask
      // that fails it has already done its clearing above.
      if (is_mask) {
        if (((p.attr >> 16) & 0x1Fu) > pc.f.alpha_ref && !pass) sten |= 1u;
        continue;
      }
      // A shadow polygon is confined to the stencil the mask left.
      if ((fl & 1024u) != 0u && (sten & 1u) == 0u) continue;

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
            // Renderer3D::plot_translucent. Equal translucent polygon ids do
            // not blend, which is what keeps a self-overlapping translucent
            // mesh from darkening along its own seams.
            uint attr = (polyattr & 0xE0F0u) | ((polyattr >> 8) & 0xFF0000u) | (1u << 22) | (dattr & 0xFF001F0Fu);
            // Equal ids do not blend. A shadow compares against whichever id
            // the destination carries -- translucent if it has one, otherwise
            // the opaque one -- which is what stops a shadow darkening the
            // very polygon that cast it.
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
    // The planes the final pass reads, written only when there will be one.
    if ((pc.f.dispcnt & ((1u << 5) | (1u << 7))) != 0u) {
      zbuf[idx] = depth;
      abuf[idx] = dattr;
    }
  }
}
