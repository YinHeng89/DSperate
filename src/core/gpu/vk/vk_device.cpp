// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "core/gpu/vk/vk_device.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__linux__)
#include <dlfcn.h>
#include "core/gpu/vk/vk_internal.h"
#define DS_VK_AVAILABLE 1
#else
#define DS_VK_AVAILABLE 0
#endif

namespace ds::gpu::vk {

#if !DS_VK_AVAILABLE

struct Device::Impl {};
Device::Device() = default;
Device::~Device() = default;
std::unique_ptr<Device> Device::create(std::string* why) {
  if (why) *why = "built without Vulkan";
  return nullptr;
}
Buffer Device::alloc(size_t, Access) { return {}; }
void   Device::free(Buffer&) {}
void   Device::invalidate(const Buffer&, size_t, size_t) {}
Buffer Device::import_host(void*, size_t) { return {}; }
void   Device::flush(const Buffer&, size_t, size_t) {}
const  DeviceInternal* Device::internal() const { return nullptr; }
u64    Device::raw_device() const { return 0; }
u64    Device::raw_queue() const { return 0; }
u32    Device::queue_family() const { return 0; }

#else

struct Device::Impl {
  void* lib = nullptr;
  Api   api{};
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue  queue = VK_NULL_HANDLE;
  u32      qfam = 0;
  bool     has_host_import = false;
  bool     device_local = true;   // the chosen CpuRead type is also DEVICE_LOCAL
  DeviceInternal internal{};
  VkPhysicalDeviceMemoryProperties memprops{};

  // The memory type for an Access.
  //
  // The one requirement is cacheability. CpuRead must be HOST_CACHED, because
  // the NEON composite reads that buffer directly and on a write-combine
  // mapping it reads at 0.12 GB/s instead of 4.5 -- a 35x cliff, the same one
  // the A30's uncached fb0 has. A part with no cached host-visible type is
  // refused outright rather than run that slowly.
  //
  // DEVICE_LOCAL is a PREFERENCE, not a requirement, and the distinction
  // matters. On the unified parts this targets every host-visible type is
  // device-local anyway, so the preference costs nothing and is always met.
  // On a part with a separate device heap -- a desktop iGPU or a discrete
  // card, which is where this gets developed -- the cached host-visible types
  // are system memory and are not device-local, and insisting would refuse a
  // perfectly workable device for a property that is only free on the target.
  // So: two passes, preferred first, and the fallback is reported rather than
  // silent (Device::name()).
  bool find_mem(u32 bits, Access a, u32* out, bool* was_device_local = nullptr) const {
    const VkMemoryPropertyFlags need =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (a == Access::CpuRead ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                            : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags want =
        need | (pass == 0 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0u);
      for (u32 i = 0; i < memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) {
          *out = i;
          if (was_device_local) *was_device_local = pass == 0;
          return true;
        }
    }
    return false;
  }
};

Device::Device() : d_(std::make_unique<Impl>()) {}

Device::~Device() {
  if (!d_) return;
  if (d_->dev) { d_->api.vkDeviceWaitIdle(d_->dev); d_->api.vkDestroyDevice(d_->dev, nullptr); }
  if (d_->inst) d_->api.vkDestroyInstance(d_->inst, nullptr);
  if (d_->lib) dlclose(d_->lib);
}

std::unique_ptr<Device> Device::create(std::string* why) {
  auto set = [&](const char* m) { if (why) *why = m; return nullptr; };

  std::unique_ptr<Device> self(new Device());
  Impl& d = *self->d_;

  d.lib = dlopen("libvulkan.so.1", RTLD_NOW);
  if (!d.lib) d.lib = dlopen("libvulkan.so", RTLD_NOW);
  if (!d.lib) return set("libvulkan not present");

  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(d.lib, "vkGetInstanceProcAddr"));
  if (!gipa) return set("vkGetInstanceProcAddr missing");

  auto create_inst = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  if (!create_inst) return set("vkCreateInstance missing");

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "DSperate";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ii.pApplicationInfo = &app;
  if (create_inst(&ii, nullptr, &d.inst) != VK_SUCCESS) return set("vkCreateInstance failed");

#define F(n) d.api.n = reinterpret_cast<PFN_##n>(gipa(d.inst, #n));
  DS_VK_FNS
#undef F
  if (!d.api.vkEnumeratePhysicalDevices || !d.api.vkCreateDevice) return set("entry points missing");

  u32 n = 0;
  d.api.vkEnumeratePhysicalDevices(d.inst, &n, nullptr);
  if (!n) return set("no Vulkan device");
  std::vector<VkPhysicalDevice> devs(n);
  d.api.vkEnumeratePhysicalDevices(d.inst, &n, devs.data());
  d.phys = devs[0];

  VkPhysicalDeviceProperties props{};
  d.api.vkGetPhysicalDeviceProperties(d.phys, &props);
  d.api.vkGetPhysicalDeviceMemoryProperties(d.phys, &d.memprops);
  self->name_ = props.deviceName;
  self->limits_.max_workgroup_invocations = props.limits.maxComputeWorkGroupInvocations;
  self->limits_.max_shared_memory = props.limits.maxComputeSharedMemorySize;
  self->limits_.max_storage_range = props.limits.maxStorageBufferRange;
  d.internal.non_coherent_atom = props.limits.nonCoherentAtomSize ? props.limits.nonCoherentAtomSize : 1;

  VkPhysicalDeviceSubgroupProperties sub{};
  sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
  VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp{};
  hostp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
  sub.pNext = &hostp;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &sub;
  if (d.api.vkGetPhysicalDeviceProperties2) {
    d.api.vkGetPhysicalDeviceProperties2(d.phys, &p2);
    self->limits_.subgroup_size = sub.subgroupSize;
    if (hostp.minImportedHostPointerAlignment) self->host_align_ = hostp.minImportedHostPointerAlignment;
  }

  u32 ne = 0;
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, nullptr);
  std::vector<VkExtensionProperties> exts(ne);
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, exts.data());
  bool has_atomic64_ext = false;
  for (const auto& e : exts) {
    if (!std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) d.has_host_import = true;
    if (!std::strcmp(e.extensionName, VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME)) has_atomic64_ext = true;
  }
  // 64-bit buffer atomics: the extension (or 1.2, where it is core) plus the
  // feature bits actually being set, and shaderInt64 alongside, since the
  // key the shader builds is a 64-bit integer before it is an atomic.
  VkPhysicalDeviceShaderAtomicInt64Features at64{};
  at64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &at64;
  if (d.api.vkGetPhysicalDeviceFeatures2 && (has_atomic64_ext || props.apiVersion >= VK_API_VERSION_1_2)) {
    d.api.vkGetPhysicalDeviceFeatures2(d.phys, &f2);
    self->limits_.int64_atomics = f2.features.shaderInt64 && at64.shaderBufferInt64Atomics;
  }
  // DS_VK_LIST_EXT=1: what this driver actually offers. Worth having as a
  // knob rather than a one-off program -- the handhelds have no compiler, so
  // "does libmali support X" is otherwise a cross-build and a copy every
  // time it comes up.
  if (std::getenv("DS_VK_LIST_EXT")) {
    std::fprintf(stderr, "vk: %s, %u device extensions:\n", props.deviceName, ne);
    for (const auto& e : exts) std::fprintf(stderr, "vk:   %s\n", e.extensionName);
  }

  u32 nq = 0;
  d.api.vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, nullptr);
  std::vector<VkQueueFamilyProperties> qf(nq);
  d.api.vkGetPhysicalDeviceQueueFamilyProperties(d.phys, &nq, qf.data());
  bool found = false;
  for (u32 i = 0; i < nq; ++i)
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      d.qfam = i; found = true;
      if (qf[i].timestampValidBits) self->limits_.timestamp_period_ns = props.limits.timestampPeriod;
      break;
    }
  if (!found) return set("no compute queue");

  std::vector<const char*> want;
  if (d.has_host_import) want.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
  if (self->limits_.int64_atomics && has_atomic64_ext) want.push_back(VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME);
  // Enable exactly the two features the raster uses and nothing else the
  // query happened to return.
  VkPhysicalDeviceShaderAtomicInt64Features en64{};
  en64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
  en64.shaderBufferInt64Atomics = VK_TRUE;
  VkPhysicalDeviceFeatures2 enf{};
  enf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  enf.pNext = &en64;
  enf.features.shaderInt64 = VK_TRUE;

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qi{};
  qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qi.queueFamilyIndex = d.qfam;
  qi.queueCount = 1;
  qi.pQueuePriorities = &prio;
  VkDeviceCreateInfo di{};
  di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = static_cast<u32>(want.size());
  di.ppEnabledExtensionNames = want.empty() ? nullptr : want.data();
  if (self->limits_.int64_atomics) di.pNext = &enf;
  if (d.api.vkCreateDevice(d.phys, &di, nullptr, &d.dev) != VK_SUCCESS) return set("vkCreateDevice failed");
  d.api.vkGetDeviceQueue(d.dev, d.qfam, 0, &d.queue);

  // The rule the whole design rests on: without a cached host-visible type
  // the composite would read GPU memory at write-combine speed. Refuse here
  // rather than run 35x slower than the software path.
  u32 dummy = 0;
  if (!d.find_mem(~0u, Access::CpuRead, &dummy, &d.device_local)) return set("no HOST_CACHED memory type");
  // Say so when the cached type is not device-local. It is workable -- and on
  // a development host it is the only thing on offer -- but on the target
  // parts it should never happen, so a timing measurement taken here is not
  // one taken there.
  if (!d.device_local) self->name_ += " (host memory, not device-local: a development fallback)";
  if (!self->limits_.int64_atomics) self->name_ += " (no 64-bit atomics: the visibility pass is off)";

  d.internal.api = &d.api;
  d.internal.dev = d.dev;
  d.internal.queue = d.queue;
  d.internal.phys = d.phys;
  d.internal.qfam = d.qfam;
  return self;
}

Buffer Device::alloc(size_t size, Access access) {
  Impl& d = *d_;
  Buffer out;

  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buf = VK_NULL_HANDLE;
  if (d.api.vkCreateBuffer(d.dev, &bi, nullptr, &buf) != VK_SUCCESS) return out;

  VkMemoryRequirements mr{};
  d.api.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
  u32 type = 0;
  if (!d.find_mem(mr.memoryTypeBits, access, &type)) { d.api.vkDestroyBuffer(d.dev, buf, nullptr); return out; }

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (d.api.vkAllocateMemory(d.dev, &ai, nullptr, &mem) != VK_SUCCESS) {
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  if (d.api.vkBindBufferMemory(d.dev, buf, mem, 0) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  void* ptr = nullptr;
  if (d.api.vkMapMemory(d.dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }

  out.ptr = ptr;
  out.size = size;
  out.handle = reinterpret_cast<u64>(buf);
  out.memory = reinterpret_cast<u64>(mem);
  return out;
}

void Device::free(Buffer& b) {
  if (!b) return;
  Impl& d = *d_;
  d.api.vkDestroyBuffer(d.dev, reinterpret_cast<VkBuffer>(b.handle), nullptr);
  d.api.vkFreeMemory(d.dev, reinterpret_cast<VkDeviceMemory>(b.memory), nullptr);
  b = {};
}

void Device::invalidate(const Buffer& b, size_t offset, size_t size) {
  if (!b) return;
  Impl& d = *d_;
  VkMappedMemoryRange mr{};
  mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  mr.memory = reinterpret_cast<VkDeviceMemory>(b.memory);
  const VkDeviceSize atom = d.internal.non_coherent_atom;
  VkDeviceSize off = offset / atom * atom;
  VkDeviceSize len = size ? ((offset + size + atom - 1) / atom * atom) - off : VK_WHOLE_SIZE;
  if (len != VK_WHOLE_SIZE && off + len > b.size) len = VK_WHOLE_SIZE;
  mr.offset = off;
  mr.size = len;
  d.api.vkInvalidateMappedMemoryRanges(d.dev, 1, &mr);
}

Buffer Device::import_host(void* ptr, size_t size) {
  Impl& d = *d_;
  Buffer out;
  if (!d.has_host_import || !ptr) return out;
  if (reinterpret_cast<uintptr_t>(ptr) % host_align_) return out;   // caller must align

  VkImportMemoryHostPointerInfoEXT ip{};
  ip.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
  ip.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  ip.pHostPointer = ptr;

  VkExternalMemoryBufferCreateInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
  ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.pNext = &ext;
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  if (d.api.vkCreateBuffer(d.dev, &bi, nullptr, &buf) != VK_SUCCESS) return out;

  VkMemoryRequirements mr{};
  d.api.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
  u32 bits = mr.memoryTypeBits;
  if (d.api.vkGetMemoryHostPointerPropertiesEXT) {
    VkMemoryHostPointerPropertiesEXT hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
    if (d.api.vkGetMemoryHostPointerPropertiesEXT(
            d.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) == VK_SUCCESS)
      bits &= hp.memoryTypeBits;
  }
  u32 type = 0;
  if (!d.find_mem(bits, Access::CpuRead, &type)) { d.api.vkDestroyBuffer(d.dev, buf, nullptr); return out; }

  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.pNext = &ip;
  ai.allocationSize = size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  if (d.api.vkAllocateMemory(d.dev, &ai, nullptr, &mem) != VK_SUCCESS) {
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  if (d.api.vkBindBufferMemory(d.dev, buf, mem, 0) != VK_SUCCESS) {
    d.api.vkFreeMemory(d.dev, mem, nullptr);
    d.api.vkDestroyBuffer(d.dev, buf, nullptr);
    return out;
  }
  out.ptr = ptr;                 // the caller's own pointer: no new mapping
  out.size = size;
  out.handle = reinterpret_cast<u64>(buf);
  out.memory = reinterpret_cast<u64>(mem);
  return out;
}

void Device::flush(const Buffer& b, size_t offset, size_t size) {
  if (!b) return;
  Impl& d = *d_;
  // Only meaningful for a non-coherent mapping; harmless otherwise, and
  // cheaper to always call than to track which type each buffer landed in.
  const VkDeviceSize atom = d.internal.non_coherent_atom;
  VkDeviceSize off = offset / atom * atom;
  VkDeviceSize len = size ? ((offset + size + atom - 1) / atom * atom) - off : VK_WHOLE_SIZE;
  if (len != VK_WHOLE_SIZE && off + len > b.size) len = VK_WHOLE_SIZE;
  VkMappedMemoryRange mr{};
  mr.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  mr.memory = reinterpret_cast<VkDeviceMemory>(b.memory);
  mr.offset = off;
  mr.size = len;
  d.api.vkFlushMappedMemoryRanges(d.dev, 1, &mr);
}

const DeviceInternal* Device::internal() const { return &d_->internal; }

u64 Device::raw_device() const { return reinterpret_cast<u64>(d_->dev); }
u64 Device::raw_queue() const  { return reinterpret_cast<u64>(d_->queue); }
u32 Device::queue_family() const { return d_->qfam; }

#endif // DS_VK_AVAILABLE

} // namespace ds::gpu::vk
