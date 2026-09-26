// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// vk_probe DUMP [iterations] [out.ppm]: draws a recorded frame (headless
// --dump-gpu-frame, vk_dump.h) on the GPU raster over and over and reports
// the cost, so a device can be measured without the emulator around it.
// DS_VK_MSAA=1 draws with 4x MSAA, DS_VK_MODE=compute takes the compute
// path, DS_VK_TIMING=1 adds the GPU's own per-pass times.
#include "core/gpu/vk/vk_device.h"
#include "core/gpu/vk/vk_dump.h"
#include "core/gpu/vk/vk_raster.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace ds;
using namespace ds::gpu::vk;

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: vk_probe DUMP [iterations] [out.ppm]\n"); return 2; }
  const int iters = argc > 2 ? std::atoi(argv[2]) : 200;
  const char* ppm = argc > 3 ? argv[3] : nullptr;

  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
  GpuDumpHeader h;
  if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != GpuDumpHeader::kMagic) { std::fprintf(stderr, "not a gpu frame dump\n"); return 1; }
  std::vector<GpuPoly> gp(h.npoly); std::vector<GpuVert> gv(h.nvert); std::vector<u32> tex(h.ntexels), shrun(static_cast<size_t>(h.npoly) * DS_SHRUN_LINES), rowpoly(h.nrows);
  auto rd = [&](void* p, size_t n) { return n == 0 || std::fread(p, n, 1, f) == 1; };
  if (!rd(gp.data(), sizeof(GpuPoly) * gp.size()) || !rd(gv.data(), sizeof(GpuVert) * gv.size()) || !rd(tex.data(), sizeof(u32) * tex.size()) ||
      !rd(shrun.data(), sizeof(u32) * shrun.size()) || !rd(rowpoly.data(), sizeof(u32) * rowpoly.size())) { std::fprintf(stderr, "short dump\n"); return 1; }
  std::fclose(f);
  // Experiment switches: DS_PROBE_DISPCNT=hex replaces DISP3DCNT (bit 4 AA, bit 5 edge
  // marking, bit 7 fog); DS_PROBE_NOTEX=1 draws every polygon untextured.
  if (const char* e = std::getenv("DS_PROBE_DISPCNT")) h.f.dispcnt = static_cast<u32>(std::strtoul(e, nullptr, 16));
  if (const char* e = std::getenv("DS_PROBE_NOTEX"); e && std::atoi(e)) for (auto& p : gp) p.flags &= ~(DS_PF_TEXTURED | DS_PF_TEX_ALPHA);
  std::printf("frame %llu: %u polygons (%u opaque prefix), %u vertices, %u texel words, %u rows, dispcnt %04x%s\n",
              (unsigned long long)h.frame, h.npoly, h.f.first_ordered, h.nvert, h.ntexels, h.nrows, h.f.dispcnt, (h.f.flags & DS_FF_WBUFFER) ? ", W-buffer" : "");

  std::string why;
  std::shared_ptr<Device> dev = Device::shared(&why);
  if (!dev) { std::fprintf(stderr, "no Vulkan device: %s\n", why.c_str()); return 1; }
  std::printf("device: %s (graphics %d, msaa4 %d, int64 atomics %d, timestamps %s)\n", dev->name().c_str(), (int)dev->limits().graphics, (int)dev->limits().msaa4,
              (int)dev->limits().int64_atomics, dev->limits().timestamp_period_ns > 0 ? "yes" : "no");
  std::unique_ptr<Raster> r = Raster::create(*dev, 1, &why);
  if (!r || !r->ready()) { std::fprintf(stderr, "raster unavailable: %s\n", why.c_str()); return 1; }
  std::printf("raster: %s path%s\n", r->tri() ? "triangle" : "compute", r->timing() ? ", timing on" : "");

  auto upload = [&]() {
    std::memcpy(r->poly_buffer(), gp.data(), sizeof(GpuPoly) * gp.size());
    std::memcpy(r->vert_buffer(), gv.data(), sizeof(GpuVert) * gv.size());
    u32 cap = 0; u32* t = r->texel_buffer(&cap);
    if (tex.size() > cap) { std::fprintf(stderr, "texel arena too small (%zu > %u)\n", tex.size(), cap); std::exit(1); }
    std::memcpy(t, tex.data(), sizeof(u32) * tex.size());
    *r->post_buffer() = h.post;
    std::memcpy(r->shadow_run_buffer(), shrun.data(), sizeof(u32) * shrun.size());
    std::memcpy(r->rowpoly_buffer(), rowpoly.data(), sizeof(u32) * rowpoly.size());
  };

  // Warm-up: pipelines, first-use allocations.
  upload();
  if (!r->submit(h.npoly, h.nvert, h.ntexels, h.f)) { std::fprintf(stderr, "submit refused\n"); return 1; }
  r->wait();

  std::vector<double> ms; ms.reserve(iters);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    const auto a = std::chrono::steady_clock::now();
    upload();
    if (!r->submit(h.npoly, h.nvert, h.ntexels, h.f)) { std::fprintf(stderr, "submit refused at %d\n", i); return 1; }
    r->wait();
    ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
  }
  const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::sort(ms.begin(), ms.end());
  auto q = [&](double p) { return ms[std::min(ms.size() - 1, static_cast<size_t>(p * ms.size()))]; };
  std::printf("submit+wait per frame: median %.2f ms, p90 %.2f, p99 %.2f, max %.2f (%d frames, %.1f ms total)\n", q(0.5), q(0.9), q(0.99), ms.back(), iters, total);
  if (r->timing()) {
    const Raster::PassTimes& t = r->pass_times();
    if (t.frames) {
      const char* names[] = {"span", "bin", "vis", "raster", "post"};
      std::printf("GPU time per frame (%llu frames):", (unsigned long long)t.frames);
      for (u32 p = 0; p < Raster::P_COUNT; ++p) std::printf(" %s %.2f ms", names[p], static_cast<double>(t.ns[p]) / 1e6 / static_cast<double>(t.frames));
      std::printf("\n");
    }
  }
  if (ppm) {
    const u32* out = r->output();
    std::FILE* o = std::fopen(ppm, "wb");
    if (!o) { std::fprintf(stderr, "cannot write %s\n", ppm); return 1; }
    std::fprintf(o, "P6\n256 192\n255\n");
    for (u32 i = 0; i < 256 * 192; ++i) {
      const u32 c = out[i];
      const unsigned char px[3] = {static_cast<unsigned char>((c & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 8) & 0x3F) * 255 / 63), static_cast<unsigned char>(((c >> 16) & 0x3F) * 255 / 63)};
      std::fwrite(px, 1, 3, o);
    }
    std::fclose(o);
    std::printf("wrote %s\n", ppm);
  }
  return 0;
}
