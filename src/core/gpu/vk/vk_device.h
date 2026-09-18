// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
#include "core/types.h"

#include <cstddef>
#include <memory>
#include <string>

// A compute-only Vulkan context, for the GPU 3D raster (docs/gpu-raster-scoping.md).
//
// Compute only, deliberately: no surface, no swapchain, no DRM master. The
// present tiers (DrmOut / DmabufOut / FbdevOut / DispOut) keep owning the
// panel exactly as they do today, and nothing here can conflict with them.
//
// libvulkan is loaded with dlopen and every entry point goes through
// vkGetInstanceProcAddr, so the emulator neither links against it nor fails
// to start on a device that has no driver: create() returns null and the
// caller falls back to the software raster.
//
// Measured on the RG DS Plus (RK3566, Mali-G52, libmali, Vulkan 1.3.303):
// instance creation ~18 ms, device ~6 ms, pipeline compile 13-45 ms. All of
// it is startup cost; none of it may happen on a frame.

namespace ds::gpu::vk {

struct DeviceInternal;   // vk_internal.h; the backend's view of this context

// Where a buffer's memory lives, which on this hardware is entirely a
// question of how the CPU sees it. Both types are DEVICE_LOCAL on a unified
// part; the difference is the CPU mapping's cacheability, and it is a 35x
// difference, so it is a named choice rather than a flag.
enum class Access {
  // DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED. Ordinary cached memory: the
  // NEON kernels read it at malloc speed (measured: blend 4.52 GB/s against
  // 4.53 GB/s on malloc). Needs invalidate() before a CPU read. This is what
  // the 3D layer the 2D compositor reads must be.
  CpuRead,
  // DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT. Typically write-combine:
  // CPU *reads* collapse to 0.12 GB/s, the same cliff as the A30's uncached
  // fb0 (display_disp.h). Only for buffers the CPU writes and never reads.
  CpuWrite,
};

// One mapped allocation. `ptr` is the CPU mapping from vkMapMemory -- always
// use it rather than mmap'ing an exported fd, whose cacheability is the
// exporter's choice and on this driver is the wrong one. (This driver cannot
// export at all: it advertises VK_EXT_external_memory_dma_buf for import
// only. See the scoping doc, rule 3.)
struct Buffer {
  void*  ptr  = nullptr;
  size_t size = 0;
  u64    handle = 0;      // opaque VkBuffer, for the backend
  u64    memory = 0;      // opaque VkDeviceMemory
  explicit operator bool() const { return ptr != nullptr; }
};

class Device {
public:
  // Null when there is no usable Vulkan: no libvulkan, no device, no compute
  // queue, or no memory type with the cacheability CpuRead needs. Never
  // throws and never aborts -- a missing GPU is an ordinary outcome.
  static std::unique_ptr<Device> create(std::string* why = nullptr);
  // The process's one context, shared by the 3D raster (core) and the
  // present stage (frontend) so the raster's output can be bound by the
  // present without leaving the device. Created on first use, released when
  // the last holder lets go; a failed creation is not cached.
  static std::shared_ptr<Device> shared(std::string* why = nullptr);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  const std::string& name() const { return name_; }

  // Allocate and map. Returns an empty Buffer on failure.
  Buffer alloc(size_t size, Access access);
  void   free(Buffer& b);

  // Make the GPU's writes visible to a CPU read of an Access::CpuRead
  // buffer. ~0.028 ms for a 192 KB range on the G52; pass the range actually
  // read rather than the whole allocation.
  void invalidate(const Buffer& b, size_t offset, size_t size);

  // Bind an existing host allocation (VK_EXT_external_memory_host) so the GPU
  // can read it with no staging copy -- the decoded texture cache is the
  // intended user. `ptr` must be aligned to host_ptr_align(). Returns an empty
  // Buffer when the import is refused.
  Buffer import_host(void* ptr, size_t size);
  size_t host_ptr_align() const { return host_align_; }

  // Make a CPU write to an Access::CpuWrite buffer visible to the GPU. A
  // coherent mapping needs no flush, but whether a buffer landed in one is
  // the driver's choice, so this is always called and is a no-op when it can
  // be. Rounded out to nonCoherentAtomSize, which vkFlushMappedMemoryRanges
  // requires and which is easy to get silently wrong.
  void flush(const Buffer& b, size_t offset, size_t size);

  // The dispatch table and handles, for vk_raster.cpp only.
  const DeviceInternal* internal() const;

  // Opaque handles for the backend (vk_raster.cpp); zero when unavailable.
  u64 raw_device() const;
  u64 raw_queue() const;
  u32 queue_family() const;

  struct Limits {
    u32 max_workgroup_invocations = 0;   // 384 on the G52 -- 16x16 fits, 32x16 does not
    u32 max_shared_memory = 0;           // 32 KB
    u32 subgroup_size = 0;               // 8
    u32 max_storage_range = 0;           // largest storage buffer the shader may bind
    // 64-bit atomics on storage buffers (VK_KHR_shader_atomic_int64, with
    // shaderInt64). The visibility pass keys a pixel's owner as one 64-bit
    // atomicMin; without this it does not run and the ordered loop draws the
    // whole list, as it always did. The Mali-G52 and RADV both have it.
    bool int64_atomics = false;
    // GPU timestamps on the compute queue: nanoseconds per tick, 0 when the
    // queue has none. DS_VK_TIMING=1 stamps every dispatch with them.
    double timestamp_period_ns = 0;
    // dma-buf IMPORT (VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf):
    // the frontend's scanout buffers can be bound as images the GPU writes
    // (docs/gpu-path-scoping.md P0.1). drm_modifier says the LINEAR layout can
    // be stated explicitly (VK_EXT_image_drm_format_modifier); without it the
    // importer falls back to VK_IMAGE_TILING_LINEAR.
    bool dmabuf_import = false;
    bool drm_modifier = false;
  };
  const Limits& limits() const { return limits_; }

private:
  Device();
  struct Impl;
  std::unique_ptr<Impl> d_;
  std::string name_;
  size_t host_align_ = 4096;
  Limits limits_{};
};

} // namespace ds::gpu::vk
