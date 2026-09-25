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

namespace ds::gpu {

// Position of the edge across the run's axis, in [0, 1] from the negative
// side (left / up), for a byte whose outside is on `neg`'s side: the polygon
// covers (cov + 1) / 32 of the pixel starting from its own side.
inline float edge_cut_pos(u8 e) {
  const float f = static_cast<float>((e & 0x1F) + 1) / 32.0f;
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

// A whole screen at N x: dst is (256 N) x (192 N).
inline void edge_cut_screen(const u32* src, const u8* edges, int N, u32* dst) {
  for (int y = 0; y < 192; ++y)
    for (int x = 0; x < 256; ++x) {
      const u32 own = src[y * 256 + x];
      const u8 e = edges[y * 256 + x];
      for (int fy = 0; fy < N; ++fy) {
        u32* row = dst + (y * N + fy) * 256 * N + x * N;
        if (!(e & 0x80)) { for (int fx = 0; fx < N; ++fx) row[fx] = own; continue; }
        for (int fx = 0; fx < N; ++fx) row[fx] = edge_cut_pixel(src, edges, x, y, fx, fy, N);
      }
    }
}

} // namespace ds::gpu
