// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/gpu_present.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

#if DSPERATE_VULKAN
#include "core/gpu/vk/vk_internal.h"
#include "core/gpu/render3d.h"

#include <chrono>
#include <unistd.h>
#include <sys/mman.h>

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

// One Vulkan context for the process (vk::Device::shared): the 3D raster in
// the core and this stage share it, so the raster's layer can be bound here.
std::shared_ptr<Device> shared_device(std::string* why) { return Device::shared(why); }

constexpr u32 kFrameWords = 256 * 192;     // one screen
constexpr u32 kPlaneWords = 4 * kFrameWords + 2 * 192;   // top, second, meta, win, line words, master brightness
constexpr u32 kMaxScale = 4;
constexpr u32 kCompWords = kFrameWords * kMaxScale * kMaxScale;
constexpr u32 kSlotWords = 2 * kFrameWords;  // both screens
constexpr int kSlots = 2;                  // frames in flight + the one being written
constexpr int kMaxBufs = 8;

struct Push {
  u32 a[4];        // presented w, h; logical w, h
  u32 b[4];        // rot, nviews, alpha, source base (words) | S << 24 | 1 << 31 (a screen is composited: view.w names it)
  s32 rect[2][4];
  s32 view[2][4];
};
struct CompPush { u32 scale, fb_base, dispcnt3d, flags; };   // flags: 1 = the edge plane is bound (smooth filter)

__asm__(".section .rodata\n.balign 4\n.globl ds_composite_spv_data\nds_composite_spv_data:\n"
        ".incbin \"" DSPERATE_PRESENT_SHADER_DIR "/composite.spv\"\n"
        ".globl ds_composite_spv_end\nds_composite_spv_end:\n.previous\n");
extern "C" const unsigned char ds_composite_spv_data[], ds_composite_spv_end[];

// The planes, once per process: both windows of a dual-window layout read
// the same engine A. Freed with the last stage.
struct Planes {
  std::shared_ptr<Device> dev;
  Buffer buf;
  ~Planes() { if (buf) dev->free(buf); }
};
std::weak_ptr<Planes> g_planes;
std::shared_ptr<Planes> shared_planes(const std::shared_ptr<Device>& dev) {
  if (auto p = g_planes.lock()) return p;
  auto p = std::make_shared<Planes>();
  p->dev = dev;
  p->buf = dev->alloc(sizeof(u32) * kPlaneWords, Access::CpuWrite);
  if (!p->buf) return nullptr;
  std::memset(p->buf.ptr, 0, sizeof(u32) * kPlaneWords);
  g_planes = p;
  return p;
}

// Whether a DS screen is the same picture as another within display
// capture's rounding: the capture keeps 5 bits a channel of the 6 the
// composite had, so a screen showing last frame's capture of the other
// differs from it by up to 2 of 63 (8 of 255) a channel. Every other row,
// every third pixel: 8 K pixels, enough to tell a copy from a new frame.
bool near_same(const u32* a, const u32* b) {
  for (u32 y = 0; y < 192; y += 2)
    for (u32 x = (y >> 1) % 3; x < 256; x += 3) {
      const u32 p = a[y * 256 + x], q = b[y * 256 + x];
      const int dr = int((p >> 16) & 255) - int((q >> 16) & 255), dg = int((p >> 8) & 255) - int((q >> 8) & 255), db = int(p & 255) - int(q & 255);
      if (dr > 12 || dr < -12 || dg > 12 || dg < -12 || db > 12 || db < -12) return false;
    }
  return true;
}

} // namespace

struct GpuPresent::Impl {
  std::shared_ptr<Device> dev;
  const DeviceInternal* vk = nullptr;
  const Api* a = nullptr;
  VkPhysicalDeviceMemoryProperties memprops{};

  struct Imported { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; VkDescriptorSet set[kSlots] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; int fd = -1; u32 stride = 0; };
  // The composite: the shared planes, a composited screen (engine A's) per slot, and a
  // descriptor set per slot rewritten each frame with that frame's layer.
  std::shared_ptr<Planes> planes;
  Buffer comp[kSlots];
  Buffer over[kSlots];                 // the frontend's overlay per slot (logical frame, pitch = lw)
  Buffer tab[kSlots];
  // Per slot: which screen its composite holds (-1 none) and whether it
  // carries the smooth filter's records; and a cached copy of the frame's
  // two DS screens, for the capture pass-through (present() below).
  int comp_screen[kSlots] = {-1, -1};
  bool comp_smooth[kSlots] = {false, false};
  std::vector<u32> fbcopy[kSlots];                  // the LCD grid's seam bitmasks per slot: view v at word v * 64, columns then rows, 32 words each (1024 bits)
  SDL_Rect over_dirty[kSlots] = {};    // what was drawn into each, to clear before its next use
  int over_lw = 0, over_lh = 0;
  VkShaderModule cmod = VK_NULL_HANDLE;
  VkDescriptorSetLayout cdsl = VK_NULL_HANDLE;
  VkPipelineLayout clayout = VK_NULL_HANDLE;
  VkPipeline cpipe = VK_NULL_HANDLE;
  VkDescriptorSet cset[kSlots] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  u64 composited = 0;   // frames that went through the composite
  u64 presented = 0;    // frames presented by this display (the dump's clock)
  int id = 0;           // which display (the dump's file name)
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
    for (int s = 0; s < kSlots; ++s) {
      VkDescriptorBufferInfo dci{}; dci.buffer = gpu::vk::vk_buf(comp[s]); dci.range = VK_WHOLE_SIZE;
      VkDescriptorBufferInfo doi{}; doi.buffer = gpu::vk::vk_buf(over[s]); doi.range = VK_WHOLE_SIZE;
      VkDescriptorBufferInfo dti{}; dti.buffer = gpu::vk::vk_buf(tab[s]); dti.range = VK_WHOLE_SIZE;
      VkDescriptorBufferInfo dpi{}; dpi.buffer = gpu::vk::vk_buf(comp[(s + 1) % kSlots]); dpi.range = VK_WHOLE_SIZE;
      VkWriteDescriptorSet w[6]{};
      w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = b.set[s]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &dii;
      w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet = b.set[s]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
      w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &dbi;
      w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[2].dstSet = b.set[s]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &dci;
      w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[3].dstSet = b.set[s]; w[3].dstBinding = 3; w[3].descriptorCount = 1;
      w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &doi;
      w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[4].dstSet = b.set[s]; w[4].dstBinding = 4; w[4].descriptorCount = 1;
      w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &dti;
      w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[5].dstSet = b.set[s]; w[5].dstBinding = 5; w[5].descriptorCount = 1;
      w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[5].pBufferInfo = &dpi;
      a->vkUpdateDescriptorSets(vk->dev, 6, w, 0, nullptr);
    }
    return true;
  }

  bool import_all(ScanoutOut& out, std::string* why) {
    for (int i = 0; i < nbufs; ++i) drop_import(bufs[i]);
    nbufs = 0;
    const int n = out.bufs();
    if (n <= 0 || n > kMaxBufs) { if (why) *why = "tier buffer count out of range"; return false; }
    // The overlays follow the tier's size (the window is small at open and
    // reopens at the panel's size; the logical frame under a rotation has
    // the same pixel count). Before the descriptor writes below, which bind them.
    {
      ScanoutOut::DmabufPlane p0;
      if (!out.dmabuf_plane(0, p0)) { if (why) *why = "tier has no dma-buf"; return false; }
      const size_t need = sizeof(u32) * p0.width * p0.height;
      for (int s = 0; s < kSlots; ++s) {
        if (over[s] && over[s].size >= need) continue;
        if (over[s]) dev->free(over[s]);
        over[s] = dev->alloc(need, Access::CpuRead);
        if (!over[s]) { if (why) *why = "overlay allocation failed"; return false; }
        std::memset(over[s].ptr, 0, need);
        dev->flush(over[s], 0, need);
      }
      over_lw = over_lh = 0;   // overlay() starts every slot clean at the new geometry
    }
    for (int i = 0; i < n; ++i) {
      ScanoutOut::DmabufPlane p;
      if (!out.dmabuf_plane(i, p)) { if (why) *why = "tier has no dma-buf for its buffer"; return false; }
      if (i == 0) { w = p.width; h = p.height; }
      bufs[i].fd = p.fd; bufs[i].stride = p.stride_bytes;
      if (!import(p, bufs[i], why)) return false;
      ++nbufs;
    }
    return true;
  }

  // DS_GPU_COMP_DUMP=<file>: after presented frames 400-403's fences, write
  // the composited screen of that slot as a PPM (when this frame composited)
  // and the panel, per display (debugging the composite's inputs).
  u32 dump_scale = 0; const u32* dump_l3d = nullptr; size_t dump_l3d_words = 0;
  void maybe_dump(int slot) {
    static const char* env = std::getenv("DS_GPU_COMP_DUMP");
    static const u64 from = std::getenv("DS_GPU_COMP_DUMP_FROM") ? std::strtoull(std::getenv("DS_GPU_COMP_DUMP_FROM"), nullptr, 10) : 400;   // first presented frame to dump
    static const u64 count = std::getenv("DS_GPU_COMP_DUMP_COUNT") ? std::strtoull(std::getenv("DS_GPU_COMP_DUMP_COUNT"), nullptr, 10) : 4;
    // DS_GPU_COMP_DUMP_ON_STALL=1: instead of a fixed window, the frames
    // presented after each GPU stall report (the frame that stalled is the
    // one being presented now).
    static const bool on_stall = std::getenv("DS_GPU_COMP_DUMP_ON_STALL") != nullptr;
    static unsigned seen_stalls = 0; static u64 dump_until = 0;
    if (on_stall) {
      const unsigned st = ds::gpu::g_gpu_stalls.load(std::memory_order_relaxed);
      if (st != seen_stalls) { seen_stalls = st; dump_until = presented + count; }
      if (!env || presented >= dump_until) return;
    } else
    if (!env || presented < from || presented >= from + count) return;
    const std::string base = std::string(env) + ".d" + std::to_string(id) + ".f" + std::to_string(presented) + (dump_scale ? "" : ".nocomp");
    const u32 W = 256 * dump_scale, H = 192 * dump_scale;
    const u32* px = static_cast<const u32*>(comp[slot].ptr);
    if (FILE* f = dump_scale ? std::fopen(base.c_str(), "wb") : nullptr) {
      std::fprintf(f, "P6\n%u %u\n255\n", W, H);
      for (u32 i = 0; i < W * H; ++i) { const u32 c = px[i]; const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c)}; std::fwrite(rgb, 1, 3, f); }
      std::fclose(f);
    }
    // And the panel buffer the tier is about to show, through its own mapping.
    if (pending_buf >= 0 && bufs[pending_buf].fd >= 0) {
      const size_t bytes = static_cast<size_t>(bufs[pending_buf].stride) * h;
      void* m = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, bufs[pending_buf].fd, 0);
      if (m != MAP_FAILED) {
        std::string pp = base + ".panel.ppm";
        if (FILE* f = std::fopen(pp.c_str(), "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", w, h);
          for (u32 y = 0; y < h; ++y) { const u32* row = reinterpret_cast<const u32*>(static_cast<const char*>(m) + static_cast<size_t>(y) * bufs[pending_buf].stride);
            for (u32 x = 0; x < w; ++x) { const u32 c = row[x]; const unsigned char rgb[3] = {static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c)}; std::fwrite(rgb, 1, 3, f); } }
          std::fclose(f);
        }
        munmap(m, bytes);
      }
      std::fprintf(stderr, "gpu present: dump at frame %llu -- slot %d, tier buffer %d, scale %u, %ux%u\n", static_cast<unsigned long long>(composited), slot, pending_buf, dump_scale, w, h);
    }
    if (dump_l3d) {
      u64 opaque = 0, trans = 0, zero = 0;
      for (size_t i = 0; i < dump_l3d_words; ++i) { const u32 a = (dump_l3d[i] >> 24) & 0x1F; if (a == 0) ++zero; else if (a == 31) ++opaque; else ++trans; }
      std::fprintf(stderr, "gpu present: dump frame 400 -- l3d %zu px: %llu opaque, %llu translucent, %llu transparent; planes mbright[0..3] %08x %08x %08x %08x top[0] %08x\n",
                   dump_l3d_words, static_cast<unsigned long long>(opaque), static_cast<unsigned long long>(trans), static_cast<unsigned long long>(zero),
                   static_cast<const u32*>(planes->buf.ptr)[4 * kFrameWords + 192], static_cast<const u32*>(planes->buf.ptr)[4 * kFrameWords + 193], static_cast<const u32*>(planes->buf.ptr)[4 * kFrameWords + 194], static_cast<const u32*>(planes->buf.ptr)[4 * kFrameWords + 195], static_cast<const u32*>(planes->buf.ptr)[0]);
    }
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
    maybe_dump(pending_slot);
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
    if (cpipe) a->vkDestroyPipeline(vk->dev, cpipe, nullptr);
    if (clayout) a->vkDestroyPipelineLayout(vk->dev, clayout, nullptr);
    if (cdsl) a->vkDestroyDescriptorSetLayout(vk->dev, cdsl, nullptr);
    if (cmod) a->vkDestroyShaderModule(vk->dev, cmod, nullptr);
    for (int s = 0; s < kSlots; ++s) if (comp[s]) dev->free(comp[s]);
    for (int s = 0; s < kSlots; ++s) if (over[s]) dev->free(over[s]);
    for (int s = 0; s < kSlots; ++s) if (tab[s]) dev->free(tab[s]);
    if (layout) a->vkDestroyPipelineLayout(vk->dev, layout, nullptr);
    if (dsl) a->vkDestroyDescriptorSetLayout(vk->dev, dsl, nullptr);
    if (mod) a->vkDestroyShaderModule(vk->dev, mod, nullptr);
    if (src) dev->free(src);
    if (frames) {
      const double k = 1.0 / (static_cast<double>(frames) * 1e6);
      std::fprintf(stderr, "gpu present: %llu frames on %s; previous-frame fence wait %.3f ms mean, %llu waits over 1 ms\n",
                   static_cast<unsigned long long>(frames), dev->name().c_str(),
                   waits ? static_cast<double>(wait_ns) / static_cast<double>(waits) / 1e6 : 0.0, static_cast<unsigned long long>(late));
      std::fprintf(stderr, "gpu present: per frame -- retire %.3f ms (fence + end_frame), begin_frame %.3f, upload %.3f, record+submit %.3f; %llu frames composited on the GPU\n",
                   static_cast<double>(t_retire) * k, static_cast<double>(t_begin) * k, static_cast<double>(t_upload) * k, static_cast<double>(t_submit) * k, static_cast<unsigned long long>(composited));
    }
  }
};

std::unique_ptr<GpuPresent> GpuPresent::open(ScanoutOut& out, std::string* why) {
  auto fail = [&](const char* m) { if (why) *why = m; return nullptr; };
  std::unique_ptr<GpuPresent> self(new GpuPresent());
  self->d_ = std::make_unique<Impl>();
  { static int next_id = 0; self->d_->id = next_id++; }
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
  for (int s = 0; s < kSlots; ++s) { d.tab[s] = d.dev->alloc(sizeof(u32) * 128, Access::CpuWrite); if (!d.tab[s]) return fail("grid table allocation failed"); std::memset(d.tab[s].ptr, 0, sizeof(u32) * 128); }

  VkShaderModuleCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  si.codeSize = static_cast<size_t>(ds_present_spv_end - ds_present_spv_data);
  si.pCode = reinterpret_cast<const u32*>(ds_present_spv_data);
  if (d.a->vkCreateShaderModule(d.vk->dev, &si, nullptr, &d.mod) != VK_SUCCESS) return fail("present.spv rejected by the driver");

  VkDescriptorSetLayoutBinding b[6]{};
  b[5].binding = 5; b[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[5].descriptorCount = 1; b[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[4].descriptorCount = 1; b[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[1].descriptorCount = 1; b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[2].descriptorCount = 1; b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[3].descriptorCount = 1; b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo dl{}; dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dl.bindingCount = 6; dl.pBindings = b;
  if (d.a->vkCreateDescriptorSetLayout(d.vk->dev, &dl, nullptr, &d.dsl) != VK_SUCCESS) return fail("descriptor set layout failed");
  VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.size = sizeof(Push);
  VkPipelineLayoutCreateInfo pl{}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pl.setLayoutCount = 1; pl.pSetLayouts = &d.dsl;
  pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
  if (d.a->vkCreatePipelineLayout(d.vk->dev, &pl, nullptr, &d.layout) != VK_SUCCESS) return fail("pipeline layout failed");
  VkComputePipelineCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cp.stage.module = d.mod; cp.stage.pName = "main"; cp.layout = d.layout;
  if (d.a->vkCreateComputePipelines(d.vk->dev, VK_NULL_HANDLE, 1, &cp, nullptr, &d.pipe) != VK_SUCCESS) return fail("present pipeline failed to compile");

  // The composite: its planes, output buffers, pipeline and per-slot sets.
  d.planes = shared_planes(d.dev);
  if (!d.planes) return fail("plane buffer allocation failed");
  for (int s = 0; s < kSlots; ++s) { d.comp[s] = d.dev->alloc(sizeof(u32) * kCompWords, Access::CpuWrite); if (!d.comp[s]) return fail("composite buffer allocation failed"); }
  {
    VkShaderModuleCreateInfo csi{}; csi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    csi.codeSize = static_cast<size_t>(ds_composite_spv_end - ds_composite_spv_data); csi.pCode = reinterpret_cast<const u32*>(ds_composite_spv_data);
    if (d.a->vkCreateShaderModule(d.vk->dev, &csi, nullptr, &d.cmod) != VK_SUCCESS) return fail("composite.spv rejected by the driver");
    VkDescriptorSetLayoutBinding cb[5]{};
    for (u32 i = 0; i < 5; ++i) { cb[i].binding = i + 1; cb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cb[i].descriptorCount = 1; cb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo cdl{}; cdl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; cdl.bindingCount = 5; cdl.pBindings = cb;
    if (d.a->vkCreateDescriptorSetLayout(d.vk->dev, &cdl, nullptr, &d.cdsl) != VK_SUCCESS) return fail("composite descriptor layout failed");
    VkPushConstantRange cpcr{}; cpcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; cpcr.size = sizeof(CompPush);
    VkPipelineLayoutCreateInfo cpl{}; cpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; cpl.setLayoutCount = 1; cpl.pSetLayouts = &d.cdsl; cpl.pushConstantRangeCount = 1; cpl.pPushConstantRanges = &cpcr;
    if (d.a->vkCreatePipelineLayout(d.vk->dev, &cpl, nullptr, &d.clayout) != VK_SUCCESS) return fail("composite pipeline layout failed");
    VkComputePipelineCreateInfo ccp{}; ccp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ccp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ccp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ccp.stage.module = d.cmod; ccp.stage.pName = "main"; ccp.layout = d.clayout;
    if (d.a->vkCreateComputePipelines(d.vk->dev, VK_NULL_HANDLE, 1, &ccp, nullptr, &d.cpipe) != VK_SUCCESS) return fail("composite pipeline failed to compile");
  }
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxBufs * kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBufs * kSlots * 5 + kSlots * 5}};
  VkDescriptorPoolCreateInfo dp{}; dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dp.maxSets = kMaxBufs * kSlots + kSlots; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (d.a->vkCreateDescriptorPool(d.vk->dev, &dp, nullptr, &d.pool) != VK_SUCCESS) return fail("descriptor pool failed");
  {
    VkDescriptorSetLayout layouts[kMaxBufs * kSlots]; for (auto& l : layouts) l = d.dsl;
    VkDescriptorSet sets[kMaxBufs * kSlots];
    VkDescriptorSetAllocateInfo da{}; da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; da.descriptorPool = d.pool; da.descriptorSetCount = kMaxBufs * kSlots; da.pSetLayouts = layouts;
    if (d.a->vkAllocateDescriptorSets(d.vk->dev, &da, sets) != VK_SUCCESS) return fail("descriptor sets failed");
    for (int i = 0; i < kMaxBufs; ++i) for (int s = 0; s < kSlots; ++s) d.bufs[i].set[s] = sets[i * kSlots + s];
    VkDescriptorSetLayout cl[kSlots]; for (auto& l : cl) l = d.cdsl;
    VkDescriptorSetAllocateInfo cda{}; cda.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; cda.descriptorPool = d.pool; cda.descriptorSetCount = kSlots; cda.pSetLayouts = cl;
    if (d.a->vkAllocateDescriptorSets(d.vk->dev, &cda, d.cset) != VK_SUCCESS) return fail("composite descriptor sets failed");
  }

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

u32* GpuPresent::overlay(int lw, int lh) {
  Impl& d = *d_;
  const int slot = static_cast<int>(d.frame % kSlots);
  if (!d.over[slot]) return nullptr;
  u32* px = static_cast<u32*>(d.over[slot].ptr);
  if (lw != d.over_lw || lh != d.over_lh) {
    // A new geometry: every slot starts clean.
    for (int s = 0; s < kSlots; ++s) { std::memset(d.over[s].ptr, 0, d.over[s].size); d.over_dirty[s] = SDL_Rect{0, 0, 0, 0}; }
    d.over_lw = lw; d.over_lh = lh;
  } else {
    const SDL_Rect r = d.over_dirty[slot];
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(lw, r.x + r.w), y1 = std::min(lh, r.y + r.h);
    for (int y = y0; y < y1; ++y) std::memset(px + static_cast<size_t>(y) * lw + x0, 0, static_cast<size_t>(x1 - x0) * sizeof(u32));
    d.over_dirty[slot] = SDL_Rect{0, 0, 0, 0};
  }
  return px;
}

GpuPresent::~GpuPresent() = default;

GpuPresent::LayerPtrs GpuPresent::layer_ptrs() {
  LayerPtrs p;
  auto pl = g_planes.lock();
  if (!pl) return p;
  u32* w = static_cast<u32*>(pl->buf.ptr);
  p.top = w; p.second = w + kFrameWords; p.meta = w + 2 * kFrameWords; p.win = w + 3 * kFrameWords;
  p.line = w + 4 * kFrameWords; p.mbright = p.line + 192;
  return p;
}

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

bool GpuPresent::present(ScanoutOut& out, const u32* const fb[2], const View* views, int nviews, int rot, int lw, int lh, u8 inset_alpha,
                         u64 hires, size_t hires_bytes, u32 scale, int hires_screen, SDL_Rect drawn, u64 edge, u32 grid) {
  Impl& d = *d_;
  const Api& a = *d.a;
  ++d.presented;
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

  // Composite engine A's screen on the GPU when this display shows it and the frame
  // has a GPU-drawn 3D layer; the planes are the core's, flushed here.
  // The 3D layer belongs to whichever screen engine A drew this frame
  // (hires_screen): a game that swaps the screens every frame, Spirit
  // Tracks, flickered when it was always screen 0.
  bool shows = false;
  for (int v = 0; v < nviews; ++v) if (views[v].screen == hires_screen && views[v].shown) shows = true;
  const bool do_comp = shows && hires != 0 && scale >= 1 && scale <= kMaxScale;
  if (do_comp) {
    d.dev->flush(d.planes->buf, 0, sizeof(u32) * kPlaneWords);
    VkDescriptorBufferInfo ci[5]{};
    ci[0].buffer = gpu::vk::vk_buf(d.src); ci[0].range = VK_WHOLE_SIZE;
    ci[1].buffer = gpu::vk::vk_buf(d.planes->buf); ci[1].range = VK_WHOLE_SIZE;
    ci[2].buffer = reinterpret_cast<VkBuffer>(hires); ci[2].range = hires_bytes;
    ci[3].buffer = gpu::vk::vk_buf(d.comp[slot]); ci[3].range = VK_WHOLE_SIZE;
    // The raster's edge plane, when this frame has one; else the output plane stands in (never read).
    ci[4].buffer = edge ? reinterpret_cast<VkBuffer>(edge) : gpu::vk::vk_buf(d.comp[slot]); ci[4].range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet cw[5]{};
    for (u32 i = 0; i < 5; ++i) { cw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; cw[i].dstSet = d.cset[slot]; cw[i].dstBinding = i + 1; cw[i].descriptorCount = 1; cw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; cw[i].pBufferInfo = &ci[i]; }
    a.vkUpdateDescriptorSets(d.vk->dev, 5, cw, 0, nullptr);
  }
  d.dump_scale = do_comp ? scale : 0;
  const bool smooth = do_comp && edge != 0 && scale == 1;   // the raster only writes the plane at 1x (vk_raster.cpp)
  // The capture pass-through. A game that shows the previous frame's display
  // capture on a screen (Spirit Tracks: the 3D swaps screens every frame and
  // the other one shows the capture of it) hands the present a DS-resolution
  // copy of a picture this stage composited a frame ago -- at panel
  // resolution, with the smooth filter's edges, or at S x. When a screen's
  // new frame is that picture within the capture's rounding, the previous
  // composite is shown again for it. Only where it would show: the filter
  // or a hi-res layer; at 1x without the filter the copy is the same picture.
  const int ps = (slot + 1) % kSlots;
  bool prev_use[2] = {false, false};
  static const bool no_pt = std::getenv("DS_GPU_NO_PT") != nullptr;   // bisecting
  const bool passthru = !no_pt && (smooth || scale > 1 || d.comp_smooth[ps]) && d.frame > 0;
  if (passthru)
    for (int v = 0; v < std::min(nviews, 2); ++v) {
      const int sc = views[v].screen;
      if (!views[v].shown || (do_comp && sc == hires_screen) || d.comp_screen[ps] != sc || d.fbcopy[ps].size() != kSlotWords) continue;
      if (near_same(fb[sc], d.fbcopy[ps].data() + static_cast<size_t>(sc) * kFrameWords)) prev_use[v] = true;
    }
  static const bool pt_debug = std::getenv("DS_GPU_PT_DEBUG") != nullptr;
  if (pt_debug && d.presented >= 398 && d.presented <= 404) {
    for (int v = 0; v < std::min(nviews, 2); ++v) {
      const int sc = views[v].screen; int maxd = 0; u32 bad = 0;
      if (d.fbcopy[ps].size() == kSlotWords) {
        const u32* a = fb[sc]; const u32* b = d.fbcopy[ps].data() + static_cast<size_t>(sc) * kFrameWords;
        for (u32 i = 0; i < kFrameWords; ++i) { const u32 p = a[i], q = b[i]; int m = 0; for (int sh = 0; sh < 24; sh += 8) m = std::max(m, std::abs(int((p >> sh) & 255) - int((q >> sh) & 255))); maxd = std::max(maxd, m); bad += m > 12; }
      }
      std::fprintf(stderr, "pt: display %d frame %llu slot %d view %d screen %d shown %d do_comp %d hires_screen %d comp_screen[ps] %d passthru %d -> use %d; vs prev fb: max diff %d, %u px over 12; edge %llx smooth %d scale %u prev_smooth %d\n",
                   d.id, static_cast<unsigned long long>(d.presented), slot, v, sc, views[v].shown ? 1 : 0, do_comp ? 1 : 0, hires_screen, d.comp_screen[ps], passthru ? 1 : 0, prev_use[v] ? 1 : 0, maxd, bad, static_cast<unsigned long long>(edge), smooth ? 1 : 0, scale, d.comp_smooth[ps] ? 1 : 0);
    }
  }
  if (smooth || scale > 1) {
    d.fbcopy[slot].resize(kSlotWords);
    for (int s = 0; s < 2; ++s) std::memcpy(d.fbcopy[slot].data() + static_cast<size_t>(s) * kFrameWords, fb[s], sizeof(u32) * kFrameWords);
  } else d.fbcopy[slot].clear();
  const bool prev_smooth = d.comp_smooth[ps];
  d.comp_screen[slot] = do_comp ? hires_screen : -1;
  d.comp_smooth[slot] = smooth;

  // The LCD grid's seam columns and rows per view, as kern::scale_row_grid
  // and Gpu::emit_scaled choose them: the first panel pixel of a source
  // pixel's run, for runs at least ceil(scale) wide (at a fractional scale
  // only the widened runs carry a seam, so the lit cells keep the integer
  // size), on every other DS pixel at exactly 2x (nine lit in sixteen rather
  // than one in four). Each axis decides for itself.
  if (grid < 256) {
    u32* t = static_cast<u32*>(d.tab[slot].ptr);
    std::memset(t, 0, sizeof(u32) * 128);
    for (int v = 0; v < std::min(nviews, 2); ++v) {
      if (!views[v].grid) continue;
      const int dim[2] = {views[v].rect.w, views[v].rect.h}, srcn[2] = {256, 192};
      for (int axis = 0; axis < 2; ++axis) {
        const u32 n = static_cast<u32>(std::max(0, dim[axis])), sn = static_cast<u32>(srcn[axis]);
        if (n == 0 || n > 1024) continue;
        const u32 min_run = std::max<u32>(2, (n + sn - 1) / sn), pitch = n == 2 * sn ? 2 : 1;
        for (u32 s = 0; s < sn; ++s) {
          const u32 x0 = (s * n + sn - 1) / sn, x1 = ((s + 1) * n + sn - 1) / sn;
          if (x1 - x0 >= min_run && s % pitch == 0 && x0 < n) t[v * 64 + axis * 32 + (x0 >> 5)] |= 1u << (x0 & 31);
        }
      }
    }
    d.dev->flush(d.tab[slot], 0, sizeof(u32) * 128);
  }
  Push pc{};
  pc.a[0] = d.w; pc.a[1] = d.h; pc.a[2] = static_cast<u32>(lw); pc.a[3] = static_cast<u32>(lh);
  static const bool no_overlay = std::getenv("DS_GPU_NO_OVERLAY") != nullptr;   // bisecting
  const bool has_over = !no_overlay && drawn.w > 0 && drawn.h > 0 && d.over[slot] && d.over_lw == lw && d.over_lh == lh;
  if (has_over) {
    d.over_dirty[slot] = drawn;
    const int y0 = std::max(0, drawn.y), y1 = std::min(lh, drawn.y + drawn.h);
    if (y1 > y0) d.dev->flush(d.over[slot], static_cast<size_t>(y0) * lw * sizeof(u32), static_cast<size_t>(y1 - y0) * lw * sizeof(u32));
  }
  if (std::getenv("DS_GPU_COMP_DUMP") && (d.composited == 5 || d.composited == 399)) {
    u64 nz = 0; if (d.over[slot]) { const u32* o = static_cast<const u32*>(d.over[slot].ptr); for (int i = 0; i < lw * lh; ++i) nz += (o[i] >> 24) != 0; }
    std::fprintf(stderr, "gpu present: overlay at frame %llu -- drawn %d,%d %dx%d, has_over %d, geometry %dx%d (recorded %dx%d), buffer %zu bytes, tier %ux%u, %llu pixels with alpha\n", static_cast<unsigned long long>(d.composited), drawn.x, drawn.y, drawn.w, drawn.h, has_over ? 1 : 0, lw, lh, d.over_lw, d.over_lh, d.over[slot].size, d.w, d.h, static_cast<unsigned long long>(nz));
  }
  pc.b[0] = static_cast<u32>(rot); pc.b[1] = static_cast<u32>(nviews > 2 ? 2 : nviews); pc.b[2] = inset_alpha | (has_over ? 0x100u : 0u) | (std::min<u32>(grid, 256) << 16);
  // The scale goes with every frame: a pass-through view reads the previous composite at it.
  pc.b[3] = static_cast<u32>(slot) * kSlotWords | (std::max<u32>(scale, 1) << 24) | (do_comp ? 0x80000000u : 0u);
  for (int v = 0; v < static_cast<int>(pc.b[1]); ++v) {
    pc.rect[v][0] = views[v].rect.x; pc.rect[v][1] = views[v].rect.y; pc.rect[v][2] = views[v].rect.w; pc.rect[v][3] = views[v].rect.h;
    pc.view[v][0] = views[v].screen; pc.view[v][1] = views[v].shown ? 1 : 0; pc.view[v][2] = views[v].blends ? 1 : 0; {
      const bool cur = do_comp && views[v].screen == hires_screen;
      pc.view[v][3] = (cur ? 1 : 0) | (views[v].grid ? 2 : 0) | (prev_use[v] ? 4 : 0) | (((cur && smooth) || (prev_use[v] && prev_smooth)) ? 8 : 0);
    }
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
  if (do_comp) {
    // The 3D layer was written by the core's raster on this same queue: order
    // its writes (compute or transfer) before this read, then the composite's
    // own writes before the present reads them.
    VkMemoryBarrier mb0{}; mb0.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb0.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT; mb0.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb0, 0, nullptr, 0, nullptr);
    CompPush cp{scale, static_cast<u32>(slot) * kSlotWords + static_cast<u32>(hires_screen) * kFrameWords, 0, smooth ? 1u : 0u};
    a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.cpipe);
    a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, 0, 1, &d.cset[slot], 0, nullptr);
    a.vkCmdPushConstants(cb, d.clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cp, &cp);
    a.vkCmdDispatch(cb, (256 * scale + 15) / 16, (192 * scale + 7) / 8, 1);
    VkMemoryBarrier mb1{}; mb1.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb1.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    a.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb1, 0, nullptr, 0, nullptr);
    ++d.composited;
  }
  a.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipe);
  a.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.bufs[buf].set[slot], 0, nullptr);
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
bool GpuPresent::present(ScanoutOut&, const u32* const*, const View*, int, int, int, int, u8, u64, size_t, u32, int, SDL_Rect, u64, u32) { return false; }
u32* GpuPresent::overlay(int, int) { return nullptr; }
GpuPresent::LayerPtrs GpuPresent::layer_ptrs() { return {}; }
const std::string& GpuPresent::device_name() const { static const std::string none; return none; }
} // namespace ds::sdl

#endif
