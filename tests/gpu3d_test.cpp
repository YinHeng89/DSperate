// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// 3D engine tests: command FIFO and status, matrix stack, position test, and
// an end-to-end quad through the geometry engine and rasteriser.
#include "core/nds.h"

#include <cstdio>

using namespace ds;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (static_cast<unsigned long long>(va_) != static_cast<unsigned long long>(vb_)) { std::fprintf(stderr, "FAIL %s:%d: %s = %llx, expected %llx\n", __FILE__, __LINE__, #a, (unsigned long long)va_, (unsigned long long)vb_); ++failures; } } while (0)

static void w32(NDS& nds, u32 a, u32 v) { nds.bus.dma_write32(Cpu::ARM9, a, v); }
static u32  r32(NDS& nds, u32 a) { return nds.bus.dma_read32(Cpu::ARM9, a); }
static void w16(NDS& nds, u32 a, u16 v) { nds.bus.dma_write16(Cpu::ARM9, a, v); }

// Command port addresses.
constexpr u32 CMD(u32 c) { return 0x04000400 + c * 4; }

static void power_on(NDS& nds) { w16(nds, 0x04000304, 0x820F); }
// Run the geometry engine for `cycles` system cycles.
static void advance(NDS& nds, u32 cycles) { nds.sched.run_until(nds.sched.now() + cycles * 2); }

// The synthesised GXSTAT (Phase 1a). There is no FIFO level, no stall and no
// execution clock: a read replays whatever is queued and then answers from a
// fixed, deliberately safe shape. This is the contract §3.4 says must not be
// weakened, so it is asserted directly rather than inferred from a drain.
static void test_gxstat_synthesis() {
  NDS nds; power_on(nds);
  auto st = [&] { return r32(nds, 0x04000600); };
  // Empty, below half full, not busy -- before anything is queued.
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
  CHECK(!(st() & (1u << 27)));
  // 300 commands: more than the old 4-deep pipe plus 256-entry FIFO plus
  // 64-entry stall queue could hold. The level still reads zero, the
  // half-full and empty bits still read set, and nothing stalls.
  for (int i = 0; i < 300; ++i) w32(nds, CMD(0x10), 0);
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
  advance(nds, 5000);
  CHECK_EQ(st() & 0x07FF0000, 0x06000000u);
}

// The busy bit (27) is the one piece of GXSTAT that is not constant: it is
// held from SWAP_BUFFERS until 650 cycles past the swap, so a game polling
// "has my swap landed?" sees a busy -> idle edge at a plausible time rather
// than an immediate idle.
static void test_gxstat_swap_busy() {
  NDS nds; power_on(nds);
  CHECK(!(r32(nds, 0x04000600) & (1u << 27)));
  w32(nds, CMD(0x50), 0);                                      // swap buffers
  CHECK(r32(nds, 0x04000600) & (1u << 27));                    // busy from the command
  // VBlank is where swap_wait_ becomes a deadline and the 650-cycle tail
  // starts; measured, the bit drops between 0.8 M and 1.0 M ARM9 cycles from
  // a cold boot, so run well past that rather than up against it.
  advance(nds, 600000);                                        // past VBlank and the 650-cycle tail
  CHECK(!(r32(nds, 0x04000600) & (1u << 27)));
}

static void test_matrix_stack() {
  NDS nds; power_on(nds);
  w32(nds, CMD(0x10), 1);                                      // position matrix
  w32(nds, CMD(0x15), 0);                                      // identity
  w32(nds, CMD(0x1C), 0x3000); w32(nds, CMD(0x1C), 0x2000); w32(nds, CMD(0x1C), 0x1000);   // translate (3,2,1)
  w32(nds, CMD(0x11), 0);                                      // push
  w32(nds, CMD(0x1B), 0x2000); w32(nds, CMD(0x1B), 0x2000); w32(nds, CMD(0x1B), 0x2000);   // scale 2
  advance(nds, 1000);
  CHECK_EQ((r32(nds, 0x04000600) >> 8) & 0x1F, 1u);            // stack pointer
  CHECK_EQ(r32(nds, 0x04000640), 0x2000u);                     // clip matrix = proj(identity) * pos
  CHECK_EQ(r32(nds, 0x04000670), 0x3000u);                     // translation survives the scale
  w32(nds, CMD(0x12), 1);                                      // pop
  advance(nds, 1000);
  CHECK_EQ((r32(nds, 0x04000600) >> 8) & 0x1F, 0u);
  CHECK_EQ(r32(nds, 0x04000640), 0x1000u);
  // Position test of (1,1,1) through the translated matrix.
  w32(nds, CMD(0x71), 0x10001000); w32(nds, CMD(0x71), 0x1000);
  advance(nds, 1000);
  CHECK_EQ(r32(nds, 0x04000620), 0x4000u);
  CHECK_EQ(r32(nds, 0x04000624), 0x3000u);
  CHECK_EQ(r32(nds, 0x04000628), 0x2000u);
  CHECK_EQ(r32(nds, 0x0400062C), 0x1000u);
}

// Orthographic projection so clip-space X/Y map directly to the screen.
static void ortho(NDS& nds) {
  w32(nds, CMD(0x10), 0);
  w32(nds, CMD(0x15), 0);
  w32(nds, CMD(0x10), 1);
  w32(nds, CMD(0x15), 0);
  w32(nds, CMD(0x60), 0xBFFF0000);                             // viewport 0,0 - 255,191
}

static void test_flat_quad() {
  NDS nds; power_on(nds);
  w32(nds, 0x04000060, 0);                                     // no texturing
  w32(nds, 0x04000350, 0x001F0000);                            // clear colour: black, alpha 31
  ortho(nds);
  w32(nds, CMD(0x29), 0x001F00C0);                             // alpha 31, both faces
  w32(nds, CMD(0x2A), 0);
  w32(nds, CMD(0x40), 1);                                      // quads
  w32(nds, CMD(0x20), 0x001F);                                 // red
  // A quad covering clip x in [-0.5, 0.5], y in [-0.5, 0.5] at z = 0 (w = 1).
  auto v = [&](s16 x, s16 y) { w32(nds, CMD(0x25), (static_cast<u16>(y) << 16) | static_cast<u16>(x)); };
  v(-0x800, -0x800); v(0x800, -0x800); v(0x800, 0x800); v(-0x800, 0x800);   // z stays 0 from reset
  w32(nds, CMD(0x41), 0);
  advance(nds, 3000);
  CHECK_EQ(r32(nds, 0x04000604), 0x00040001u);                 // one polygon, four vertices
  w32(nds, CMD(0x50), 0);                                      // swap buffers
  // Run to the frame after next so the polygon is flushed, rendered and displayed.
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME * 2);
  const u32* line = nds.gpu3d.line(nds.gpu3d.frame_ref(), 96);
  CHECK_EQ(line[128] & 0x1F00003F, 0x1F00003Fu);              // centre: red, opaque
  CHECK_EQ(line[10] >> 24, 0x1Fu);                              // outside the quad: clear colour
  CHECK_EQ(line[10] & 0xFFFFFF, 0u);
  // Quad spans x 64..191 on the 256-wide viewport.
  CHECK_EQ(line[64] & 0x3F, 0x3Fu);
  CHECK_EQ(line[63] & 0x3F, 0u);
  CHECK_EQ(line[191] & 0x3F, 0x3Fu);
  CHECK_EQ(line[192] & 0x3F, 0u);
}

// video.aa = enhanced (Renderer3D::set_aa(2)). A red shape at z 0 over a blue
// quad at z 0.5, DS AA on; returns how many pixels mix the two (red and blue
// both present) in a window around the shape. `dup`: the red polygons are
// drawn twice, as a back face under its front face would be. `quad`: the red
// shape is two triangles sharing the diagonal x + y = 0 (a square), not one.
static u32 mixed_pixels(int aa, bool dup, bool quad, u32* inner_blue = nullptr, u64* hash = nullptr, bool two_tone = false) {
  NDS nds; power_on(nds);
  nds.gpu3d.renderer().set_aa(aa);
  w32(nds, 0x04000060, 1u << 4);                               // DS anti-aliasing, no texturing
  w32(nds, 0x04000350, 0x001F0000);
  ortho(nds);
  auto v16 = [&](s16 x, s16 y, s16 z) { w32(nds, CMD(0x23), (static_cast<u32>(static_cast<u16>(y)) << 16) | static_cast<u16>(x)); w32(nds, CMD(0x23), static_cast<u16>(z)); };
  w32(nds, CMD(0x29), 0x011F00C0);                             // id 1, alpha 31, both faces
  w32(nds, CMD(0x2A), 0);
  w32(nds, CMD(0x40), 1);                                      // quads
  w32(nds, CMD(0x20), 0x7C00);                                 // blue
  v16(-0xF00, -0xF00, 0x800); v16(0xF00, -0xF00, 0x800); v16(0xF00, 0xF00, 0x800); v16(-0xF00, 0xF00, 0x800);
  w32(nds, CMD(0x41), 0);
  for (int n = 0; n < (dup ? 2 : 1); ++n) {
    w32(nds, CMD(0x29), 0x001F00C0);                           // id 0
    w32(nds, CMD(0x40), 0);                                    // triangles
    w32(nds, CMD(0x20), 0x001F);                               // red
    v16(-0x800, -0x800, 0); v16(0x800, -0x800, 0); v16(-0x800, 0x800, 0);
    if (quad) {
      if (two_tone) w32(nds, CMD(0x20), 0x03E0);                 // the second triangle green
      v16(0x800, -0x800, 0); v16(0x800, 0x800, 0); v16(-0x800, 0x800, 0);
    }
    w32(nds, CMD(0x41), 0);
  }
  advance(nds, 6000);
  w32(nds, CMD(0x50), 0);
  nds.sched.run_until(nds.sched.now() + CYCLES_PER_FRAME * 2);
  u32 mixed = 0, blue_in = 0;
  u64 h = 1469598103934665603ull;
  for (int y = 40; y < 152; ++y) {
    const u32* line = nds.gpu3d.line(nds.gpu3d.frame_ref(), static_cast<u32>(y));
    for (int x = 0; x < 256; ++x) h = (h ^ line[x]) * 1099511628211ull;
    for (int x = 56; x < 200; ++x) {
      const u32 c = line[x], r = c & 0x3F, b = (c >> 16) & 0x3F;
      if (r && b) ++mixed;
      if (quad && b && x > 68 && x < 186 && y > 52 && y < 140) ++blue_in;   // well inside the square
    }
  }
  if (inner_blue) *inner_blue = blue_in;
  if (hash) *hash = h;
  return mixed;
}

static void test_aa_enhanced() {
  const u32 acc = mixed_pixels(1, false, false);
  const u32 acc_dup = mixed_pixels(1, true, false);
  const u32 enh_dup = mixed_pixels(2, true, false);
  // The DS blends a lone silhouette, loses it under a repeat of its own
  // surface; enhanced keeps it.
  CHECK(acc > 40);
  CHECK(acc_dup * 4 < acc);
  CHECK(enh_dup * 5 >= acc * 4);
  CHECK_EQ(mixed_pixels(2, false, false), acc);                // nothing to change without a repeat
  // Two triangles of one square: the shared diagonal shows no blue.
  u32 in_acc = 0, in_enh = 0;
  mixed_pixels(1, false, true, &in_acc);
  mixed_pixels(2, false, true, &in_enh);
  CHECK_EQ(in_acc, 0u);
  CHECK_EQ(in_enh, 0u);
  // Where two polygons of a surface meet from opposite sides the hardware's
  // blend of the two is right (a red/green square's diagonal): enhanced
  // leaves it exactly as accurate draws it.
  u64 h_acc = 0, h_enh = 0;
  mixed_pixels(1, false, true, nullptr, &h_acc, true);
  mixed_pixels(2, false, true, nullptr, &h_enh, true);
  CHECK_EQ(h_enh, h_acc);
  std::printf("aa: silhouette mixed px accurate %u, repeated %u, enhanced repeated %u\n", acc, acc_dup, enh_dup);
}

static void test_final_pass() {
  NDS nds; power_on(nds);
  // Each pass alone (AA, edge marking, fog with and without colour), then all together.
  static const u32 modes[] = {1u << 4, 1u << 5, 1u << 7, (1u << 7) | (1u << 6), (1u << 4) | (1u << 5) | (1u << 7)};
  for (u32 m : modes) for (u32 seed = 0; seed < 4; ++seed) {
    const u32 d = nds.gpu3d.renderer().selftest_final_pass(seed, m);
    if (d) std::fprintf(stderr, "final_pass dispcnt %x seed %u: %u diffs\n", m, seed, d);
    CHECK_EQ(d, 0u);
  }
}

int main() {
  test_gxstat_synthesis();
  test_gxstat_swap_busy();
  test_matrix_stack();
  test_flat_quad();
  test_final_pass();
  test_aa_enhanced();
  if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  std::puts("gpu3d: ok");
  return 0;
}
