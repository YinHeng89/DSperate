// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Vulkan compute feasibility probe for the RG DS Plus (RK3566, Mali-G52,
// ARM proprietary driver, Vulkan 1.3).
//
// Decides, before any renderer is written, whether a compute 3D raster can
// pay for itself on this board. The three numbers that matter:
//
//   1. Round-trip latency: submit a dispatch that writes a DS-sized plane,
//      wait on the fence, invalidate, read it back. This is the floor cost
//      of one GPU frame, and it replaces per-line `sync_line` overlap.
//   2. CPU time of that round trip, separately from wall time. The GLES2
//      A/B lost because the driver's CPU cost exceeded the work it replaced
//      on a board where four A55s are already spoken for. Wall time being
//      good is not enough; the emulation thread needs the core back.
//   3. How both scale to 2x / 4x internal resolution, which is the only
//      thing a GPU raster can do that three NEON workers cannot.
//
// Plus two capability checks that decide the pipeline's shape:
//   - dma-buf export (VK_EXT_external_memory_dma_buf): can the GPU's output
//     be scanned out by DmabufOut with no readback at all?
//   - host-pointer import (VK_EXT_external_memory_host): can the decoded
//     texture cache and VRAM be visible to the GPU without a staging copy?
//
// Loads libvulkan by dlopen so it cross-links with no Vulkan library on the
// build host. The SPIR-V is compiled on the device by glslc; see
// tools/vk_probe.sh.
//
//   aarch64-linux-gnu-gcc -O2 -o vk_probe tools/vk_probe.c -ldl
//   ./vk_probe trivial.spv fill.spv

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_NONE
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// ---- plumbing ---------------------------------------------------------------

#define DIE(...) do { fprintf(stderr, "vk_probe: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VKOK(e)  do { VkResult r_ = (e); if (r_ != VK_SUCCESS) DIE("%s -> VkResult %d", #e, (int)r_); } while (0)

static double now_wall(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;   // ms
}
static double now_cpu(void) {
  struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
  return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;   // ms
}
// "1,2,4" -> the list, capped at 8 entries; a NULL or empty string leaves
// the caller's default in place.
static void parse_list(const char* s, uint32_t* out, uint32_t* n) {
  if (!s || !*s) return;
  uint32_t k = 0;
  for (const char* p = s; *p && k < 8; ) {
    out[k++] = (uint32_t)strtoul(p, (char**)&p, 10);
    while (*p == ',' || *p == ' ') ++p;
  }
  if (k) *n = k;
}

static int cmp_d(const void* a, const void* b) {
  double x = *(const double*)a, y = *(const double*)b;
  return (x > y) - (x < y);
}

// The entry points we use, fetched through vkGetInstanceProcAddr.
#define VK_FNS \
  F(vkCreateInstance) F(vkEnumeratePhysicalDevices) F(vkGetPhysicalDeviceProperties) \
  F(vkGetPhysicalDeviceProperties2) F(vkGetPhysicalDeviceMemoryProperties) \
  F(vkGetPhysicalDeviceQueueFamilyProperties) F(vkEnumerateDeviceExtensionProperties) \
  F(vkCreateDevice) F(vkGetDeviceQueue) F(vkCreateBuffer) F(vkGetBufferMemoryRequirements) \
  F(vkAllocateMemory) F(vkBindBufferMemory) F(vkMapMemory) F(vkInvalidateMappedMemoryRanges) \
  F(vkCreateShaderModule) F(vkCreateDescriptorSetLayout) F(vkCreatePipelineLayout) \
  F(vkCreateComputePipelines) F(vkCreateDescriptorPool) F(vkAllocateDescriptorSets) \
  F(vkUpdateDescriptorSets) F(vkCreateCommandPool) F(vkAllocateCommandBuffers) \
  F(vkBeginCommandBuffer) F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) \
  F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkEndCommandBuffer) F(vkQueueSubmit) \
  F(vkWaitForFences) F(vkResetFences) F(vkCreateFence) F(vkDeviceWaitIdle) F(vkDestroyBuffer) \
  F(vkFreeMemory) F(vkGetMemoryFdKHR) F(vkGetPhysicalDeviceExternalBufferProperties) \
  F(vkCreateImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory) F(vkDestroyImage) \
  F(vkGetPhysicalDeviceImageFormatProperties2)

#define F(n) static PFN_##n p##n;
VK_FNS
#undef F

// ---- device setup -----------------------------------------------------------

static VkInstance       inst;
static VkPhysicalDevice phys;
static VkDevice         dev;
static VkQueue          queue;
static uint32_t         qfam;
static VkPhysicalDeviceMemoryProperties memprops;
static int has_dmabuf, has_hostmem;
static VkDeviceSize host_ptr_align = 4096;

// A memory type that is device-local and host-visible: on a unified-memory
// part this is what makes readback a cached load rather than a copy.
// `cached` picks HOST_CACHED (CPU reads) over HOST_COHERENT (CPU writes).
static uint32_t find_mem(uint32_t bits, int cached) {
  VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                             | (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return i;
  DIE("no host-visible memory type");
}

static void init_vulkan(void) {
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
  if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
  if (!lib) DIE("dlopen libvulkan: %s", dlerror());
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  if (!gipa) DIE("no vkGetInstanceProcAddr");

  PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "dsperate-vk-probe", .apiVersion = VK_API_VERSION_1_1 };
  VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
  double t0 = now_wall();
  VKOK(ci(&ii, NULL, &inst));
  double t_inst = now_wall() - t0;

#define F(n) p##n = (PFN_##n)gipa(inst, #n);
  VK_FNS
#undef F

  uint32_t n = 0;
  VKOK(pvkEnumeratePhysicalDevices(inst, &n, NULL));
  if (!n) DIE("no physical devices");
  VkPhysicalDevice list[8]; if (n > 8) n = 8;
  VKOK(pvkEnumeratePhysicalDevices(inst, &n, list));
  phys = list[0];

  VkPhysicalDeviceProperties props;
  pvkGetPhysicalDeviceProperties(phys, &props);
  pvkGetPhysicalDeviceMemoryProperties(phys, &memprops);

  uint32_t ne = 0;
  pvkEnumerateDeviceExtensionProperties(phys, NULL, &ne, NULL);
  VkExtensionProperties* ext = calloc(ne, sizeof *ext);
  pvkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ext);
  for (uint32_t i = 0; i < ne; ++i) {
    if (!strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) has_dmabuf = 1;
    if (!strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))    has_hostmem = 1;
  }
  free(ext);

  if (has_hostmem) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
    pvkGetPhysicalDeviceProperties2(phys, &p2);
    host_ptr_align = hp.minImportedHostPointerAlignment;
  }

  uint32_t nq = 0;
  pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
  VkQueueFamilyProperties* qf = calloc(nq, sizeof *qf);
  pvkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
  qfam = ~0u;
  for (uint32_t i = 0; i < nq; ++i) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
  if (qfam == ~0u) DIE("no compute queue");
  free(qf);

  const char* denable[4]; uint32_t nde = 0;
  if (has_dmabuf)  { denable[nde++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
                     denable[nde++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME; }
  if (has_hostmem) { denable[nde++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME; }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
  VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi, .enabledExtensionCount = nde, .ppEnabledExtensionNames = denable };
  double t1 = now_wall();
  VKOK(pvkCreateDevice(phys, &di, NULL, &dev));
  double t_dev = now_wall() - t1;
  pvkGetDeviceQueue(dev, qfam, 0, &queue);

  printf("device      : %s  api %u.%u.%u  driver 0x%x\n", props.deviceName,
         VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
         VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
  printf("init        : instance %.2f ms, device %.2f ms\n", t_inst, t_dev);
  printf("limits      : wg_invocations %u, shared %u KB, timestampPeriod %.1f ns\n",
         props.limits.maxComputeWorkGroupInvocations,
         props.limits.maxComputeSharedMemorySize / 1024, props.limits.timestampPeriod);
  printf("extensions  : dma_buf %s, external_memory_host %s (align %llu)\n",
         has_dmabuf ? "yes" : "NO", has_hostmem ? "yes" : "NO", (unsigned long long)host_ptr_align);
}

// ---- one compute pass -------------------------------------------------------

typedef struct {
  VkBuffer buf_in, buf_out;
  VkDeviceMemory mem_in, mem_out;
  void *map_in, *map_out;
  VkDeviceSize size_in, size_out;
  VkPipeline pipe;
  VkPipelineLayout layout;
  VkDescriptorSet dset;
  VkCommandBuffer cmd;              // one buffer, `splits` dispatches in it
  VkCommandBuffer cmds[192];        // one buffer per split, for the naive arm
  VkCommandPool   cpool;
  VkFence fence;
} Pass;

#define MAX_SPLITS 192

static void make_buffer(VkDeviceSize size, int cached, VkBuffer* buf, VkDeviceMemory* mem, void** map) {
  VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
  VKOK(pvkCreateBuffer(dev, &bi, NULL, buf));
  VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, *buf, &mr);
  VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                              .memoryTypeIndex = find_mem(mr.memoryTypeBits, cached) };
  VKOK(pvkAllocateMemory(dev, &ai, NULL, mem));
  VKOK(pvkBindBufferMemory(dev, *buf, *mem, 0));
  VKOK(pvkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
}

static VkShaderModule load_spv(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) DIE("open %s", path);
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint32_t* code = malloc((size_t)n);
  if (fread(code, 1, (size_t)n, f) != (size_t)n) DIE("read %s", path);
  fclose(f);
  VkShaderModuleCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                  .codeSize = (size_t)n, .pCode = code };
  VkShaderModule m; VKOK(pvkCreateShaderModule(dev, &si, NULL, &m)); free(code);
  return m;
}

// Push constants the shaders take. The 2D shader uses all of them; the 3D
// stand-ins read only the first three, which is legal -- a shader's block
// may be a prefix of the layout's range.
typedef struct { uint32_t w, h, work, y0, rows, bgs, diverge; } Push;

static void build_pass(Pass* p, const char* spv, VkDeviceSize size_in, VkDeviceSize size_out) {
  p->size_in = size_in; p->size_out = size_out;
  make_buffer(size_in,  0, &p->buf_in,  &p->mem_in,  &p->map_in);    // coherent: CPU writes
  make_buffer(size_out, 1, &p->buf_out, &p->mem_out, &p->map_out);   // cached:   CPU reads
  memset(p->map_in, 0x5A, size_in);

  VkDescriptorSetLayoutBinding b[2] = {
    { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
  VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                         .bindingCount = 2, .pBindings = b };
  VkDescriptorSetLayout dsl; VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));

  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(Push) };
  VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .setLayoutCount = 1, .pSetLayouts = &dsl,
                                    .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
  VKOK(pvkCreatePipelineLayout(dev, &pl, NULL, &p->layout));

  VkShaderModule sm = load_spv(spv);
  VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
    .layout = p->layout };
  double t0 = now_wall();
  VKOK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p->pipe));
  printf("pipeline    : %-12s compiled in %.2f ms\n", spv, now_wall() - t0);

  VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
  VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                    .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
  VkDescriptorPool pool; VKOK(pvkCreateDescriptorPool(dev, &dp, NULL, &pool));
  VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                     .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
  VKOK(pvkAllocateDescriptorSets(dev, &da, &p->dset));

  VkDescriptorBufferInfo bi0 = { .buffer = p->buf_in,  .range = VK_WHOLE_SIZE };
  VkDescriptorBufferInfo bi1 = { .buffer = p->buf_out, .range = VK_WHOLE_SIZE };
  VkWriteDescriptorSet w[2] = {
    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->dset, .dstBinding = 0,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi0 },
    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p->dset, .dstBinding = 1,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi1 } };
  pvkUpdateDescriptorSets(dev, 2, w, 0, NULL);

  VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = qfam };
  VkCommandPool cpool; VKOK(pvkCreateCommandPool(dev, &cpi, NULL, &cpool));
  VkCommandBufferAllocateInfo cba = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1 };
  VKOK(pvkAllocateCommandBuffers(dev, &cba, &p->cmd));
  p->cpool = cpool;
  VkCommandBufferAllocateInfo cbn = cba; cbn.commandBufferCount = MAX_SPLITS;
  VKOK(pvkAllocateCommandBuffers(dev, &cbn, p->cmds));
  VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
  VKOK(pvkCreateFence(dev, &fi, NULL, &p->fence));
}

// Record the frame the way a renderer would: one command buffer, reused
// every frame. `splits` is how many line-ranges the frame is cut into --
// 1 for the lazy-2D common case (Engine2D batches the whole frame at the
// last display line), more for a frame whose journal forces `replay_to()`
// catch-ups at mid-frame VRAM writes, display capture or the display FIFO.
// The ranges write disjoint lines, so they need no barrier between them;
// only the host read at the end does.
static void record_split(Pass* p, Push pc, uint32_t splits) {
  if (splits < 1) splits = 1;
  VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  VKOK(pvkBeginCommandBuffer(p->cmd, &bi));
  pvkCmdBindPipeline(p->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
  pvkCmdBindDescriptorSets(p->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &p->dset, 0, NULL);
  uint32_t per = (pc.h + splits - 1) / splits;
  for (uint32_t s = 0; s < splits; ++s) {
    Push q = pc; q.y0 = s * per; q.rows = (q.y0 + per > pc.h) ? pc.h - q.y0 : per;
    if (q.rows == 0) continue;
    pvkCmdPushConstants(p->cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
    pvkCmdDispatch(p->cmd, (pc.w + 15) / 16, (q.rows + 15) / 16, 1);
  }
  VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
  pvkCmdPipelineBarrier(p->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                        0, 1, &mb, 0, NULL, 0, NULL);
  VKOK(pvkEndCommandBuffer(p->cmd));
}

// The naive arm of the same thing: one submit and one fence wait per
// catch-up, which is what a port that syncs at every journal boundary would
// actually do. The gap between this and record_split is the cost of getting
// the batching wrong.
static void record_each(Pass* p, Push pc, uint32_t splits) {
  uint32_t per = (pc.h + splits - 1) / splits;
  for (uint32_t s = 0; s < splits; ++s) {
    Push q = pc; q.y0 = s * per; q.rows = (q.y0 + per > pc.h) ? pc.h - q.y0 : per;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VKOK(pvkBeginCommandBuffer(p->cmds[s], &bi));
    pvkCmdBindPipeline(p->cmds[s], VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    pvkCmdBindDescriptorSets(p->cmds[s], VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &p->dset, 0, NULL);
    pvkCmdPushConstants(p->cmds[s], p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof q, &q);
    if (q.rows) pvkCmdDispatch(p->cmds[s], (pc.w + 15) / 16, (q.rows + 15) / 16, 1);
    VKOK(pvkEndCommandBuffer(p->cmds[s]));
  }
}

// One frame: submit, wait, invalidate, touch every output pixel (what the
// 2D compositor would do when it reads the 3D layer back).
static void run_frame(Pass* p, double* wall, double* cpu, double* readback, uint64_t* sink) {
  double w0 = now_wall(), c0 = now_cpu();
  VKOK(pvkResetFences(dev, 1, &p->fence));
  VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &p->cmd };
  VKOK(pvkQueueSubmit(queue, 1, &si, p->fence));
  VKOK(pvkWaitForFences(dev, 1, &p->fence, VK_TRUE, 1000ull * 1000 * 1000));
  double w1 = now_wall();
  VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                             .memory = p->mem_out, .offset = 0, .size = VK_WHOLE_SIZE };
  VKOK(pvkInvalidateMappedMemoryRanges(dev, 1, &mr));
  uint64_t s = 0;
  const uint32_t* px = (const uint32_t*)p->map_out;
  for (VkDeviceSize i = 0; i < p->size_out / 4; ++i) s += px[i];
  double w2 = now_wall(), c1 = now_cpu();
  *wall = w1 - w0; *readback = w2 - w1; *cpu = c1 - c0; *sink += s;
}

// The naive arm's frame: `splits` submits, each waited on before the next,
// as a port that synchronises at every journal boundary would.
static void run_frame_each(Pass* p, uint32_t splits, double* wall, double* cpu, uint64_t* sink) {
  double w0 = now_wall(), c0 = now_cpu();
  for (uint32_t s = 0; s < splits; ++s) {
    VKOK(pvkResetFences(dev, 1, &p->fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &p->cmds[s] };
    VKOK(pvkQueueSubmit(queue, 1, &si, p->fence));
    VKOK(pvkWaitForFences(dev, 1, &p->fence, VK_TRUE, 1000ull * 1000 * 1000));
  }
  double w1 = now_wall(), c1 = now_cpu();
  *wall = w1 - w0; *cpu = c1 - c0; *sink += 1;
}

static void bench(const char* label, Pass* p, Push pc, int iters) {
  record_split(p, pc, 1);
  double *w = malloc(iters * sizeof *w), *c = malloc(iters * sizeof *c), *r = malloc(iters * sizeof *r);
  uint64_t sink = 0;
  for (int i = 0; i < 16; ++i) run_frame(p, &w[0], &c[0], &r[0], &sink);   // warm
  for (int i = 0; i < iters; ++i) run_frame(p, &w[i], &c[i], &r[i], &sink);
  qsort(w, iters, sizeof *w, cmp_d); qsort(c, iters, sizeof *c, cmp_d); qsort(r, iters, sizeof *r, cmp_d);
  int p50 = iters / 2, p99 = (iters * 99) / 100;
  printf("  %-22s dispatch p50 %6.3f  p99 %6.3f | cpu p50 %6.3f  p99 %6.3f | readback p50 %6.3f  (sink %llu)\n",
         label, w[p50], w[p99], c[p50], c[p99], r[p50], (unsigned long long)sink);
  free(w); free(c); free(r);
}

// ---- capability checks ------------------------------------------------------

// Is a dma-buf export actually available, and for what? The earlier probe
// reported success with fd -1, which is not a thing: VK_SUCCESS must yield a
// real fd. The cause is that nothing asked the driver whether *buffers* are
// exportable before trying -- and for scanout the object that matters is an
// image anyway, since DmabufOut needs a DRM format modifier, which only
// images carry. So: query the capability for each object kind, then export
// the kind the query says is supported, and treat a success with fd < 0 as
// the failure it is.
static void check_dmabuf(void) {
  if (!has_dmabuf) { printf("dma-buf     : extension absent\n"); return; }
  const VkExternalMemoryHandleTypeFlagBits H = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  // --- can a BUFFER be exported? ---
  VkPhysicalDeviceExternalBufferInfo bq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .handleType = H };
  VkExternalBufferProperties bp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
  pvkGetPhysicalDeviceExternalBufferProperties(phys, &bq, &bp);
  const int buf_exportable =
    (bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;

  // --- can an IMAGE be exported? this is the one scanout needs ---
  VkPhysicalDeviceExternalImageFormatInfo eif = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType = H };
  VkPhysicalDeviceImageFormatInfo2 ifi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
    .pNext = &eif, .format = VK_FORMAT_B8G8R8A8_UNORM, .type = VK_IMAGE_TYPE_2D,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
  VkExternalImageFormatProperties eip = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
  VkImageFormatProperties2 ifp = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &eip };
  VkResult ir = pvkGetPhysicalDeviceImageFormatProperties2(phys, &ifi, &ifp);
  const int img_exportable = ir == VK_SUCCESS &&
    (eip.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;

  printf("dma-buf     : query -- buffer exportable %s, image exportable %s\n",
         buf_exportable ? "yes" : "NO", img_exportable ? "yes" : "NO");

  // --- actually export a buffer ---
  if (buf_exportable) {
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                             .handleTypes = H };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ext,
                              .size = 256 * 192 * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;
    if (pvkCreateBuffer(dev, &bi, NULL, &buf) == VK_SUCCESS) {
      VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, buf, &mr);
      VkExportMemoryAllocateInfo em = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = H };
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &em,
                                  .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, 1) };
      VkDeviceMemory mem;
      VkResult r = pvkAllocateMemory(dev, &ai, NULL, &mem);
      if (r != VK_SUCCESS) printf("dma-buf     : buffer alloc FAILED (%d)\n", (int)r);
      else {
        VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = H };
        int fd = -1;
        r = pvkGetMemoryFdKHR(dev, &gi, &fd);
        printf("dma-buf     : buffer export %s (fd %d)\n",
               (r == VK_SUCCESS && fd >= 0) ? "OK" : "FAILED", fd);
        if (fd >= 0) close(fd);
        pvkFreeMemory(dev, mem, NULL);
      }
      pvkDestroyBuffer(dev, buf, NULL);
    }
  }

  // --- actually export an image: the scanout object ---
  if (img_exportable) {
    VkExternalMemoryImageCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                            .handleTypes = H };
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext,
      .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 256, 192, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    VkResult r = pvkCreateImage(dev, &ii, NULL, &img);
    if (r != VK_SUCCESS) { printf("dma-buf     : image create FAILED (%d)\n", (int)r); return; }
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(dev, img, &mr);
    VkExportMemoryAllocateInfo em = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = H };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &em,
                                .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, 1) };
    VkDeviceMemory mem;
    r = pvkAllocateMemory(dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) { printf("dma-buf     : image alloc FAILED (%d)\n", (int)r); pvkDestroyImage(dev, img, NULL); return; }
    VkMemoryGetFdInfoKHR gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem, .handleType = H };
    int fd = -1;
    r = pvkGetMemoryFdKHR(dev, &gi, &fd);
    printf("dma-buf     : image export %s (fd %d, %llu KB) -- %s\n",
           (r == VK_SUCCESS && fd >= 0) ? "OK" : "FAILED", fd,
           (unsigned long long)(mr.size / 1024),
           (r == VK_SUCCESS && fd >= 0) ? "scanout without readback is available"
                                        : "output must come back through the CPU");
    if (fd >= 0) close(fd);
    pvkFreeMemory(dev, mem, NULL);
    pvkDestroyImage(dev, img, NULL);
  }
}

static void check_hostmem(void) {
  if (!has_hostmem) { printf("host import : extension absent\n"); return; }
  size_t sz = ((4u << 20) + host_ptr_align - 1) & ~(size_t)(host_ptr_align - 1);
  void* p = aligned_alloc(host_ptr_align, sz);
  if (!p) { printf("host import : aligned_alloc failed\n"); return; }
  memset(p, 0, sz);
  VkImportMemoryHostPointerInfoEXT ip = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = p };
  // Any host-visible type is a legal target; the driver reports which in
  // vkGetMemoryHostPointerPropertiesEXT, but every type here is on one heap.
  VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ip,
                              .allocationSize = sz, .memoryTypeIndex = find_mem(~0u, 1) };
  VkDeviceMemory mem;
  VkResult r = pvkAllocateMemory(dev, &ai, NULL, &mem);
  printf("host import : %s -- %s\n", r == VK_SUCCESS ? "OK" : "FAILED",
         r == VK_SUCCESS ? "texcache / VRAM can be bound with no staging copy"
                         : "textures must be copied into device memory each frame");
  if (r == VK_SUCCESS) pvkFreeMemory(dev, mem, NULL);
}

// ---- main -------------------------------------------------------------------

int main(int argc, char** argv) {
  const char* spv_trivial = argc > 1 ? argv[1] : "trivial.spv";
  const char* spv_fill    = argc > 2 ? argv[2] : "fill.spv";
  const int   iters       = argc > 3 ? atoi(argv[3]) : 200;

  // Line-buffered: the probe is normally run over ssh, where stdout is a
  // pipe and a block-buffered run that is killed prints nothing at all.
  setvbuf(stdout, NULL, _IOLBF, 0);

  init_vulkan();
  check_dmabuf();
  check_hostmem();

  const char* suite = getenv("VK_SUITE");
  if (suite && !strcmp(suite, "mem")) {
    // Can the NEON 2D kernels read a GPU-written buffer at full speed?
    //
    // The 3D renderer already hands the 2D compositor a plain line of 18-bit
    // records (`line3d` in kernels.h resolve16_full / layer16_3d), so a
    // hybrid -- Vulkan compute for the 3D raster, NEON for the composite --
    // only works if a GPU-written buffer reads back as fast as malloc'd
    // memory. That is entirely a question of the CPU mapping's cacheability:
    // a HOST_CACHED mapping is ordinary cached memory, a HOST_COHERENT one
    // is typically write-combine, where reads fall off a cliff. (This is the
    // same trap as the A30's uncached fb0 mmap, where a 16-byte store
    // pattern cost 5-8 ms -- see display_disp.h.)
    //
    // Three arenas, the same two passes over each: a sequential read of a
    // DS-sized 3D layer, and a blend of two lines into a third, which is the
    // shape of the composite stage.
    const VkDeviceSize N = 256 * 192 * 4;        // one 3D layer
    const int reps = 200;

    VkBuffer b_c, b_h; VkDeviceMemory m_c, m_h; void *p_c, *p_h;
    make_buffer(N * 3, 1, &b_c, &m_c, &p_c);     // HOST_CACHED
    make_buffer(N * 3, 0, &b_h, &m_h, &p_h);     // HOST_COHERENT
    void* p_m = aligned_alloc(4096, N * 3);
    memset(p_m, 0x31, N * 3); memset(p_c, 0x31, N * 3); memset(p_h, 0x31, N * 3);

    struct { const char* name; void* base; VkDeviceMemory mem; } arena[3] = {
      { "malloc (baseline)",      p_m, VK_NULL_HANDLE },
      { "vk HOST_CACHED",         p_c, m_c },
      { "vk HOST_COHERENT",       p_h, m_h },
    };

    printf("\nNEON-shaped access to GPU-visible memory (%llu KB per pass, %d reps)\n",
           (unsigned long long)(N / 1024), reps);
    for (int a = 0; a < 3; ++a) {
      volatile uint64_t sink = 0;
      const uint32_t* src = (const uint32_t*)arena[a].base;
      const uint32_t* s2  = src + N / 4;
      uint32_t* dstp      = (uint32_t*)((char*)arena[a].base + 2 * N);

      // Sequential read, as the composite reads the 3D line.
      double t0 = now_wall();
      for (int r = 0; r < reps; ++r) {
        if (arena[a].mem) {
          VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                     .memory = arena[a].mem, .offset = 0, .size = VK_WHOLE_SIZE };
          pvkInvalidateMappedMemoryRanges(dev, 1, &mr);
        }
        uint64_t acc = 0;
        for (VkDeviceSize i = 0; i < N / 4; ++i) acc += src[i];
        sink += acc;
      }
      double t_read = (now_wall() - t0) / reps;

      // Blend two lines into a third: the composite's own shape.
      t0 = now_wall();
      for (int r = 0; r < reps; ++r)
        for (VkDeviceSize i = 0; i < N / 4; ++i) {
          uint32_t x = src[i], y = s2[i];
          dstp[i] = (((x & 0xFEFEFEFEu) >> 1) + ((y & 0xFEFEFEFEu) >> 1)) | 0xFF000000u;
        }
      double t_blend = (now_wall() - t0) / reps;

      printf("  %-20s read %6.3f ms (%5.2f GB/s) | blend %6.3f ms (%5.2f GB/s)%s\n",
             arena[a].name, t_read, (double)N / t_read / 1e6,
             t_blend, (double)(3 * N) / t_blend / 1e6,
             a == 0 ? "   <- target" : "");
    }
    printf("\n  A HOST_CACHED arena at baseline speed means the hybrid works: Vulkan\n"
           "  compute writes the 3D layer, the NEON kernels composite from it in place.\n");
    return 0;
  }

  if (suite && !strcmp(suite, "2d")) {
    // The 2D suite. Both engines' worth of pixels (A and B are independent,
    // so one dispatch grid of 2*192 rows covers both) at 1x / 2x / 4x, and
    // then the question the lazy-2D journal actually poses: what does it
    // cost when a frame cannot be composited in one batch?
    const char* spv = argc > 4 ? argv[4] : "composite2d.spv";
    uint32_t scales[8] = { 1, 2, 4 }, nscale = 3;
    parse_list(getenv("VK_SCALES"), scales, &nscale);

    for (uint32_t i = 0; i < nscale; ++i) {
      uint32_t w = 256 * scales[i], h = 2 * 192 * scales[i];   // both engines
      VkDeviceSize out = (VkDeviceSize)w * h * 4;
      printf("\n2D composite, both engines, %ux%u (%ux)  (%llu KB out)\n",
             w, h, scales[i], (unsigned long long)(out / 1024));
      Pass p; memset(&p, 0, sizeof p);
      build_pass(&p, spv, 1u << 20, out);         // >= VRAM + palettes, power of two for the mask
      for (uint32_t d = 0; d < 2; ++d) {
        Push pc = { w, h, 0, 0, h, 4, d };
        char label[64];
        snprintf(label, sizeof label, d ? "mixed 1-4 BG (mean 2.5)" : "4 BG, all pixels");
        bench(label, &p, pc, iters);
      }
      pvkDeviceWaitIdle(dev);
    }

    // Dispatch granularity, at 1x only: the common frame batches once, but a
    // frame with mid-frame VRAM writes, display capture or the display FIFO
    // replays in pieces. Batched = K dispatches, one submit, one fence.
    // Naive = K submits, K fences.
    printf("\ndispatch granularity (256x384, 1x) -- batched vs one submit per catch-up\n");
    {
      uint32_t w = 256, h = 2 * 192;
      Pass p; memset(&p, 0, sizeof p);
      build_pass(&p, spv, 1u << 20, (VkDeviceSize)w * h * 4);
      const uint32_t splits[] = { 1, 4, 24, 96, 192 };
      for (size_t k = 0; k < sizeof splits / sizeof *splits; ++k) {
        uint32_t K = splits[k];
        Push pc = { w, h, 0, 0, h, 4, 1 };
        double *wa = malloc(iters * sizeof *wa), *ca = malloc(iters * sizeof *ca), *ra = malloc(iters * sizeof *ra);
        uint64_t sink = 0;

        record_split(&p, pc, K);
        for (int t = 0; t < 8; ++t) run_frame(&p, &wa[0], &ca[0], &ra[0], &sink);
        for (int t = 0; t < iters; ++t) run_frame(&p, &wa[t], &ca[t], &ra[t], &sink);
        qsort(wa, iters, sizeof *wa, cmp_d); qsort(ca, iters, sizeof *ca, cmp_d);
        double bw = wa[iters / 2], bc = ca[iters / 2];

        double nw = 0, nc = 0;
        if (K <= MAX_SPLITS) {
          record_each(&p, pc, K);
          for (int t = 0; t < 8; ++t) run_frame_each(&p, K, &wa[0], &ca[0], &sink);
          for (int t = 0; t < iters; ++t) run_frame_each(&p, K, &wa[t], &ca[t], &sink);
          qsort(wa, iters, sizeof *wa, cmp_d); qsort(ca, iters, sizeof *ca, cmp_d);
          nw = wa[iters / 2]; nc = ca[iters / 2];
        }
        printf("  %3u catch-up(s): batched %6.3f ms (cpu %6.3f) | per-submit %7.3f ms (cpu %6.3f)\n",
               K, bw, bc, nw, nc);
        free(wa); free(ca); free(ra);
      }
      pvkDeviceWaitIdle(dev);
    }
    return 0;
  }

  // The DS plane at 1x / 2x / 4x internal resolution. `work` stands in for
  // per-pixel shading: a DS pixel costs a texture fetch, a palette lookup,
  // a depth test and a blend, so the loop is scaled to that order.
  // Both lists are overridable, because the heavy corner (4x at a high work
  // count) is minutes per configuration on this part and is not worth
  // re-measuring once it is known: VK_SCALES=1,2 VK_WORKS=0,8 ./vk_probe ...
  uint32_t scales[8] = { 1, 2, 4 }, nscale = 3;
  uint32_t works[8]  = { 0, 16, 64 }, nwork  = 3;
  parse_list(getenv("VK_SCALES"), scales, &nscale);
  parse_list(getenv("VK_WORKS"),  works,  &nwork);

  printf("\nround trip = submit -> fence -> invalidate; readback = CPU sum of every output pixel\n");
  for (uint32_t i = 0; i < nscale; ++i) {
    uint32_t w = 256 * scales[i], h = 192 * scales[i];
    VkDeviceSize out = (VkDeviceSize)w * h * 4;
    printf("\n%ux%u (%ux)  (%llu KB out)\n", w, h, scales[i], (unsigned long long)(out / 1024));
    for (uint32_t k = 0; k < nwork; ++k) {
      Pass p; memset(&p, 0, sizeof p);
      // 64 KB decoded texture, not a 4 MB scratch: the working set is the point.
      build_pass(&p, works[k] == 0 ? spv_trivial : spv_fill, 64u << 10, out);
      char label[64];
      if (works[k] == 0) snprintf(label, sizeof label, "dispatch floor");
      else snprintf(label, sizeof label, "overdraw %ux", works[k]);
      Push pc = { w, h, works[k] };
      bench(label, &p, pc, iters);
      pvkDeviceWaitIdle(dev);
    }
  }
  return 0;
}
