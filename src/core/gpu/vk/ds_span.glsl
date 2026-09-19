// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#ifndef DS_SPAN_GLSL
#define DS_SPAN_GLSL
// The Y stage, shared by the span pass that computes it and the raster that
// reads the result. Included after the includer has declared the Polys and
// Verts buffers and the `pc` push constant, which the code below reads.
//
// Everything here is a transcription of Renderer3D (render3d.cpp) rather than
// a reimplementation: the 18-bit slope fraction, the two-stage perspective
// interpolation (9 fractional bits along Y, 8 along X), the edge fill rules
// and their swapped-edge variants are the same arithmetic in the same order.

// ---- 64-bit intermediates --------------------------------------------------
//
// Several of the hardware's expressions overflow 32 bits on the way to a
// result that fits: the CPU spells them with an s64 cast. GLSL has no 64-bit
// integer here, but umulExtended gives the full 64-bit product of two uints,
// which is all that is needed -- every one of them is (a * b) >> k.
uint mul_shr(uint a, uint b, uint k) {
  uint lo, hi;
  umulExtended(a, b, hi, lo);
  if (k == 0u) return lo;
  if (k >= 32u) return hi >> (k - 32u);
  return (lo >> k) | (hi << (32u - k));
}

// ---- the interpolator, transcribed from Renderer3D::Interp -----------------
//
// dir 1 is the Y stage (down an edge, 9 fractional bits), dir 0 the X stage
// (along a span, 8). The attribute itself is never divided per pixel: a
// perspective factor between the two endpoints is computed once and the
// attribute is then interpolated linearly by it.
struct Interp {
  int  x0, xdiff, x;
  uint shift;
  bool linear, wbuffer;
  int  xrecip_z, w0n, w0d, w1d;
  uint yfactor, recip;
  int  dir;
};

Interp interp_setup(int x0_, int x1_, int w0, int w1, bool wbuf, int dir) {
  Interp ip;
  ip.dir = dir;
  ip.x0 = x0_;
  ip.xdiff = x1_ - x0_;
  ip.wbuffer = wbuf;
  ip.x = 0;
  ip.yfactor = 0u;
  // Integer division has no hardware on this part and is the dominant cost of
  // the whole shader, so each one is computed only when something will read
  // it: xrecip_z only for Z-buffered depth, recip only for the linear
  // attribute path. `recip` is the CPU's own trick (div64.h recip_ceil32):
  // one division per span instead of one per attribute per pixel, which for
  // the five interpolated attributes is the difference between six divisions
  // a pixel and two.
  int mask = dir != 0 ? 0x7E : 0x7F;
  ip.linear = (w0 == w1) && ((w0 & mask) == 0) && ((w1 & mask) == 0);
  ip.xrecip_z = (!wbuf && ip.xdiff != 0) ? (1 << 22) / ip.xdiff : 0;
  ip.recip = (ip.linear && ip.xdiff >= 2) ? (0xFFFFFFFFu / uint(ip.xdiff) + 1u) : 0u;
  if (dir != 0) {
    ip.w0n = w0 >> 1;
    ip.w0d = (w0 + ((w0 & ~w1) & 1)) >> 1;
    ip.w1d = w1 >> 1;
    ip.shift = 9u;
  } else {
    ip.w0n = w0; ip.w0d = w0; ip.w1d = w1;
    ip.shift = 8u;
  }
  return ip;
}

void interp_set_x(inout Interp ip, int xv) {
  xv -= ip.x0;
  ip.x = xv;
  if (ip.xdiff != 0 && (!ip.linear || ip.wbuffer)) {
    uint num = uint(xv * ip.w0n) << ip.shift;
    uint den = uint(xv * ip.w0d) + uint((ip.xdiff - xv) * ip.w1d);
    // The CPU's u32 division, wrapping arithmetic included. The factor is in
    // [0, 1 << shift] for any xv inside the span; clamping it costs nothing
    // and keeps a degenerate w pair from turning into a wild multiply below.
    ip.yfactor = den == 0u ? 0u : min(num / den, 1u << ip.shift);
  }
}

int interp_val(Interp ip, int y0, int y1) {
  if (ip.xdiff == 0 || y0 == y1) return y0;
  if (!ip.linear) {
    if (y0 < y1) return y0 + int(mul_shr(uint(y1 - y0), ip.yfactor, ip.shift));
    return y1 + int(mul_shr(uint(y0 - y1), (1u << ip.shift) - ip.yfactor, ip.shift));
  }
  // Linear: d * f / xdiff, through the reciprocal when there is one. q is the
  // quotient or one too many and a single compare fixes it, exactly as the
  // CPU does it.
  uint d = uint(y0 < y1 ? y1 - y0 : y0 - y1);
  uint fa = uint(y0 < y1 ? ip.x : ip.xdiff - ip.x);
  uint n = d * fa;
  uint q;
  if (ip.recip != 0u) { q = mul_shr(n, ip.recip, 32u); if (q * uint(ip.xdiff) > n) --q; }
  else if (ip.xdiff == 1) q = n;
  else q = n / uint(ip.xdiff);
  return min(y0, y1) + int(q);
}

int interp_z(Interp ip, int z0, int z1) {
  if (ip.xdiff == 0 || z0 == z1) return z0;
  if (ip.wbuffer) {
    if (z0 < z1) return z0 + int(mul_shr(uint(z1 - z0), ip.yfactor, ip.shift));
    return z1 + int(mul_shr(uint(z0 - z1), (1u << ip.shift) - ip.yfactor, ip.shift));
  }
  // Z-buffering interpolates linearly through a reciprocal of the span.
  int base, disp, factor;
  if (z0 < z1) { base = z0; disp = z1 - z0; factor = ip.x; }
  else         { base = z1; disp = z0 - z1; factor = ip.xdiff - ip.x; }
  if (ip.dir != 0) {
    uint sh = 0u;
    while (disp > 0x3FF) { disp >>= 1; ++sh; }
    return base + int(mul_shr(uint(disp * factor), uint(ip.xrecip_z), 22u) << sh);
  }
  disp >>= 9;
  return base + int(mul_shr(uint(disp * factor), uint(ip.xrecip_z), 13u));
}

// The X stage's interpolator for a row, as the span pass left it: the two
// per-row divisions are already in the row (xrz, rcp), so setting this up is
// a handful of moves. interp_set_x then does the one per-pixel division --
// only when the row is perspective (or W-buffered), which is why callers that
// need only a Z-buffered depth may skip it and set `x` directly.
Interp row_interp(GpuRow rw) {
  Interp ix;
  ix.dir = 0; ix.shift = 8u;
  ix.x0 = rw.xstart; ix.xdiff = rw.xend + 1 - rw.xstart; ix.x = 0;
  ix.wbuffer = (rw.fl & 8u) != 0u;
  ix.linear = (rw.fl & 16u) != 0u;
  ix.w0n = rw.wl; ix.w0d = rw.wl; ix.w1d = rw.wr;
  ix.xrecip_z = rw.xrz; ix.recip = rw.rcp; ix.yfactor = 0u;
  return ix;
}

// ---- the slope, transcribed from Renderer3D::Slope -------------------------

struct Slope {
  int  x0, xmin, xmax, xlen, ylen, increment, dx;
  bool negative, xmajor;
  Interp ip;
};

// side: 0 = left edge, 1 = right edge. `y` is the scanline; the CPU reaches it
// by stepping, this evaluates it directly -- same value, since setup() already
// folds the walk into dx += (y - y0) * increment.
Slope slope_setup(int x0_, int x1_, int y0, int y1, int w0, int w1, int y, bool wbuf, int side) {
  Slope s;
  s.x0 = x0_;
  if (x1_ > x0_)      { s.xmin = x0_;    s.xmax = x1_ - 1; s.negative = false; }
  else if (x1_ < x0_) { s.xmin = x1_;    s.xmax = x0_ - 1; s.negative = true;  }
  else                { s.xmin = x0_;    s.xmax = x0_;     s.negative = false; }
  s.xlen = s.xmax + 1 - s.xmin;
  s.ylen = y1 - y0;
  if (s.ylen == 0)                              s.increment = 0;
  else if (s.ylen == s.xlen && s.xlen != 1)     s.increment = 0x40000;
  else                                          s.increment = abs((x1_ - x0_) * ((1 << 18) / s.ylen));
  s.xmajor = s.increment > 0x40000;

  int dx;
  if (side != 0) {
    if (s.xmajor)             dx = s.negative ? (0x20000 + 0x40000) : (s.increment - 0x20000);
    else if (s.increment != 0) dx = s.negative ? 0x40000 : 0;
    else                       dx = 0;
  } else {
    if (s.xmajor)             dx = s.negative ? ((s.increment - 0x20000) + 0x40000) : 0x20000;
    else if (s.increment != 0) dx = s.negative ? 0x40000 : 0;
    else                       dx = 0;
  }
  s.dx = dx + (y - y0) * s.increment;

  int interpoffset = (s.increment >= 0x40000 && ((side ^ int(s.negative)) != 0)) ? 1 : 0;
  s.ip = interp_setup(y0 - interpoffset, y1 - interpoffset, w0, w1, wbuf, 1);
  interp_set_x(s.ip, y);
  return s;
}

Slope slope_dummy(int x0_, int w, bool wbuf) {
  Slope s;
  s.x0 = x0_; s.xmin = x0_; s.xmax = x0_; s.xlen = 1; s.ylen = 0;
  s.increment = 0; s.dx = 0; s.negative = false; s.xmajor = false;
  s.ip = interp_setup(0, 0, 0, 0, wbuf, 1);
  interp_set_x(s.ip, 0);
  return s;
}

int slope_x(Slope s) {
  int x = s.negative ? s.x0 - (s.dx >> 18) : s.x0 + (s.dx >> 18);
  return clamp(x, s.xmin, s.xmax);
}

// Renderer3D::Slope::edge_params, the non-AA half: how many pixels of this
// scanline belong to the edge itself. Only X-major edges cover more than one.
int edge_len(Slope s, int side, bool swapped) {
  if (!s.xmajor) return 1;
  if (swapped) return 1;
  if ((side ^ int(s.negative)) != 0) return (s.dx >> 18) - ((s.dx - s.increment) >> 18);
  return ((s.dx + s.increment) >> 18) - (s.dx >> 18);
}

// Renderer3D::Slope::edge_params, the anti-aliasing half: the coverage of the
// edge's run on this scanline. X-major edges pack the first pixel's coverage
// and a per-pixel increment behind bit 31; Y-major edges a single 5-bit
// coverage. `len` is the raw run length the CPU computed in the same call
// (before its swapped override to 1); it only matters for the right side.
int edge_rawlen(Slope s, int side) {
  if ((side ^ int(s.negative)) != 0) return (s.dx >> 18) - ((s.dx - s.increment) >> 18);
  return ((s.dx + s.increment) >> 18) - (s.dx >> 18);
}
int edge_cov(Slope s, int side, bool swapped, int len) {
  if (s.xmajor) {
    int startx = s.dx >> 18;
    if (s.negative) startx = s.xlen - startx;
    if (side != 0) startx = startx - len + 1;
    int startcov = (((startx << 10) + 0x1FF) * s.ylen) / s.xlen;
    int xcov_incr = (s.ylen << 10) / s.xlen;
    return int(0x80000000u) | ((startcov & 0x3FF) << 12) | (xcov_incr & 0x3FF);
  }
  if (s.increment == 0) return swapped ? 0 : 31;
  int cov = ((s.dx >> 9) + (s.increment >> 10)) >> 4;
  if ((cov >> 5) != (s.dx >> 18)) cov = 31;
  cov &= 0x1F;
  if (swapped) { if ((side ^ int(s.negative)) != 0) cov = 0x1F - cov; }
  else         { if ((side ^ int(s.negative)) == 0) cov = 0x1F - cov; }
  return cov;
}

// ---- edge chains -----------------------------------------------------------

uint chain_step(uint cur, int dir, uint n) {
  if (dir > 0) { uint nx = cur + 1u; return nx >= n ? 0u : nx; }
  return cur == 0u ? n - 1u : cur - 1u;
}

// setup_left_edge / setup_right_edge: walk from vtop towards vbot until the
// edge (cur, next) spans scanline y. Bounded by nverts, which is at most 10.
void chain_at(GpuPoly p, int y, int dir, out uint cur, out uint nxt) {
  uint n = p.nverts;
  cur = p.vtop;
  nxt = chain_step(cur, dir, n);
  for (uint i = 0u; i < n; ++i) {
    if (!(y >= verts[p.first_vert + nxt].sy && cur != p.vbot)) break;
    cur = nxt;
    nxt = chain_step(cur, dir, n);
  }
}


// ---- the scanline ----------------------------------------------------------
//
// Depends on the polygon and the scanline and NOT on x, so it is computed
// once per (polygon, scanline) for the whole frame by the span pass, and the
// raster only reads it. Getting here took three tries: per pixel it ran 256
// times per tile per polygon for a handful of distinct answers, and since
// almost all of it is integer division on a part with no integer divide that
// was 54 ms a frame; hoisting it into shared memory with a barrier per
// polygon was worse at 86; batching the barriers got it to 26. All of those
// still recomputed it once per TILE, which is what this removes -- and which
// the tile-shape sweep showed was the only reason tile shape mattered.
// Thirteen words, not nineteen: the five attributes at each end pack into two
// words apiece -- three 9-bit colours into 10:10:10, two 12.4 texture
// coordinates into 16:16. Shared memory is what decides how many of these
// workgroups a shader core can hold at once, and at 256 threads a workgroup
// the difference between 19 KB and 13 KB is the difference between one
// resident workgroup and two, which is the only latency hiding there is.


uint pack_rgb(int r, int g, int b) { return uint(r) | (uint(g) << 10) | (uint(b) << 20); }
uint pack_st(int sc, int tc) { return (uint(sc) & 0xFFFFu) | (uint(tc) << 16); }
int  unpack_r(uint v) { return int(v & 0x3FFu); }
int  unpack_g(uint v) { return int((v >> 10) & 0x3FFu); }
int  unpack_b(uint v) { return int((v >> 20) & 0x3FFu); }
// Sign-extended: texture coordinates are signed 12.4 and routinely negative.
int  unpack_s(uint v) { return (int(v) << 16) >> 16; }
int  unpack_t(uint v) { return int(v) >> 16; }

GpuRow compute_row(GpuPoly p, int y) {
  GpuRow o;
  o.poly = 0u;
  o.fl = 0u;
  o.lcov = 0u; o.rcov = 0u;
  bool isflat = p.ytop == p.ybot;
  // ybot is EXCLUSIVE: a polygon draws [ytop, ybot), and a flat one draws its
  // single line at ytop (render_chunk, seed_active). A row at y == ybot used
  // to be computed here and drawn, one stray line under every polygon -- the
  // bulk of the seam residual, and tile-dependent only because the bounding
  // box cull happened to hide part of it.
  if (y < p.ytop || y > p.ybot || (y == p.ybot && !isflat)) return o;

  bool wbuf  = (p.flags & DS_PF_WBUFFER) != 0u;
  bool front = (p.flags & DS_PF_FRONTFACING) != 0u;

  Slope sl, sr;
  uint cl, nl, cr, nr;
  if (isflat) {
    // A flat polygon is one span from its leftmost to its rightmost vertex.
    // The CPU examines slots 1 and n-1 only, and so does this.
    uint vl = 0u, vr = 0u;
    uint cand[2]; cand[0] = 1u; cand[1] = p.nverts - 1u;
    for (int k = 0; k < 2; ++k) {
      uint c = cand[k];
      if (verts[p.first_vert + c].sx < verts[p.first_vert + vl].sx) vl = c;
      if (verts[p.first_vert + c].sx > verts[p.first_vert + vr].sx) vr = c;
    }
    cl = vl; nl = vl; cr = vr; nr = vr;
    sl = slope_dummy(verts[p.first_vert + cl].sx, verts[p.first_vert + cl].w, wbuf);
    sr = slope_dummy(verts[p.first_vert + cr].sx, verts[p.first_vert + cr].w, wbuf);
  } else {
    chain_at(p, y, front ?  1 : -1, cl, nl);
    chain_at(p, y, front ? -1 :  1, cr, nr);
    GpuVert a = verts[p.first_vert + cl], b = verts[p.first_vert + nl];
    sl = slope_setup(a.sx, b.sx, a.sy, b.sy, a.w, b.w, y, wbuf, 0);
    a = verts[p.first_vert + cr]; b = verts[p.first_vert + nr];
    sr = slope_setup(a.sx, b.sx, a.sy, b.sy, a.w, b.w, y, wbuf, 1);
  }

  int xstart = slope_x(sl), xend = slope_x(sr);

  GpuVert vcl = verts[p.first_vert + cl], vnl = verts[p.first_vert + nl];
  GpuVert vcr = verts[p.first_vert + cr], vnr = verts[p.first_vert + nr];

  int wl = interp_val(sl.ip, vcl.w, vnl.w);
  int wr = interp_val(sr.ip, vcr.w, vnr.w);
  int zl = interp_z  (sl.ip, vcl.z, vnl.z);
  int zr = interp_z  (sr.ip, vcr.z, vnr.z);

  bool l_incr0 = sl.increment == 0, r_incr0 = sr.increment == 0;
  // Right vertical edges are pushed one pixel left unless the span is a
  // single pixel at the screen's left edge.
  if (r_incr0 && (!l_incr0 || xstart != xend) && xend != 0) --xend;

  bool bottom_fill = (y == p.ybot - 1) && (vnl.sx != vnr.sx);
  // Edge marking (and anti-aliasing) make every edge fill, which is why this
  // reads DISP3DCNT and not only the polygon: turning edge marking on changes
  // the fill rules, not just the final pass.
  bool always_fill = (pc.f.dispcnt & ((1u << 4) | (1u << 5))) != 0u ||
                     ((((p.attr >> 16) & 0x1Fu) < 31u) && ((pc.f.dispcnt & 8u) != 0u));

  // Which end of the span each edge owns, and whether that end fills. The
  // swapped case is the hardware walking the edges backwards, which changes
  // both the lengths and the rules.
  int l_len, r_len;
  bool l_fill, r_fill;
  Interp istart, iend;
  GpuVert alv, alw, arv, arw;
  bool aa = (pc.f.dispcnt & 16u) != 0u;
  int l_cov = 0, r_cov = 0;
  if (xstart > xend) {
    alv = vcr; alw = vnr; arv = vcl; arw = vnl;
    istart = sr.ip; iend = sl.ip;
    if (aa) { l_cov = edge_cov(sr, 1, true, edge_rawlen(sr, 1)); r_cov = edge_cov(sl, 0, true, 0); }
    l_len = edge_len(sr, 1, true);
    r_len = edge_len(sl, 0, true);
    int tmp = xstart; xstart = xend; xend = tmp;
    tmp = wl; wl = wr; wr = tmp;
    tmp = zl; zl = zr; zr = tmp;
    if (always_fill) { l_fill = true; r_fill = true; }
    else {
      l_fill = (sr.negative || !sr.xmajor) || (bottom_fill && sr.xmajor);
      r_fill = (!sl.negative && sl.xmajor) || (!(sl.negative && sl.xmajor) && r_incr0) || (bottom_fill && sl.xmajor);
    }
  } else {
    alv = vcl; alw = vnl; arv = vcr; arw = vnr;
    istart = sl.ip; iend = sr.ip;
    if (aa) { l_cov = edge_cov(sl, 0, false, edge_rawlen(sl, 0)); r_cov = edge_cov(sr, 1, false, edge_rawlen(sr, 1)); }
    l_len = edge_len(sl, 0, false);
    r_len = edge_len(sr, 1, false);
    if (always_fill) { l_fill = true; r_fill = true; }
    else {
      l_fill = (sl.negative || !sl.xmajor) || (bottom_fill && sl.xmajor) ||
               ((sl.increment == sr.increment) && (xstart + l_len == xend + 1));
      r_fill = (!sr.negative && sr.xmajor) || r_incr0 || (bottom_fill && sr.xmajor);
    }
  }

  // The X stage's setup depends on the span, not on x: its two divisions are
  // per row, not per pixel, and this is where they belong. What is left in
  // the pixel is interp_set_x's single num/den, which really is per pixel.
  Interp ix = interp_setup(xstart, xend + 1, wl, wr, wbuf, 0);

  int W = 256 * int(pc.f.scale);
  o.xrz = ix.xrecip_z;
  o.rcp = ix.recip;
  o.lcov = uint(l_cov); o.rcov = uint(r_cov);
  o.xstart = xstart;
  o.xend   = xend;
  o.lim0   = min(min(xstart + l_len, xend + 1), W);
  o.lim1   = min(min(xend - r_len + 1, xend + 1), W);
  o.wl = wl; o.wr = wr;
  o.zl = zl; o.zr = zr;
  o.lrgb = pack_rgb(interp_val(istart, alv.r, alw.r),
                           interp_val(istart, alv.g, alw.g),
                           interp_val(istart, alv.b, alw.b));
  o.lst  = pack_st (interp_val(istart, alv.s, alw.s),
                           interp_val(istart, alv.t, alw.t));
  o.rrgb = pack_rgb(interp_val(iend, arv.r, arw.r),
                           interp_val(iend, arv.g, arw.g),
                           interp_val(iend, arv.b, arw.b));
  o.rst  = pack_st (interp_val(iend, arv.s, arw.s),
                           interp_val(iend, arv.t, arw.t));
  // Top and bottom edge flags for this scanline of this polygon (0x4 / 0x8),
  // as stage_line's yedge.
  uint yedge = (y == p.ytop) ? 4u : ((y == p.ybot - 1) ? 8u : 0u);
  o.fl = 1u | (l_fill ? 2u : 0u) | (r_fill ? 4u : 0u) | (wbuf ? 8u : 0u) |
         (ix.linear ? 16u : 0u) | (yedge << 5) |
         ((p.flags & DS_PF_SHADOW_MASK) != 0u ? 512u : 0u) |
         ((p.flags & DS_PF_SHADOW) != 0u ? 1024u : 0u);
  return o;
}


#endif // DS_SPAN_GLSL
