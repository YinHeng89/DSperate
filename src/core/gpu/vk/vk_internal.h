// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#pragma once
// The backend's view of the Vulkan context: the dispatch table and the handles
// vk_raster.cpp needs to build a pipeline. Not part of vk_device.h, which the
// rest of the emulator includes and which must stay free of vulkan.h.
//
// Everything is resolved through vkGetInstanceProcAddr (vk_device.cpp), so
// nothing in the binary links against libvulkan and a device with no driver is
// an ordinary outcome rather than a failure to start.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "core/gpu/vk/vk_device.h"

namespace ds::gpu::vk {

// Context and allocation.
#define DS_VK_FNS_CORE \
  F(vkCreateInstance) F(vkDestroyInstance) F(vkEnumeratePhysicalDevices) \
  F(vkGetPhysicalDeviceProperties) F(vkGetPhysicalDeviceProperties2) F(vkGetPhysicalDeviceFeatures2) \
  F(vkGetPhysicalDeviceMemoryProperties) F(vkGetPhysicalDeviceQueueFamilyProperties) \
  F(vkEnumerateDeviceExtensionProperties) F(vkCreateDevice) F(vkDestroyDevice) \
  F(vkGetDeviceQueue) F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) \
  F(vkAllocateMemory) F(vkFreeMemory) F(vkBindBufferMemory) F(vkMapMemory) \
  F(vkInvalidateMappedMemoryRanges) F(vkFlushMappedMemoryRanges) \
  F(vkGetMemoryHostPointerPropertiesEXT) F(vkDeviceWaitIdle)

// Pipeline, descriptors, command recording and submission.
#define DS_VK_FNS_PIPE \
  F(vkCreateShaderModule) F(vkDestroyShaderModule) \
  F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) \
  F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) \
  F(vkCreateComputePipelines) F(vkDestroyPipeline) \
  F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
  F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) \
  F(vkCreateCommandPool) F(vkDestroyCommandPool) F(vkAllocateCommandBuffers) \
  F(vkResetCommandBuffer) F(vkBeginCommandBuffer) F(vkEndCommandBuffer) \
  F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) \
  F(vkCmdDispatch) F(vkCmdPipelineBarrier) F(vkCmdFillBuffer) \
  F(vkCreateFence) F(vkDestroyFence) F(vkResetFences) F(vkWaitForFences) \
  F(vkCreateQueryPool) F(vkDestroyQueryPool) F(vkCmdResetQueryPool) F(vkCmdWriteTimestamp) F(vkGetQueryPoolResults) \
  F(vkQueueSubmit) \
  F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkBindImageMemory) \
  F(vkCreateImageView) F(vkDestroyImageView) F(vkGetImageSubresourceLayout) \
  F(vkGetMemoryFdPropertiesKHR) F(vkGetPhysicalDeviceImageFormatProperties2)

#define DS_VK_FNS DS_VK_FNS_CORE DS_VK_FNS_PIPE

struct Api {
#define F(n) PFN_##n n = nullptr;
  DS_VK_FNS
#undef F
};

// What Device hands the backend. Lifetime is the Device's.
struct DeviceInternal {
  const Api*       api = nullptr;
  VkDevice         dev = VK_NULL_HANDLE;
  VkQueue          queue = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  u32              qfam = 0;
  VkDeviceSize     non_coherent_atom = 1;
};

inline VkBuffer vk_buf(const Buffer& b) { return reinterpret_cast<VkBuffer>(b.handle); }

} // namespace ds::gpu::vk
