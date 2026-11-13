// bf1943: NVIDIA DLSS super resolution (bf_dlss.h).
// This software contains source code provided by NVIDIA Corporation (the
// DLSS SDK's headers, thirdparty/dlss, under NVIDIA's own licence there).

#include "xenia/gpu/vulkan/bf_dlss.h"

#include <cmath>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_vk.h"

namespace xe {
namespace gpu {
namespace bfdlss {
namespace {

// Not a registered NVIDIA application: a project id of our own, engine custom.
constexpr char kProjectId[] = "4f1c9b2e-7a3d-4e8b-9c51-1943b0f1e7a2";
constexpr char kEngineVersion[] = "1.0";
const wchar_t* kDataPath = L".";

bool tried = false, ok = false;
NVSDK_NGX_Parameter* params = nullptr;
NVSDK_NGX_Handle* feature = nullptr;
uint32_t feature_render_w = 0, feature_render_h = 0, feature_display_w = 0, feature_display_h = 0;

NVSDK_NGX_FeatureDiscoveryInfo Discovery() {
  NVSDK_NGX_FeatureDiscoveryInfo info = {};
  info.SDKVersion = NVSDK_NGX_Version_API;
  info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
  info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
  info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
  info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
  info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
  info.ApplicationDataPath = kDataPath;
  return info;
}

std::vector<std::string> DeviceExtensions(VkInstance instance, VkPhysicalDevice physical_device) {
  std::vector<std::string> names;
  NVSDK_NGX_FeatureDiscoveryInfo info = Discovery();
  uint32_t count = 0;
  VkExtensionProperties* extensions = nullptr;
  NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical_device, &info,
                                                                              &count, &extensions);
  if (NVSDK_NGX_FAILED(r)) {
    XELOGI("dlss: no device extension requirements ({:08X})", uint32_t(r));
    return names;
  }
  for (uint32_t i = 0; i < count; ++i) names.emplace_back(extensions[i].extensionName);
  XELOGI("dlss: {} device extensions asked for", count);
  return names;
}

NVSDK_NGX_Resource_VK Resource(VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height,
                               bool write) {
  return NVSDK_NGX_Create_ImageView_Resource_VK(view, image, ui::vulkan::util::InitializeSubresourceRange(), format,
                                                width, height, write);
}

bool EnsureFeature(VkCommandBuffer cb, const bffsr::Frame& f) {
  if (feature && feature_render_w == f.render_width && feature_render_h == f.render_height &&
      feature_display_w == f.display_width && feature_display_h == f.display_height) {
    return true;
  }
  if (feature) {
    NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    feature = nullptr;
  }
  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = f.render_width;
  create.Feature.InHeight = f.render_height;
  create.Feature.InTargetWidth = f.display_width;
  create.Feature.InTargetHeight = f.display_height;
  create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
  // Depth: reversed (the game's); the colour: the tone-mapped frame (not HDR);
  // the motion vectors: render size, without the jitter.
  create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
  NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT(cb, 1, 1, &feature, params, &create);
  if (NVSDK_NGX_FAILED(r)) {
    XELOGE("dlss: no feature for {}x{} -> {}x{} ({:08X})", f.render_width, f.render_height, f.display_width,
           f.display_height, uint32_t(r));
    feature = nullptr;
    ok = false;  // not again
    return false;
  }
  feature_render_w = f.render_width, feature_render_h = f.render_height;
  feature_display_w = f.display_width, feature_display_h = f.display_height;
  XELOGI("dlss: feature {}x{} -> {}x{}", f.render_width, f.render_height, f.display_width, f.display_height);
  return true;
}

float Halton(uint32_t index, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  for (; index; index /= base) {
    f /= float(base);
    r += f * float(index % base);
  }
  return r;
}

}  // namespace

void Prepare() { ui::vulkan::bf_extra_device_extensions = DeviceExtensions; }

bool Available(const ui::vulkan::VulkanDevice* device) {
  if (tried) return ok;
  tried = true;
  const ui::vulkan::VulkanInstance* instance = device->vulkan_instance();
  NVSDK_NGX_FeatureDiscoveryInfo info = Discovery();
  NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
      kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, kDataPath, instance->instance(),
      device->physical_device(), device->device(), instance->functions().vkGetInstanceProcAddr,
      instance->functions().vkGetDeviceProcAddr, info.FeatureInfo);
  if (NVSDK_NGX_FAILED(r)) {
    XELOGI("dlss: NGX did not start ({:08X})", uint32_t(r));
    return false;
  }
  if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)) || !params) {
    XELOGI("dlss: no capability parameters");
    return false;
  }
  int available = 0, needs_driver = 0;
  NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
  XELOGI("dlss: {}{}", available ? "available" : "not available on this GPU",
         needs_driver ? " (needs a newer driver)" : "");
  ok = available != 0;
  return ok;
}

bool Evaluate(const ui::vulkan::VulkanDevice* device, VkCommandBuffer cb, const bffsr::Frame& f) {
  if (!Available(device) || !ok || !EnsureFeature(cb, f)) return false;
  NVSDK_NGX_Resource_VK color = Resource(f.color, f.color_view, f.color_format, f.render_width, f.render_height, false);
  NVSDK_NGX_Resource_VK depth =
      Resource(f.depth, f.depth_view, VK_FORMAT_R32_SFLOAT, f.render_width, f.render_height, false);
  NVSDK_NGX_Resource_VK motion =
      Resource(f.motion, f.motion_view, VK_FORMAT_R16G16_SFLOAT, f.render_width, f.render_height, false);
  NVSDK_NGX_Resource_VK output =
      Resource(f.output, f.output_view, f.output_format, f.display_width, f.display_height, true);
  NVSDK_NGX_VK_DLSS_Eval_Params e = {};
  e.Feature.pInColor = &color;
  e.Feature.pInOutput = &output;
  e.pInDepth = &depth;
  e.pInMotionVectors = &motion;
  e.InJitterOffsetX = f.jitter_x;
  e.InJitterOffsetY = f.jitter_y;
  e.InRenderSubrectDimensions = {f.render_width, f.render_height};
  e.InReset = f.reset ? 1 : 0;
  e.InMVScaleX = e.InMVScaleY = 1.0f;  // already in render pixels
  e.InPreExposure = 1.0f;
  e.InExposureScale = 1.0f;
  e.InFrameTimeDeltaInMsec = f.frame_ms;
  NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cb, feature, params, &e);
  if (NVSDK_NGX_FAILED(r)) {
    static int logs = 0;
    if (logs++ < 5) XELOGE("dlss: evaluate failed ({:08X})", uint32_t(r));
    return false;
  }
  return true;
}

void Jitter(uint32_t n, uint32_t render_width, uint32_t display_width, float& x, float& y) {
  float scale = render_width ? float(display_width) / float(render_width) : 1.0f;
  uint32_t phases = std::max(8u, uint32_t(std::ceil(8.0f * scale * scale)));
  uint32_t i = n % phases + 1;
  x = Halton(i, 2) - 0.5f;
  y = Halton(i, 3) - 0.5f;
}

void Shutdown(const ui::vulkan::VulkanDevice* device) {
  if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
  feature = nullptr;
  if (params) NVSDK_NGX_VULKAN_DestroyParameters(params);
  params = nullptr;
  if (tried && device) NVSDK_NGX_VULKAN_Shutdown1(device->device());
  tried = ok = false;
}

}  // namespace bfdlss
}  // namespace gpu
}  // namespace xe
