// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include <dirent.h>
#include <unistd.h>
#include <string>
#include <chrono>
#include "core/gpu/vk/vk_raster.h"
#include <array>
#include <atomic>
#include <vector>

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
               B_POST = 5, B_DEPTH = 6, B_ATTR = 7, B_SHCLEAR = 8, B_ROWS = 9, B_KEYS = 10, B_ROWPOLY = 11, B_OUTNAT = 12, B_COUNT = 13 };

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
  // At S >= 2 out[] is the hi-res layer and these are its native-resolution
  // reduction (downsample.comp), which is what the CPU reads. At S = 1 they
  // alias out[] (the same Buffer, not allocated twice).
  Buffer out_nat[3];
  bool cpu_down = false;                                   // S >= 2: the native plane is reduced on the CPU (reduce_line), not by downsample.comp
  std::vector<u32> nat[3];                                 // the CPU-built native planes, one per slot
  std::array<std::atomic<u8>, 192> nat_done[3]{};          // per slot: which lines are reduced for the frame the slot holds
  VkShaderModule mod_down = VK_NULL_HANDLE;
  VkPipeline pipe_down = VK_NULL_HANDLE;
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

  // The triangle path (P3). Images the render pass draws into and the copies
  // back into the compute path's buffers, so out[], depth and attr -- and the
  // final pass, and the host -- see exactly what they see from the compute
  // raster.
  bool tri = false, roaa = false;
  VkShaderModule        mod_tv = VK_NULL_HANDLE, mod_tof = VK_NULL_HANDLE, mod_ttf = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl_g = VK_NULL_HANDLE, dsl_in = VK_NULL_HANDLE;
  VkPipelineLayout      layout_g = VK_NULL_HANDLE;
  VkDescriptorSet       set_g[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE}, set_in = VK_NULL_HANDLE;
  VkRenderPass          rp = VK_NULL_HANDLE;
  VkFramebuffer         fb = VK_NULL_HANDLE;
  // No-copy form: LINEAR images aliased on out[i], depth and attr, so the
  // render pass writes the buffers the host, the final pass and the
  // downsample read. One colour image and framebuffer per out[] slot.
  bool alias = false;
  VkFramebuffer fb3[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDescriptorSet set_in3[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkPipeline            pipe_op[2][2] = {{VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE}};   // [wbuffer][front-facing: equal depth passes]
  VkPipeline            pipe_tail[2][2] = {{VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE}};   // [wbuffer][depth write]
  // Shadow volumes: a mask polygon marks the stencil where its depth test
  // FAILS (the volume's interior), a shadow polygon draws only there.
  VkPipeline            pipe_mask = VK_NULL_HANDLE;   // shadow masks: no depth test, writes the shadow plane only (tri_mask.frag)
  VkShaderModule        mod_tmf = VK_NULL_HANDLE;
  VkFormat              ds_format = VK_FORMAT_D32_SFLOAT;
  bool                  stencil = false;   // shadows drawn (the name is historical: the stencil attachment is gone, see tri_mask.frag)
  struct Img { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
  Img img_col, img_attr, img_z, img_d, img_sh;   // img_sh: the shadow plane (R32_UINT run ids), colour attachment 3
  Img img_col3[3];

  u64  gen = 0;              // frames submitted; the buffer is gen % 3
  u32  bands = 1;
  bool post_frame = false;   // the frame in flight ends with the final pass
  u32  in_flight = 0;        // bitmask of bands submitted and not yet waited for (bit `bands` is the final pass)
  std::mutex wait_mutex;     // waited from the compositor and the emulation thread
};

// The triangle path's resources: three R32_UINT colour attachments (the 3D
// layer record, the attribute plane, the depth plane) and a D32 depth buffer,
// one render pass whose single subpass also reads the colour and attribute
// attachments as input attachments (ordered per pixel with
// VK_EXT_rasterization_order_attachment_access, else behind a per-polygon
// barrier), and six pipelines: the opaque prefix and the translucent tail,
// each for the Z-buffer (LESS on z) and W-buffer (GREATER on 1 / depth) modes, the
// tail with and without the depth write.
bool Raster::tri_setup(Impl& d, Device& dev, std::string* why) {
  const Api& a = *d.api;
  const DeviceInternal* vk = d.vk;
  auto fail = [&](const char* m) { if (why) *why = m; return false; };
  const u32 W = 256 * scale_, H = 192 * scale_;
  d.roaa = dev.limits().ordered_attachments && !std::getenv("DS_VK_TRI_NOROAA");   // DS_VK_TRI_NOROAA=1: the per-polygon barrier fallback, for bisecting

  auto shader = [&](Spirv s, VkShaderModule* out) {
    VkShaderModuleCreateInfo ci{}; ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = s.bytes; ci.pCode = s.code;
    return a.vkCreateShaderModule(vk->dev, &ci, nullptr, out) == VK_SUCCESS;
  };
  if (!shader(shader_tri_vert(), &d.mod_tv) || !shader(shader_tri_opaque(), &d.mod_tof) || !shader(shader_tri_tail(), &d.mod_ttf) || !shader(shader_tri_mask(), &d.mod_tmf)) return fail("triangle shaders rejected by the driver");
  if (std::getenv("DS_VK_TRI_FLAT")) { if (!shader(shader_tri_flat(), &d.mod_tof)) return fail("flat shader rejected"); std::fprintf(stderr, "gpu raster: ATTRIBUTION -- flat fragment stage\n"); }

  // Set 0: the same buffers as the compute passes, for the vertex and fragment stages.
  VkDescriptorSetLayoutBinding binds[B_COUNT]{};
  for (u32 i = 0; i < B_COUNT; ++i) { binds[i].binding = i; binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; binds[i].descriptorCount = 1; binds[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; }
  VkDescriptorSetLayoutCreateInfo dsli{}; dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dsli.bindingCount = B_COUNT; dsli.pBindings = binds;
  if (a.vkCreateDescriptorSetLayout(vk->dev, &dsli, nullptr, &d.dsl_g) != VK_SUCCESS) return fail("graphics descriptor layout");
  // Set 1: the colour, attribute, depth and shadow attachments, read back in the tail.
  VkDescriptorSetLayoutBinding inb[4]{};
  for (u32 i = 0; i < 4; ++i) { inb[i].binding = i; inb[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; inb[i].descriptorCount = 1; inb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; }
  VkDescriptorSetLayoutCreateInfo dsli2{}; dsli2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dsli2.bindingCount = 4; dsli2.pBindings = inb;
  if (a.vkCreateDescriptorSetLayout(vk->dev, &dsli2, nullptr, &d.dsl_in) != VK_SUCCESS) return fail("input attachment layout");
  VkDescriptorSetLayout sets2[2] = {d.dsl_g, d.dsl_in};
  VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; pcr.size = sizeof(GpuFrame);
  VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 2; pli.pSetLayouts = sets2; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
  if (a.vkCreatePipelineLayout(vk->dev, &pli, nullptr, &d.layout_g) != VK_SUCCESS) return fail("graphics pipeline layout");

  VkDescriptorSetLayout lg[4] = {d.dsl_g, d.dsl_g, d.dsl_g, d.dsl_in};
  VkDescriptorSet got[4];
  VkDescriptorSetAllocateInfo dsa{}; dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa.descriptorPool = d.pool; dsa.descriptorSetCount = 4; dsa.pSetLayouts = lg;
  if (a.vkAllocateDescriptorSets(vk->dev, &dsa, got) != VK_SUCCESS) return fail("graphics descriptor sets");
  for (int i = 0; i < 3; ++i) d.set_g[i] = got[i];
  d.set_in = got[3];
  for (int i = 0; i < 3; ++i) {
    VkDescriptorBufferInfo bi[B_COUNT]{};
    const Buffer* src[B_COUNT] = {&d.polys, &d.verts, &d.tiles, &d.texels, &d.out[i], &d.post, &d.depth, &d.attr, &d.shclear, &d.rows, &d.keys, &d.rowpoly, &d.out_nat[i]};
    VkWriteDescriptorSet w[B_COUNT]{};
    for (u32 b = 0; b < B_COUNT; ++b) {
      bi[b].buffer = vk_buf(*src[b]); bi[b].range = VK_WHOLE_SIZE;
      w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[b].dstSet = d.set_g[i]; w[b].dstBinding = b; w[b].descriptorCount = 1;
      w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[b].pBufferInfo = &bi[b];
    }
    a.vkUpdateDescriptorSets(vk->dev, B_COUNT, w, 0, nullptr);
  }

  VkPhysicalDeviceMemoryProperties mp{}; a.vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
  auto make_image = [&](VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, Impl::Img& o) {
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {W, H, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a.vkCreateImage(vk->dev, &ii, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{}; a.vkGetImageMemoryRequirements(vk->dev, o.img, &mr);
    u32 type = ~0u;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
    for (u32 t = 0; t < mp.memoryTypeCount && type == ~0u; ++t) if (mr.memoryTypeBits & (1u << t)) type = t;
    if (type == ~0u) return false;
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    if (a.vkAllocateMemory(vk->dev, &ai, nullptr, &o.mem) != VK_SUCCESS) return false;
    if (a.vkBindImageMemory(vk->dev, o.img, o.mem, 0) != VK_SUCCESS) return false;
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return a.vkCreateImageView(vk->dev, &vi, nullptr, &o.view) == VK_SUCCESS;
  };
  const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  // A LINEAR image bound to a buffer's memory: the render pass then writes
  // the buffer itself and the three copies (1.9 ms a frame on the G52, a
  // tiled-to-linear conversion) go away. Needs the format to render linear,
  // the driver's row pitch to be ours, and a memory type both can share.
  auto alias_image = [&](const Buffer& buf, Impl::Img& o) {
    VkImageCreateInfo ii{}; ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R32_UINT;
    ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a.vkCreateImage(vk->dev, &ii, nullptr, &o.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{}; a.vkGetImageMemoryRequirements(vk->dev, o.img, &mr);
    VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout lay{}; a.vkGetImageSubresourceLayout(vk->dev, o.img, &sr, &lay);
    if (lay.offset != 0 || lay.rowPitch != static_cast<VkDeviceSize>(W) * 4 || mr.size > buf.size) {
      std::fprintf(stderr, "gpu raster: linear alias refused -- offset %llu pitch %llu (ours %u) size %llu (buffer %zu) typebits %x\n", (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch, W * 4, (unsigned long long)mr.size, buf.size, mr.memoryTypeBits);
      a.vkDestroyImage(vk->dev, o.img, nullptr); o.img = VK_NULL_HANDLE; return false;
    }
    if (a.vkBindImageMemory(vk->dev, o.img, reinterpret_cast<VkDeviceMemory>(buf.memory), 0) != VK_SUCCESS) { std::fprintf(stderr, "gpu raster: linear alias refused -- bind failed\n"); a.vkDestroyImage(vk->dev, o.img, nullptr); o.img = VK_NULL_HANDLE; return false; }
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = o.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R32_UINT; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return a.vkCreateImageView(vk->dev, &vi, nullptr, &o.view) == VK_SUCCESS;
  };
  {
    VkFormatProperties fp{}; if (a.vkGetPhysicalDeviceFormatProperties) a.vkGetPhysicalDeviceFormatProperties(vk->phys, VK_FORMAT_R32_UINT, &fp);
    const bool linear_ok = (fp.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 && std::getenv("DS_VK_TRI_COPY") == nullptr;
    if (std::getenv("DS_VK_LIST_EXT") && a.vkGetPhysicalDeviceFormatProperties) {
      const VkFormat fmts[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_S8_UINT};
      const char* names[] = {"R32_UINT", "R8G8B8A8_UINT", "R8G8B8A8_UNORM", "B8G8R8A8_UNORM", "R32_SFLOAT", "R16G16_UINT", "D24_UNORM_S8", "D32_SFLOAT_S8", "S8_UINT"};
      for (int i = 0; i < 9; ++i) { VkFormatProperties f2{}; a.vkGetPhysicalDeviceFormatProperties(vk->phys, fmts[i], &f2);
        std::fprintf(stderr, "vk: %-15s linear features %08x (colour attachment %s, input %s)  optimal %08x (depth/stencil attachment %s)\n", names[i], f2.linearTilingFeatures, (f2.linearTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? "yes" : "no", (f2.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "yes" : "no", f2.optimalTilingFeatures, (f2.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? "yes" : "no"); }
    }
    d.alias = linear_ok;
    if (d.alias) for (int i = 0; i < 3 && d.alias; ++i) d.alias = alias_image(d.out[i], d.img_col3[i]);
    if (d.alias) d.alias = alias_image(d.attr, d.img_attr) && alias_image(d.depth, d.img_z);
    if (!d.alias) {
      for (auto& im : d.img_col3) { if (im.view) a.vkDestroyImageView(vk->dev, im.view, nullptr); if (im.img) a.vkDestroyImage(vk->dev, im.img, nullptr); im = Impl::Img{}; }
      for (Impl::Img* im : {&d.img_attr, &d.img_z}) { if (im->view) a.vkDestroyImageView(vk->dev, im->view, nullptr); if (im->img) a.vkDestroyImage(vk->dev, im->img, nullptr); *im = Impl::Img{}; }
    }
  }
  if (!d.alias) {
    if (!make_image(VK_FORMAT_R32_UINT, cu, VK_IMAGE_ASPECT_COLOR_BIT, d.img_col)) return fail("colour attachment");
    if (!make_image(VK_FORMAT_R32_UINT, cu, VK_IMAGE_ASPECT_COLOR_BIT, d.img_attr)) return fail("attribute attachment");
    if (!make_image(VK_FORMAT_R32_UINT, cu, VK_IMAGE_ASPECT_COLOR_BIT, d.img_z)) return fail("depth plane attachment");
  }
  // No stencil attachment: shadow volumes go through the shadow plane
  // (tri_mask.frag) after libmali held job completion for up to a second on
  // frames with stencil masks. DS_VK_TRI_NOSTENCIL=1 still means "no shadows".
  d.ds_format = VK_FORMAT_D32_SFLOAT;
  d.stencil = !std::getenv("DS_VK_TRI_NOSTENCIL");
  if (!make_image(VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, d.img_sh)) return fail("shadow plane attachment");
  if (!make_image(d.ds_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, d.img_d)) return fail("depth buffer");
  {
    VkDescriptorSetLayout l3[3] = {d.dsl_in, d.dsl_in, d.dsl_in};
    VkDescriptorSetAllocateInfo dsa3{}; dsa3.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsa3.descriptorPool = d.pool; dsa3.descriptorSetCount = 3; dsa3.pSetLayouts = l3;
    if (a.vkAllocateDescriptorSets(vk->dev, &dsa3, d.set_in3) != VK_SUCCESS) return fail("input attachment sets");
    for (int k = 0; k < 3; ++k) {
      VkDescriptorImageInfo di[4]{};
      di[0].imageView = d.alias ? d.img_col3[k].view : d.img_col.view; di[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      di[1].imageView = d.img_attr.view; di[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      di[2].imageView = d.img_z.view; di[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      di[3].imageView = d.img_sh.view; di[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet w[4]{};
      for (u32 i = 0; i < 4; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = d.set_in3[k]; w[i].dstBinding = i; w[i].descriptorCount = 1; w[i].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; w[i].pImageInfo = &di[i]; }
      a.vkUpdateDescriptorSets(vk->dev, 4, w, 0, nullptr);
    }
    d.set_in = d.set_in3[0];
  }

  VkAttachmentDescription at[5]{};   // colour, attribute, depth record, shadow plane; the depth buffer
  for (u32 i = 0; i < 4; ++i) {
    at[i].format = VK_FORMAT_R32_UINT; at[i].samples = VK_SAMPLE_COUNT_1_BIT;
    at[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  at[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;   // the shadow plane lives inside the pass
  at[4] = at[0]; at[4].format = d.ds_format; at[4].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; at[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference cr[4] = {{0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}, {2, VK_IMAGE_LAYOUT_GENERAL}, {3, VK_IMAGE_LAYOUT_GENERAL}};
  VkAttachmentReference ir[4] = {{0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}, {2, VK_IMAGE_LAYOUT_GENERAL}, {3, VK_IMAGE_LAYOUT_GENERAL}};
  VkAttachmentReference dr{4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp{};
  sp.flags = d.roaa ? VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT : 0u;
  sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.inputAttachmentCount = 4; sp.pInputAttachments = ir;
  sp.colorAttachmentCount = 4; sp.pColorAttachments = cr;
  sp.pDepthStencilAttachment = &dr;
  // The self-dependency that makes reading the attachment just written legal
  // (and, without ordered access, the barrier the host puts between tail polygons).
  VkSubpassDependency dep{};
  dep.srcSubpass = 0; dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  VkRenderPassCreateInfo rpi{}; rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rpi.attachmentCount = 5; rpi.pAttachments = at; rpi.subpassCount = 1; rpi.pSubpasses = &sp; rpi.dependencyCount = 1; rpi.pDependencies = &dep;
  if (a.vkCreateRenderPass(vk->dev, &rpi, nullptr, &d.rp) != VK_SUCCESS) return fail("render pass");
  for (int k = 0; k < 3; ++k) {
    VkImageView views[5] = {d.alias ? d.img_col3[k].view : d.img_col.view, d.img_attr.view, d.img_z.view, d.img_sh.view, d.img_d.view};
    VkFramebufferCreateInfo fbi{}; fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fbi.renderPass = d.rp; fbi.attachmentCount = 5; fbi.pAttachments = views; fbi.width = W; fbi.height = H; fbi.layers = 1;
    if (a.vkCreateFramebuffer(vk->dev, &fbi, nullptr, &d.fb3[k]) != VK_SUCCESS) return fail("framebuffer");
  }
  d.fb = d.fb3[0];

  // Pipelines. Compile time is 13-45 ms each on the G52; six of them, at startup.
  auto pipeline = [&](VkShaderModule fs, bool wbuf, bool depth_write, VkPipeline* out, int kind = 0, bool equal_passes = false) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = d.mod_tv; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{}; vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{}; ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0.f, 0.f, static_cast<float>(W), static_cast<float>(H), 0.f, 1.f}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps{}; vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{}; rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{}; ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{}; dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE; dss.depthCompareOp = wbuf ? (equal_passes ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_GREATER) : (equal_passes ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS);   // W-buffer: GREATER on 1 / depth (tri.vert)
    if (kind == 1) {
      // The mask: no depth test (the shader does the DS's, against the depth
      // record), no depth write, the shadow plane its only output.
      dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE;
    }
    VkPipelineColorBlendAttachmentState cba[4]{};
    for (u32 i = 0; i < 4; ++i) cba[i].colorWriteMask = (i == 3) == (kind == 1) ? 0xFu : 0u;   // the mask writes the shadow plane alone; everything else never touches it
    VkPipelineColorBlendStateCreateInfo cbs{}; cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = 4; cbs.pAttachments = cba;
    if (d.roaa) cbs.flags = VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
    VkGraphicsPipelineCreateInfo gpi{}; gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; gpi.stageCount = 2; gpi.pStages = st;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cbs; gpi.layout = d.layout_g; gpi.renderPass = d.rp;
    return a.vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &gpi, nullptr, out) == VK_SUCCESS;
  };
  for (int wb = 0; wb < 2; ++wb) {
    if (!pipeline(d.mod_tof, wb != 0, true, &d.pipe_op[wb][0])) return fail("opaque pipeline");
    if (!pipeline(d.mod_tof, wb != 0, true, &d.pipe_op[wb][1], 0, true)) return fail("opaque front-facing pipeline");
    for (int wr = 0; wr < 2; ++wr) if (!pipeline(d.mod_ttf, wb != 0, wr != 0, &d.pipe_tail[wb][wr])) return fail("tail pipeline");
  }
  if (!pipeline(d.mod_tmf, false, false, &d.pipe_mask, 1)) return fail("mask pipeline");
  return true;
}

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
  // DS_VK_MODE: "tri" for the triangle path, "compute" for the compute
  // passes. The triangle path is the default where the device can run it.
  {
    const Api& a0 = *d.api;
    const bool tri_wanted = [] { const char* e = std::getenv("DS_VK_MODE"); return !e || std::strcmp(e, "compute") != 0; }();
    d.tri = tri_wanted && dev.limits().graphics && a0.vkCreateGraphicsPipelines && a0.vkCreateRenderPass && a0.vkCmdCopyImageToBuffer;
  }

  // The tile shape the pixel pipeline uses has to fit the device; check it
  // here so a part with a smaller limit fails at startup rather than in the
  // middle of a scene.
  if (dev.limits().max_workgroup_invocations < DS_TILE_W * DS_TILE_H)
    return set_why("workgroup invocations below 16x16");

  // DS_VK_SCALE: the internal resolution of the triangle path (1..4). The
  // compute passes stay at 1.
  if (d.tri) if (const char* e = std::getenv("DS_VK_SCALE")) { const int s = std::atoi(e); if (s >= 1 && s <= 4) self->scale_ = static_cast<u32>(s); }
  // A little past the plane: a LINEAR image aliased on the buffer (the
  // triangle path's no-copy form) asks for slightly more than W*H*4 on
  // libmali (198208 for 196608), and the aliasing is refused without it.
  const size_t out_bytes = static_cast<size_t>(256 * self->scale_) *
                           static_cast<size_t>(192 * self->scale_) * sizeof(u32) + 16384;

  // The 3D layer, one per frame in flight. Access::CpuRead is not a
  // preference: the NEON composite reads this buffer directly.
  for (int i = 0; i < 3; ++i) {
    d.out[i] = dev.alloc(out_bytes, Access::CpuRead);
    if (!d.out[i]) return set_why("3D layer allocation failed");
    // Measured a LOSS on the device (docs/gpu-path-scoping.md): the dispatch
    // is ~0.15 ms of GPU, the strided gather lands on the thread that reads
    // the line -- st 2x +0.5 ms median. Opt-in for a host with CPU to spare.
    d.cpu_down = self->scale_ > 1 && std::getenv("DS_VK_CPU_DOWNSAMPLE");
    if (self->scale_ > 1 && !d.cpu_down) { d.out_nat[i] = dev.alloc(256 * 192 * sizeof(u32), Access::CpuRead); if (!d.out_nat[i]) return set_why("native layer allocation failed"); }
    else d.out_nat[i] = d.out[i];   // (binding 12 must name a buffer; with cpu_down it is never read)
    if (d.cpu_down) d.nat[i].assign(256 * 192, 0u);
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
  if (self->scale_ > 1) {
    if (!shader(shader_downsample(), &d.mod_down)) return set_why("downsample.spv rejected by the driver");
    if (!pipeline(d.mod_down, &d.pipe_down)) return set_why("downsample pipeline failed to compile");
  }
  if (self->vis_) {
    if (!pipeline(d.mod_vis, &d.pipe_vis)) return set_why("visibility pipeline failed to compile");
    if (!pipeline(d.mod_raster_vis, &d.pipe_raster_vis)) return set_why("seeded raster pipeline failed to compile");
  }

  VkDescriptorPoolSize psz[2]{};
  psz[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  psz[0].descriptorCount = B_COUNT * 6;
  psz[1].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
  psz[1].descriptorCount = 16;   // four input attachments per set, three sets (set_in3)
  VkDescriptorPoolCreateInfo dpi{};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.maxSets = 10;
  dpi.poolSizeCount = 2;
  dpi.pPoolSizes = psz;
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
                                  &d.post, &d.depth, &d.attr, &d.shclear, &d.rows, &d.keys, &d.rowpoly, &d.out_nat[i]};
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

  if (d.tri) {
    std::string treason;
    if (!self->tri_setup(d, dev, &treason)) {
      std::fprintf(stderr, "gpu raster: triangle path unavailable (%s); compute passes instead\n", treason.c_str());
      d.tri = false;
      // The compute path wants its bands back; the command buffers and
      // fences below are sized from d.bands.
      d.bands = band_count();
      self->bands_ = d.bands;
    } else {
      // The host lays out the list as it does for the visibility pass: the
      // real order-free prefix, and only the tail binned.
      self->vis_ = true;
      std::fprintf(stderr, "gpu raster: triangle path at %ux, ordered attachment access %s, %s, %s\n", self->scale_, d.roaa ? "on" : "OFF (a barrier per tail polygon)", d.alias ? "attachments aliased on the buffers" : "attachments copied into the buffers", d.stencil ? "shadows through the shadow plane" : "shadows OFF");
      if (self->scale_ > 1) std::fprintf(stderr, "gpu raster: native plane %s\n", d.cpu_down ? "reduced on the CPU a line at a time (DS_VK_CPU_DOWNSAMPLE)" : "by downsample.comp on the GPU");
    }
  }

  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = vk->qfam;
  if (a.vkCreateCommandPool(vk->dev, &cpi, nullptr, &d.cmdpool) != VK_SUCCESS)
    return set_why("command pool failed");
  d.bands = d.tri ? 1 : band_count();
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
  if (d.cpu_down) for (auto& f : d.nat_done[back]) f.store(0, std::memory_order_relaxed);   // this slot now holds a new frame

  // A stamp after a dispatch, at the bottom of the pipe, closes it: the
  // passes are serialised by barriers, so the interval to the stamp before
  // it is that dispatch alone. Unused slots stay zero (the pool is reset) and
  // read as an empty interval.
  auto stamp = [&](VkCommandBuffer cb, u32 q) {
    if (d.timing) a.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, d.qpool[back], q);
  };

  if (d.tri) {
    // One command buffer, one band: the render pass, then the three
    // attachments copied into the compute path's buffers so out[], the
    // depth and attribute planes, the final pass and the host all read what
    // they always read.
    VkCommandBuffer cb = d.cmd[back][0];
    a.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return false;
    if (d.timing) a.vkCmdResetQueryPool(cb, d.qpool[back], 0, d.qcount);
    for (u32 q = 0; q < 4; ++q) stamp(cb, q);   // the compute passes' slots: empty intervals (4/5 time the opaque draw below)
    if (!(d.post_frame && (st & 10) == 10)) { stamp(cb, 6); stamp(cb, 7); }
    stamp(cb, 8);
    const u32 W = 256 * scale_, H = 192 * scale_;
    const bool wbuf = (frame.flags & DS_FF_WBUFFER) != 0u;
    VkClearValue cv[5]{};
    cv[0].color.uint32[0] = frame.clear_color;
    cv[1].color.uint32[0] = frame.clear_attr;
    cv[2].color.uint32[0] = frame.clear_depth;
    cv[3].color.uint32[0] = 0xFFFFFFFFu;   // the shadow plane: no run yet -- a shadow polygon before any mask (run 0 in its flags) must match nothing
    cv[4].depthStencil.depth = wbuf ? 1.f / static_cast<float>(std::max<u32>(frame.clear_depth, 1)) : static_cast<float>(frame.clear_depth) / 16777215.f;
    VkRenderPassBeginInfo rb{};
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rb.renderPass = d.rp; rb.framebuffer = d.fb3[back]; rb.renderArea = {{0, 0}, {W, H}}; rb.clearValueCount = 5; rb.pClearValues = cv;
    a.vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkDescriptorSet gs[2] = {d.set_g[back], d.set_in3[back]};
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout_g, 0, 2, gs, 0, nullptr);
    a.vkCmdPushConstants(cb, d.layout_g, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &frame);
    stamp(cb, 4);
    static const bool no_opaque = std::getenv("DS_VK_TRI_NOOPAQUE") != nullptr;   // attribution: the pass with nothing drawn
    if ((st & 2) && !no_opaque && frame.first_ordered) {
      // Back faces, then front faces with equal depth passing (depth_pass
      // mode 1); the flag in the push constant tells tri.vert which to keep.
      GpuFrame fpass = frame;
      fpass.flags = frame.flags | DS_FF_FACE_BACK;
      a.vkCmdPushConstants(cb, d.layout_g, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &fpass);
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_op[wbuf ? 1 : 0][0]);
      a.vkCmdDraw(cb, 24, frame.first_ordered, 0, 0);
      fpass.flags = frame.flags | DS_FF_FACE_FRONT;
      a.vkCmdPushConstants(cb, d.layout_g, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &fpass);
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_op[wbuf ? 1 : 0][1]);
      a.vkCmdDraw(cb, 24, frame.first_ordered, 0, 0);
      a.vkCmdPushConstants(cb, d.layout_g, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &frame);
    }
    stamp(cb, 5);
    // The tail in runs of one kind (translucent, shadow mask, shadow) and one
    // depth-write setting. A run of masks gets a fresh run id, which the
    // masks write into the shadow plane where their depth test fails
    // (tri_mask.frag) and the shadow polygons after them test for
    // (tri_tail.frag); the id travels in the push constant's flags. The DS
    // clears its stencil per scanline when a run begins; a new id per run is
    // that, frame-wide.
    const GpuPoly* gp = static_cast<const GpuPoly*>(d.polys.ptr);
    static const bool no_tail = std::getenv("DS_VK_TRI_NOTAIL") != nullptr;   // attribution: leave the translucent tail out
    static const bool no_shadow_draw = std::getenv("DS_VK_TRI_NOSHADOWDRAW") != nullptr;   // bisecting: masks and shadows are not drawn
    static const bool no_mask = std::getenv("DS_VK_TRI_NOMASK") != nullptr;                // bisecting: masks not drawn (shadows then match nothing)
    static const bool no_shadow = std::getenv("DS_VK_TRI_NOSHADOW") != nullptr;            // bisecting: shadow polygons not drawn
    static const bool mask_as_tail = std::getenv("DS_VK_TRI_MASKASTAIL") != nullptr;       // bisecting: masks drawn with the tail pipeline (their geometry, not their pipeline)
    auto kind_of = [&](const GpuPoly& p) { return (p.flags & DS_PF_SHADOW_MASK) ? 1 : (p.flags & DS_PF_SHADOW) ? 2 : 0; };
    auto barrier = [&] {
      VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, nullptr, 0, nullptr);
    };
    u32 run = 0;
    GpuFrame ftail = frame;
    for (u32 i = frame.first_ordered; (st & 2) && !no_tail && i < npoly;) {
      const int kind = kind_of(gp[i]);
      const bool wr = (gp[i].attr & 0x800u) != 0u;
      u32 j = i + 1;
      while (j < npoly && kind_of(gp[j]) == kind && ((gp[j].attr & 0x800u) != 0u) == wr) ++j;
      if (kind != 0 && (!d.stencil || no_shadow_draw)) { i = j; continue; }   // shadows off
      if ((kind == 1 && no_mask) || (kind == 2 && no_shadow)) { i = j; continue; }
      if (kind == 1 && !mask_as_tail) {
        ftail.flags = frame.flags | (++run << DS_FF_RUN_SHIFT);
        a.vkCmdPushConstants(cb, d.layout_g, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuFrame), &ftail);
        a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_mask);
        a.vkCmdDraw(cb, 24, j - i, 0, i);
        if (!d.roaa) barrier();   // the shadows read what the masks wrote
        i = j;
        continue;
      }
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipe_tail[wbuf ? 1 : 0][wr ? 1 : 0]);
      if (d.roaa) a.vkCmdDraw(cb, 24, j - i, 0, i);
      else for (u32 k = i; k < j; ++k) { a.vkCmdDraw(cb, 24, 1, 0, k); barrier(); }
      i = j;
    }
    a.vkCmdEndRenderPass(cb);
    if (d.alias) {
      // The attachments ARE the buffers: hand the writes to the host, the
      // final pass and the downsample.
      VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    } else {
    // The attachments into the buffers: colour -> out[back], depth plane -> depth, attributes -> attr.
    {
      VkImageMemoryBarrier ib[3]{};
      VkImage imgs[3] = {d.img_col.img, d.img_z.img, d.img_attr.img};
      for (u32 i = 0; i < 3; ++i) {
        ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib[i].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib[i].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL; ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib[i].image = imgs[i]; ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, ib);
      VkBufferImageCopy rg{}; rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rg.imageExtent = {W, H, 1};
      static const bool no_copy = std::getenv("DS_VK_TRI_NOCOPY") != nullptr;   // attribution: the pass without its copies (the output is stale)
      if (!no_copy) {
        a.vkCmdCopyImageToBuffer(cb, d.img_col.img, VK_IMAGE_LAYOUT_GENERAL, vk_buf(d.out[back]), 1, &rg);
        // The depth and attribute planes are read by the final pass alone.
        if (d.post_frame) {
          a.vkCmdCopyImageToBuffer(cb, d.img_z.img, VK_IMAGE_LAYOUT_GENERAL, vk_buf(d.depth), 1, &rg);
          a.vkCmdCopyImageToBuffer(cb, d.img_attr.img, VK_IMAGE_LAYOUT_GENERAL, vk_buf(d.attr), 1, &rg);
        }
      }
      VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    }
    if (scale_ > 1 && !d.cpu_down && !(d.post_frame && (st & 10) == 10)) {
      a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.set[back], 0, nullptr);
      a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuFrame), &frame);
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_down);
      a.vkCmdDispatch(cb, (256 * 192 + 63) / 64, 1, 1);
      VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    stamp(cb, 9);
    if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) return false;
    a.vkResetFences(d.vk->dev, 1, &d.fence[0]);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (a.vkQueueSubmit(d.vk->queue, 1, &si, d.fence[0]) != VK_SUCCESS) return false;
    d.in_flight |= 1u;
  } else
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
    if (scale_ > 1 && !d.cpu_down) {
      VkMemoryBarrier mb2{}; mb2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb2.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
      a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe_down);
      a.vkCmdDispatch(cb, (256 * 192 + 63) / 64, 1, 1);
      VkMemoryBarrier mb3{}; mb3.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb3.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb3.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb3, 0, nullptr, 0, nullptr);
    }
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

bool Raster::tri() const { return d_ && d_->tri; }
bool Raster::tri_ordered() const { return d_ && d_->tri && d_->roaa; }

const Raster::PassTimes& Raster::pass_times() const { return d_->times; }
bool Raster::timing() const { return d_ && d_->timing; }

// The stall probe's reader: this process's context under
// /sys/kernel/debug/mali0/ctx/<pid>_<n>/ (atoms in flight, memory pools) and
// the device-wide utilisation and power state.
static void stall_probe_dump(const char* when) {
#if defined(__linux__)
  auto cat = [&](const std::string& path, int max_lines) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "  %s: (unreadable)\n", path.c_str()); return; }
    char line[512]; int n = 0;
    while (n < max_lines && std::fgets(line, sizeof line, f)) { std::fprintf(stderr, "  %s", line); ++n; }
    std::fclose(f);
  };
  std::fprintf(stderr, "gpu raster: STALL PROBE %s (pid %d)\n", when, static_cast<int>(getpid()));
  const std::string base = "/sys/kernel/debug/mali0/";
  char pfx[32]; std::snprintf(pfx, sizeof pfx, "%d_", static_cast<int>(getpid()));
  if (DIR* dir = opendir((base + "ctx").c_str())) {
    while (dirent* e = readdir(dir)) {
      if (std::strncmp(e->d_name, pfx, std::strlen(pfx)) != 0) continue;
      const std::string c = base + "ctx/" + e->d_name + "/";
      std::fprintf(stderr, " ctx %s atoms:\n", e->d_name); cat(c + "atoms", 40);
      std::fprintf(stderr, " mem_pool_size / lp / jit_used / jit_phys:\n"); cat(c + "mem_pool_size", 2); cat(c + "lp_mem_pool_size", 2); cat(c + "mem_jit_used", 2); cat(c + "mem_jit_phys", 2);
    }
    closedir(dir);
  }
  std::fprintf(stderr, " dvfs_utilization:\n"); cat(base + "dvfs_utilization", 2);
  std::fprintf(stderr, " gpu_memory:\n"); cat(base + "gpu_memory", 12);
  std::fprintf(stderr, " power_policy / cur_freq:\n"); cat("/sys/devices/platform/fde60000.gpu/power_policy", 1); cat("/sys/class/devfreq/fde60000.gpu/cur_freq", 1);
#else
  (void)when;
#endif
}

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
  // DS_GPU_STALL_PROBE=1: a fence that is not signalled within 100 ms is a
  // stall (docs/gpu-path-scoping.md); read the Mali driver's view of this
  // process's atoms and the GPU's utilisation WHILE it is happening, then
  // keep waiting. Linux debugfs, root -- the RG DS Plus rig.
  static const bool probe = std::getenv("DS_GPU_STALL_PROBE") != nullptr;
  if (probe) {
    VkResult r = a.vkWaitForFences(d.vk->dev, n, pending, VK_TRUE, 100'000'000ull);
    if (r == VK_TIMEOUT) {
      const auto t0 = std::chrono::steady_clock::now();
      stall_probe_dump("at +100 ms");
      a.vkWaitForFences(d.vk->dev, n, pending, VK_TRUE, UINT64_MAX);
      std::fprintf(stderr, "gpu raster: probe -- fence signalled %.1f ms after the probe\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      stall_probe_dump("after");
    }
  } else
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
  const u32 s = (d_->gen + 2) % 3;
  return d_->cpu_down ? d_->nat[s].data() : static_cast<const u32*>(d_->out_nat[s].ptr);
}

const u32* Raster::output_prev() const {
  const u32 s = (d_->gen + 1) % 3;
  return d_->cpu_down ? d_->nat[s].data() : static_cast<const u32*>(d_->out_nat[s].ptr);
}

void Raster::reduce_line(const u32* nat, u32 y) {
  Impl& d = *d_;
  if (!d.cpu_down || y >= 192) return;
  u32 s = 0;
  while (s < 3 && d.nat[s].data() != nat) ++s;
  if (s == 3) return;   // not one of ours (a CPU frame)
  if (d.nat_done[s][y].load(std::memory_order_acquire)) return;
  const u32 S = scale_, W = 256 * S;
  const u32* src = static_cast<const u32*>(d.out[s].ptr) + static_cast<size_t>(y) * S * W;
  u32* dst = d.nat[s].data() + static_cast<size_t>(y) * 256;
  for (u32 x = 0; x < 256; ++x) dst[x] = src[x * S];   // the top-left subpixel, as downsample.comp took it
  d.nat_done[s][y].store(1, std::memory_order_release);
}

void Raster::reduce_all(const u32* nat) {
  for (u32 y = 0; y < 192; ++y) reduce_line(nat, y);
}

u64 Raster::output_hires_handle() const { return d_->out[(d_->gen + 2) % 3].handle; }
size_t Raster::output_hires_bytes() const { return d_->out[0].size; }

} // namespace ds::gpu::vk
