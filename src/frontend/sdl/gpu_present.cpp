// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gpu_present.h"

#include <cstdio>
#include <cstring>

#if DSPERATE_VULKAN
#include "core/gpu/vk/vk_internal.h"

#include <chrono>
#include <unistd.h>

namespace ds::sdl {

using gpu::vk::Access;
using gpu::vk::Api;
using gpu::vk::Buffer;
using gpu::vk::Device;
using gpu::vk::DeviceInternal;

namespace {

// The SPIR-V, tracked beside the .comp (tools/gen_shaders.sh), linked in the
// way the core links its passes (vk_shaders.cpp).
__asm__(".section .rodata\n.balign 4\n.globl ds_present_spv_data\nds_present_spv_data:\n"
        ".incbin \"" DSPERATE_PRESENT_SHADER_DIR "/present.spv\"\n"
        ".globl ds_present_spv_end\nds_present_spv_end:\n.previous\n");
extern "C" const unsigned char ds_present_spv_data[], ds_present_spv_end[];

// One Vulkan context for the process: a dual-window layout has two Displays
// and there is no reason for two devices (instance creation is ~18 ms).
std::shared_ptr<Device> shared_device(std::string* why) {
  static std::weak_ptr<Device> weak;
  if (auto d = weak.lock()) return d;
  std::unique_ptr<Device> u = Device::create(why);
  if (!u) return nullptr;
  std::shared_ptr<Device> d(std::move(u));
  weak = d;
  return d;
}

constexpr u32 kFrameWords = 256 * 192;     // one screen
constexpr u32 kSlotWords = 2 * kFrameWords;  // both screens
constexpr int kSlots = 2;                  // frames in flight + the one being written
constexpr int kMaxBufs = 8;

struct Push {
  u32 a[4];        // presented w, h; logical w, h
  u32 b[4];        // rot, nviews, alpha, source base (words)
  s32 rect[2][4];
  s32 view[2][4];
};

} // namespace

struct GpuPresent::Impl {
  std::shared_ptr<Device> dev;
  const DeviceInternal* vk = nullptr;
  const Api* a = nullptr;
  VkPhysicalDeviceMemoryProperties memprops{};

  struct Imported { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDescriptorSet set = VK_NULL_HANDLE; };
  Imported bufs[kMaxBufs];
  int nbufs = 0;
  u32 w = 0, h = 0;

  Buffer src;                                  // kSlots x kSlotWords
  VkShaderModule mod = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkCommandPool cmdpool = VK_NULL_HANDLE;
  VkCommandBuffer cmd[kSlots]{};
  VkFence fence[kSlots]{};
  bool fence_live[kSlots] = {false, false};    // a submit is outstanding on this slot
  int pending_buf = -1;                        // the tier buffer of the frame in flight, not yet end_frame()'d
  int pending_slot = -1;
  u64 frame = 0;
  // Statistics for the exit line: how long the previous frame's fence made us wait.
  u64 wait_ns = 0, waits = 0, frames = 0, late = 0;
  u64 t_begin = 0, t_upload = 0, t_submit = 0, t_retire = 0;   // where present() spends its time, per frame

  bool ok() const { return pipe != VK_NULL_HANDLE; }

  u32 find_mem(u32 bits, VkMemoryPropertyFlags want) const {
    for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    for (u32 i = 0; i < memprops.memoryTypeCount; ++i) if (bits & (1u << i)) return i;
    return ~0u;
  }

  void drop_import(Imported& b) {
    if (b.view) a->vkDestroyImageView(vk->dev, b.view, nullptr);
    if (b.img) a->vkDestroyImage(vk->dev, b.img, nullptr);
    if (b.mem) a->vkFreeMemory(vk->dev, b.mem, nullptr);
    b.view = VK_NULL_HANDLE; b.img = VK_NULL_HANDLE; b.mem = VK_NULL_HANDLE;
  }

  // Import one of the tier's dma-bufs as a LINEAR B8G8R8A8 image (P0.1: the
  // explicit modifier layout with size 0, or plain LINEAR tiling without
  // the extension) and point a descriptor set at it.
  bool import(const ScanoutOut::DmabufPlane& p, Imported& b, std::string* why) {
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    const bool mod = dev->limits().drm_modifier;
    VkSubresourceLayout plane{}; plane.offset = p.offset; plane.rowPitch = p.stride_bytes;
    VkImageDrmFormatModifierExplicitCreateInfoEXT mex{};
    mex.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mex.drmFormatModifier = 0;   // DRM_FORMAT_MOD_LINEAR
    mex.drmFormatModifierPlaneCount = 1;
    mex.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.pNext = mod ? static_cast<const void*>(&mex) : nullptr;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.pNext = &ext;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {p.width, p.height, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = mod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (a->vkCreateImage(vk->dev, &ii, nullptr, &b.img) != VK_SUCCESS) { if (why) *why = "vkCreateImage (imported) failed"; return false; }
    if (!mod) {
      // Without the modifier extension the driver picks the pitch; it has to
      // be the tier's or the picture is sheared.
      VkImageSubresource sr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}; VkSubresourceLayout got{};
      a->vkGetImageSubresourceLayout(vk->dev, b.img, &sr, &got);
      if (got.rowPitch != p.stride_bytes) { if (why) *why = "LINEAR image pitch differs from the scanout pitch"; drop_import(b); return false; }
    }
    VkMemoryRequirements mr{}; a->vkGetImageMemoryRequirements(vk->dev, b.img, &mr);
    VkMemoryFdPropertiesKHR fp{}; fp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (a->vkGetMemoryFdPropertiesKHR(vk->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p.fd, &fp) != VK_SUCCESS) { if (why) *why = "vkGetMemoryFdPropertiesKHR failed"; drop_import(b); return false; }
    const u32 type = find_mem(fp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == ~0u) { if (why) *why = "no memory type for the imported dma-buf"; drop_import(b); return false; }
    const int dfd = dup(p.fd);   // the import takes ownership of the fd on success
    VkMemoryDedicatedAllocateInfo ded{}; ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO; ded.image = b.img;
    VkImportMemoryFdInfoKHR imp{}; imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR; imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = dfd;
    VkMemoryAllocateInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.pNext = &imp;
    ai.allocationSize = mr.size; ai.memoryTypeIndex = type;
    if (a->vkAllocateMemory(vk->dev, &ai, nullptr, &b.mem) != VK_SUCCESS) { close(dfd); if (why) *why = "vkAllocateMemory (import) failed"; drop_import(b); return false; }
    if (a->vkBindImageMemory(vk->dev, b.img, b.mem, 0) != VK_SUCCESS) { if (why) *why = "vkBindImageMemory failed"; drop_import(b); return false; }
    VkImageViewCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = b.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (a->vkCreateImageView(vk->dev, &vi, nullptr, &b.view) != VK_SUCCESS) { if (why) *why = "vkCreateImageView failed"; drop_import(b); return false; }
    VkDescriptorImageInfo dii{}; dii.imageView = b.view; dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo dbi{}; dbi.buffer = gpu::vk::vk_buf(src); dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = b.set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &dii;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = b.set; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &dbi;
    a->vkUpdateDescriptorSets(vk->dev, 2, w, 0, nullptr);
    return true;
  }

  bool import_all(ScanoutOut& out, std::string* why) {
    for (int i = 0; i < nbufs; ++i) drop_import(bufs[i]);
    nbufs = 0;
    const int n = out.bufs();
    if (n <= 0 || n > kMaxBufs) { if (why) *why = "tier buffer count out of range"; return false; }
    for (int i = 0; i < n; ++i) {
      ScanoutOut::DmabufPlane p;
      if (!out.dmabuf_plane(i, p)) { if (why) *why = "tier has no dma-buf for its buffer"; return false; }
      if (i == 0) { w = p.width; h = p.height; }
      if (!import(p, bufs[i], why)) return false;
      ++nbufs;
    }
    return true;
  }

  // Block on the frame in flight and give its buffer to the tier.
  void retire(ScanoutOut& out) {
    if (pending_slot < 0) return;
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = a->vkWaitForFences(vk->dev, 1, &fence[pending_slot], VK_TRUE, 100'000'000ull);
    const u64 ns = static_cast<u64>((std::chrono::steady_clock::now() - t0).count());
    wait_ns += ns; ++waits; if (ns > 1'000'000) ++late;
    if (r != VK_SUCCESS) std::fprintf(stderr, "gpu present: fence wait failed (%d); the frame is shown as it is\n", static_cast<int>(r));
    fence_live[pending_slot] = false;
    out.end_frame();
    pending_slot = -1; pending_buf = -1;
  }

  ~Impl() {
    if (!vk) return;
    a->vkDeviceWaitIdle(vk->dev);
    for (int i = 0; i < nbufs; ++i) drop_import(bufs[i]);
    for (int s = 0; s < kSlots; ++s) if (fence[s]) a->vkDestroyFence(vk->dev, fence[s], nullptr);
    if (cmdpool) a->vkDestroyCommandPool(vk->dev, cmdpool, nullptr);
    if (pool) a->vkDestroyDescriptorPool(vk->dev, pool, nullptr);
    if (pipe) a->vkDestroyPipeline(vk->dev, pipe, nullptr);
    if (layout) a->vkDestroyPipelineLayout(vk->dev, layout, nullptr);
    if (dsl) a->vkDestroyDescriptorSetLayout(vk->dev, dsl, nullptr);
    if (mod) a->vkDestroyShaderModule(vk->dev, mod, nullptr);
    if (src) dev->free(src);
    if (frames) {
      const double k = 1.0 / (static_cast<double>(frames) * 1e6);
      std::fprintf(stderr, "gpu present: %llu frames on %s; previous-frame fence wait %.3f ms mean, %llu waits over 1 ms\n",
                   static_cast<unsigned long long>(frames), dev->name().c_str(),
                   waits ? static_cast<double>(wait_ns) / static_cast<double>(waits) / 1e6 : 0.0, static_cast<unsigned long long>(late));
      std::fprintf(stderr, "gpu present: per frame -- retire %.3f ms (fence + end_frame), begin_frame %.3f, upload %.3f, record+submit %.3f\n",
                   static_cast<double>(t_retire) * k, static_cast<double>(t_begin) * k, static_cast<double>(t_upload) * k, static_cast<double>(t_submit) * k);
    }
  }
};

std::unique_ptr<GpuPresent> GpuPresent::open(ScanoutOut& out, std::string* why) {
  auto fail = [&](const char* m) { if (why) *why = m; return nullptr; };
  std::unique_ptr<GpuPresent> self(new GpuPresent());
  self->d_ = std::make_unique<Impl>();
  Impl& d = *self->d_;
  d.dev = shared_device(why);
  if (!d.dev) return nullptr;
  if (!d.dev->limits().dmabuf_import) return fail("driver has no dma-buf import");
  d.vk = d.dev->internal();
  if (!d.vk || !d.vk->api) return fail("no Vulkan context");
  d.a = d.vk->api;
  if (!d.a->vkCreateImage || !d.a->vkGetMemoryFdPropertiesKHR) return fail("image entry points missing");
  d.a->vkGetPhysicalDeviceMemoryProperties(d.vk->phys, &d.memprops);

  d.src = d.dev->alloc(sizeof(u32) * kSlotWords * kSlots, Access::CpuWrite);
  if (!d.src) return fail("source buffer allocation failed");

  VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  si.codeSize = static_cast<size_t>(ds_present_spv_end - ds_present_spv_data);
  si.pCode = reinterpret_cast<const u32*>(ds_present_spv_data);
  if (d.a->vkCreateShaderModule(d.vk->dev, &si, nullptr, &d.mod) != VK_SUCCESS) return fail("present.spv rejected by the driver");

  VkDescriptorSetLayoutBinding b[2]{};
  b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dl{}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dl.bindingCount = 2; dl.pBindings = b;
  if (d.a->vkCreateDescriptorSetLayout(d.vk->dev, &dl, nullptr, &d.dsl) != VK_SUCCESS) return fail("descriptor set layout failed");
  VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.size = sizeof(Push);
  VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &d.dsl;
  pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
  if (d.a->vkCreatePipelineLayout(d.vk->dev, &pl, nullptr, &d.layout) != VK_SUCCESS) return fail("pipeline layout failed");
  VkComputePipelineCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cp.stage.module = d.mod; cp.stage.pName = "main"; cp.layout = d.layout;
  if (d.a->vkCreateComputePipelines(d.vk->dev, VK_NULL_HANDLE, 1, &cp, nullptr, &d.pipe) != VK_SUCCESS) return fail("present pipeline failed to compile");

  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBufs}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBufs}};
  VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dp.maxSets = kMaxBufs; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (d.a->vkCreateDescriptorPool(d.vk->dev, &dp, nullptr, &d.pool) != VK_SUCCESS) return fail("descriptor pool failed");
  VkDescriptorSetLayout layouts[kMaxBufs]; for (auto& l : layouts) l = d.dsl;
  VkDescriptorSet sets[kMaxBufs];
  VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; da.descriptorPool = d.pool; da.descriptorSetCount = kMaxBufs; da.pSetLayouts = layouts;
  if (d.a->vkAllocateDescriptorSets(d.vk->dev, &da, sets) != VK_SUCCESS) return fail("descriptor sets failed");
  for (int i = 0; i < kMaxBufs; ++i) d.bufs[i].set = sets[i];

  VkCommandPoolCreateInfo cpi{}; cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex = d.vk->qfam;
  if (d.a->vkCreateCommandPool(d.vk->dev, &cpi, nullptr, &d.cmdpool) != VK_SUCCESS) return fail("command pool failed");
  VkCommandBufferAllocateInfo cba{}; cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cba.commandPool = d.cmdpool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cba.commandBufferCount = kSlots;
  if (d.a->vkAllocateCommandBuffers(d.vk->dev, &cba, d.cmd) != VK_SUCCESS) return fail("command buffers failed");
  VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (int s = 0; s < kSlots; ++s) if (d.a->vkCreateFence(d.vk->dev, &fi, nullptr, &d.fence[s]) != VK_SUCCESS) return fail("fence creation failed");

  if (!d.import_all(out, why)) return nullptr;
  return self;
}

GpuPresent::~GpuPresent() = default;

const std::string& GpuPresent::device_name() const { return d_->dev->name(); }

bool GpuPresent::reimport(ScanoutOut& out) {
  Impl& d = *d_;
  d.a->vkDeviceWaitIdle(d.vk->dev);
  d.pending_slot = -1; d.pending_buf = -1;
  d.fence_live[0] = d.fence_live[1] = false;
  std::string why;
  if (d.import_all(out, &why)) return true;
  std::fprintf(stderr, "gpu present: re-import after resize failed (%s)\n", why.c_str());
  return false;
}

void GpuPresent::flush(ScanoutOut& out) { d_->retire(out); }

bool GpuPresent::present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha) {
  Impl& d = *d_;
  const Api& a = *d.a;
  // The previous frame first: its fence, then its buffer to the tier. Only
  // then is a new buffer taken, so the tier's queue sees frames in order.
  auto now = [] { return static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()); };
  u64 t0 = now();
  d.retire(out);
  u64 t1 = now(); d.t_retire += t1 - t0;
  u32* px = out.begin_frame();
  if (!px) return false;
  u64 t2 = now(); d.t_begin += t2 - t1;
  const int buf = out.current();
  if (buf < 0 || buf >= d.nbufs) { out.end_frame(); return false; }
  const int slot = static_cast<int>(d.frame % kSlots);
  if (d.fence_live[slot]) { a.vkWaitForFences(d.vk->dev, 1, &d.fence[slot], VK_TRUE, ~0ull); d.fence_live[slot] = false; }

  // The two DS frames, into this slot's half of the source buffer.
  u32* dst = static_cast<u32*>(d.src.ptr) + static_cast<size_t>(slot) * kSlotWords;
  for (int s = 0; s < 2; ++s) std::memcpy(dst + static_cast<size_t>(s) * kFrameWords, fb[s], sizeof(u32) * kFrameWords);
  d.dev->flush(d.src, static_cast<size_t>(slot) * kSlotWords * sizeof(u32), kSlotWords * sizeof(u32));
  u64 t3 = now(); d.t_upload += t3 - t2;

  Push pc{};
  pc.a[0] = d.w; pc.a[1] = d.h; pc.a[2] = static_cast<u32>(lw); pc.a[3] = static_cast<u32>(lh);
  pc.b[0] = static_cast<u32>(rot); pc.b[1] = static_cast<u32>(nviews > 2 ? 2 : nviews); pc.b[2] = inset_alpha; pc.b[3] = static_cast<u32>(slot) * kSlotWords;
  for (int v = 0; v < static_cast<int>(pc.b[1]); ++v) {
    pc.rect[v][0] = views[v].rect.x; pc.rect[v][1] = views[v].rect.y; pc.rect[v][2] = views[v].rect.w; pc.rect[v][3] = views[v].rect.h;
    pc.view[v][0] = views[v].screen; pc.view[v][1] = views[v].shown ? 1 : 0; pc.view[v][2] = views[v].blends ? 1 : 0; pc.view[v][3] = 0;
  }

  VkCommandBuffer cb = d.cmd[slot];
  a.vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (a.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { out.end_frame(); return false; }
  // The imported image's contents are the display's business between
  // frames; every pixel is rewritten here, so UNDEFINED is the honest
  // starting layout and costs no load.
  VkImageMemoryBarrier ib{}; ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  ib.image = d.bufs[buf].img; ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ib);
  a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.bufs[buf].set, 0, nullptr);
  a.vkCmdPushConstants(cb, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
  a.vkCmdDispatch(cb, (d.w + 15) / 16, (d.h + 7) / 8, 1);
  // Hand the writes to whoever reads the dma-buf next (the compositor or the CRTC).
  VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  if (a.vkEndCommandBuffer(cb) != VK_SUCCESS) { out.end_frame(); return false; }
  VkSubmitInfo sub{}; sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
  a.vkResetFences(d.vk->dev, 1, &d.fence[slot]);
  if (a.vkQueueSubmit(d.vk->queue, 1, &sub, d.fence[slot]) != VK_SUCCESS) { out.end_frame(); return false; }
  d.fence_live[slot] = true;
  d.pending_slot = slot; d.pending_buf = buf;
  d.t_submit += now() - t3;
  ++d.frame; ++d.frames;
  return true;
}

} // namespace ds::sdl

#else  // no Vulkan in this build

namespace ds::sdl {
struct GpuPresent::Impl {};
std::unique_ptr<GpuPresent> GpuPresent::open(ScanoutOut&, std::string* why) { if (why) *why = "built without Vulkan"; return nullptr; }
GpuPresent::~GpuPresent() = default;
bool GpuPresent::reimport(ScanoutOut&) { return false; }
void GpuPresent::flush(ScanoutOut&) {}
bool GpuPresent::present(ScanoutOut&, const u32* const*, const View*, int, int, int, int, u8) { return false; }
const std::string& GpuPresent::device_name() const { static const std::string none; return none; }
} // namespace ds::sdl

#endif
