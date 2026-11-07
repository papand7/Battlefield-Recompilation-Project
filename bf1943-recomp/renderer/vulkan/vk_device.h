// Vulkan 1.3 instance/device ownership. This is the only graphics backend:
// there is deliberately no RHI abstraction above it.
#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>

namespace bf::vk {

const char* ResultName(VkResult r);

struct DeviceOptions {
  bool validation = false;        // VK_LAYER_KHRONOS_validation + debug messenger
  bool headless = false;          // no surface/swapchain extensions
  int preferred_device = -1;      // index into vkEnumeratePhysicalDevices, -1 = auto
  std::string pipeline_cache_path; // empty = no persistent cache
};

// Optional features discovered at device creation (used for fast paths).
struct DeviceCaps {
  bool descriptor_indexing = false;  // runtime arrays, update-after-bind, partially bound
  bool descriptor_buffer = false;    // VK_EXT_descriptor_buffer
  bool memory_budget = false;        // VK_EXT_memory_budget
  bool extended_dynamic_state3 = false;
  bool shader_float16_int8 = false;
  bool fragment_shader_interlock = false;  // ROV-like EDRAM emulation path
  VkPhysicalDeviceProperties props{};
};

class Device {
 public:
  bool Init(const DeviceOptions& opts);
  void Shutdown();
  // Writes the pipeline cache to DeviceOptions::pipeline_cache_path.
  bool SavePipelineCache();

  VkInstance instance() const { return instance_; }
  VkPhysicalDevice physical() const { return physical_; }
  VkDevice device() const { return device_; }
  VkQueue queue() const { return queue_; }
  uint32_t queue_family() const { return queue_family_; }
  VmaAllocator allocator() const { return allocator_; }
  VkPipelineCache pipeline_cache() const { return pipeline_cache_; }
  const DeviceCaps& caps() const { return caps_; }
  uint32_t validation_errors() const { return validation_errors_; }

 private:
  bool CreateInstance();
  bool PickPhysicalDevice();
  bool CreateDevice();
  bool CreatePipelineCache();
  static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

  DeviceOptions opts_;
  DeviceCaps caps_;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  uint32_t validation_errors_ = 0;
};

}  // namespace bf::vk
