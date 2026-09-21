// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// P0.4 of the GPU path: what does the PRE-RESOLVED 2D/3D composite cost on the GPU?
//
// libmali on the RK3566 cannot EXPORT a dma-buf (tools/vk_probe.c, rule 3),
// so a zero-copy present has to go the other way: allocate from the dma-heap
// exactly as DmabufOut/DrmOut do, IMPORT that fd into Vulkan, let a shader
// write it, and let the existing tiers scan it out untouched. This probe
// answers, on the device:
//
//   1. does the driver import a dma-heap fd as a storage BUFFER?
//   2. does it import one as a LINEAR IMAGE (DRM modifier 0), the form a
//      graphics/present pass would rather have?
//   3. after a GPU write + fence, does the CPU (and so the display) see the
//      pixels without DMA_BUF_IOCTL_SYNC, or is the ioctl still needed?
//   4. how fast does the GPU fill the imported buffer against its own memory?
//   5. is the dma-heap's CPU mapping cached (read GB/s vs malloc)? -- decides
//      whether a readback fallback through the same buffer is viable.
//
//   aarch64-linux-gnu-gcc -O2 -std=gnu11 -o vk_composite_probe tools/vk_composite_probe.c -ldl
//   ./vk_composite_probe fill.spv img.spv [W H]        (see tools/vk_composite_probe.sh)
//   DS_DMA_HEAP=<name> picks the heap; default tries linux,cma, default_cma_region, system.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DIE(...) do { fprintf(stderr, "vk_composite_probe: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VKOK(e)  do { VkResult r_ = (e); if (r_ != VK_SUCCESS) DIE("%s -> VkResult %d", #e, (int)r_); } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// ---- Vulkan plumbing ----------------------------------------------------------
#define VK_FNS \
  F(vkCreateInstance) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties) \
  F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceQueueFamilyProperties) \
  F(vkEnumerateDeviceExtensionProperties) F(vkCreateDevice) F(vkGetDeviceQueue) \
  F(vkCreateBuffer) F(vkGetBufferMemoryRequirements) F(vkAllocateMemory) F(vkBindBufferMemory) \
  F(vkMapMemory) F(vkInvalidateMappedMemoryRanges) F(vkCreateShaderModule) \
  F(vkCreateDescriptorSetLayout) F(vkCreatePipelineLayout) F(vkCreateComputePipelines) \
  F(vkCreateDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) \
  F(vkCreateCommandPool) F(vkAllocateCommandBuffers) F(vkBeginCommandBuffer) F(vkCmdBindPipeline) \
  F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) F(vkCmdDispatch) F(vkCmdPipelineBarrier) \
  F(vkEndCommandBuffer) F(vkQueueSubmit) F(vkWaitForFences) F(vkResetFences) F(vkCreateFence) \
  F(vkDestroyBuffer) F(vkFreeMemory) F(vkGetPhysicalDeviceExternalBufferProperties) \
  F(vkCreateImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory) F(vkDestroyImage) \
  F(vkGetPhysicalDeviceImageFormatProperties2) F(vkGetMemoryFdPropertiesKHR) F(vkCreateImageView) \
  F(vkGetImageSubresourceLayout) F(vkQueueWaitIdle) F(vkGetPhysicalDeviceFormatProperties2)
#define F(n) static PFN_##n p##n;
VK_FNS
#undef F

static VkInstance inst; static VkPhysicalDevice phys; static VkDevice dev; static VkQueue queue; static uint32_t qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static int has_dmabuf, has_modifier, has_foreign;
static VkCommandPool cpool; static VkCommandBuffer cmd; static VkFence fence;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) if (bits & (1u << i)) return i;
  DIE("no memory type for bits %x", bits);
}

static void init_vulkan(void) {
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW); if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
  if (!lib) DIE("dlopen libvulkan: %s", dlerror());
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dsperate-vk-composite-probe", .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
  VKOK(ci(&ii, NULL, &inst));
#define F(n) p##n = (PFN_##n)gipa(inst, #n);
  VK_FNS
#undef F
  uint32_t n = 0; VKOK(pvkEnumeratePhysicalDevices(inst, &n, NULL)); if (!n) DIE("no device");
  VkPhysicalDevice devs[8]; if (n > 8) n = 8; VKOK(pvkEnumeratePhysicalDevices(inst, &n, devs)); phys = devs[0];
  VkPhysicalDeviceProperties props; pvkGetPhysicalDeviceProperties(phys, &props);
  pvkGetPhysicalDeviceMemoryProperties(phys, &memprops);
  uint32_t ne = 0; pvkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
  VkExtensionProperties* ext = calloc(ne, sizeof *ext); pvkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ext);
  int has_fd = 0;
  for (uint32_t i = 0; i < ne; ++i) {
    if (!strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) has_dmabuf = 1;
    if (!strcmp(ext[i].extensionName, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) has_fd = 1;
    if (!strcmp(ext[i].extensionName, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)) has_modifier = 1;
    if (!strcmp(ext[i].extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) has_foreign = 1;
  }
  free(ext);
  if (!has_dmabuf || !has_fd) { has_dmabuf = has_fd = 0; }
  uint32_t nq = 0; pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
  VkQueueFamilyProperties qf[8]; if (nq > 8) nq = 8; pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
  qfam = ~0u; for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
  const char* en[8]; uint32_t nen = 0;
  en[nen++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME; en[nen++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
  if (has_modifier) { en[nen++] = VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME; en[nen++] = VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME; }
  if (has_foreign) en[nen++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME;
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
  VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                            .enabledExtensionCount = nen, .ppEnabledExtensionNames = en };
  VkResult r = pvkCreateDevice(phys, &di, NULL, &dev);
  if (r != VK_SUCCESS && has_modifier) { // some loaders reject image_format_list; retry without the modifier pair
    nen = 2; if (has_foreign) en[nen++] = VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME; has_modifier = 0;
    di.enabledExtensionCount = nen; VKOK(pvkCreateDevice(phys, &di, NULL, &dev));
  } else VKOK(r);
  pvkGetDeviceQueue(dev, qfam, 0, &queue);
  printf("device      : %s  driver 0x%x\n", props.deviceName, props.driverVersion);
  printf("extensions  : dma_buf import yes, image_drm_format_modifier %s, queue_family_foreign %s\n",
         has_modifier ? "yes" : "NO", has_foreign ? "yes" : "NO");
  VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
  VKOK(pvkCreateCommandPool(dev, &cpi, NULL, &cpool));
  VkCommandBufferAllocateInfo cba = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
  VKOK(pvkAllocateCommandBuffers(dev, &cba, &cmd));
  VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; VKOK(pvkCreateFence(dev, &fi, NULL, &fence));
}

static VkShaderModule load_spv(const char* path) {
  FILE* f = fopen(path, "rb"); if (!f) DIE("open %s", path);
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint32_t* code = malloc((size_t)n); if (fread(code, 1, (size_t)n, f) != (size_t)n) DIE("read %s", path); fclose(f);
  VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code };
  VkShaderModule m; VKOK(pvkCreateShaderModule(dev, &si, NULL, &m)); free(code); return m;
}

typedef struct { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; VkDescriptorPool pool; } Pipe;

static Pipe make_pipe(const char* spv, VkDescriptorType type) {
  Pipe p;
  VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
  VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
  VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &p.dsl));
  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
  VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
  VKOK(pvkCreatePipelineLayout(dev, &pl, NULL, &p.layout));
  VkShaderModule sm = load_spv(spv);
  VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = p.layout };
  VKOK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p.pipe));
  VkDescriptorPoolSize ps = { .type = type, .descriptorCount = 8 };
  VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 1, .pPoolSizes = &ps };
  VKOK(pvkCreateDescriptorPool(dev, &dp, NULL, &p.pool));
  return p;
}
static VkDescriptorSet alloc_set(Pipe* p) {
  VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl };
  VkDescriptorSet s; VKOK(pvkAllocateDescriptorSets(dev, &da, &s)); return s;
}

int main(int argc, char** argv) {
  const char* spv = argc > 1 ? argv[1] : "composite.spv";
  const int iters = 100;
  setvbuf(stdout, NULL, _IOLBF, 0);
  init_vulkan();
  Pipe p; VkDescriptorSetLayoutBinding b[7]; VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * 4 };
  for (int i = 0; i < 7; ++i) b[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
  VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 7, .pBindings = b };
  VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &p.dsl));
  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
  VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
  VKOK(pvkCreatePipelineLayout(dev, &pl, NULL, &p.layout));
  VkShaderModule sm = load_spv(spv);
  VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = p.layout };
  double tc = now_ms(); VKOK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p.pipe)); printf("pipeline    : compiled in %.1f ms\n", now_ms() - tc);
  VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 1, .pPoolSizes = &ps };
  VKOK(pvkCreateDescriptorPool(dev, &dp, NULL, &p.pool));
  srand(1);
  for (uint32_t S = 1; S <= 3; ++S) {
    const uint32_t W = 256 * S, H = 192 * S; const size_t nat = 256 * 192 * 4, big = (size_t)W * H * 4;
    size_t sizes[7] = { nat, nat, nat, nat, 192 * 4, big, big };
    VkBuffer buf[7]; VkDeviceMemory mem[7]; void* map[7];
    for (int i = 0; i < 7; ++i) {
      VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizes[i], .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
      VKOK(pvkCreateBuffer(dev, &bi, NULL, &buf[i]));
      VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, buf[i], &mr);
      VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | (i == 6 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, want) };
      VKOK(pvkAllocateMemory(dev, &ai, NULL, &mem[i])); VKOK(pvkBindBufferMemory(dev, buf[i], mem[i], 0));
      VKOK(pvkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, &map[i]));
    }
    uint32_t *top = map[0], *sec = map[1], *meta = map[2], *win = map[3], *lp = map[4], *l3d = map[5];
    for (uint32_t i = 0; i < 256 * 192; ++i) {
      top[i] = 0xFF000000u | (rand() & 0x3F3F3F); sec[i] = 0xFF000000u | (rand() & 0x3F3F3F);
      uint32_t kind = (rand() & 1) ? 1u : ((rand() & 7) == 0 ? 2u : 0u);          // half 3D, some semi-OBJ
      meta[i] = (1u << (rand() & 5)) | (kind << 8) | ((rand() & 15) << 16) | ((1u << (rand() & 5)) << 24);
      win[i] = 0x20u | (rand() & 0x1F);
    }
    for (uint32_t y = 0; y < 192; ++y) { uint32_t eff = (y % 3 == 0) ? 1u : (y % 3 == 1 ? 0u : 2u); lp[y] = (0x3F3Fu | (eff << 6)) | (8u << 16) | (8u << 21) | (6u << 26); }
    for (uint32_t i = 0; i < W * H; ++i) { uint32_t a = (rand() & 3) == 0 ? 0u : ((rand() & 1) ? 31u : (rand() & 31)); l3d[i] = (rand() & 0x3F3F3F) | (a << 24); }
    VkDescriptorSet set = alloc_set(&p);
    VkDescriptorBufferInfo dbi[7]; VkWriteDescriptorSet w[7];
    for (int i = 0; i < 7; ++i) { dbi[i] = (VkDescriptorBufferInfo){ .buffer = buf[i], .range = VK_WHOLE_SIZE };
      w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] }; }
    pvkUpdateDescriptorSets(dev, 7, w, 0, NULL);
    struct { uint32_t scale, flags; } pcv = { S, 0 };
    double best = 1e9, sum = 0;
    for (int rep = 0; rep < 3; ++rep) {
      VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      VKOK(pvkBeginCommandBuffer(cmd, &cbi));
      pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
      pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, NULL);
      pvkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pcv);
      for (int i = 0; i < iters; ++i) {
        pvkCmdDispatch(cmd, W / 16, H / 8, 1);
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT };
        pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
      }
      VKOK(pvkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      double t0 = now_ms(); VKOK(pvkResetFences(dev, 1, &fence)); VKOK(pvkQueueSubmit(queue, 1, &si, fence)); VKOK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
      double t = (now_ms() - t0) / iters; if (t < best) best = t; sum += t;
    }
    // One frame alone, fence per frame: the latency a real frame pays.
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    double one = 1e9;
    for (int rep = 0; rep < 20; ++rep) {
      VKOK(pvkBeginCommandBuffer(cmd, &cbi));
      pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipe);
      pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, NULL);
      pvkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pcv);
      pvkCmdDispatch(cmd, W / 16, H / 8, 1);
      VKOK(pvkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      double t0 = now_ms(); VKOK(pvkResetFences(dev, 1, &fence)); VKOK(pvkQueueSubmit(queue, 1, &si, fence)); VKOK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
      double t = now_ms() - t0; if (t < one) one = t;
    }
    volatile uint32_t sink = ((uint32_t*)map[6])[W * H / 2]; (void)sink;
    printf("composite   : S=%u %ux%u  throughput %.3f ms/frame (best of 3x%d)  single frame round trip %.3f ms\n", S, W, H, best, iters, one);
  }
  pvkQueueWaitIdle(queue);
  return 0;
}
