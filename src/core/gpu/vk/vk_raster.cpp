// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_raster.h"

#include "core/gpu/vk/vk_internal.h"
#include "core/gpu/vk/vk_shaders.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ds::gpu::vk {

namespace {

// The texel arena. The DS can address 512 KB of texture VRAM, but the cache
// decodes to one 32-bit word per texel, so a frame's worth of distinct
// decoded textures is bounded by the number of polygons rather than by VRAM.
// 8 MB is two million texels, which no real frame approaches; a frame that
// did would be refused rather than truncated.
constexpr u32 kTexelWords = 2u * 1024 * 1024;

constexpr size_t kTilesBytes =
  sizeof(u32) * DS_TILE_COUNT + sizeof(u32) * 4 +
  3 * sizeof(u32) * static_cast<size_t>(DS_TILE_COUNT) * DS_TILE_POLYS;   // list, bias, yrng
// Only the counters and the overflow word are cleared each frame; the lists
// themselves are written before they are read.
constexpr size_t kTilesHeader = sizeof(u32) * DS_TILE_COUNT + sizeof(u32) * 4;

enum Binding { B_POLYS = 0, B_VERTS = 1, B_TILES = 2, B_TEXELS = 3, B_OUT = 4,
               B_POST = 5, B_DEPTH = 6, B_ATTR = 7, B_SHCLEAR = 8, B_ROWS = 9, B_KEYS = 10, B_ROWPOLY = 11, B_COUNT = 12 };

// DS_VK_VIS: 0 sends the whole list through the ordered loop, as before the
// visibility pass existed. Attribution and a safety valve, not a feature: the
// two settings draw the same picture, and the A/B harness is what says so.
bool vis_wanted() {
  static const bool v = [] { const char* e = std::getenv("DS_VK_VIS"); return !e || std::atoi(e) != 0; }();
  return v;
}

// DS_VK_STAGES: which passes a frame runs -- 1 binning, 2 span + raster, 4
// the visibility pass, 8 the final pass; 15 (the default) all of them. Attribution, not a
// feature: the frame's GPU time is one fence wait, and the only way to learn
// which dispatch it is spent in is to leave one out. With bit 4 clear the
// raster finds every key untouched and shades the clear, so the difference
// against the full frame is the visibility pass's own cost. A frame drawn
// with any bit clear is wrong on purpose, so it is never something a user
// turns on.
constexpr u32 kMaxBands = 16;

// DS_VK_BANDS: completion checkpoints per frame. Must divide the tile rows,
// so it is snapped down to the nearest divisor rather than rejected.
//
// Eight, measured (Etrian Odyssey, RG DS Plus, composite wait per frame):
// 9.358 / 8.990 / 8.883 / 8.415 / 8.643 / 8.688 ms at 1 / 2 / 4 / 8 / 12 / 16.
// A shallow minimum, and a far smaller win than the reasoning predicted --
// see the note in vk_raster.h about why.
u32 band_count() {
  static const u32 n = [] {
    const char* e = std::getenv("DS_VK_BANDS");
    long v = e ? std::atol(e) : 8;
    if (v < 1) v = 1;
    if (v > static_cast<long>(kMaxBands)) v = kMaxBands;
    if (v > DS_TILES_Y) v = DS_TILES_Y;
    while (DS_TILES_Y % v) --v;        // must divide the tile rows evenly
    return static_cast<u32>(v);
  }();
  return n;
}

// DS_VK_TIMING: stamp every dispatch with a GPU timestamp and accumulate the
// time per pass (Raster::pass_times). Attribution: it adds a barrier between
// the binning and span passes, which otherwise overlap, so that each stamp
// closes exactly one dispatch.
bool timing_wanted() {
  static const bool v = [] { const char* e = std::getenv("DS_VK_TIMING"); return e && std::atoi(e) != 0; }();
  return v;
}

int stages() {
  static const int s = [] {
    const char* e = std::getenv("DS_VK_STAGES");
    return e ? std::atoi(e) : 15;
  }();
  return s;
}

} // namespace

struct Raster::Impl {
  Device* dev = nullptr;
  const DeviceInternal* vk = nullptr;
  const Api* api = nullptr;

  Buffer polys, verts, tiles, texels, post, shclear, rows;
  Buffer keys;               // the visibility pass's per-pixel owner keys, 64-bit
  Buffer rowpoly;            // each span row's polygon, written by the host with the table layout
  Buffer depth, attr;        // the depth and attribute planes: the resolve pass writes them, the raster reads them
  // Three, so that a deferred composite can read frame N-2 while N-1 is in
  // flight and N is about to be submitted. Two sufficed while the compositor
  // read the frame just drawn.
  Buffer out[3];
  Buffer host_textures;      // an imported arena, when one is ever bound

  VkShaderModule       mod_bin = VK_NULL_HANDLE, mod_raster = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout     layout = VK_NULL_HANDLE;
  VkPipeline           pipe_bin = VK_NULL_HANDLE, pipe_raster = VK_NULL_HANDLE, pipe_post = VK_NULL_HANDLE, pipe_span = VK_NULL_HANDLE;
  VkPipeline           pipe_vis = VK_NULL_HANDLE, pipe_raster_vis = VK_NULL_HANDLE;
  VkShaderModule       mod_post = VK_NULL_HANDLE, mod_span = VK_NULL_HANDLE;
  VkShaderModule       mod_vis = VK_NULL_HANDLE, mod_raster_vis = VK_NULL_HANDLE;
  VkDescriptorPool     pool = VK_NULL_HANDLE;
  VkDescriptorSet      set[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkCommandPool        cmdpool = VK_NULL_HANDLE;
  VkCommandBuffer      cmd[3][kMaxBands + 1]{};
  VkFence              fence[kMaxBands + 1]{};

  // Timestamps, when on: per frame in flight, two per pass plus two per
  // raster band. Slot layout: 0-1 bin, 2-3 span, 4-5 vis, 6-7 post,
  // 8 + 2b .. 9 + 2b band b's raster.
  VkQueryPool          qpool[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  u32                  qcount = 0;
  bool                 timing = false;
  bool                 stamped[3] = {false, false, false};   // that frame wrote stamps worth reading
  double               tick_ns = 0;
  Raster::PassTimes    times;

  u64  gen = 0;              // frames submitted; the buffer is gen % 3
  u32  bands = 1;
  bool post_frame = false;   // the frame in flight ends with the final pass
  u32  in_flight = 0;        // bitmask of bands submitted and not yet waited for (bit `bands` is the final pass)
  std::mutex wait_mutex;     // waited from the compositor and the emulation thread
};

std::unique_ptr<Raster> Raster::create(Device& dev, std::string* why) {
  auto set_why = [&](const char* m) { if (why) *why = m; return nullptr; };

  const DeviceInternal* vk = dev.internal();
  if (!vk || !vk->api) return set_why("no Vulkan context");

  std::unique_ptr<Raster> self(new Raster());
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.dev = &dev;
  d.vk = vk;
  d.api = vk->api;
  self->scale_ = 1;

  // The tile shape the pixel pipeline uses has to fit the device; check it
  // here so a part with a smaller limit fails at startup rather than in the
  // middle of a scene.
  if (dev.limits().max_workgroup_invocations < DS_TILE_W * DS_TILE_H)
    return set_why("workgroup invocations below 16x16");

  const size_t out_bytes = static_cast<size_t>(256 * self->scale_) *
                           static_cast<size_t>(192 * self->scale_) * sizeof(u32);

  // The 3D layer, one per frame in flight. Access::CpuRead is not a
  // preference: the NEON composite reads this buffer directly.
  for (int i = 0; i < 3; ++i) {
    d.out[i] = dev.alloc(out_bytes, Access::CpuRead);
    if (!d.out[i]) return set_why("3D layer allocation failed");
  }
  // Everything the CPU writes and never reads back. Write-combine is the
  // right side of the 35x cliff for these.
  d.polys  = dev.alloc(sizeof(GpuPoly) * DS_MAX_POLYS, Access::CpuWrite);
  d.verts  = dev.alloc(sizeof(GpuVert) * DS_MAX_VERTS, Access::CpuWrite);
  d.tiles  = dev.alloc(kTilesBytes, Access::CpuWrite);
  d.texels = dev.alloc(sizeof(u32) * kTexelWords, Access::CpuWrite);
  d.post   = dev.alloc(sizeof(GpuPost), Access::CpuWrite);
  d.shclear = dev.alloc(sizeof(u32) * DS_MAX_POLYS * DS_SHRUN_LINES, Access::CpuWrite);
  // The span table. GPU-only: the span pass writes it, the raster reads it.
  d.rows = dev.alloc(sizeof(GpuRow) * DS_MAX_SPAN_ROWS, Access::CpuWrite);
  // GPU-only planes: the CPU never reads them.
  d.depth  = dev.alloc(out_bytes, Access::CpuWrite);
  d.attr   = dev.alloc(out_bytes, Access::CpuWrite);
  d.keys   = dev.alloc(out_bytes * 2, Access::CpuWrite);
  d.rowpoly = dev.alloc(sizeof(u32) * DS_MAX_SPAN_ROWS, Access::CpuWrite);
  if (!d.polys || !d.verts || !d.tiles || !d.texels || !d.post || !d.depth || !d.attr || !d.shclear || !d.rows || !d.keys || !d.rowpoly)
    return set_why("frame buffer allocation failed");

  const Api& a = *d.api;

  auto shader = [&](Spirv s, VkShaderModule* out) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = s.bytes;
    ci.pCode = s.code;
    return a.vkCreateShaderModule(vk->dev, &ci, nullptr, out) == VK_SUCCESS;
  };
  if (!shader(shader_bin(), &d.mod_bin)) return set_why("bin.spv rejected by the driver");
  if (!shader(shader_raster(), &d.mod_raster)) return set_why("raster.spv rejected by the driver");
  if (!shader(shader_post(), &d.mod_post)) return set_why("post.spv rejected by the driver");
  if (!shader(shader_span(), &d.mod_span)) return set_why("span.spv rejected by the driver");
  // The visibility pass needs 64-bit atomics; its SPIR-V declares the
  // capability, so on a part without them the module is never even created
  // and the whole list goes through the ordered loop instead.
  self->vis_ = dev.limits().int64_atomics && vis_wanted();
  if (self->vis_) {
    if (!shader(shader_vis(), &d.mod_vis)) return set_why("vis.spv rejected by the driver");
    if (!shader(shader_raster_vis(), &d.mod_raster_vis)) return set_why("raster_vis.spv rejected by the driver");
  }

  VkDescriptorSetLayoutBinding binds[B_COUNT]{};
  for (u32 i = 0; i < B_COUNT; ++i) {
    binds[i].binding = i;
    binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dsli{};
  dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsli.bindingCount = B_COUNT;
  dsli.pBindings = binds;
  if (a.vkCreateDescriptorSetLayout(vk->dev, &dsli, nullptr, &d.dsl) != VK_SUCCESS)
    return set_why("descriptor set layout failed");

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(GpuFrame);
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &d.dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout) != VK_SUCCESS)
    return set_why("pipeline layout failed");

  // Pipeline compilation is 13-45 ms a shader on the G52. It happens here,
  // at startup, and never on a frame.
  auto pipeline = [&](VkShaderModule m, VkPipeline* out) {
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = d.layout;
    return a.vkCreateComputePipelines(vk->dev, VK_NULL_HANDLE, 1, &ci, nullptr, out) == VK_SUCCESS;
  };
  if (!pipeline(d.mod_bin, &d.pipe_bin)) return set_why("bin pipeline failed to compile");
  if (!pipeline(d.mod_raster, &d.pipe_raster)) return set_why("raster pipeline failed to compile");
  if (!pipeline(d.mod_post, &d.pipe_post)) return set_why("final pass pipeline failed to compile");
  if (!pipeline(d.mod_span, &d.pipe_span)) return set_why("span pipeline failed to compile");
  if (self->vis_) {
    if (!pipeline(d.mod_vis, &d.pipe_vis)) return set_why("visibility pipeline failed to compile");
    if (!pipeline(d.mod_raster_vis, &d.pipe_raster_vis)) return set_why("seeded raster pipeline failed to compile");
  }

  VkDescriptorPoolSize psz{};
  psz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  psz.descriptorCount = B_COUNT * 3;
  VkDescriptorPoolCreateInfo dpi{};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.maxSets = 3;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &psz;
  if (a.vkCreateDescriptorPool(vk->dev, &dpi, nullptr, &d.pool) != VK_SUCCESS)
    return set_why("descriptor pool failed");

  // One set per output buffer. Everything but the output is the same in both,
  // so a frame changes which set it binds rather than rewriting descriptors.
  VkDescriptorSetLayout layouts[3] = {d.dsl, d.dsl, d.dsl};
  VkDescriptorSetAllocateInfo dsa{};
  dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsa.descriptorPool = d.pool;
  dsa.descriptorSetCount = 3;
  dsa.pSetLayouts = layouts;
  if (a.vkAllocateDescriptorSets(vk->dev, &dsa, d.set) != VK_SUCCESS)
    return set_why("descriptor set allocation failed");

  for (int i = 0; i < 3; ++i) {
    VkDescriptorBufferInfo bi[B_COUNT]{};
    const Buffer* src[B_COUNT] = {&d.polys, &d.verts, &d.tiles, &d.texels, &d.out[i],
                                  &d.post, &d.depth, &d.attr, &d.shclear, &d.rows, &d.keys, &d.rowpoly};
    VkWriteDescriptorSet w[B_COUNT]{};
    for (u32 b = 0; b < B_COUNT; ++b) {
      bi[b].buffer = vk_buf(*src[b]);
      bi[b].offset = 0;
      bi[b].range = VK_WHOLE_SIZE;
      w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[b].dstSet = d.set[i];
      w[b].dstBinding = b;
      w[b].descriptorCount = 1;
      w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[b].pBufferInfo = &bi[b];
    }
    a.vkUpdateDescriptorSets(vk->dev, B_COUNT, w, 0, nullptr);
  }

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = vk->qfam;
  if (a.vkCreateCommandPool(vk->dev, &cpi, nullptr, &d.cmdpool) != VK_SUCCESS)
    return set_why("command pool failed");
  d.bands = band_count();
  self->bands_ = d.bands;
  for (int i = 0; i < 3; ++i) {
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = d.cmdpool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = d.bands + 1;   // one more for the final pass
    if (a.vkAllocateCommandBuffers(vk->dev, &cbi, d.cmd[i]) != VK_SUCCESS)
      return set_why("command buffer allocation failed");
  }

  d.timing = timing_wanted() && dev.limits().timestamp_period_ns > 0 && a.vkCreateQueryPool && a.vkCmdWriteTimestamp;
  if (timing_wanted() && !d.timing)
    std::fprintf(stderr, "gpu raster: DS_VK_TIMING asked for but unavailable (timestamp period %.3f ns, entry points %s)\n",
                 dev.limits().timestamp_period_ns, (a.vkCreateQueryPool && a.vkCmdWriteTimestamp) ? "present" : "missing");
  if (d.timing) {
    d.tick_ns = dev.limits().timestamp_period_ns;
    d.qcount = 8 + 2 * d.bands;
    VkQueryPoolCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = d.qcount;
    for (int i = 0; i < 3; ++i)
      if (a.vkCreateQueryPool(vk->dev, &qi, nullptr, &d.qpool[i]) != VK_SUCCESS) return set_why("timestamp query pool failed");
  }

  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (u32 b = 0; b <= d.bands; ++b)
    if (a.vkCreateFence(vk->dev, &fi, nullptr, &d.fence[b]) != VK_SUCCESS)
      return set_why("fence creation failed");

  // Start from a cleared layer, so a frame read before anything is drawn is
  // black rather than whatever the allocation held.
  for (int i = 0; i < 3; ++i) std::memset(d.out[i].ptr, 0, out_bytes);

  self->ready_ = true;
  if (why) *why = dev.name();
  return self;
}

Raster::~Raster() {
  if (!d_ || !d_->dev) return;
  Impl& d = *d_;
  const Api& a = *d.api;
  if (d.vk && d.vk->dev) {
    a.vkDeviceWaitIdle(d.vk->dev);
    for (u32 b = 0; b <= d.bands; ++b)
      if (d.fence[b]) a.vkDestroyFence(d.vk->dev, d.fence[b], nullptr);
    if (d.cmdpool) a.vkDestroyCommandPool(d.vk->dev, d.cmdpool, nullptr);
    for (auto q : d.qpool) if (q) a.vkDestroyQueryPool(d.vk->dev, q, nullptr);
    if (d.pool) a.vkDestroyDescriptorPool(d.vk->dev, d.pool, nullptr);
    if (d.pipe_bin) a.vkDestroyPipeline(d.vk->dev, d.pipe_bin, nullptr);
    if (d.pipe_raster) a.vkDestroyPipeline(d.vk->dev, d.pipe_raster, nullptr);
    if (d.pipe_post) a.vkDestroyPipeline(d.vk->dev, d.pipe_post, nullptr);
    if (d.pipe_span) a.vkDestroyPipeline(d.vk->dev, d.pipe_span, nullptr);
    if (d.pipe_vis) a.vkDestroyPipeline(d.vk->dev, d.pipe_vis, nullptr);
    if (d.pipe_raster_vis) a.vkDestroyPipeline(d.vk->dev, d.pipe_raster_vis, nullptr);
    if (d.layout) a.vkDestroyPipelineLayout(d.vk->dev, d.layout, nullptr);
    if (d.dsl) a.vkDestroyDescriptorSetLayout(d.vk->dev, d.dsl, nullptr);
    if (d.mod_bin) a.vkDestroyShaderModule(d.vk->dev, d.mod_bin, nullptr);
    if (d.mod_raster) a.vkDestroyShaderModule(d.vk->dev, d.mod_raster, nullptr);
    if (d.mod_post) a.vkDestroyShaderModule(d.vk->dev, d.mod_post, nullptr);
    if (d.mod_span) a.vkDestroyShaderModule(d.vk->dev, d.mod_span, nullptr);
    if (d.mod_vis) a.vkDestroyShaderModule(d.vk->dev, d.mod_vis, nullptr);
    if (d.mod_raster_vis) a.vkDestroyShaderModule(d.vk->dev, d.mod_raster_vis, nullptr);
  }
  for (auto& b : d.out) d.dev->free(b);
  d.dev->free(d.polys);
  d.dev->free(d.verts);
  d.dev->free(d.tiles);
  d.dev->free(d.texels);
  d.dev->free(d.post);
  d.dev->free(d.shclear);
  d.dev->free(d.rows);
  d.dev->free(d.keys);
  d.dev->free(d.rowpoly);
  d.dev->free(d.depth);
  d.dev->free(d.attr);
  if (d.host_textures) d.dev->free(d.host_textures);
}

GpuPoly* Raster::poly_buffer() { return static_cast<GpuPoly*>(d_->polys.ptr); }
GpuVert* Raster::vert_buffer() { return static_cast<GpuVert*>(d_->verts.ptr); }

GpuPost* Raster::post_buffer() { return static_cast<GpuPost*>(d_->post.ptr); }
u32* Raster::shadow_run_buffer() { return static_cast<u32*>(d_->shclear.ptr); }
u32* Raster::rowpoly_buffer() { return static_cast<u32*>(d_->rowpoly.ptr); }

u32 Raster::frame_bands() const { return d_->post_frame ? 1u : bands_; }

u32* Raster::texel_buffer(u32* capacity) {
  if (capacity) *capacity = kTexelWords;
  return static_cast<u32*>(d_->texels.ptr);
}

bool Raster::bind_texture_arena(void* ptr, size_t size) {
  // Kept for when the texture cache keeps its texels in one arena; today each
  // entry owns its own vector, so there is no single pointer to import and
  // the upload copies through texel_buffer() instead.
  Impl& d = *d_;
  if (d.host_textures) d.dev->free(d.host_textures);
  d.host_textures = d.dev->import_host(ptr, size);
  return static_cast<bool>(d.host_textures);
}

s32 Raster::band_line(u32 b) const {
  const u32 rows = DS_TILES_Y / bands_;
  const u32 y = (b >= bands_ ? DS_TILES_Y : b * rows) * DS_TILE_H;
  return static_cast<s32>(y * scale_);
}

bool Raster::submit(u32 npoly, u32 nvert, u32 ntexels, const GpuFrame& frame) {
  if (!ready_) return false;
  Impl& d = *d_;
  const Api& a = *d.api;
  // npoly == 0 is a real frame, not a refusal: the binning dispatch is zero
  // workgroups and the raster still clears the layer, which is exactly what
  // the CPU does for a frame with no live polygons.
  if (npoly > DS_MAX_POLYS || nvert > DS_MAX_VERTS || ntexels > kTexelWords) return false;

  wait();                       // the previous frame must be off the buffers

  // Collect the stamps of the frame that used this slot last (three frames
  // ago -- long finished) before its pool is reset below.
  const u32 slot = static_cast<u32>(d.gen % 3);
  if (d.timing && d.stamped[slot]) {
    std::vector<u64> t(d.qcount, 0);
    if (a.vkGetQueryPoolResults(d.vk->dev, d.qpool[slot], 0, d.qcount, sizeof(u64) * d.qcount, t.data(),
                                sizeof(u64), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      auto span_ns = [&](u32 i) -> u64 { return t[i + 1] > t[i] ? static_cast<u64>(static_cast<double>(t[i + 1] - t[i]) * d.tick_ns) : 0; };
      d.times.ns[P_BIN]  += span_ns(0);
      d.times.ns[P_SPAN] += span_ns(2);
      d.times.ns[P_VIS]  += span_ns(4);
      d.times.ns[P_POST] += span_ns(6);
      for (u32 b = 0; b < d.bands; ++b) d.times.ns[P_RASTER] += span_ns(8 + 2 * b);
      ++d.times.frames;
    }
    d.stamped[slot] = false;
  }

  // Make the uploads visible to the GPU. A coherent mapping needs none of
  // this; whether one was handed out is the driver's choice, so it is always
  // called and costs nothing when it is a no-op.
  d.dev->flush(d.polys, 0, sizeof(GpuPoly) * npoly);
  d.dev->flush(d.verts, 0, sizeof(GpuVert) * nvert);
  if (ntexels) d.dev->flush(d.texels, 0, sizeof(u32) * ntexels);
  d.dev->flush(d.post, 0, sizeof(GpuPost));
  d.dev->flush(d.shclear, 0, sizeof(u32) * npoly * DS_SHRUN_LINES);
  d.dev->flush(d.rowpoly, 0, sizeof(u32) * frame.nrows);

  // Edge marking or fog: the raster writes its depth and attribute planes and
  // a final pass follows it. Edge marking reads across band boundaries, so
  // such a frame reports one band -- no part of the picture is final until
  // the whole of it is.
  d.post_frame = (frame.dispcnt & ((1u << 5) | (1u << 7))) != 0u;

  const int st = stages();
  const u32 back = static_cast<u32>(d.gen % 3);
  const u32 rows = DS_TILES_Y / d.bands;

  // A stamp after a dispatch, at the bottom of the pipe, closes it: the
  // passes are serialised by barriers, so the interval to the stamp before
  // it is that dispatch alone. Unused slots stay zero (the pool is reset) and
  // read as an empty interval.
  auto stamp = [&](VkCommandBuffer cb, u32 q) {
    if (d.timing) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool[back], q);
  };

  for (u32 b = 0; b < d.bands; ++b) {
    VkCommandBuffer cb = d.cmd[back][b];
    a.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    if (b == 0 && d.timing) a.vkCmdResetQueryPool(cb, d.qpool[back], 0, d.qcount);

    auto barrier = [&](VkPipelineStageFlags from, VkAccessFlags srcmask) {
      VkMemoryBarrier mb{};
      mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      mb.srcAccessMask = srcmask;
      mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      a.vkCmdPipelineBarrier(cb, from, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    if (b == 0) {
      // Band 0 carries the setup the rest depend on: clear the bin counters,
      // then bin. Every later band opens with a barrier instead, which orders
      // it after this binning in queue submission order.
      a.vkCmdFillBuffer(cb, vk_buf(d.tiles), 0, kTilesHeader, 0);
      // Every key starts as "nobody": all ones, which no fragment's key
      // reaches. The 32-bit fill pattern repeats into the 64-bit words.
      if (vis_) a.vkCmdFillBuffer(cb, vk_buf(d.keys), 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
      barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.set[back], 0, nullptr);
      a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuFrame), &frame);
      // One invocation per TILE, not per polygon: the binning pass scans the
      // polygon list in order so the raster gets its list sorted (bin.comp).
      stamp(cb, 0);
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_bin);
      if (st & 1) a.vkCmdDispatch(cb, (DS_TILE_COUNT + 63) / 64, 1, 1);
      stamp(cb, 1);
      // The span pass: one workgroup per polygon, its threads taking that
      // polygon's scanlines in stride. Independent of binning, so the two
      // need no barrier between them (the raster reads both) -- except when
      // being timed, when a barrier makes each stamp close one dispatch.
      if (d.timing) { barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT); stamp(cb, 2); }
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_span);
      if ((st & 2) && frame.nrows) a.vkCmdDispatch(cb, (frame.nrows + 63) / 64, 1, 1);
      stamp(cb, 3);
      // Every stamp slot is written every frame, dispatch or no dispatch: a
      // query left unwritten is "unavailable" and makes the whole readback
      // fail, so an absent pass records an empty interval instead.
      stamp(cb, 4);
      if (vis_ && (st & 6) == 6 && frame.opaque_rows) {
        // The order-free prefix: one workgroup per eight span rows of it,
        // deciding each pixel's owner. It needs the span pass's rows; the
        // raster's bands then shade the winners and continue with the tail.
        barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
        a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_vis);
        a.vkCmdDispatch(cb, (frame.opaque_rows + DS_VIS_ROWS - 1) / DS_VIS_ROWS, 1, 1);
      }
      stamp(cb, 5);
    } else {
      a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.set[back], 0, nullptr);
    }
    // The raster reads what the binning pass wrote, in band 0's submission.
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);

    GpuFrame bf = frame;
    bf.tile_y0 = b * rows;
    a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuFrame), &bf);
    stamp(cb, 8 + 2 * b);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, vis_ ? d.pipe_raster_vis : d.pipe_raster);
    if (st & 2) a.vkCmdDispatch(cb, DS_TILES_X, rows, 1);
    stamp(cb, 9 + 2 * b);
    // No final pass this frame: its two slots still have to exist (see above).
    if (b + 1 == d.bands && !(d.post_frame && (st & 10) == 10)) { stamp(cb, 6); stamp(cb, 7); }

    if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;

    a.vkResetFences(d.vk->dev, 1, &d.fence[b]);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (a.vkQueueSubmit(d.vk->queue, 1, &si, d.fence[b]) != VK_SUCCESS) return false;
    d.in_flight |= 1u << b;
  }

  if (d.post_frame && (st & 10) == 10) {
    VkCommandBuffer cb = d.cmd[back][d.bands];
    a.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 1, &mb, 0, nullptr, 0, nullptr);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.set[back], 0, nullptr);
    a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuFrame), &frame);
    stamp(cb, 6);
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_post);
    const u32 px = 256 * scale_ * 192 * scale_;
    a.vkCmdDispatch(cb, (px + 63) / 64, 1, 1);
    stamp(cb, 7);
    if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;

    a.vkResetFences(d.vk->dev, 1, &d.fence[d.bands]);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (a.vkQueueSubmit(d.vk->queue, 1, &si, d.fence[d.bands]) != VK_SUCCESS) return false;
    d.in_flight |= 1u << d.bands;
  }

  d.stamped[back] = d.timing;
  ++d.gen;                      // output() and the next frame name different buffers
  return true;
}

const Raster::PassTimes& Raster::pass_times() const { return d_->times; }
bool Raster::timing() const { return d_ && d_->timing; }

void Raster::wait_band(u32 b) {
  if (!d_) return;
  Impl& d = *d_;
  // A frame with a final pass has no partial result to hand out: wait for all
  // of it, the final pass included.
  if (d.post_frame) b = d.bands;
  else if (b >= d.bands) b = d.bands - 1;
  std::lock_guard<std::mutex> lk(d.wait_mutex);
  // Bands 0..b, not band b alone: without explicit synchronisation between
  // submissions the driver may complete them in any order, so an earlier
  // band's fence is not implied by a later one's.
  VkFence pending[kMaxBands];
  u32 n = 0;
  for (u32 i = 0; i <= b; ++i)
    if (d.in_flight & (1u << i)) pending[n++] = d.fence[i];
  if (!n) return;
  const Api& a = *d.api;
  a.vkWaitForFences(d.vk->dev, n, pending, VK_TRUE, UINT64_MAX);

  // Invalidate only the scanlines these bands wrote -- the composite reads
  // the top of the frame long before the bottom exists.
  const Buffer& done = d.out[(d.gen + 2) % 3];   // the frame just submitted
  const size_t stride = static_cast<size_t>(256 * scale_) * sizeof(u32);
  for (u32 i = 0; i <= b; ++i) {
    if (!(d.in_flight & (1u << i))) continue;
    if (i < d.bands) {
      const size_t y0 = static_cast<size_t>(band_line(i)), y1 = static_cast<size_t>(band_line(i + 1));
      d.dev->invalidate(done, y0 * stride, (y1 - y0) * stride);
    } else {
      d.dev->invalidate(done, 0, done.size);   // the final pass rewrote the whole layer
    }
    d.in_flight &= ~(1u << i);
  }
}

const u32* Raster::output() const {
  return static_cast<const u32*>(d_->out[(d_->gen + 2) % 3].ptr);
}

const u32* Raster::output_prev() const {
  return static_cast<const u32*>(d_->out[(d_->gen + 1) % 3].ptr);
}

} // namespace ds::gpu::vk
