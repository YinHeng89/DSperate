// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Enhanced AA at panel density: the reference cut of a source pixel's panel
// block along its 3D edge (Renderer3D::set_edge_export for the byte's
// layout). Every panel pixel takes either the pixel's own colour or the
// outside neighbour's, never a mix, so a silhouette steps at panel density
// instead of the DS's. The edge line through the block comes from the
// coverage (its position across the run's axis) and tilts by the coverage
// of the next edge pixel along the run on the side the panel pixel is on.
// The present compute shader (present.comp) mirrors this; the CPU scaler
// can call it directly.
#pragma once
#include "core/types.h"
#include <cstdlib>

namespace ds::gpu {

// Position of the edge across the run's axis, in [0, 1] from the negative
// side (left / up), for a byte whose outside is on `neg`'s side: the polygon
// covers (cov + 1) / 32 of the pixel starting from its own side.
// Byte layout: bit 7 edge, bit 6 outside negative (left / up), bit 5 the
// covered fraction runs vertically, bit 4 an alpha-to-coverage edge (a
// texture hole; the rest are polygon edges and depth crossings), bits 0-3
// the coverage's upper four bits.
inline float edge_cut_pos(u8 e) {
  const float f = static_cast<float>((e & 0x0F) + 1) / 16.0f;
  return (e & 0x40) ? 1.0f - f : f;   // outside negative: polygon at the positive end
}
// The colour of panel pixel (fx, fy) of the N x N block of source pixel
// (x, y). `src` is the composited 256x192 frame, `edges` its edge bytes.
inline u32 edge_cut_pixel(const u32* src, const u8* edges, int x, int y, int fx, int fy, int N) {
  const u32 own = src[y * 256 + x];
  const u8 e = edges[y * 256 + x];
  if (!(e & 0x80)) return own;
  const bool vertical = e & 0x20;   // covered fraction runs vertically: outside above or below
  const bool neg = e & 0x40;        // outside is the negative neighbour (left / up)
  // Along the run: the pixel before and after this one, if they are edge
  // pixels of the same kind (same axis and side), give the line's tilt.
  auto compatible = [&](int nx, int ny) -> bool {
    if (nx < 0 || nx > 255 || ny < 0 || ny > 191) return false;
    const u8 n = edges[ny * 256 + nx];
    return (n & 0xE0) == (e & 0xE0);
  };
  const float pos = edge_cut_pos(e);
  float t, along;   // t: the panel pixel's centre across the axis in [0,1); along: its centre along the run, -0.5..0.5 from the pixel centre
  if (vertical) { t = (fy + 0.5f) / N; along = (fx + 0.5f) / N - 0.5f; }
  else          { t = (fx + 0.5f) / N; along = (fy + 0.5f) / N - 0.5f; }
  const int ax = vertical ? 1 : 0, ay = vertical ? 0 : 1;   // step along the run
  const int nx = x + (along < 0 ? -ax : ax), ny = y + (along < 0 ? -ay : ay);
  float pos_n = pos;
  if (compatible(nx, ny)) pos_n = edge_cut_pos(edges[ny * 256 + nx]);
  const float line = pos + (pos_n - pos) * (along < 0 ? -along : along);
  // Inside the polygon: on the far side of the line from the outside.
  const bool inside = neg ? (t >= line) : (t < line);
  if (inside) return own;
  const int px = vertical ? x : (neg ? x - 1 : x + 1), py = vertical ? (neg ? y - 1 : y + 1) : y;
  if (px < 0 || px > 255 || py < 0 || py > 191) return own;
  return src[py * 256 + px];
}

// The colour the 1x anti-aliased pixel would have: its own mixed with the
// outside neighbour's by the coverage.
inline u32 edge_blend_1x(const u32* src, const u8* edges, int x, int y) {
  const u32 own = src[y * 256 + x];
  const u8 e = edges[y * 256 + x];
  if (!(e & 0x80)) return own;
  const bool vertical = e & 0x20, neg = e & 0x40;
  const int px = vertical ? x : (neg ? x - 1 : x + 1), py = vertical ? (neg ? y - 1 : y + 1) : y;
  if (px < 0 || px > 255 || py < 0 || py > 191) return own;
  const u32 out = src[py * 256 + px];
  const u32 c = ((e & 0x0F) << 1) + 1;   // of 32
  u32 r = 0;
  for (int sh = 0; sh < 24; sh += 8) r |= ((((own >> sh) & 0xFF) * c + ((out >> sh) & 0xFF) * (32 - c)) >> 5) << sh;
  return r | (own & 0xFF000000u);
}
// Soft modes (DS_GEOM_SOFT, DS_CUT_SOFT, DS_ALL_SOFT): mode bit 0 resamples
// polygon edges, bit 1 alpha-to-coverage edges. A panel pixel whose 2x2
// source taps include such an edge is the bilinear mix of the taps, each
// edge tap at its 1x anti-aliased colour, so the edge becomes a ramp one
// source pixel wide (what a bilinear 2x render gives) instead of a step.
inline bool edge_soft_tap(u8 e, int mode) { return (e & 0x80) && (mode & ((e & 0x10) ? 2 : 1)); }
inline u32 edge_soft_pixel(const u32* src, const u8* edges, int x, int y, int fx, int fy, int N, int mode) {
  const float u = (fx + 0.5f) / N - 0.5f, v = (fy + 0.5f) / N - 0.5f;   // offset from the source pixel's centre
  const int x1 = u < 0 ? x - 1 : x + 1, y1 = v < 0 ? y - 1 : y + 1;
  const float wu = u < 0 ? -u : u, wv = v < 0 ? -v : v;
  const int xs[2] = {x, x1 < 0 || x1 > 255 ? x : x1}, ys[2] = {y, y1 < 0 || y1 > 191 ? y : y1};
  bool soft = false;
  for (int j = 0; j < 2 && !soft; ++j) for (int i = 0; i < 2; ++i) if (edge_soft_tap(edges[ys[j] * 256 + xs[i]], mode)) { soft = true; break; }
  if (!soft) return edge_cut_pixel(src, edges, x, y, fx, fy, N);
  float acc[3] = {0, 0, 0};
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const u32 c = edge_blend_1x(src, edges, xs[i], ys[j]);
      const float w = (i ? wu : 1.0f - wu) * (j ? wv : 1.0f - wv);
      acc[0] += ((c >> 16) & 0xFF) * w; acc[1] += ((c >> 8) & 0xFF) * w; acc[2] += (c & 0xFF) * w;
    }
  const u32 r = static_cast<u32>(acc[0] + 0.5f), g = static_cast<u32>(acc[1] + 0.5f), b = static_cast<u32>(acc[2] + 0.5f);
  return (src[y * 256 + x] & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

// A whole screen at N x: dst is (256 N) x (192 N).
inline void edge_cut_screen(const u32* src, const u8* edges, int N, u32* dst, int soft = 0) {
  for (int y = 0; y < 192; ++y)
    for (int x = 0; x < 256; ++x) {
      const u32 own = src[y * 256 + x];
      const u8 e = edges[y * 256 + x];
      for (int fy = 0; fy < N; ++fy) {
        u32* row = dst + (y * N + fy) * 256 * N + x * N;
        if (soft) { for (int fx = 0; fx < N; ++fx) row[fx] = edge_soft_pixel(src, edges, x, y, fx, fy, N, soft); continue; }
        if (!(e & 0x80)) { for (int fx = 0; fx < N; ++fx) row[fx] = own; continue; }
        for (int fx = 0; fx < N; ++fx) row[fx] = edge_cut_pixel(src, edges, x, y, fx, fy, N);
      }
    }
}
// The soft mode from the environment: DS_ALL_SOFT (both), DS_GEOM_SOFT
// (polygon edges), DS_CUT_SOFT (alpha-to-coverage edges); a value of 2 on
// the cut-out ones also drops the CPU's grid sampling (cutout_coverage).
inline int edge_soft_mode_env() {
  int m = 0;
  if (const char* a = std::getenv("DS_ALL_SOFT")) if (std::atoi(a)) m |= 3;
  if (const char* a = std::getenv("DS_GEOM_SOFT")) if (std::atoi(a)) m |= 1;
  if (const char* a = std::getenv("DS_CUT_SOFT")) if (std::atoi(a)) m |= 2;
  return m;
}

} // namespace ds::gpu
