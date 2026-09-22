// What the device's Vulkan stack reports at RUNTIME, as a separate binary
// from tools/gles_probe.c on purpose: the two stacks can pick different
// vendors (glvnd chooses the EGL one, the Vulkan loader chooses its own ICD),
// and a single process linking both would not tell us which answer belongs to
// which. Linked -lvulkan DYNAMICALLY so the loader resolves the ICD an
// application would get.
//
//   tools/gles_probe.sh [ssh-host]      builds and runs BOTH probes
//
// The GPU present stage needs Vulkan 1.1 plus external-memory dma-buf import;
// the doc's SS3.34 table was assembled from ICD json and library strings, and
// this is the version that asks the driver.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* kWant[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    VK_KHR_SHADER_ATOMIC_INT64_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

int main(void) {
  uint32_t loader_ver = 0;
  // vkEnumerateInstanceVersion is 1.1; its absence IS the 1.0 answer.
  PFN_vkEnumerateInstanceVersion ev =
      (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
  if (ev && ev(&loader_ver) == VK_SUCCESS)
    printf("loader instance version: %u.%u.%u\n", VK_VERSION_MAJOR(loader_ver),
           VK_VERSION_MINOR(loader_ver), VK_VERSION_PATCH(loader_ver));
  else
    printf("loader instance version: 1.0 (no vkEnumerateInstanceVersion)\n");

  VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                           .pApplicationName = "dsperate-vk-caps",
                           // The same version DSperate asks for, so this
                           // probe reports what the emulator would get.
                           .apiVersion = VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
  VkInstance inst = VK_NULL_HANDLE;
  VkResult r = vkCreateInstance(&ici, NULL, &inst);
  if (r != VK_SUCCESS) { printf("FATAL: vkCreateInstance = %d\n", r); return 1; }

  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, NULL);
  if (!n) { printf("no physical devices\n"); return 1; }
  VkPhysicalDevice* devs = calloc(n, sizeof *devs);
  vkEnumeratePhysicalDevices(inst, &n, devs);
  printf("physical devices: %u\n", n);

  for (uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    printf("\n[%u] %s\n", i, p.deviceName);
    printf("    apiVersion   %u.%u.%u\n", VK_VERSION_MAJOR(p.apiVersion),
           VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion));
    printf("    driverVersion 0x%x   vendorID 0x%x   type %u\n", p.driverVersion, p.vendorID, p.deviceType);

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
    VkExtensionProperties* exts = calloc(ne ? ne : 1, sizeof *exts);
    vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, exts);
    printf("    %u device extensions; the ones the present stage needs:\n", ne);
    for (size_t w = 0; w < sizeof kWant / sizeof *kWant; ++w) {
      int found = 0;
      for (uint32_t e = 0; e < ne; ++e)
        if (!strcmp(exts[e].extensionName, kWant[w])) { found = 1; break; }
      printf("      %-48s %s\n", kWant[w], found ? "YES" : "no");
    }

    // Can a dma-buf actually be imported as memory for a buffer? The
    // extension being listed is a claim; this asks the driver what handle
    // types it will accept. (P0.1 in docs/gpu-path-scoping.md imported for
    // real; this is the cheap always-safe version of that question.)
    PFN_vkGetPhysicalDeviceExternalBufferProperties gb =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)vkGetInstanceProcAddr(
            inst, "vkGetPhysicalDeviceExternalBufferProperties");
    if (gb) {
      VkPhysicalDeviceExternalBufferInfo bi = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
          .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
      VkExternalBufferProperties bp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      gb(devs[i], &bi, &bp);
      const VkExternalMemoryFeatureFlags f = bp.externalMemoryProperties.externalMemoryFeatures;
      printf("    dma-buf as a storage BUFFER: importable %s, exportable %s\n",
             (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? "YES" : "no",
             (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? "YES" : "no");
    } else {
      printf("    vkGetPhysicalDeviceExternalBufferProperties missing (pre-1.1 device?)\n");
    }
    free(exts);
  }
  free(devs);
  vkDestroyInstance(inst, NULL);
  return 0;
}
