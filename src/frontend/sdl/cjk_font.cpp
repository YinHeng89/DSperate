// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/cjk_font.h"

// One translation unit holds the implementation: stb_truetype's own, and the
// byte array, which is far too large to see twice.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "vendor/stb_truetype.h"

#include "font_cn_data.inc"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace ds::sdl {
namespace {

stbtt_fontinfo g_font{};
bool           g_ready = false;

// One rasterised glyph, placed relative to the square's *top-left*.
//
// stb measures a glyph box against the baseline: `y0` is how far the ink rises
// above it and so comes back negative for everything but a descender. The 5x7
// grid the menu is built on has no baseline at all -- a row is `y` plus seven
// pixel rows, and the leading that separates it from the next row is measured
// from there. So the ink is centred in the square here, once, rather than at
// every blit: a Han glyph fills its square and a Latin one does not, and the
// two only share a row if the tall one is pulled in off the edges.
struct Glyph {
  std::vector<u8> px;
  int w = 0, h = 0, dx = 0, dy = 0, adv = 0;
  bool ok = false;
};

// Keyed by code point *and* the square it was rasterised for: the menu redraws
// at one scale per canvas, but a window resize or a tier change draws at
// another, and a glyph rasterised for the old size is useless rather than
// merely small. Both together are still a few dozen entries in practice -- a
// page shows a screenful of text, not a corpus -- so a map with a ceiling is
// all the cache this needs.
using Key = std::pair<u32, int>;
std::map<Key, Glyph> g_cache;
// Sized so the busiest page still fills without evicting anything: a screenful
// of rows at the list size, twice over for headroom. Past it the whole thing is
// dropped rather than aged entry by entry, because at this level nothing is hot
// for long enough for the difference to show.
constexpr size_t kCacheMax = 512;

const Glyph* glyph_for(u32 cp, int box) {
  const Key key{cp, box};
  auto it = g_cache.find(key);
  if (it != g_cache.end()) return &it->second;

  Glyph g;
  const int gi = stbtt_FindGlyphIndex(&g_font, static_cast<int>(cp));
  if (gi != 0) {
    // Sized to the square by pixel height, so the ink is as large as will fit
    // and a glyph with less ink than a Han one simply comes out smaller.
    const float scale = stbtt_ScaleForPixelHeight(&g_font, static_cast<float>(box));
    // Real horizontal advance (hmtx), so a Latin glyph is not laid out at the
    // wide CJK step. Cached per (cp, box) like the rest of the Glyph.
    // GetGlyphHMetrics, not GetCodepointHMetrics: `gi` is a glyph *index*, and
    // passing it where a code point is expected reads the advance of whatever
    // glyph happens to have that number -- which is what made "H" narrower
    // than "B" the first time this was wired up.
    int ax = 0;
    stbtt_GetGlyphHMetrics(&g_font, gi, &ax, nullptr);
    g.adv = static_cast<int>(std::lround(ax * scale));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&g_font, gi, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w <= box * 2 && h <= box * 2) {
      g.w = w; g.h = h;
      // A Han glyph fills its square, so it is centred in it: its own side
      // bearings already put the ink where a square's worth of ink belongs.
      //
      // Latin is placed on a baseline instead. Centring each glyph separately
      // would be fine for caps, which are all one height, but "a" would then
      // sit lower than "b" and "g" lower still, because each would be centred
      // on its own ink -- and the name editor draws what is actually being
      // typed, lower case included. One baseline for the whole row puts them
      // back on a line: it is taken from the face's own ascent, so a capital's
      // top lands on the top of the box the way a Han glyph's does.
      if (cp >= 0x2E80) {
        g.dx = x0 + (box - w) / 2;
        g.dy = (box - h) / 2;
      } else {
        int asc = 0, desc = 0, gap = 0;
        stbtt_GetFontVMetrics(&g_font, &asc, &desc, &gap);
        g.dx = x0;
        g.dy = static_cast<int>(std::floor(asc * scale)) + y0;
      }
      g.px.assign(static_cast<size_t>(w) * h, 0);
      // Rendered with a positive y going down, which is the direction the
      // canvas counts rows in: this bitmap is blitted, not flipped.
      stbtt_MakeGlyphBitmap(&g_font, g.px.data(), w, h, w, scale, scale, gi);
      g.ok = true;
    }
  }
  if (g_cache.size() >= kCacheMax) g_cache.clear();
  it = g_cache.emplace(key, std::move(g)).first;
  return &it->second;
}

// One pixel written through the glyph's coverage. Straight alpha rather than
// the coverage-only bytes stb hands out being enough on their own: the menu
// draws in one flat colour per row, grey included, and skipping a partially
// covered pixel is what makes a small glyph look chewed.
inline void blend(u32& dst, u32 rgb, u32 a) {
  if (a == 0) return;
  const u32 inv = 255 - a;
  const u32 r = (((dst >> 16) & 0xFF) * inv + ((rgb >> 16) & 0xFF) * a + 127) / 255;
  const u32 g = (((dst >> 8) & 0xFF) * inv + ((rgb >> 8) & 0xFF) * a + 127) / 255;
  const u32 b = ((dst & 0xFF) * inv + (rgb & 0xFF) * a + 127) / 255;
  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

bool cjk_init_once() {
  if (g_ready) return true;
  // stb reads the whole file in place; the array is static, so nothing is
  // copied and nothing has to be freed.
  g_ready = stbtt_InitFont(&g_font, kMenuFontData, 0) != 0;
  return g_ready;
}

// One glyph, or the fact that there is none. Both cjk_wide and cjk_blit go
// through this, so the measurement and the drawing can never disagree about
// whether a code point is going to be wide.
bool glyph_ok(u32 cp, int box) {
  if (!cjk_init_once()) return false;
  return glyph_for(cp, box)->ok;
}

} // namespace

void cjk_init() { cjk_init_once(); }

bool cjk_ready() { return g_ready; }

bool cjk_wide(u32 cp) {
  if (!cjk_init_once()) return false;
  // Whether the face carries a glyph for this code point, in any script. This is
  // the single question draw_text asks to decide which path a code point takes;
  // with the embedded face now carrying both Latin and CJK it is true for both.
  return glyph_ok(cp, kCjkH);
}

bool cjk_has(u32 cp) { return cjk_wide(cp); }

// The blocks the layout gives a square. Everything below them comes out of the
// same face but is drawn and measured by the 5x7 grid, so this is where the
// hand-over between the two is made, and it is made once: cjk_advance_for steps
// a square at the same threshold it took to get here.
bool cjk_square(u32 cp) {
  if (!cjk_init_once()) return false;
  if (cp < 0x2E80) return false;
  return glyph_ok(cp, kCjkH);
}

// The horizontal step for `cp` at `scale`, in pixels. Han glyphs keep the wide
// step the layout was tuned for (kCjkAdvance); everything else -- Latin above
// all -- takes its real advance so an English line is not spaced as if it were
// Chinese. Returns -1 when the face has no glyph, so the caller can fall back
// to the 5x7 grid's own advance.
int cjk_advance_for(u32 cp, int scale) {
  if (!cjk_init_once()) return -1;
  if (cp >= 0x2E80) return kCjkAdvance * scale;
  const Glyph* g = glyph_for(cp, kCjkH * scale);
  if (!g || !g->ok) return -1;
  // A Latin face's advance is set for running text at a readable size, and at
  // seven pixels it leaves a word's gap narrower than its letters. One pixel of
  // letter spacing -- two on a space, which is what separates the words -- puts
  // the gap back without changing the height the two scripts now share.
  return g->adv + scale + (cp == ' ' ? scale : 0);
}

bool cjk_blit(const Canvas& d, int x, int y, int box, u32 colour, u32 cp,
              int clip_x0, int clip_x1) {
  if (!cjk_init_once()) return false;
  const Glyph* g = glyph_for(cp, box);
  if (!g->ok) return false;

  // Three bounds meet here: the glyph's own box, the caller's clips, and the
  // canvas edge. The last one is the one that matters most in practice -- a
  // Chinese line is wider than the Latin one it replaced, and the rows that
  // now run to the panel edge are drawn with clip alone.
  const u32 rgb = colour & 0x00FFFFFFu;
  const int x0 = x + g->dx, y0 = y + g->dy;
  const int lo = std::max(std::max(x0, clip_x0), 0);
  const int hi = std::min(std::min(x0 + g->w, clip_x1), d.w);
  if (lo >= hi) return true;                      // drawn, only nowhere visible
  for (int r = 0; r < g->h; ++r) {
    const int yy = y0 + r;
    if (yy < 0 || yy >= d.h) continue;
    u32* row = d.px + static_cast<size_t>(yy) * d.pitch;
    const u8* src = g->px.data() + static_cast<size_t>(r) * g->w;
    for (int c = lo - x0; c < hi - x0; ++c) blend(row[x0 + c], rgb, src[c]);
  }
  return true;
}

} // namespace ds::sdl
