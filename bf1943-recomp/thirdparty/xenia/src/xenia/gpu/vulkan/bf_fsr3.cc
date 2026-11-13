// bf1943: AMD FSR 3 temporal upscaling (bf_fsr3.h).

#include "xenia/gpu/vulkan/bf_fsr3.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "pbr_shaders.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/bf_dlss.h"
#include "xenia/gpu/vulkan/scene_fx.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#define _WINDOWS
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"

namespace xe {
namespace gpu {
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/bf_fsr_glue_cs.h"
}  // namespace shaders
namespace bffsr {
namespace {

bool tried = false;
HMODULE module = nullptr;
ffxFunctions ffx = {};
ffxContext context = nullptr;
uint32_t context_render_w = 0, context_render_h = 0, context_display_w = 0, context_display_h = 0;

void Message(uint32_t type, const wchar_t* message) {
  char text[512];
  size_t n = 0;
  for (; message[n] && n + 1 < sizeof(text); ++n) text[n] = char(message[n] < 128 ? message[n] : '?');
  text[n] = 0;
  XELOGI("fsr3: {}{}", type == FFX_API_MESSAGE_TYPE_ERROR ? "error: " : "", text);
}

FfxApiResource Image(VkImage image, VkFormat format, uint32_t width, uint32_t height, bool storage) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
  // All of them are in VK_IMAGE_LAYOUT_GENERAL (FSR's state for that).
  return ffxApiGetResourceVK(reinterpret_cast<void*>(image),
                             ffxApiGetImageResourceDescriptionVK(image, info, 0),
                             FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool EnsureContext(const ui::vulkan::VulkanDevice* device, const Frame& f) {
  if (context && context_render_w == f.render_width && context_render_h == f.render_height &&
      context_display_w == f.display_width && context_display_h == f.display_height) {
    return true;
  }
  if (context) {
    ffx.DestroyContext(&context, nullptr);
    context = nullptr;
  }
  ffxCreateBackendVKDesc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext.vkDevice = device->device();
  backend.deviceContext.vkPhysicalDevice = device->physical_device();
  backend.deviceContext.vkDeviceProcAddr = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &backend.header;
  // Depth: reversed (the game's), the colour: the tone-mapped frame.
  desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  desc.maxRenderSize = {f.render_width, f.render_height};
  desc.maxUpscaleSize = {f.display_width, f.display_height};
  desc.fpMessage = Message;
  if (ffx.CreateContext(&context, &desc.header, nullptr) != FFX_API_RETURN_OK) {
    XELOGE("fsr3: no context for {}x{} -> {}x{}", f.render_width, f.render_height, f.display_width,
           f.display_height);
    context = nullptr;
    return false;
  }
  context_render_w = f.render_width, context_render_h = f.render_height;
  context_display_w = f.display_width, context_display_h = f.display_height;
  XELOGI("fsr3: context {}x{} -> {}x{}", f.render_width, f.render_height, f.display_width,
         f.display_height);
  return true;
}

}  // namespace

bool Available() {
  if (!tried) {
    tried = true;
    module = LoadLibraryW(L"amd_fidelityfx_loader_vk.dll");
    if (module) {
      ffxLoadFunctions(&ffx, module);
      if (!ffx.CreateContext || !ffx.Dispatch || !ffx.Query || !ffx.DestroyContext) {
        FreeLibrary(module);
        module = nullptr;
      }
    }
    XELOGI("fsr3: {}", module ? "loaded" : "amd_fidelityfx_loader_vk.dll not found or incomplete");
  }
  return module != nullptr;
}

void Jitter(uint32_t n, uint32_t render_width, uint32_t display_width, float& x, float& y) {
  x = y = 0.0f;
  if (!Available() || !context) return;
  int32_t phases = 8;
  ffxQueryDescUpscaleGetJitterPhaseCount count = {};
  count.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  count.renderWidth = render_width;
  count.displayWidth = display_width;
  count.pOutPhaseCount = &phases;
  ffx.Query(&context, &count.header);
  ffxQueryDescUpscaleGetJitterOffset offset = {};
  offset.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
  offset.index = int32_t(n % uint32_t(std::max(phases, 1)));
  offset.phaseCount = phases;
  offset.pOutX = &x;
  offset.pOutY = &y;
  ffx.Query(&context, &offset.header);
}

bool Dispatch(const ui::vulkan::VulkanDevice* device, VkCommandBuffer cb, const Frame& f) {
  if (!Available() || !EnsureContext(device, f)) return false;
  ffxDispatchDescUpscale d = {};
  d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  d.commandList = cb;
  d.color = Image(f.color, f.color_format, f.render_width, f.render_height, false);
  d.depth = Image(f.depth, VK_FORMAT_R32_SFLOAT, f.render_width, f.render_height, false);
  d.motionVectors = Image(f.motion, VK_FORMAT_R16G16_SFLOAT, f.render_width, f.render_height, false);
  d.output = Image(f.output, f.output_format, f.display_width, f.display_height, true);
  d.jitterOffset = {f.jitter_x, f.jitter_y};
  d.motionVectorScale = {1.0f, 1.0f};  // already in render pixels
  d.renderSize = {f.render_width, f.render_height};
  d.upscaleSize = {f.display_width, f.display_height};
  d.enableSharpening = f.sharpness > 0.0f;
  d.sharpness = f.sharpness;
  d.frameTimeDelta = f.frame_ms;
  d.preExposure = 1.0f;
  d.reset = f.reset;
  d.cameraNear = f.near_plane;
  d.cameraFar = f.far_plane;
  d.cameraFovAngleVertical = f.fov_y;
  d.viewSpaceToMetersFactor = 1.0f;
  d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  ffxReturnCode_t r = ffx.Dispatch(&context, &d.header);
  if (r != FFX_API_RETURN_OK) {
    static int logs = 0;
    if (logs++ < 5) XELOGE("fsr3: dispatch failed ({})", uint32_t(r));
    return false;
  }
  return true;
}

namespace {

// The glue compute shader (bf_fsr_glue.cs.glsl) and the images around FSR.
constexpr uint32_t kSlots = 4;
struct Glue {
  bool tried = false, ok = false;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[kSlots] = {};
  VkImage pre_hud = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
  VkDeviceMemory pre_hud_memory = VK_NULL_HANDLE, out_memory = VK_NULL_HANDLE;
  VkImageView pre_hud_view = VK_NULL_HANDLE, out_view = VK_NULL_HANDLE;
  uint32_t render_w = 0, render_h = 0, display_w = 0, display_h = 0;
  uint32_t frame = 0;
  Frame frames[kSlots];
  std::pair<const ui::vulkan::VulkanDevice*, Frame*> run_args[kSlots];
  std::chrono::steady_clock::time_point last;
  // Images of an old size, freed once no frame in flight uses them.
  struct Old {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    uint32_t swaps_left;
  };
  std::vector<Old> old;
} glue;

struct Params {
  uint32_t mode, render_width, render_height, display_width, display_height, color_offset, pitch;
};

bool MakeImage(const ui::vulkan::VulkanDevice* device, uint32_t w, uint32_t h, VkImage& image,
               VkDeviceMemory& memory, VkImageView& view) {
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {w, h, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image, memory)) {
    return false;
  }
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = info.format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  return device->functions().vkCreateImageView(device->device(), &view_info, nullptr, &view) ==
         VK_SUCCESS;
}

void FreeImage(const ui::vulkan::VulkanDevice* device, VkImage& image, VkDeviceMemory& memory,
               VkImageView& view) {
  const auto& dfn = device->functions();
  if (view) dfn.vkDestroyImageView(device->device(), view, nullptr);
  if (image) dfn.vkDestroyImage(device->device(), image, nullptr);
  if (memory) dfn.vkFreeMemory(device->device(), memory, nullptr);
  view = VK_NULL_HANDLE, image = VK_NULL_HANDLE, memory = VK_NULL_HANDLE;
}

bool EnsureGlue(const ui::vulkan::VulkanDevice* device) {
  if (glue.tried) return glue.ok;
  glue.tried = true;
  const auto& dfn = device->functions();
  VkDevice d = device->device();
  VkDescriptorSetLayoutBinding b[4] = {};
  const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                     VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
  for (uint32_t i = 0; i < 4; ++i) {
    b[i].binding = i;
    b[i].descriptorType = types[i];
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 4;
  set_info.pBindings = b;
  if (dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &glue.set_layout) != VK_SUCCESS) return false;
  VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params)};
  VkPipelineLayoutCreateInfo layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &glue.set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push;
  if (dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &glue.layout) != VK_SUCCESS) return false;
  glue.pipeline = ui::vulkan::util::CreateComputePipeline(device, glue.layout, shaders::bf_fsr_glue_cs,
                                                         sizeof(shaders::bf_fsr_glue_cs));
  if (!glue.pipeline) return false;
  VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots},
                                   {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlots}};
  VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kSlots;
  pool_info.poolSizeCount = 3;
  pool_info.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(d, &pool_info, nullptr, &glue.pool) != VK_SUCCESS) return false;
  VkDescriptorSetLayout layouts[kSlots];
  for (auto& l : layouts) l = glue.set_layout;
  VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = glue.pool;
  alloc.descriptorSetCount = kSlots;
  alloc.pSetLayouts = layouts;
  if (dfn.vkAllocateDescriptorSets(d, &alloc, glue.sets) != VK_SUCCESS) return false;
  glue.ok = true;
  XELOGI("fsr3: glue ready");
  return true;
}

// The upscale, when the deferred commands are executed.
void RunFsr(VkCommandBuffer cb, void* data) {
  auto* a = static_cast<std::pair<const ui::vulkan::VulkanDevice*, Frame*>*>(data);
  if (a->second->use_dlss) {
    bfdlss::Evaluate(a->first, cb, *a->second);
  } else {
    Dispatch(a->first, cb, *a->second);
  }
}

void Bind(vulkan::VulkanCommandProcessor& cp, uint32_t slot, const Params& p) {
  auto& cb = cp.deferred_command_buffer();
  cp.BindExternalComputePipeline(glue.pipeline);
  cb.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, glue.layout, 0, 1, &glue.sets[slot], 0,
                             nullptr);
  cb.CmdVkPushConstants(glue.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
}

}  // namespace

VkImageView Record(vulkan::VulkanCommandProcessor& cp, VkImageView final_view,
                   uint32_t& width, uint32_t& height) {
  const uint32_t render_width = width, render_height = height;
  const ui::vulkan::VulkanDevice* device = cp.GetVulkanDevice();
  uint32_t display_w = display_width, display_h = display_height;
  VkBuffer materials = cp.bf_material_buffer();
  VkImage depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE, motion_view = VK_NULL_HANDLE;
  uint32_t mw = 0, mh = 0;
  const bool use_dlss = enabled && dlss && bfdlss::Available(device);
  // (FSR does not scale down: a window smaller than the frame goes without.)
  if (!enabled || (!use_dlss && !Available()) || display_w < render_width || display_h < render_height || !materials ||
      !scenefx::FsrInputs(depth, motion, mw, mh, &depth_view, &motion_view) || mw != render_width || mh != render_height ||
      !EnsureGlue(device)) {
    active = false;
    static int logs = 0;
    if (enabled && logs++ < 5) {
      XELOGI("fsr3: frame skipped (render {}x{}, scene effects {}x{}, display {}x{})", render_width,
             render_height, mw, mh, display_w, display_h);
    }
    return VK_NULL_HANDLE;
  }
  const auto& dfn = device->functions();
  if (glue.render_w != render_width || glue.render_h != render_height || glue.display_w != display_w ||
      glue.display_h != display_h) {
    for (auto* i : {&glue.pre_hud, &glue.out}) {
      bool pre = i == &glue.pre_hud;
      if (*i) {
        glue.old.push_back({*i, pre ? glue.pre_hud_memory : glue.out_memory,
                            pre ? glue.pre_hud_view : glue.out_view, 8});
      }
    }
    glue.pre_hud = glue.out = VK_NULL_HANDLE;
    glue.pre_hud_memory = glue.out_memory = VK_NULL_HANDLE;
    glue.pre_hud_view = glue.out_view = VK_NULL_HANDLE;
    glue.render_w = 0;
    if (!MakeImage(device, render_width, render_height, glue.pre_hud, glue.pre_hud_memory,
                   glue.pre_hud_view) ||
        !MakeImage(device, display_w, display_h, glue.out, glue.out_memory, glue.out_view)) {
      XELOGE("fsr3: no images for {}x{} -> {}x{}", render_width, render_height, display_w, display_h);
      return VK_NULL_HANDLE;
    }
    glue.render_w = render_width, glue.render_h = render_height;
    glue.display_w = display_w, glue.display_h = display_h;
    glue.frame = 0;
  }
  for (size_t i = 0; i < glue.old.size();) {
    if (--glue.old[i].swaps_left) {
      ++i;
      continue;
    }
    FreeImage(device, glue.old[i].image, glue.old[i].memory, glue.old[i].view);
    glue.old.erase(glue.old.begin() + i);
  }
  uint32_t slot = glue.frame % kSlots;
  VkDescriptorBufferInfo buffer_info = {materials, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo pre_info = {VK_NULL_HANDLE, glue.pre_hud_view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo final_info = {VK_NULL_HANDLE, final_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkDescriptorImageInfo out_info = {VK_NULL_HANDLE, glue.out_view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[i].dstSet = glue.sets[slot];
    w[i].dstBinding = i;
    w[i].descriptorCount = 1;
  }
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[0].pBufferInfo = &buffer_info;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &pre_info;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, w[2].pImageInfo = &final_info;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[3].pImageInfo = &out_info;
  dfn.vkUpdateDescriptorSets(device->device(), 4, w, 0, nullptr);

  const VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  const VkPipelineStageFlags fs = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  const VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  const VkImageSubresourceRange range = ui::vulkan::util::InitializeSubresourceRange();
  auto& cb = cp.deferred_command_buffer();
  cp.PushImageMemoryBarrier(glue.pre_hud, range, cs, cs, rw, rw, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_GENERAL);
  cp.PushImageMemoryBarrier(glue.out, range, fs, cs, VK_ACCESS_SHADER_READ_BIT, rw,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  cp.SubmitBarriers(true);
  // Everything the frame wrote (the tone mapping pass's pre-HUD copy in the
  // buffer, the final frame, the scene effects' depth and motion) visible to
  // compute.
  VkMemoryBarrier frame_done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT};
  cb.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, cs, 0, 1, &frame_done, 0, nullptr, 0,
                          nullptr);
  Params p = {0, render_width, render_height, display_w, display_h, pbr::kMaterialColorOffset,
              pbr::kMaterialPitch};
  Bind(cp, slot, p);
  cb.CmdVkDispatch((render_width + 7) / 8, (render_height + 7) / 8, 1);
  VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, rw, rw};
  cb.CmdVkPipelineBarrier(cs, cs, 0, 1, &barrier, 0, nullptr, 0, nullptr);

  // FSR 3, recorded into the real command buffer when these commands run.
  auto now = std::chrono::steady_clock::now();
  Frame& f = glue.frames[slot];
  f = Frame{};
  f.color = glue.pre_hud, f.color_format = VK_FORMAT_R8G8B8A8_UNORM;
  f.depth = depth, f.motion = motion;
  f.color_view = glue.pre_hud_view, f.output_view = glue.out_view;
  f.depth_view = depth_view, f.motion_view = motion_view;
  f.use_dlss = use_dlss;
  f.output = glue.out, f.output_format = VK_FORMAT_R8G8B8A8_UNORM;
  f.render_width = render_width, f.render_height = render_height;
  f.display_width = display_w, f.display_height = display_h;
  f.jitter_x = jitter_x, f.jitter_y = jitter_y;
  f.frame_ms = glue.frame ? std::chrono::duration<float, std::milli>(now - glue.last).count() : 16.7f;
  f.reset = glue.frame == 0;
  f.sharpness = sharpness;
  glue.last = now;
  glue.run_args[slot] = {device, &f};
  cb.CmdBfCallback(RunFsr, &glue.run_args[slot]);
  // FSR bound its own pipelines and descriptors.
  cp.ForgetExternalComputePipeline();
  cb.CmdVkPipelineBarrier(cs, cs, 0, 1, &barrier, 0, nullptr, 0, nullptr);

  // The HUD back on top.
  p.mode = 1;
  Bind(cp, slot, p);
  cb.CmdVkDispatch((display_w + 7) / 8, (display_h + 7) / 8, 1);
  cp.PushImageMemoryBarrier(glue.out, range, cs, fs, rw, VK_ACCESS_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // The next frame's jitter (the context exists after the first dispatch;
  // until then frames go unjittered).
  ++glue.frame;
  float jx, jy;
  if (use_dlss) {
    bfdlss::Jitter(glue.frame, render_width, display_w, jx, jy);
  } else {
    Jitter(glue.frame, render_width, display_w, jx, jy);
  }
  jitter_x = jx, jitter_y = jy;
  past_tonemap = false;
  active = true;
  width = display_w, height = display_h;
  return glue.out_view;
}

void Shutdown() {
  if (context) ffx.DestroyContext(&context, nullptr);
  context = nullptr;
}

}  // namespace bffsr
}  // namespace gpu
}  // namespace xe
