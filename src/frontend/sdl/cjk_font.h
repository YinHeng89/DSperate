// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"
#include "frontend/sdl/menu.h"

namespace ds::sdl {

// The menu's own font is a 5x7 grid (menu.cpp), which has room for an ASCII
// capital and nothing like room for a Han character. These calls carry the
// code points it has no glyph for, and exist so menu.cpp keeps doing the
// laying out: what it gets back is one advance, the same way ASCII leaves it
// one `kAdvance * scale` further along.
//
// The face travels in the binary (font_cn_data.inc) and is rasterised on
// demand. A compiled-in bitmap font was the other candidate, but a CJK grid
// needs 12x12 cells where ASCII takes 5x7, and a row is one height for every
// script: writing Chinese would have pushed the glyph scale down two steps
// (6 -> 4 on a 960x720 panel) and cost a scrolling list five of its twelve
// visible rows. Rasterising at the size actually being drawn costs nothing on
// the panel and keeps every measurements a remotely configured row has ever
// had. Glyphs are cached, and nothing in this file is touched while a game is
// running -- it is only ever reached from the menu drawing itself.

// Loads the face. Idempotent, and safe to skip: every call below answers false
// or draws nothing until it has run once.
void   cjk_init();
bool   cjk_ready();
// True when the embedded face carries a glyph for `cp`, in any script.
bool   cjk_wide(u32 cp);

// True when the layout steps for `cp` as a square, which is the *only* question
// text_width and draw_text both ask, and so the one thing that keeps the two
// from disagreeing: a square the measurement reserved is a square the drawing
// fills, and a code point neither took does not leave a hole.
//
// The face carries Latin as well -- the subset was cut from the strings the menu
// draws, ASCII included -- but a Latin letter is not drawn here. It keeps the
// 5x7 grid's own glyph and its own advance, so an English line is unchanged in
// width and in look. Only from the CJK blocks onward does a code point take the
// square, where a 12x12 cell is the least a reader needs and the 5x7 grid has
// no glyph at all.
bool   cjk_square(u32 cp);

// True when the face carries this code point. Anything else -- kana, hangul, a
// symbol nobody put in the subset -- falls back to the 5x7 '?', which is what
// the menu drew for those before there was a Chinese font at all.
bool   cjk_has(u32 cp);

// How tall one ideograph is drawn, and how far to step for the next one. Both
// come from the caller's glyph scale so a mixed row (values and units are
// still ASCII) lines up without anyone knowing which is which.
//
// The height is the 5x7 grid's own kGlyphH, not a number of its own. Every
// measurement in menu.cpp is built on that height -- row_h is kGlyphH * s plus
// two scales of leading, and the leading below a line is what a descender
// lives in -- so a taller box would eat that leading and set the next row's
// text off against the bottom of this one. Matching it means a page of Chinese
// has the rows a page of English had.
//
// The step is one unit wider than the box: a Chinese line has to read as
// characters, where a Latin one reads as words and can be allowed to run
// together, and Han glyphs are drawn out to the edges of their square.
constexpr int kCjkH = 7, kCjkAdvance = 9;
inline int cjk_box(int scale) { return kCjkH * scale; }
inline int cjk_advance(int scale) { return kCjkAdvance * scale; }

// The horizontal step for `cp` at `scale` in pixels. Han glyphs keep
// kCjkAdvance; Latin and everything else take their real advance. Returns -1
// when the face has no glyph, so the caller can fall back to kAdvance.
int cjk_advance_for(u32 cp, int scale);

// Draws one code point with its top-left at (x, y) in a `box` pixel square and
// leaves `d` clipped to [clip_x0, clip_x1), which is what lets a row scroll
// under the panel edge the way the ASCII path does. Returns false and draws
// nothing when there is nothing to draw, so the caller can fall back.
bool   cjk_blit(const Canvas& d, int x, int y, int box, u32 colour, u32 cp,
                int clip_x0, int clip_x1);

} // namespace ds::sdl
