#include "vk_device.h"

#include "vk_log.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace bf::vk {

const char* ResultName(VkResult r) {
  switch (r) {
#define BF_CASE(x) case x: return #x;
    BF_CASE(VK_SUCCESS)
    BF_CASE(VK_NOT_READY)
    BF_CASE(VK_TIMEOUT)
    BF_CASE(VK_INCOMPLETE)
    BF_CASE(VK_SUBOPTIMAL_KHR)
    BF_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
    BF_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
    BF_CASE(VK_ERROR_INITIALIZATION_FAILED)
    BF_CASE(VK_ERROR_DEVICE_LOST)
    BF_CASE(VK_ERROR_LAYER_NOT_PRESENT)
    BF_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
    BF_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
    BF_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
    BF_CASE(VK_ERROR_SURFACE_LOST_KHR)
    BF_CASE(VK_ERROR_OUT_OF_DATE_KHR)
#undef BF_CASE
    default: return "VK_ERROR_<other>";
  }
}

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
  for (const auto& e : exts)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}

bool HasLayer(const char* name) {
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> layers(n);
  vkEnumerateInstanceLayerProperties(&n, layers.data());
  for (const auto& l : layers)
    if (std::strcmp(l.layerName, name) == 0) return true;
  return false;
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL Device::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
  auto* self = static_cast<Device*>(user);
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++self->validation_errors_;
    BF_VK_ERROR("validation: %s", data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    BF_VK_WARN("validation: %s", data->pMessage);
  }
  return VK_FALSE;
}

bool Device::Init(const DeviceOptions& opts) {
  opts_ = opts;
  return CreateInstance() && PickPhysicalDevice() && CreateDevice() && CreatePipelineCache();
}

bool Device::CreateInstance() {
  uint32_t api = 0;
  vkEnumerateInstanceVersion(&api);
  if (api < VK_API_VERSION_1_3) {
    BF_VK_ERROR("Vulkan loader reports %u.%u; 1.3 is required", VK_API_VERSION_MAJOR(api),
                VK_API_VERSION_MINOR(api));
    return false;
  }

  std::vector<const char*> exts;
  std::vector<const char*> layers;
  if (!opts_.headless) {
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef _WIN32
    exts.push_back("VK_KHR_win32_surface");
#endif
  }
  bool validation = opts_.validation;
  if (validation && !HasLayer("VK_LAYER_KHRONOS_validation")) {
    BF_VK_WARN("validation requested but VK_LAYER_KHRONOS_validation is not installed");
    validation = false;
  }
  if (validation) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "bf1943";
  app.pEngineName = "bf1943-recomp";
  app.apiVersion = VK_API_VERSION_1_3;

  VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
  dbg.pfnUserCallback = DebugCallback;
  dbg.pUserData = this;

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  ci.enabledExtensionCount = uint32_t(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  if (validation) ci.pNext = &dbg;
  BF_VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));

  if (validation) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
    if (create) BF_VK_CHECK(create(instance_, &dbg, nullptr, &messenger_));
    BF_VK_INFO("validation layer enabled");
  }
  return true;
}

bool Device::PickPhysicalDevice() {
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> devs(n);
  vkEnumeratePhysicalDevices(instance_, &n, devs.data());
  if (devs.empty()) {
    BF_VK_ERROR("no Vulkan physical devices");
    return false;
  }

  int best = -1, best_score = -1;
  for (uint32_t i = 0; i < n; ++i) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(devs[i], &p);
    bool ok = p.apiVersion >= VK_API_VERSION_1_3;
    int score = !ok ? -1
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                           : 1;
    BF_VK_INFO("device %u: %s (Vulkan %u.%u.%u)%s", i, p.deviceName,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
               VK_API_VERSION_PATCH(p.apiVersion), ok ? "" : " [unsupported: < 1.3]");
    if (int(i) == opts_.preferred_device && ok) score = 100;
    if (score > best_score) best = int(i), best_score = score;
  }
  if (best_score < 0) {
    BF_VK_ERROR("no device supports Vulkan 1.3");
    return false;
  }
  physical_ = devs[best];
  vkGetPhysicalDeviceProperties(physical_, &caps_.props);

  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &qn, qf.data());
  for (uint32_t i = 0; i < qn; ++i) {
    // On Windows every graphics queue family of a desktop GPU can present;
    // the swapchain re-checks vkGetPhysicalDeviceSurfaceSupportKHR.
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      queue_family_ = i;
      BF_VK_INFO("selected %s, queue family %u", caps_.props.deviceName, i);
      return true;
    }
  }
  BF_VK_ERROR("no graphics queue family");
  return false;
}

bool Device::CreateDevice() {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> avail(n);
  vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, avail.data());

  std::vector<const char*> exts;
  if (!opts_.headless) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  auto optional = [&](const char* name) {
    if (!HasExtension(avail, name)) return false;
    exts.push_back(name);
    return true;
  };
  caps_.memory_budget = optional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  caps_.descriptor_buffer = optional(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
  caps_.extended_dynamic_state3 = optional(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
  caps_.fragment_shader_interlock = optional(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);

  // Query core 1.2/1.3 features, then enable what we use.
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(physical_, &f2);
  if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) {
    BF_VK_ERROR("device lacks dynamicRendering/synchronization2/timelineSemaphore");
    return false;
  }

  VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  e13.dynamicRendering = VK_TRUE;
  e13.synchronization2 = VK_TRUE;
  e13.maintenance4 = f13.maintenance4;
  VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  e12.pNext = &e13;
  e12.timelineSemaphore = VK_TRUE;
  e12.bufferDeviceAddress = f12.bufferDeviceAddress;
  e12.shaderFloat16 = f12.shaderFloat16;
  e12.shaderInt8 = f12.shaderInt8;
  caps_.shader_float16_int8 = f12.shaderFloat16 && f12.shaderInt8;
  caps_.descriptor_indexing = f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound &&
                              f12.shaderSampledImageArrayNonUniformIndexing &&
                              f12.descriptorBindingSampledImageUpdateAfterBind;
  if (caps_.descriptor_indexing) {
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingVariableDescriptorCount = f12.descriptorBindingVariableDescriptorCount;
    e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  }
  VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  e2.pNext = &e12;
  // Features Xenos emulation commonly needs (Xenia relies on the same set).
  e2.features.independentBlend = f2.features.independentBlend;
  e2.features.geometryShader = f2.features.geometryShader;
  e2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
  e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
  e2.features.textureCompressionBC = f2.features.textureCompressionBC;
  e2.features.depthClamp = f2.features.depthClamp;
  e2.features.depthBiasClamp = f2.features.depthBiasClamp;
  e2.features.fragmentStoresAndAtomics = f2.features.fragmentStoresAndAtomics;
  e2.features.vertexPipelineStoresAndAtomics = f2.features.vertexPipelineStoresAndAtomics;
  e2.features.shaderClipDistance = f2.features.shaderClipDistance;

  float prio = 1.0f;
  VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  q.queueFamilyIndex = queue_family_;
  q.queueCount = 1;
  q.pQueuePriorities = &prio;

  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.pNext = &e2;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &q;
  ci.enabledExtensionCount = uint32_t(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  BF_VK_CHECK(vkCreateDevice(physical_, &ci, nullptr, &device_));
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  BF_VK_INFO("caps: descriptor_indexing=%d descriptor_buffer=%d memory_budget=%d eds3=%d "
             "f16i8=%d fsi=%d",
             caps_.descriptor_indexing, caps_.descriptor_buffer, caps_.memory_budget,
             caps_.extended_dynamic_state3, caps_.shader_float16_int8,
             caps_.fragment_shader_interlock);

  VmaAllocatorCreateInfo ai{};
  ai.vulkanApiVersion = VK_API_VERSION_1_3;
  ai.instance = instance_;
  ai.physicalDevice = physical_;
  ai.device = device_;
  if (caps_.memory_budget) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
  if (e12.bufferDeviceAddress) ai.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  BF_VK_CHECK(vmaCreateAllocator(&ai, &allocator_));
  return true;
}

bool Device::CreatePipelineCache() {
  std::vector<char> blob;
  if (!opts_.pipeline_cache_path.empty()) {
    std::ifstream f(opts_.pipeline_cache_path, std::ios::binary);
    if (f) blob.assign(std::istreambuf_iterator<char>(f), {});
  }
  // Reject a cache from another driver/device; the header layout is fixed by the spec.
  if (blob.size() >= 16 + VK_UUID_SIZE) {
    uint32_t vendor, device;
    std::memcpy(&vendor, blob.data() + 8, 4);
    std::memcpy(&device, blob.data() + 12, 4);
    if (vendor != caps_.props.vendorID || device != caps_.props.deviceID ||
        std::memcmp(blob.data() + 16, caps_.props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
      BF_VK_WARN("pipeline cache is from another device/driver; discarding");
      blob.clear();
    }
  } else {
    blob.clear();
  }
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = blob.size();
  ci.pInitialData = blob.empty() ? nullptr : blob.data();
  BF_VK_CHECK(vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_));
  if (!blob.empty()) BF_VK_INFO("loaded pipeline cache (%zu bytes)", blob.size());
  return true;
}

bool Device::SavePipelineCache() {
  if (!pipeline_cache_ || opts_.pipeline_cache_path.empty()) return false;
  size_t size = 0;
  BF_VK_CHECK(vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr));
  std::vector<char> blob(size);
  BF_VK_CHECK(vkGetPipelineCacheData(device_, pipeline_cache_, &size, blob.data()));
  std::string tmp = opts_.pipeline_cache_path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(blob.data(), std::streamsize(size))) return false;
  }
  std::remove(opts_.pipeline_cache_path.c_str());
  if (std::rename(tmp.c_str(), opts_.pipeline_cache_path.c_str()) != 0) return false;
  BF_VK_INFO("saved pipeline cache (%zu bytes)", size);
  return true;
}

void Device::Shutdown() {
  if (device_) vkDeviceWaitIdle(device_);
  if (pipeline_cache_) {
    SavePipelineCache();
    vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    pipeline_cache_ = VK_NULL_HANDLE;
  }
  if (allocator_) vmaDestroyAllocator(allocator_), allocator_ = VK_NULL_HANDLE;
  if (device_) vkDestroyDevice(device_, nullptr), device_ = VK_NULL_HANDLE;
  if (messenger_) {
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance_, messenger_, nullptr);
    messenger_ = VK_NULL_HANDLE;
  }
  if (instance_) vkDestroyInstance(instance_, nullptr), instance_ = VK_NULL_HANDLE;
}

}  // namespace bf::vk
