// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstddef>
#include <memory>
#include <string>

// A compute-only Vulkan context for the frontend's dma-buf import presenter
// (frontend/sdl/gpu_present.cpp): no surface, no swapchain, no DRM master, so
// it can't conflict with the present tiers' ownership of the panel. libvulkan
// is dlopen'd and resolved through vkGetInstanceProcAddr; create() returns
// null on a driverless machine rather than failing to start.

namespace ds::gpu::vk {

struct DeviceInternal;   // vk_internal.h; the presenter's view of this context

// Both are DEVICE_LOCAL on a unified part; the difference is the CPU mapping's cacheability.
enum class Access {
  CpuRead,    // DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED, near-malloc CPU access.
  CpuWrite,   // DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT, typically write-combine (slow CPU reads). Write-only buffers.
};

// One mapped allocation. `ptr` is vkMapMemory's CPU mapping -- use it, not an
// mmap'd exported fd (this driver only imports dma-buf, never exports).
struct Buffer {
  void*  ptr  = nullptr;
  size_t size = 0;
  u64    handle = 0;      // opaque VkBuffer
  u64    memory = 0;      // opaque VkDeviceMemory
  explicit operator bool() const { return ptr != nullptr; }
};

class Device {
public:
  // Null when there's no usable Vulkan (no libvulkan/device/compute
  // queue/CpuRead memory type). Never throws or aborts.
  static std::unique_ptr<Device> create(std::string* why = nullptr);
  // The process's one context, shared by every presenter (both windows of a
  // dual-window layout). Created on first use; a failed creation is not cached.
  static std::shared_ptr<Device> shared(std::string* why = nullptr);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const { return name_; }

  // Allocate and map. Returns an empty Buffer on failure.
  Buffer alloc(size_t size, Access access);
  void   free(Buffer& b);

  // Makes a CPU write visible to the GPU; always call this, it's a no-op when
  // the mapping is already coherent. Rounds to nonCoherentAtomSize as
  // vkFlushMappedMemoryRanges requires.
  void flush(const Buffer& b, size_t offset, size_t size);

  // The dispatch table and handles, for the presenter only.
  const DeviceInternal* internal() const;

  struct Limits {
    // dma-buf IMPORT: frontend scanout buffers bindable as images the GPU
    // writes. drm_modifier: LINEAR layout stated explicitly, else VK_IMAGE_TILING_LINEAR.
    bool dmabuf_import = false;
    bool drm_modifier = false;
  };
  const Limits& limits() const { return limits_; }

private:
  Device();
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::string name_;
  Limits limits_{};
};

} // namespace ds::gpu::vk
