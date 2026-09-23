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

std::shared_ptr<Device> Device::shared(std::string* why) {
  static std::weak_ptr<Device> weak;
  if (auto d = weak.lock()) return d;
  std::unique_ptr<Device> u = create(why);
  if (!u) return nullptr;
  std::shared_ptr<Device> d(std::move(u));
  weak = d;
  return d;
}


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
void   Device::flush(const Buffer&, size_t, size_t) {}
const  DeviceInternal* Device::internal() const { return nullptr; }

#else

struct Device::Impl {
  void* lib = nullptr;
  Api   api{};
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue  queue = VK_NULL_HANDLE;
  u32      qfam = 0;
  bool     device_local = true;   // the chosen CpuRead type is also DEVICE_LOCAL
  DeviceInternal internal{};
  VkPhysicalDeviceMemoryProperties memprops{};

  // CpuRead must be HOST_CACHED (the CPU draws the frontend's overlay into
  // it; write-combine is drastically slower), refused otherwise. DEVICE_LOCAL is only
  // a preference (costs nothing on unified parts; a discrete dev card's
  // cached host-visible type may be system memory) so it's tried first, and
  // falling back is reported via Device::name() rather than silent.
  bool find_mem(u32 bits, Access a, u32* out, bool* was_device_local = nullptr) const {
    const VkMemoryPropertyFlags need =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      (a == Access::CpuRead ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                            : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags want =
        need | (pass == 0 ? VkMemoryPropertyFlags{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT} : 0u);
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
  // DS_VK_DEVICE: pick physical device by index or name substring; DS_VK_LIST_EXT lists them.
  d.phys = devs[0];
  if (const char* pick = std::getenv("DS_VK_DEVICE")) {
    char* end = nullptr;
    const long idx = std::strtol(pick, &end, 10);
    for (u32 i = 0; i < n; ++i) {
      VkPhysicalDeviceProperties pp{}; d.api.vkGetPhysicalDeviceProperties(devs[i], &pp);
      const bool by_index = end && *end == '\0' && idx == static_cast<long>(i);
      if (by_index || (!(end && *end == '\0') && std::strstr(pp.deviceName, pick))) { d.phys = devs[i]; break; }
    }
  }
  if (std::getenv("DS_VK_LIST_EXT"))
    for (u32 i = 0; i < n; ++i) { VkPhysicalDeviceProperties pp{}; d.api.vkGetPhysicalDeviceProperties(devs[i], &pp); std::fprintf(stderr, "vk: device %u: %s%s\n", i, pp.deviceName, devs[i] == d.phys ? " (selected)" : ""); }

  VkPhysicalDeviceProperties props{};
  d.api.vkGetPhysicalDeviceProperties(d.phys, &props);
  d.api.vkGetPhysicalDeviceMemoryProperties(d.phys, &d.memprops);
  self->name_ = props.deviceName;
  d.internal.non_coherent_atom = props.limits.nonCoherentAtomSize ? props.limits.nonCoherentAtomSize : 1;

  u32 ne = 0;
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, nullptr);
  std::vector<VkExtensionProperties> exts(ne);
  d.api.vkEnumerateDeviceExtensionProperties(d.phys, nullptr, &ne, exts.data());
  bool has_fd = false, has_dmabuf = false, has_modifier = false, has_fmtlist = false, has_foreign = false;
  for (const auto& e : exts) {
    if (!std::strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) has_fd = true;
    if (!std::strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) has_dmabuf = true;
    if (!std::strcmp(e.extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) has_modifier = true;
    if (!std::strcmp(e.extensionName, VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME)) has_fmtlist = true;
    if (!std::strcmp(e.extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) has_foreign = true;
  }
  self->limits_.dmabuf_import = has_fd && has_dmabuf;
  self->limits_.drm_modifier = self->limits_.dmabuf_import && has_modifier && has_fmtlist;
  // DS_VK_LIST_EXT=1: what this driver actually offers.
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
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { d.qfam = i; found = true; break; }
  if (!found) return set("no compute queue");

  std::vector<const char*> want;
  if (self->limits_.dmabuf_import) { want.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME); want.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME); }
  if (self->limits_.drm_modifier) { want.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME); want.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME); }
  if (self->limits_.dmabuf_import && has_foreign) want.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

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
  if (d.api.vkCreateDevice(d.phys, &di, nullptr, &d.dev) != VK_SUCCESS) return set("vkCreateDevice failed");
  d.api.vkGetDeviceQueue(d.dev, d.qfam, 0, &d.queue);

  u32 dummy = 0;
  if (!d.find_mem(~0u, Access::CpuRead, &dummy, &d.device_local)) return set("no HOST_CACHED memory type");
  if (!d.device_local) self->name_ += " (host memory, not device-local: a development fallback)";

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
  bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
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

void Device::flush(const Buffer& b, size_t offset, size_t size) {
  if (!b) return;
  Impl& d = *d_;
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

#endif // DS_VK_AVAILABLE

} // namespace ds::gpu::vk
