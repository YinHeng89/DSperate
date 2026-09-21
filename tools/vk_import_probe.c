// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// P0.1 of the GPU path: can the GPU write the frontend's scanout buffers?
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
//   aarch64-linux-gnu-gcc -O2 -std=gnu11 -o vk_import_probe tools/vk_import_probe.c -ldl
//   ./vk_import_probe fill.spv img.spv [W H]        (see tools/vk_import_probe.sh)
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

#define DIE(...) do { fprintf(stderr, "vk_import_probe: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VKOK(e)  do { VkResult r_ = (e); if (r_ != VK_SUCCESS) DIE("%s -> VkResult %d", #e, (int)r_); } while (0)

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// ---- dma-heap / dma-buf (declared here so the sysroot need not carry them) ----
struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
#define DMA_BUF_SYNC_READ  1
#define DMA_BUF_SYNC_WRITE 2
#define DMA_BUF_SYNC_START 0
#define DMA_BUF_SYNC_END   4

static const char* heap_used = "?";
static int heap_alloc(size_t len) {
  const char* names[4] = { getenv("DS_DMA_HEAP"), "linux,cma", "default_cma_region", "system" };
  for (int i = 0; i < 4; ++i) {
    if (!names[i]) continue;
    char path[256]; snprintf(path, sizeof path, "%s%s", names[i][0] == '/' ? "" : "/dev/dma_heap/", names[i]);
    int h = open(path, O_RDONLY | O_CLOEXEC);
    if (h < 0) continue;
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    int r = ioctl(h, DMA_HEAP_IOCTL_ALLOC, &a);
    close(h);
    if (r == 0) { heap_used = names[i]; return (int)a.fd; }
  }
  return -1;
}
static int dbuf_sync(int fd, uint64_t flags) { struct dma_buf_sync s = { flags }; return ioctl(fd, DMA_BUF_IOCTL_SYNC, &s); }

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
  VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dsperate-vk-import-probe", .apiVersion = VK_API_VERSION_1_1 };
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
  if (!has_dmabuf || !has_fd) DIE("no dma-buf import extension");
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

typedef struct { uint32_t w, h, stride_px, seed; } Push;
typedef struct { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; VkDescriptorPool pool; } Pipe;

static Pipe make_pipe(const char* spv, VkDescriptorType type) {
  Pipe p;
  VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = type, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
  VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b };
  VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &p.dsl));
  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(Push) };
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

// Record + submit + wait one fill; returns wall ms of the round trip.
static double run_fill(Pipe* p, VkDescriptorSet set, Push pc, VkImage img_for_layout, int iters) {
  VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  VKOK(pvkBeginCommandBuffer(cmd, &bi));
  if (img_for_layout) {
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img_for_layout, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
  }
  pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
  pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
  for (int i = 0; i < iters; ++i) {
    Push q = pc; q.seed = pc.seed + (uint32_t)i;
    pvkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
    pvkCmdDispatch(cmd, (pc.w + 15) / 16, (pc.h + 15) / 16, 1);
    if (i + 1 < iters) {
      VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT };
      pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
  }
  // Make the writes visible to the host / foreign readers (the display).
  VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT };
  pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
  VKOK(pvkEndCommandBuffer(cmd));
  VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
  double t0 = now_ms();
  VKOK(pvkResetFences(dev, 1, &fence)); VKOK(pvkQueueSubmit(queue, 1, &si, fence));
  VKOK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
  return now_ms() - t0;
}

static uint32_t expect(uint32_t x, uint32_t y, uint32_t seed) { return 0xFF000000u | ((x + seed) & 0xFFu) | ((y & 0xFFu) << 8) | (((x ^ y) & 0xFFu) << 16); }
static size_t count_bad(const uint32_t* px, uint32_t w, uint32_t h, uint32_t stride_px, uint32_t seed) {
  size_t bad = 0;
  for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) bad += px[y * stride_px + x] != expect(x, y, seed);
  return bad;
}

// Import `fd` (dup'd inside) as device memory; returns the memory or NULL.
static VkDeviceMemory import_fd(int fd, VkDeviceSize size, VkBuffer ded_buf, VkImage ded_img, uint32_t bits_hint, const char** why) {
  VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
  VkResult r = pvkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
  if (r != VK_SUCCESS) { *why = "vkGetMemoryFdPropertiesKHR failed"; return VK_NULL_HANDLE; }
  uint32_t bits = fp.memoryTypeBits & bits_hint;
  if (!bits) { *why = "fd memoryTypeBits disjoint from the object's"; return VK_NULL_HANDLE; }
  int dfd = dup(fd);
  VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = ded_buf, .image = ded_img };
  VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dfd };
  VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size, .memoryTypeIndex = find_mem(bits, 0) };
  VkDeviceMemory mem;
  r = pvkAllocateMemory(dev, &ai, NULL, &mem);
  if (r != VK_SUCCESS) { close(dfd); *why = "vkAllocateMemory(import) failed"; return VK_NULL_HANDLE; }
  return mem;
}

static void check_pixels(const char* what, int fd, void* map, Push pc, uint32_t last_seed) {
  // Without the ioctl first: what the display controller would see if we did nothing.
  size_t bad_nosync = count_bad(map, pc.w, pc.h, pc.stride_px, last_seed);
  dbuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
  size_t bad_sync = count_bad(map, pc.w, pc.h, pc.stride_px, last_seed);
  dbuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
  printf("%-12s: pixels wrong without DMA_BUF_IOCTL_SYNC %zu, with %zu (of %u) -- %s\n", what, bad_nosync, bad_sync,
         pc.w * pc.h, bad_sync ? "IMPORT WRITES NOT VISIBLE" : bad_nosync ? "ioctl needed" : "coherent, no ioctl needed");
}

int main(int argc, char** argv) {
  const char* spv_fill = argc > 1 ? argv[1] : "import_fill.spv";
  const char* spv_img  = argc > 2 ? argv[2] : "import_img.spv";
  uint32_t W = argc > 3 ? (uint32_t)atoi(argv[3]) : 640, H = argc > 4 ? (uint32_t)atoi(argv[4]) : 480;
  const int iters = 50;
  setvbuf(stdout, NULL, _IOLBF, 0);
  init_vulkan();

  const uint32_t stride_px = W;                     // DmabufOut/DrmOut: tight rows, XR24
  const size_t len = (size_t)stride_px * H * 4;
  int fd = heap_alloc(len);
  if (fd < 0) DIE("dma-heap alloc failed (DS_DMA_HEAP?)");
  printf("dma-heap    : %s, %zu KB, fd %d\n", heap_used, len / 1024, fd);
  void* map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) DIE("mmap dma-buf");

  // 5. CPU cacheability of the dma-heap mapping, vs malloc.
  {
    uint32_t* m = malloc(len); memset(m, 1, len);
    dbuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE); memset(map, 1, len);
    volatile uint64_t sink = 0;
    for (int arm = 0; arm < 2; ++arm) {
      const uint32_t* src = arm ? (const uint32_t*)map : m;
      double best_r = 1e9, best_w = 1e9;
      for (int rep = 0; rep < 5; ++rep) {
        double t0 = now_ms(); uint64_t s = 0; for (size_t i = 0; i < len / 4; ++i) s += src[i]; sink += s; double tr = now_ms() - t0;
        t0 = now_ms(); memset((void*)src, rep, len); double tw = now_ms() - t0;
        if (tr < best_r) best_r = tr;
        if (tw < best_w) best_w = tw;
      }
      printf("cpu %-8s: read %.2f GB/s, write %.2f GB/s\n", arm ? "dma-heap" : "malloc", len / best_r / 1e6, len / best_w / 1e6);
    }
    dbuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    free(m);
  }

  Push pc = { W, H, stride_px, 0 };

  // 1. Import as a storage BUFFER.
  {
    VkPhysicalDeviceExternalBufferInfo bq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkExternalBufferProperties bp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    pvkGetPhysicalDeviceExternalBufferProperties(phys, &bq, &bp);
    int importable = (bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    printf("buffer      : query -- importable %s%s\n", importable ? "yes" : "NO",
           (bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? " (dedicated only)" : "");
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext, .size = len, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf; VKOK(pvkCreateBuffer(dev, &bi, NULL, &buf));
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, buf, &mr);
    const char* why = ""; VkDeviceMemory mem = import_fd(fd, mr.size > len ? mr.size : len, buf, VK_NULL_HANDLE, mr.memoryTypeBits, &why);
    if (!mem) printf("buffer      : import FAILED -- %s\n", why);
    else {
      VKOK(pvkBindBufferMemory(dev, buf, mem, 0));
      Pipe p = make_pipe(spv_fill, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
      VkDescriptorSet set = alloc_set(&p);
      VkDescriptorBufferInfo dbi = { .buffer = buf, .range = VK_WHOLE_SIZE };
      VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
      pvkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
      run_fill(&p, set, pc, VK_NULL_HANDLE, 1);                       // warm
      double t = run_fill(&p, set, pc, VK_NULL_HANDLE, iters);
      printf("buffer      : import OK; GPU fill %ux%u = %.3f ms/frame (%.2f GB/s)\n", W, H, t / iters, len / (t / iters) / 1e6);
      check_pixels("buffer", fd, map, pc, (uint32_t)(iters - 1));
      // 4. Same fill into the driver's own device-local memory, for the ratio.
      VkBufferCreateInfo bi2 = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
      VkBuffer own; VKOK(pvkCreateBuffer(dev, &bi2, NULL, &own));
      VkMemoryRequirements mr2; pvkGetBufferMemoryRequirements(dev, own, &mr2);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr2.size, .memoryTypeIndex = find_mem(mr2.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      VkDeviceMemory om; VKOK(pvkAllocateMemory(dev, &ai, NULL, &om)); VKOK(pvkBindBufferMemory(dev, own, om, 0));
      VkDescriptorSet set2 = alloc_set(&p); dbi.buffer = own; w.dstSet = set2; pvkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
      run_fill(&p, set2, pc, VK_NULL_HANDLE, 1);
      double t2 = run_fill(&p, set2, pc, VK_NULL_HANDLE, iters);
      printf("buffer      : same fill into device-local memory %.3f ms/frame -- imported is %.2fx\n", t2 / iters, (t / iters) / (t2 / iters));
    }
  }

  // 2. Import as a LINEAR IMAGE (DRM modifier 0 when the extension exists, else LINEAR tiling).
  {
    const VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;   // XR24 / AR24
    const uint64_t LINEAR = 0;
    if (has_modifier) {
      VkDrmFormatModifierPropertiesListEXT ml = { .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
      VkFormatProperties2 fp2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &ml };
      pvkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp2);
      VkDrmFormatModifierPropertiesEXT mods[32]; ml.pDrmFormatModifierProperties = mods; if (ml.drmFormatModifierCount > 32) ml.drmFormatModifierCount = 32;
      pvkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp2);
      printf("image       : %u modifiers for B8G8R8A8:", ml.drmFormatModifierCount);
      for (uint32_t i = 0; i < ml.drmFormatModifierCount; ++i)
        printf(" 0x%llx(planes %u, storage %s)", (unsigned long long)mods[i].drmFormatModifier, mods[i].drmFormatModifierPlaneCount,
               (mods[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "y" : "n");
      printf("\n");
    }
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, .drmFormatModifier = LINEAR, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceExternalImageFormatInfo eif = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .pNext = has_modifier ? (void*)&modi : NULL, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkPhysicalDeviceImageFormatInfo2 ifi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &eif, .format = fmt, .type = VK_IMAGE_TYPE_2D,
      .tiling = has_modifier ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_STORAGE_BIT };
    VkExternalImageFormatProperties eip = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 ifp = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eip };
    VkResult qr = pvkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp);
    int importable = qr == VK_SUCCESS && (eip.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
    printf("image       : query (%s, STORAGE) -- %s, importable %s\n", has_modifier ? "modifier LINEAR" : "LINEAR tiling",
           qr == VK_SUCCESS ? "format supported" : "FORMAT/TILING/USAGE UNSUPPORTED", importable ? "yes" : "NO");
    if (importable) {
      VkSubresourceLayout plane = { .offset = 0, .size = 0, .rowPitch = stride_px * 4 };
      VkImageDrmFormatModifierExplicitCreateInfoEXT mex = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .drmFormatModifier = LINEAR, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane };
      VkExternalMemoryImageCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = has_modifier ? (void*)&mex : NULL, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
      VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = ifi.tiling, .usage = VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
      VkImage img; VkResult r = pvkCreateImage(dev, &ii, NULL, &img);
      if (r != VK_SUCCESS && has_modifier) {
        printf("image       : create with explicit modifier layout FAILED (%d) -- retrying as LINEAR tiling\n", (int)r);
        ext.pNext = NULL; ii.tiling = VK_IMAGE_TILING_LINEAR; has_modifier = 0;
        r = pvkCreateImage(dev, &ii, NULL, &img);
      }
      if (r != VK_SUCCESS) printf("image       : create FAILED (%d)\n", (int)r);
      else {
        VkMemoryRequirements mr; pvkGetImageMemoryRequirements(dev, img, &mr);
        VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout got;
        if (has_modifier) sr.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
        pvkGetImageSubresourceLayout(dev, img, &sr, &got);
        printf("image       : requires %llu KB align %llu, driver rowPitch %llu (ours %u)\n", (unsigned long long)mr.size / 1024, (unsigned long long)mr.alignment, (unsigned long long)got.rowPitch, stride_px * 4);
        const char* why = ""; VkDeviceMemory mem = import_fd(fd, mr.size > len ? mr.size : len, VK_NULL_HANDLE, img, mr.memoryTypeBits, &why);
        if (!mem) printf("image       : import FAILED -- %s\n", why);
        else if (got.rowPitch != stride_px * 4) printf("image       : driver pitch differs from the scanout pitch -- LINEAR image import unusable as-is\n");
        else {
          VKOK(pvkBindImageMemory(dev, img, mem, 0));
          VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
          VkImageView view; VKOK(pvkCreateImageView(dev, &vi, NULL, &view));
          Pipe p = make_pipe(spv_img, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
          VkDescriptorSet set = alloc_set(&p);
          VkDescriptorImageInfo dii = { .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
          VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &dii };
          pvkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
          run_fill(&p, set, pc, img, 1);
          double t = run_fill(&p, set, pc, img, iters);
          printf("image       : import OK; GPU imageStore fill = %.3f ms/frame\n", t / iters);
          check_pixels("image", fd, map, pc, (uint32_t)(iters - 1));
        }
      }
    }
  }
  pvkQueueWaitIdle(queue);
  printf("done\n");
  return 0;
}
