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

#define DIE(...) do { fprintf(stderr, "aa_probe: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
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
  VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "dsperate-aa-probe", .apiVersion = VK_API_VERSION_1_1 };
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


// ---- the same FXAA in C, single thread, for the CPU side --------------------
// Built -O3 -mcpu=cortex-a55: the luma plane vectorises; the per-pixel
// walk does not (it is divergent), which is what a hand-written NEON
// version would also fight. So this is a floor for the NEON route, not a
// ceiling: a real NEON kernel might be 2-3x better on the walk.
#include <math.h>
static float* g_luma; static int g_w, g_h;
static inline float L(int x, int y) { x = x < 0 ? 0 : (x >= g_w ? g_w - 1 : x); y = y < 0 ? 0 : (y >= g_h ? g_h - 1 : y); return g_luma[y * g_w + x]; }
static inline float L2(int x, int y, int dx, int dy) { return 0.5f * (L(x, y) + L(x + dx, y + dy)); }
static const float STEPS[12] = {1, 1, 1, 1, 1, 1.5f, 2, 2, 2, 2, 4, 8};
static void luma_plane(const uint32_t* src, int w, int h, float* out) {
  for (int i = 0; i < w * h; ++i) { const uint32_t c = src[i]; out[i] = ((c >> 16) & 255) * (0.299f / 255) + ((c >> 8) & 255) * (0.587f / 255) + (c & 255) * (0.114f / 255); }
}
static void fxaa_cpu(const uint32_t* src, uint32_t* dst, int w, int h) {
  g_w = w; g_h = h;
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
    const uint32_t cM = src[y * w + x];
    const float lM = L(x, y), lN = L(x, y - 1), lS = L(x, y + 1), lE = L(x + 1, y), lW = L(x - 1, y);
    float lmin = fminf(lM, fminf(fminf(lN, lS), fminf(lE, lW))), lmax = fmaxf(lM, fmaxf(fmaxf(lN, lS), fmaxf(lE, lW)));
    const float range = lmax - lmin;
    if (range < fmaxf(0.0312f, lmax * 0.125f)) { dst[y * w + x] = cM; continue; }
    const float lNW = L(x - 1, y - 1), lNE = L(x + 1, y - 1), lSW = L(x - 1, y + 1), lSE = L(x + 1, y + 1);
    const float edgeH = fabsf(-2 * lW + lNW + lSW) + fabsf(-2 * lM + lN + lS) * 2 + fabsf(-2 * lE + lNE + lSE);
    const float edgeV = fabsf(-2 * lN + lNW + lNE) + fabsf(-2 * lM + lW + lE) * 2 + fabsf(-2 * lS + lSW + lSE);
    const int horz = edgeH >= edgeV;
    const float l1 = horz ? lS : lE, l2 = horz ? lN : lW, g1 = l1 - lM, g2 = l2 - lM;
    const int s1 = fabsf(g1) >= fabsf(g2);
    const float gscaled = 0.25f * fmaxf(fabsf(g1), fabsf(g2));
    const int step = s1 ? 1 : -1;
    const float lavg = 0.5f * ((s1 ? l1 : l2) + lM);
    const int dx = horz ? 1 : 0, dy = horz ? 0 : 1, px = horz ? 0 : step, py = horz ? step : 0;
    float d1 = 0, d2 = 0, e1 = 0, e2 = 0; int r1 = 0, r2 = 0;
    for (int i = 0; i < 12; ++i) {
      if (!r1) { d1 += STEPS[i]; e1 = L2(x - (int)d1 * dx, y - (int)d1 * dy, px, py) - lavg; r1 = fabsf(e1) >= gscaled; }
      if (!r2) { d2 += STEPS[i]; e2 = L2(x + (int)d2 * dx, y + (int)d2 * dy, px, py) - lavg; r2 = fabsf(e2) >= gscaled; }
      if (r1 && r2) break;
    }
    const float dist = fminf(d1, d2), span = d1 + d2;
    const int dir1 = d1 < d2, lowerM = lM - lavg < 0;
    const int correct = ((dir1 ? e1 : e2) < 0) != lowerM;
    float off = correct ? (-dist / span + 0.5f) : 0.f;
    const float lall = (1.f / 12) * (2 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE);
    const float sp1 = fminf(1.f, fmaxf(0.f, fabsf(lall - lM) / range)), sp2 = (-2 * sp1 + 3) * sp1 * sp1;
    off = fmaxf(off, sp2 * sp2 * 0.75f);
    const int nx = x + px < 0 ? 0 : (x + px >= w ? w - 1 : x + px), ny = y + py < 0 ? 0 : (y + py >= h ? h - 1 : y + py);
    const uint32_t c1 = src[ny * w + nx];
    uint32_t o = 0xFF000000u;
    for (int sh = 0; sh <= 16; sh += 8) { const float a = (cM >> sh) & 255, b = (c1 >> sh) & 255; o |= (uint32_t)lrintf(a + (b - a) * off) << sh; }
    dst[y * w + x] = o;
  }
}

// A synthetic frame with a game's edge statistics: 1x scene of overlapping
// flat and textured polygons, nearest-scaled 4x (the RG DS Plus panel).
static void make_scene(uint32_t* out, int W, int H, int S) {
  const int w = W / S, h = H / S; uint32_t* nat = malloc((size_t)w * h * 4);
  for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) nat[y * w + x] = 0xFF000000u | ((uint32_t)(60 + y / 3) << 8) | (uint32_t)(120 + x / 4);   // a sky/ground gradient
  srand(7);
  for (int n = 0; n < 60; ++n) {   // triangles
    int x0 = rand() % w, y0 = rand() % h, x1 = x0 + rand() % 90 - 45, y1 = y0 + rand() % 90 - 45, x2 = x0 + rand() % 90 - 45, y2 = y0 + rand() % 90 - 45;
    uint32_t col = 0xFF000000u | (uint32_t)(rand() & 0xFFFFFF); int tex = (rand() & 3) == 0;
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    for (int y = miny < 0 ? 0 : miny; y <= maxy && y < h; ++y) for (int x = minx < 0 ? 0 : minx; x <= maxx && x < w; ++x) {
      int a = (x1 - x0) * (y - y0) - (y1 - y0) * (x - x0), b = (x2 - x1) * (y - y1) - (y2 - y1) * (x - x1), c = (x0 - x2) * (y - y2) - (y0 - y2) * (x - x2);
      if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0)) nat[y * w + x] = tex ? (col ^ (((x ^ y) & 4) ? 0x303030u : 0u)) : col;
    }
  }
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) out[y * W + x] = nat[(y / S) * w + x / S];
  free(nat);
}

static Pipe make_pipe_n(const char* spv, int n) {
  Pipe p; VkDescriptorSetLayoutBinding b[3];
  for (int i = 0; i < n; ++i) b[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
  VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = (uint32_t)n, .pBindings = b };
  VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &p.dsl));
  VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
  VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
  VKOK(pvkCreatePipelineLayout(dev, &pl, NULL, &p.layout));
  VkShaderModule sm = load_spv(spv);
  VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = p.layout };
  VKOK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p.pipe));
  VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)n * 2 };
  VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
  VKOK(pvkCreateDescriptorPool(dev, &dp, NULL, &p.pool));
  return p;
}
static VkDescriptorSet set_for(Pipe* p, VkBuffer* bufs, int n) {
  VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->pool, .descriptorSetCount = 1, .pSetLayouts = &p->dsl };
  VkDescriptorSet set; VKOK(pvkAllocateDescriptorSets(dev, &da, &set));
  VkDescriptorBufferInfo dbi[3]; VkWriteDescriptorSet w[3];
  for (int i = 0; i < n; ++i) { dbi[i] = (VkDescriptorBufferInfo){ .buffer = bufs[i], .range = VK_WHOLE_SIZE }; w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] }; }
  pvkUpdateDescriptorSets(dev, (uint32_t)n, w, 0, NULL);
  return set;
}

int main(int argc, char** argv) {
  const char* spv = argc > 1 ? argv[1] : "aa_probe_fxaa.spv";
  const int W = 1024, H = 768, iters = 50;
  setvbuf(stdout, NULL, _IOLBF, 0);
  uint32_t* scene = malloc((size_t)W * H * 4); make_scene(scene, W, H, 4);

  // ---- CPU ----
  {
    uint32_t* dst = malloc((size_t)W * H * 4); float* lum = malloc((size_t)W * H * sizeof(float)); g_luma = lum;
    double best_l = 1e9, best_f = 1e9;
    for (int rep = 0; rep < 8; ++rep) {
      double t0 = now_ms(); luma_plane(scene, W, H, lum); double t1 = now_ms(); fxaa_cpu(scene, dst, W, H); double t2 = now_ms();
      if (t1 - t0 < best_l) best_l = t1 - t0; if (t2 - t1 < best_f) best_f = t2 - t1;
    }
    unsigned changed = 0; for (int i = 0; i < W * H; ++i) changed += dst[i] != scene[i];
    printf("cpu (one A55 core, -O3): luma plane %.2f ms + fxaa %.2f ms = %.2f ms per 1024x768 panel; %u px changed (%.1f %%)\n", best_l, best_f, best_l + best_f, changed, 100.0 * changed / (W * H));
    // memory floor: a plain copy of the panel
    double best_c = 1e9; for (int rep = 0; rep < 8; ++rep) { double t0 = now_ms(); memcpy(dst, scene, (size_t)W * H * 4); double t = now_ms() - t0; if (t < best_c) best_c = t; }
    printf("cpu memcpy of one panel: %.2f ms (the floor for any pass over it)\n", best_c);
    free(dst); free(lum);
  }

  // ---- GPU ----
  init_vulkan();
  Pipe p;
  {
    VkDescriptorSetLayoutBinding b[2]; for (int i = 0; i < 2; ++i) b[i] = (VkDescriptorSetLayoutBinding){ .binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b };
    VKOK(pvkCreateDescriptorSetLayout(dev, &dl, NULL, &p.dsl));
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8 };
    VkPipelineLayoutCreateInfo pl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VKOK(pvkCreatePipelineLayout(dev, &pl, NULL, &p.layout));
    VkShaderModule sm = load_spv(spv);
    VkComputePipelineCreateInfo cp = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = p.layout };
    double tc = now_ms(); VKOK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, NULL, &p.pipe)); printf("gpu pipeline compiled in %.1f ms\n", now_ms() - tc);
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
    VkDescriptorPoolCreateInfo dp = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
    VKOK(pvkCreateDescriptorPool(dev, &dp, NULL, &p.pool));
  }
  VkBuffer buf[2]; VkDeviceMemory mem[2]; void* map[2];
  for (int i = 0; i < 2; ++i) {
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (size_t)W * H * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VKOK(pvkCreateBuffer(dev, &bi, NULL, &buf[i]));
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, buf[i], &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VKOK(pvkAllocateMemory(dev, &ai, NULL, &mem[i])); VKOK(pvkBindBufferMemory(dev, buf[i], mem[i], 0)); VKOK(pvkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, &map[i]));
  }
  memcpy(map[0], scene, (size_t)W * H * 4);
  VkDescriptorSetAllocateInfo da = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p.pool, .descriptorSetCount = 1, .pSetLayouts = &p.dsl };
  VkDescriptorSet set; VKOK(pvkAllocateDescriptorSets(dev, &da, &set));
  VkDescriptorBufferInfo dbi[2]; VkWriteDescriptorSet w[2];
  for (int i = 0; i < 2; ++i) { dbi[i] = (VkDescriptorBufferInfo){ .buffer = buf[i], .range = VK_WHOLE_SIZE }; w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi[i] }; }
  pvkUpdateDescriptorSets(dev, 2, w, 0, NULL);
  struct { int w, h; } pcv = { W, H };
  double best = 1e9;
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
    double t = (now_ms() - t0) / iters; if (t < best) best = t;
  }
  double one = 1e9;
  for (int rep = 0; rep < 20; ++rep) {
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
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
  const uint32_t* out = map[1]; unsigned changed = 0; for (int i = 0; i < W * H; ++i) changed += out[i] != scene[i];
  printf("gpu (Mali-G52): fxaa %.2f ms per 1024x768 panel (throughput, best of 3x%d), single dispatch round trip %.2f ms; %u px changed (%.1f %%)\n", best, iters, one, changed, 100.0 * changed / (W * H));
  // ---- the two-pass form: luma plane, then FXAA from it ----
  if (argc > 3) {
    Pipe pl = make_pipe_n(argv[2], 2), pf = make_pipe_n(argv[3], 3);
    VkBuffer lbuf; VkDeviceMemory lmem;
    { VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (size_t)W * H * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
      VKOK(pvkCreateBuffer(dev, &bi, NULL, &lbuf)); VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(dev, lbuf, &mr);
      VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      VKOK(pvkAllocateMemory(dev, &ai, NULL, &lmem)); VKOK(pvkBindBufferMemory(dev, lbuf, lmem, 0)); }
    VkBuffer lb[2] = { buf[0], lbuf }, fb[3] = { buf[0], lbuf, buf[1] };
    VkDescriptorSet sl = set_for(&pl, lb, 2), sf = set_for(&pf, fb, 3);
    for (int which = 0; which < 3; ++which) {   // 0 luma alone, 1 fxaa alone, 2 both
      double bestt = 1e9;
      for (int rep = 0; rep < 3; ++rep) {
        VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VKOK(pvkBeginCommandBuffer(cmd, &cbi));
        for (int i = 0; i < iters; ++i) {
          VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
          if (which != 1) { pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl.pipe); pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl.layout, 0, 1, &sl, 0, NULL); pvkCmdPushConstants(cmd, pl.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pcv); pvkCmdDispatch(cmd, (W * H + 63) / 64, 1, 1);
            pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }
          if (which != 0) { pvkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pf.pipe); pvkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pf.layout, 0, 1, &sf, 0, NULL); pvkCmdPushConstants(cmd, pf.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, &pcv); pvkCmdDispatch(cmd, W / 16, H / 8, 1);
            pvkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL); }
        }
        VKOK(pvkEndCommandBuffer(cmd));
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        double t0 = now_ms(); VKOK(pvkResetFences(dev, 1, &fence)); VKOK(pvkQueueSubmit(queue, 1, &si, fence)); VKOK(pvkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));
        double t = (now_ms() - t0) / iters; if (t < bestt) bestt = t;
      }
      printf("gpu two-pass: %s %.2f ms per panel\n", which == 0 ? "luma plane alone" : which == 1 ? "fxaa from the plane alone" : "both", bestt);
    }
    const uint32_t* out2 = map[1]; unsigned ch2 = 0; for (int i = 0; i < W * H; ++i) ch2 += out2[i] != scene[i];
    printf("gpu two-pass: %u px changed\n", ch2);
  }
  pvkQueueWaitIdle(queue);
  return 0;
}
