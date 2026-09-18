// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_VK_LAYOUT_H
#define DS_VK_LAYOUT_H
// The host/shader interface for the GPU raster. Included by C++ and, with
// DS_GLSL defined, textually by the shaders, so a field can never drift
// between the two: change it here and both sides move.
//
// std430 layout throughout. Every member is 4 bytes and every struct is a
// multiple of 16, so the C++ and GLSL layouts agree without padding rules
// coming into it.

#ifndef DS_GLSL
#include "core/types.h"
namespace ds::gpu::vk {
using uint = u32;
#define DS_INT s32
#else
#define DS_INT int
#endif

// Tiles. One workgroup draws one tile, one invocation one pixel, so the tile
// holds a workgroup's worth of pixels: W*H must stay inside the G52's limit
// of 384 invocations, and 256 is the shape that divides the screen evenly.
//
// Which 256 used to be a real trade: the raster recomputed each polygon's
// scanline setup once per tile it touched, so wide tiles spared a
// screen-spanning polygon that work while narrow ones culled better. The
// span pass removed the recomputation, and with it the trade -- narrow is now
// simply better, because the per-pixel loop walks every polygon binned to the
// tile and a narrow tile bins fewer.
//
// Measured before and after, fence wait per frame on Etrian Odyssey:
//   before the span pass:  16x16 14.14   32x8 10.89   64x4 10.33   128x2 11.95
//   after:                  8x32  5.61   16x16 5.83   32x8  6.29   64x4  6.84
// The optimum walked from 64x4 to 8x32 exactly as removing the redundancy
// predicted it would. mlbis agrees (2.50 / 2.72 / 2.91 / 3.53); Golden Sun is
// flat between 8x32 and 16x16 (8.32 / 8.27) and slower either side.
#ifndef DS_TILE_W
#define DS_TILE_W 8
#endif
// The tile's height, i.e. the workgroup's lane count over its width. 256
// lanes by default; overridable for the raster-floor sweep.
#ifndef DS_TILE_H
#define DS_TILE_H (256 / DS_TILE_W)
#endif
#define DS_TILES_X (256 / DS_TILE_W)
#define DS_TILES_Y (192 / DS_TILE_H)
#define DS_TILE_COUNT (DS_TILES_X * DS_TILES_Y)
// Polygons a single tile will hold. Overflow is not silently dropped: the
// binning pass records it and the frame falls back to the CPU raster, which
// keeps "the GPU path is either right or absent" true.
#define DS_TILE_POLYS 512

// The hardware's own limits, so the buffers are sized once at startup and a
// frame can never need a bigger one. 2048 polygons, and a clipped polygon has
// at most 10 vertices.
#define DS_MAX_POLYS 2048
#define DS_MAX_VERTS (DS_MAX_POLYS * 10)

// Polygon flags.
#define DS_PF_TRANSLUCENT 0x0001u
#define DS_PF_WBUFFER     0x0002u
#define DS_PF_FRONTFACING 0x0004u
#define DS_PF_TEXTURED    0x0008u
#define DS_PF_SHADOW_MASK 0x0010u
#define DS_PF_SHADOW      0x0020u
// The texture can produce a 0-alpha texel: colour-0-transparent (TEXIMAGE_PARAM
// bit 29), the compressed format's transparent index, or the direct-colour
// format's alpha bit. Without it every texel of an opaque-group polygon has
// alpha 31 and its alpha test needs no texture sample at all.
#define DS_PF_TEX_ALPHA   0x0040u

// Shadow mask RUN INDEX, per polygon per scanline.
//
// The DS clears the shadow stencil for a whole SCANLINE when a run of shadow
// mask polygons begins -- when the previous polygon drawn on that line was not
// itself a mask. A tiled raster cannot see that: the polygon that ended the
// run may cover the line somewhere else entirely and never reach this tile.
//
// Recording "this mask clears here" is not enough either, and that was a real
// bug rather than a theoretical one: a pixel only sees the masks binned to ITS
// tile, so it can miss the very mask that carried the clear, and the amount it
// misses depends on the tile width. Narrowing the tiles made Spirit Tracks
// measurably worse, which is how it was found.
//
// Identifying the RUN instead is tile-independent. The host numbers the runs
// per scanline; a pixel clears whenever the run it sees differs from the run
// it saw last, so it reaches the right answer from any subsequence of the
// masks -- including one that skips whole runs, because a run it never saw
// contributed nothing to its stencil anyway.
#define DS_SHRUN_LINES 192

// One screen-space vertex of one polygon, after the viewport transform and
// after clipping -- flattened per polygon rather than shared, because z and w
// live in the Polygon (p.z[i], p.w[i]) while position and attributes live in
// the Vertex, and the raster always wants them together. 48 bytes.
struct GpuVert {
  DS_INT sx, sy;        // screen position
  DS_INT z, w;          // depth and normalised W for this polygon's slot
  DS_INT r, g, b;       // Vertex::fcol, 9-bit; the raster narrows to 6 with >> 3
  DS_INT s, t;          // 12.4 texture coordinates
  uint   pad_[3];
};

// One polygon. 64 bytes.
struct GpuPoly {
  uint   first_vert;    // index into the vertex buffer; slot i is first_vert + i
  uint   nverts;
  uint   attr;          // POLYGON_ATTR as the hardware holds it
  uint   texparam;      // TEXIMAGE_PARAM: size, repeat and flip bits
  uint   tex_offset;    // word offset of the decoded texture in the arena
  uint   tex_w, tex_h;  // decoded texture dimensions, texels
  uint   flags;         // DS_PF_*
  DS_INT ytop, ybot;    // screen scanline range, inclusive
  uint   vtop, vbot;    // slots of the top and bottom vertices: where the edge chains start and end
  DS_INT xmin, xmax;    // screen x bounds, for the binning pass
  uint   row_base;      // this polygon's first row in the span table
  uint   pad_;
};

// One scanline of one polygon, computed once for the whole frame.
//
// This is the Y stage: walking the two edge chains, setting up both slopes,
// and interpolating w, z and the five attributes to the ends of the span. It
// depends on the polygon and the scanline and NOT on x, so it wants to exist
// exactly once per (polygon, scanline) -- which is what the span pass makes
// it. Previously the raster recomputed it per tile, and since a polygon
// spans several tile columns that was the dominant redundancy AND the reason
// tile shape mattered at all (see the scoping doc's sweep).
//
// Sixteen words. The five attributes at each end pack into two apiece: three
// 9-bit colours into 10:10:10, two 12.4 texture coordinates into 16:16.
struct GpuRow {
  DS_INT xstart, xend, lim0, lim1;
  DS_INT wl, wr, zl, zr;
  uint   lrgb, lst, rrgb, rst;
  DS_INT xrz;           // the X stage's (1<<22)/xdiff
  uint   rcp;           // and its reciprocal-of-xdiff
  uint   fl;            // 1 valid, 2 l_fill, 4 r_fill, 8 w-buffer, 16 linear, 5-8 yedge, 512 mask, 1024 shadow
  uint   poly;          // the polygon this row belongs to, so a pass driven by rows can find it
};

// The visibility pass's workgroup: DS_VIS_ROWS span rows per workgroup, a
// subgroup of DS_VIS_LANES lanes per row, the lanes taking the row's pixels
// in stride. One row per 32-lane workgroup was the first shape and it was
// launch-bound on Mario & Luigi: 5000 workgroups a frame for 13-pixel spans,
// with most lanes idle. Eight lanes is the G52's subgroup, so a row is one
// subgroup and rows never diverge against each other.
#define DS_VIS_ROWS  8
#define DS_VIS_LANES 8

// Rows the span table holds. A frame needs one per visible polygon scanline
// -- the sum of the polygons' clamped heights -- and one that wants more is
// refused rather than truncated, like every other budget here. Measured peak
// over the six benchmark scenes is 15874 (Spirit Tracks), so this is about
// three times the worst seen; the theoretical worst is 2048 * 192, which no
// real frame approaches and which would fall back to the CPU.
#define DS_MAX_SPAN_ROWS 49152

// Per-frame constants. Pushed, not a buffer: it changes every frame and is
// small enough for the push-constant budget.
struct GpuFrame {
  uint   npoly;
  uint   scale;         // internal resolution multiplier (1 for P1)
  uint   dispcnt;       // DISP3DCNT: bit 3 is the blend enable the alpha blend reads
  uint   alpha_ref;     // already zeroed by the engine when DISP3DCNT bit 2 is clear
  uint   clear_color;   // the record clear_line() fills: RGB666 + alpha << 24
  uint   clear_depth;
  uint   clear_attr;    // polygon id in bits 24-29, fog bit 15
  uint   tile_y0;       // first tile row of this dispatch: see the bands note in vk_raster.h
  // The ORDER-FREE PREFIX of the list. The hardware sorts every opaque polygon
  // before every translucent one, and for an opaque polygon the pixel's final
  // owner is a minimum under a total order -- (z, back-facing, list index) --
  // so draw order does not matter for them (see vis.comp). Polygons
  // [0, first_ordered) are drawn that way; [first_ordered, npoly) are the
  // ordered tail the per-pixel loop still walks in list order. The first
  // translucent, shadow-mask or shadow polygon ends the prefix, so the tail
  // may still hold opaque polygons and that is fine: the loop handles them
  // exactly as before. opaque_rows is the prefix's span-table size.
  uint   first_ordered;
  uint   opaque_rows;
  uint   nrows;         // span rows this frame, all polygons: the span pass is one lane per row
  uint   pad_;
};

// Render state too large for the push constants: the fog table, the edge
// colours and the toon table. Uploaded every frame -- it is 320 bytes, which
// is cheaper to write than to track for changes.
//
// The final pass exists because edge marking reads a pixel's four NEIGHBOURS,
// which in a tiled raster are in other workgroups. Fog needs no neighbour and
// could have been folded into the raster -- but the hardware applies edge
// marking first and fog to its result, so they belong in the same pass, in
// that order, exactly as Renderer3D::final_pass_ref has them.
struct GpuPost {
  uint fog_color;       // RenderState::fog_color: 15-bit colour in 0-14, 5-bit alpha in 16-20
  uint fog_offset;
  uint fog_shift;
  uint pad0_;
  uint density[34];     // RenderState::fog_density, 34 entries: index 32 is the last, 33 its repeat
  uint pad1_[2];
  uint edge[8];         // RenderState::edge, 15-bit colours indexed by polygon id >> 3
  uint toon[32];        // RenderState::toon, 15-bit colours indexed by the vertex red >> 1
};

// The binning pass's output: a polygon list per tile, plus a counter.
// `overflow` is set when any tile exceeded DS_TILE_POLYS.
struct GpuTiles {
  uint count[DS_TILE_COUNT];
  uint overflow;
  uint pad_[3];
  uint list[DS_TILE_COUNT * DS_TILE_POLYS];
  uint bias[DS_TILE_COUNT * DS_TILE_POLYS];
  uint yrng[DS_TILE_COUNT * DS_TILE_POLYS];
};

#ifndef DS_GLSL
#undef DS_INT
} // namespace ds::gpu::vk
#else
#undef DS_INT
#endif

#endif // DS_VK_LAYOUT_H
