#include "xenia/gpu/vulkan/scene_fx.h"
#include "pbr_shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/bf_gore.h"
#include "xenia/gpu/vulkan/scene_fx_rt.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/scene_fx_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/scene_fx_rt_cs.h"
}  // namespace shaders

namespace scenefx {

namespace {
Capture latest;
bool fresh = false;

// A lit main-view vertex shader: position from c12-c15, world position from
// c16-c18 and the three shadow slice coordinates from c0-c10.
bool IsLitVertexShader(const Shader* shader) {
  static std::unordered_map<uint64_t, bool> known;
  auto [it, added] = known.try_emplace(shader->ucode_data_hash(), false);
  if (added) {
    const std::string& d = shader->ucode_disassembly();
    it->second = d.find("dp4 oPos.x___, c12.zxyw") != std::string::npos &&
                 d.find("dp4 o0.x000, c0.zxyw") != std::string::npos &&
                 d.find("dp4 o2.x000, c8.zxyw") != std::string::npos &&
                 d.find("c16.zxyw") != std::string::npos;
  }
  return it->second;
}

// Whether a lit vertex shader's slice rows (c0) take the world position (a
// register other than the one c12 projects) rather than the object's.
bool SlicesTakeWorld(const Shader* shader) {
  static std::unordered_map<uint64_t, bool> known;
  auto [it, added] = known.try_emplace(shader->ucode_data_hash(), false);
  if (added) {
    const std::string& d = shader->ucode_disassembly();
    auto source = [&](const std::string& tag) {
      size_t at = d.find(tag);
      if (at == std::string::npos) return std::string();
      at += tag.size();
      size_t end = at;
      while (end < d.size() && d[end] >= '0' && d[end] <= '9') ++end;
      return d.substr(at, end - at);
    };
    std::string object = source("c12.zxyw, r");
    std::string slice = source("dp4 o0.x000, c0.zxyw, r");
    it->second = !object.empty() && !slice.empty() && object != slice;
  }
  return it->second;
}

bool enabled = false;

// The muzzle flash light taken over from the game's draws this frame.
struct FlashLight {
  bool seen = false;
  float pos[3] = {}, color[3] = {};
} flash;
// Between the world depth copy and the scene resolve: the first-person
// weapon, sky and particles.
bool after_depth_copy = false;
// The eye at the last pass, for telling the flash's light from others.
float flash_eye[3] = {};
bool have_flash_eye = false;

// The lit pixel shaders' version with a point light: the vector from the
// surface to c6 (add rA, -rB, c6).
bool IsPointLightShader(const Shader* shader) {
  static std::unordered_map<uint64_t, bool> known;
  auto [it, added] = known.try_emplace(shader->ucode_data_hash(), false);
  if (added) {
    const std::string& d = shader->ucode_disassembly();
    for (size_t at = d.find(", c6."); at != std::string::npos; at = d.find(", c6.", at + 1)) {
      size_t line = d.rfind('\n', at);
      line = line == std::string::npos ? 0 : line + 1;
      std::string text = d.substr(line, at - line);
      if (text.find(" add ") != std::string::npos && text.find(", -r") != std::string::npos) {
        it->second = true;
        break;
      }
    }
  }
  return it->second;
}
Settings settings;

Settings ParseSettings(const char* text_in) {
  Settings s;
  std::string text = text_in ? text_in : "";
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(',', at);
    if (end == std::string::npos) end = text.size();
    std::string item = text.substr(at, end - at);
    at = end + 1;
    size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    std::string key = item.substr(0, eq);
    float v = float(atof(item.c_str() + eq + 1));
    if (key == "distance") s.max_distance = v;
    else if (key == "steps") s.steps = std::max(0, int(v));  // 0: no fog
    else if (key == "density") s.density = v;
    else if (key == "height") s.falloff = 1.0f / std::max(v, 1.0f);
    else if (key == "base") s.base_height = v;
    else if (key == "shafts") s.shafts = v;
    else if (key == "bias") s.shadow_bias = v;
    else if (key == "debug") s.debug = uint32_t(v);
    else if (key == "eye") s.eye_height = v;
    else if (key == "sky") s.sky = v;
    else if (key == "shadows") s.ss_strength = v;
    else if (key == "shadow_length") s.ss_length = v;
    else if (key == "shadow_steps") s.ss_steps = std::max(1, int(v));
    else if (key == "shadow_thickness") s.ss_thickness = v;
    else if (key == "shadow_map") s.ss_map = v;
    else if (key == "rt") s.rt = v;
    else if (key == "rt_distance") s.rt_distance = v;
    else if (key == "gi") s.gi = v;
    else if (key == "gi_rays") s.gi_rays = std::clamp(int(v), 1, 16);
    else if (key == "gi_distance") s.gi_distance = v;
    else if (key == "gi_frames") s.gi_frames = std::max(v, 1.0f);
    else if (key == "gi_contact") s.gi_contact = std::max(v, 0.0f);
    else if (key == "own_shadows") s.shadows = v;
    else if (key == "reflect") s.reflect = v;
    else if (key == "gloss") s.gloss = v;
    else if (key == "waves") s.waves = v;
    else if (key == "rt_cell") s.rt_cell = std::max(v, 0.05f);
    else if (key == "clouds") s.clouds = std::clamp(v, 0.0f, 1.0f);
    else if (key == "cloud_base") s.cloud_base = v;
    else if (key == "cloud_thickness") s.cloud_thickness = std::max(v, 50.0f);
    else if (key == "cloud_steps") s.cloud_steps = std::clamp(int(v), 4, 128);
    else if (key == "cloud_density") s.cloud_density = std::max(v, 0.0f);
    else if (key == "cloud_wind") s.cloud_wind = v;
    else if (key == "cloud_shadows") s.cloud_shadows = std::max(v, 0.0f);
    else if (key == "ao") s.ao = std::max(v, 0.0f);
    else if (key == "ao_radius") s.ao_radius = std::max(v, 0.05f);
    else if (key == "ao_direct") s.ao_direct = std::clamp(v, 0.0f, 1.0f);
    else if (key == "ao_distance") s.ao_distance = std::max(v, 1.0f);
    else if (key == "water_refract") s.water_refract = std::max(v, 0.0f);
    else if (key == "water_foam") s.water_foam = std::max(v, 0.0f);
    else if (key == "water_shallow") s.water_shallow = std::max(v, 0.0f);
    else if (key == "foam_width") s.foam_width = std::max(v, 0.1f);
    else if (key == "detail") s.detail = std::max(v, 0.0f);
    else if (key == "detail_distance") s.detail_distance = std::max(v, 1.0f);
    else if (key == "pebble_size") s.pebble_size = std::max(v, 0.01f);
    else if (key == "crack_size") s.crack_size = std::max(v, 0.05f);
    else if (key == "flash") s.flash = std::max(v, 0.0f);
    else if (key == "flash_radius") s.flash_radius = std::max(v, 0.5f);
  }
  return s;
}

// Row-major 4x4 inverse (Gauss-Jordan); false if singular.
bool Invert(const double m[4][4], double out[4][4]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c)
      a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (std::abs(a[pivot][c]) < 1e-12) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r][c] = a[r][c + 4];
  return true;
}

// Blood particles at most (bf_gore.h), two float4 each.
constexpr uint32_t kGoreMax = 64;

// The shader's uniform block (std140).
struct Params {
  float inv_view_proj[16];  // column-major
  float slices[9][4];
  float camera[4];
  float sun_dir[4];
  float sun_color[4];
  float ambient[4];
  float screen[4];
  float march[4];
  float strength[4];
  uint32_t shadow_info[4];
  float water[4];
  float view_proj[16];      // world -> (NDC x, y, depth value), column-major
  float ss[4];              // screen-space shadows: strength, length, steps, thickness
  float rt[4];              // ray-traced shadows: on, length (m), instances
  float prev_view_proj[16]; // last frame's view_proj
  float prev_camera[4];     // last frame's eye, w: 1 if the history is usable
  float gi[4];              // strength, rays, cache cell (m), frame
  float gi2[4];             // longest bounce ray (m), frames averaged
  float refl[4];            // the sea, other surfaces, seconds, waves
  float gore_info[4];       // blood (bf_gore.h): x count
  float gore[2 * kGoreMax][4];
  float cloud[4];           // cover (0 off), base over the sea (m), thickness (m), seconds
  float cloud2[4];          // steps, extinction per metre, wind (m/s), shadows
  float ao[4];              // strength (0 off), radius (m), sun's share, furthest (m)
  float wfx[4];             // water: foam, refraction, shallow, foam width (m)
  float flash[4];           // muzzle flash light: world position, strength (0 none)
  float flash2[4];          // its colour (the game's c7), radius (m)
  float detail[4];          // surface detail: strength, furthest (m), pebble, crack cell (m)
};
static_assert(sizeof(Params) <= 4096);

// Vulkan objects, created on first use.
constexpr uint32_t kSlots = 8;
constexpr VkDeviceSize kSlotSize = 4096;
constexpr VkDeviceSize kHorizonSize = (66 + 128) * 16;
struct Pipeline {
  bool tried = false, ok = false;
  bool rt = false;  // the ray query variant, with the TLAS in binding 6
  uint32_t rt_instances = 0;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[kSlots] = {};
  VkBuffer uniforms = VK_NULL_HANDLE;
  VkDeviceMemory uniforms_memory = VK_NULL_HANDLE;
  uint32_t uniforms_memory_type = 0;
  VkDeviceSize uniforms_memory_size = 0;
  uint8_t* uniforms_mapping = nullptr;
  uint64_t slot_submission[kSlots] = {};
  uint32_t next_slot = 0;
  uint32_t frame = 0;
  // The world depth saved by the first pass for the second.
  VkImage saved_depth = VK_NULL_HANDLE;
  VkDeviceMemory saved_depth_memory = VK_NULL_HANDLE;
  VkImageView saved_depth_view = VK_NULL_HANDLE;
  uint32_t saved_width = 0, saved_height = 0;
  bool saved_initialized = false;
  // The measured horizon sky colours, kept from frame to frame.
  VkBuffer horizon = VK_NULL_HANDLE;
  VkDeviceMemory horizon_memory = VK_NULL_HANDLE;
  bool horizon_initialized = false;
  // Ray-traced light (with rt): the world-space colour cache and, at half
  // resolution, two frames of the light (rgba16f) and their distances (r32f),
  // written and read in turn.
  VkBuffer cache = VK_NULL_HANDLE;
  VkDeviceMemory cache_memory = VK_NULL_HANDLE;
  bool cache_initialized = false;
  bool materials_initialized = false;
  VkImage light[2] = {}, light_dist[2] = {};
  VkDeviceMemory light_memory[2] = {}, light_dist_memory[2] = {};
  VkImageView light_view[2] = {}, light_dist_view[2] = {};
  bool light_initialized = false;
  uint32_t light_parity = 0;
  // The volumetric clouds at half resolution (rgba16f: light, how much they
  // hide), for the full-resolution pass.
  VkImage clouds = VK_NULL_HANDLE;
  VkDeviceMemory clouds_memory = VK_NULL_HANDLE;
  VkImageView clouds_view = VK_NULL_HANDLE;
  bool clouds_initialized = false;
  // The ambient occlusion at half resolution (rg16f: visibility, distance).
  VkImage ao = VK_NULL_HANDLE;
  VkDeviceMemory ao_memory = VK_NULL_HANDLE;
  VkImageView ao_view = VK_NULL_HANDLE;
  bool ao_initialized = false;
  // The scene with what is seen through the sea bent (pass 7), read by pass 1.
  VkImage refract = VK_NULL_HANDLE;
  VkDeviceMemory refract_memory = VK_NULL_HANDLE;
  VkImageView refract_view = VK_NULL_HANDLE;
  bool refract_initialized = false;
  // Last frame's camera, for the light kept over frames.
  bool have_prev = false;
  float prev_view_proj[16] = {};
  float prev_camera[3] = {};
} vk;

// The colour cache: 2^21 cells of 16 bytes (scene_fx.cs.glsl CacheLoad).
constexpr VkDeviceSize kCacheSize = VkDeviceSize(16) << 21;

bool CreateStorageImage(const ui::vulkan::VulkanDevice* device, VkFormat format,
                        uint32_t width, uint32_t height, VkImage& image,
                        VkDeviceMemory& memory, VkImageView& view) {
  const auto& dfn = device->functions();
  VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          device, image_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          image, memory))
    return false;
  VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  return dfn.vkCreateImageView(device->device(), &view_info, nullptr, &view) ==
         VK_SUCCESS;
}

bool CreateSavedDepth(const ui::vulkan::VulkanDevice* device, uint32_t width,
                      uint32_t height) {
  if (!CreateStorageImage(device, VK_FORMAT_R32_SFLOAT, width, height,
                          vk.saved_depth, vk.saved_depth_memory,
                          vk.saved_depth_view))
    return false;
  vk.saved_width = width;
  vk.saved_height = height;
  if (!CreateStorageImage(device, VK_FORMAT_R16G16B16A16_SFLOAT, (width + 1) / 2,
                          (height + 1) / 2, vk.clouds, vk.clouds_memory, vk.clouds_view))
    return false;
  if (!CreateStorageImage(device, VK_FORMAT_R16G16_SFLOAT, (width + 1) / 2,
                          (height + 1) / 2, vk.ao, vk.ao_memory, vk.ao_view))
    return false;
  if (!CreateStorageImage(device, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, vk.refract,
                          vk.refract_memory, vk.refract_view))
    return false;
  if (!vk.rt) return true;
  for (int i = 0; i < 2; ++i) {
    if (!CreateStorageImage(device, VK_FORMAT_R16G16B16A16_SFLOAT, (width + 1) / 2,
                            (height + 1) / 2, vk.light[i], vk.light_memory[i],
                            vk.light_view[i]) ||
        !CreateStorageImage(device, VK_FORMAT_R32_SFLOAT, (width + 1) / 2,
                            (height + 1) / 2, vk.light_dist[i],
                            vk.light_dist_memory[i], vk.light_dist_view[i]))
      return false;
  }
  return true;
}

// Bindings 0-5, 14 (the clouds), 15 (the ambient occlusion) and 16 (the
// refracted scene) always; 6-13
// in the ray query variant: the TLAS, the triangle normals, the colour cache,
// the light read (last frame's) and written, their distances read and
// written, the --pbr materials (pbr::MaterialOutput).
constexpr uint32_t kBindings = 17;
constexpr uint32_t kCloudBinding = 14;
constexpr uint32_t kAoBinding = 15;
constexpr uint32_t kRefractBinding = 16;
// The bindings a variant has, in order.
uint32_t BindingList(uint32_t out[kBindings]) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kBindings; ++i) {
    if (vk.rt || i < 6 || i == kCloudBinding || i == kAoBinding || i == kRefractBinding)
      out[n++] = i;
  }
  return n;
}
const VkDescriptorType kBindingTypes[kBindings] = {
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};

bool CreatePipeline(const ui::vulkan::VulkanDevice* device) {
  const auto& dfn = device->functions();
  VkDevice d = device->device();
  uint32_t list[kBindings];
  const uint32_t binding_count = BindingList(list);
  VkDescriptorSetLayoutBinding bindings[kBindings] = {};
  const VkDescriptorType* types = kBindingTypes;
  for (uint32_t i = 0; i < binding_count; ++i) {
    bindings[i].binding = list[i];
    bindings[i].descriptorType = types[list[i]];
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo set_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = binding_count;
  set_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(d, &set_info, nullptr, &vk.set_layout) !=
      VK_SUCCESS)
    return false;
  VkPipelineLayoutCreateInfo layout_info = {
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &vk.set_layout;
  VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(uint32_t)};
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  if (dfn.vkCreatePipelineLayout(d, &layout_info, nullptr, &vk.layout) !=
      VK_SUCCESS)
    return false;
  vk.pipeline =
      vk.rt ? ui::vulkan::util::CreateComputePipeline(
                  device, vk.layout, shaders::scene_fx_rt_cs,
                  sizeof(shaders::scene_fx_rt_cs))
            : ui::vulkan::util::CreateComputePipeline(
                  device, vk.layout, shaders::scene_fx_cs,
                  sizeof(shaders::scene_fx_cs));
  if (vk.pipeline == VK_NULL_HANDLE) return false;

  VkDescriptorPoolSize sizes[kBindings];
  for (uint32_t i = 0; i < binding_count; ++i) sizes[i] = {types[list[i]], kSlots};
  VkDescriptorPoolCreateInfo pool_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = kSlots;
  pool_info.poolSizeCount = binding_count;
  pool_info.pPoolSizes = sizes;
  if (dfn.vkCreateDescriptorPool(d, &pool_info, nullptr, &vk.pool) !=
      VK_SUCCESS)
    return false;
  VkDescriptorSetLayout layouts[kSlots];
  for (auto& l : layouts) l = vk.set_layout;
  VkDescriptorSetAllocateInfo alloc_info = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc_info.descriptorPool = vk.pool;
  alloc_info.descriptorSetCount = kSlots;
  alloc_info.pSetLayouts = layouts;
  if (dfn.vkAllocateDescriptorSets(d, &alloc_info, vk.sets) != VK_SUCCESS)
    return false;

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kSlots * kSlotSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kUpload, vk.uniforms,
          vk.uniforms_memory, &vk.uniforms_memory_type,
          &vk.uniforms_memory_size))
    return false;
  void* mapping;
  if (dfn.vkMapMemory(d, vk.uniforms_memory, 0, VK_WHOLE_SIZE, 0, &mapping) !=
      VK_SUCCESS)
    return false;
  vk.uniforms_mapping = static_cast<uint8_t*>(mapping);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kHorizonSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, vk.horizon,
          vk.horizon_memory))
    return false;
  if (vk.rt &&
      !ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, kCacheSize,
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, vk.cache,
          vk.cache_memory))
    return false;
  return true;
}
}  // namespace

void Configure(const char* text) {
  enabled = true;
  // Tests: BF_SCENE_FX_EXTRA ("debug=13,...") wins over the settings page's.
  std::string all = text ? text : "";
  if (const char* extra = std::getenv("BF_SCENE_FX_EXTRA")) all += std::string(",") + extra;
  settings = ParseSettings(all.c_str());
  pbr::SetGameShadowsOff(settings.shadows > 0.0f);
}

bool Enabled() { return enabled; }

namespace {
std::atomic<float> eye_x{0}, eye_y{0}, eye_z{0};
std::atomic<int64_t> eye_ms{-1000000};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// The eye from a lit draw's world-view-projection (c12-c15) and world
// (c16-c18): clip (0, 0, 1, 0) maps back to it. A few times a second.
void UpdateEye(const float* vs) {
  int64_t now = NowMs();
  if (now - eye_ms.load(std::memory_order_relaxed) < 50) return;
  double wvp[4][4], w[4][4] = {}, inv[4][4];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      wvp[r][c] = vs[(12 + r) * 4 + c];
      w[r][c] = r < 3 ? vs[(16 + r) * 4 + c] : (c == 3 ? 1.0 : 0.0);
    }
  if (!Invert(wvp, inv)) return;
  double e[4];
  for (int r = 0; r < 4; ++r) {
    e[r] = 0.0;
    for (int k = 0; k < 4; ++k) e[r] += w[r][k] * inv[k][2];
  }
  if (std::abs(e[3]) < 1e-12) return;
  eye_x = float(e[0] / e[3]), eye_y = float(e[1] / e[3]), eye_z = float(e[2] / e[3]);
  eye_ms = now;
}
}  // namespace

bool Eye(float out[3]) {
  if (NowMs() - eye_ms.load() > 500) return false;
  out[0] = eye_x, out[1] = eye_y, out[2] = eye_z;
  return true;
}

const Settings& GetSettings() { return settings; }

void CaptureDraw(const Shader* vertex_shader, const Shader* pixel_shader,
                 const RegisterFile& regs, const void* depth_target,
                 const void* color_target, Memory& memory) {
  bool fx = Enabled();
  // The eye, for the PC vehicle keys (which soldier is ours).
  if (vertex_shader && depth_target &&
      std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE)) >= 0.5f &&
      IsLitVertexShader(vertex_shader)) {
    UpdateEye(reinterpret_cast<const float*>(
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]));
  }
  if (!(fx || bflook::gore) || !vertex_shader || !pixel_shader ||
      !depth_target || !color_target) {
    return;
  }
  // Every world-space mesh for the ray-traced shadows, whatever its depth
  // range: the game draws the world in slices by distance (viewport z scale
  // about 1 near, 0.002-0.009 farther out).
  if (fx && settings.rt > 0.0f) {
    if (rt::IsTerrainVertexShader(*vertex_shader)) {
      rt::RecordTerrain(regs, memory);
    } else {
      rt::RecordDraw(*vertex_shader, *pixel_shader, regs, depth_target, memory);
    }
  }
  // The scene depth is described by the nearest slice's draws (the
  // first-person weapon is drawn after the depth copy).
  float z_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  if (std::abs(z_scale) < 0.5f) return;
  if (!IsLitVertexShader(vertex_shader)) return;
  const auto* f = reinterpret_cast<const float*>(
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  // The camera for blood on hit (bf_gore.h).
  if (bflook::gore) {
    bflook::SetCamera(reinterpret_cast<const float(*)[4]>(f));
  }
  if (!fx) return;
  Capture& c = latest;
  std::memcpy(c.vs, f, sizeof(c.vs));
  std::memcpy(c.ps, f + 256 * 4, sizeof(c.ps));
  std::memcpy(c.tf0, &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              sizeof(c.tf0));
  c.viewport_z_scale = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
  c.viewport_z_offset = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
  c.depth_target = depth_target;
  c.color_target = color_target;
  c.vs_hash = vertex_shader->ucode_data_hash();
  c.slices_world = SlicesTakeWorld(vertex_shader);
  c.ps_hash = pixel_shader->ucode_data_hash();
  fresh = true;
}

bool TakeOverFlash(const Shader* pixel_shader, const RegisterFile& regs) {
  if (!Enabled() || settings.flash <= 0.0f || !pixel_shader || after_depth_copy ||
      !have_flash_eye || !IsPointLightShader(pixel_shader)) {
    return false;
  }
  const auto* c = reinterpret_cast<const float*>(
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X]);
  const float* pos = c + 6 * 4;
  float dx = pos[0] - flash_eye[0], dy = pos[1] - flash_eye[1], dz = pos[2] - flash_eye[2];
  // Only a light at the gun (explosions and others stay the game's).
  if (dx * dx + dy * dy + dz * dz > 4.0f * 4.0f) return false;
  flash.seen = true;
  std::memcpy(flash.pos, pos, sizeof(flash.pos));
  std::memcpy(flash.color, c + 7 * 4, sizeof(flash.color));
  return true;
}

bool TakeFresh(Capture& out) {
  if (!fresh) return false;
  fresh = false;
  out = latest;
  return true;
}

}  // namespace scenefx

namespace vulkan {

namespace {
// The frame's world pass, kept from the depth copy for the scene resolve.
struct PendingScene {
  bool valid = false;
  scenefx::Capture cap;
} pending;
}  // namespace

void VulkanRenderTargetCache::ApplySceneFx(
    const draw_util::ResolveInfo& resolve_info,
    VulkanSharedMemory& shared_memory, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch) {
  using namespace scenefx;
  if (!Enabled()) return;
  // Two passes: when the game copies the world's depth out (it clears it
  // afterwards for the first-person weapon) the depth is saved; when it
  // resolves the finished HDR scene (with the sky, sea and particles drawn)
  // the fog is applied.
  const bool save_pass = resolve_info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram =
      save_pass ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
  if (edram.msaa_samples != xenos::MsaaSamples::k1X ||
      resolve_info.coordinate_info.width_div_8 * 8 < 640) {
    return;
  }
  Capture cap;
  if (save_pass) {
    // Only the frame's first copy: the game copies the depth again after the
    // weapon is drawn.
    if (pending.valid || !TakeFresh(cap)) return;
  } else {
    if (!pending.valid) return;
    cap = pending.cap;
  }
  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  if (dump_rectangles_.size() != 1 ||
      dump_rectangles_[0].render_target !=
          (save_pass ? cap.depth_target : cap.color_target)) {
    return;
  }
  pending.valid = false;
  if (!save_pass) rt::ResumeRecording();
  auto& depth = *static_cast<VulkanRenderTarget*>(const_cast<RenderTarget*>(
      static_cast<const RenderTarget*>(cap.depth_target)));
  auto& color = *static_cast<VulkanRenderTarget*>(const_cast<RenderTarget*>(
      static_cast<const RenderTarget*>(cap.color_target)));
  RenderTargetKey color_key = color.key(), depth_key = depth.key();
  xenos::ColorRenderTargetFormat format = color_key.GetColorFormat();
  if (color_key.is_depth || !depth_key.is_depth ||
      (save_pass ? depth_key.base_tiles : color_key.base_tiles) !=
          edram.base_tiles ||
      (format != xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT &&
       format !=
           xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16) ||
      color_key.msaa_samples != xenos::MsaaSamples::k1X ||
      depth_key.msaa_samples != xenos::MsaaSamples::k1X ||
      depth_key.pitch_tiles_at_32bpp != color_key.pitch_tiles_at_32bpp) {
    return;
  }

  // The shadow map: three stacked 24_8 slices in guest memory.
  xenos::xe_gpu_texture_fetch_t tf;
  std::memcpy(&tf, cap.tf0, sizeof(tf));
  uint32_t slice_size = tf.size_2d.width + 1;
  if (tf.format != xenos::TextureFormat::k_24_8 || !tf.tiled || !tf.stacked ||
      tf.size_2d.stack_depth + 1 < 3 || slice_size != tf.size_2d.height + 1 ||
      (slice_size & 31) || (tf.pitch << 5) != slice_size) {
    return;
  }
  VkDeviceSize shadow_base = VkDeviceSize(tf.base_address) << 12;
  VkDeviceSize slice_bytes = VkDeviceSize(slice_size) * slice_size * 4;

  uint32_t scale_x = GetKeyScaleX(color_key), scale_y = GetKeyScaleY(color_key);
  uint32_t origin_x = resolve_info.coordinate_info.edram_offset_x_div_8 * 8;
  uint32_t origin_y = resolve_info.coordinate_info.edram_offset_y_div_8 * 8;
  uint32_t width = resolve_info.coordinate_info.width_div_8 * 8 * scale_x;
  uint32_t height = resolve_info.height_div_8 * 8 * scale_y;

  const ui::vulkan::VulkanDevice* device = command_processor_.GetVulkanDevice();
  const auto& dfn = device->functions();
  if (!vk.tried) {
    vk.tried = true;
    vk.rt = GetSettings().rt > 0.0f && rt::Init(device);
    vk.ok = CreatePipeline(device) && CreateSavedDepth(device, width, height);
    XELOGI("scenefx: pipeline {} ({}x{}){}", vk.ok ? "ready" : "FAILED", width,
           height, vk.rt ? ", ray-traced shadows" : "");
  }
  if (!vk.ok || width != vk.saved_width || height != vk.saved_height) return;
  uint32_t slot = vk.next_slot;
  if (vk.slot_submission[slot] > command_processor_.GetCompletedSubmission()) {
    return;  // the GPU is still using this slot's descriptors
  }
  vk.next_slot = (slot + 1) % kSlots;
  vk.slot_submission[slot] = command_processor_.GetCurrentSubmission();

  // Matrices: clip = WVP * object, world = W * object, so world from clip is
  // W * WVP^-1.
  double wvp[4][4], w[4][4] = {}, wvp_inv[4][4], inv_vp[4][4];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      wvp[r][c] = cap.vs[12 + r][c];
      w[r][c] = r < 3 ? cap.vs[16 + r][c] : (c == 3 ? 1.0 : 0.0);
    }
  if (!Invert(wvp, wvp_inv)) return;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += w[r][k] * wvp_inv[k][c];
      inv_vp[r][c] = sum;
    }
  // The eye: clip (0, 0, z, 0) maps back to it.
  double eye[3] = {inv_vp[0][2] / inv_vp[3][2], inv_vp[1][2] / inv_vp[3][2],
                   inv_vp[2][2] / inv_vp[3][2]};
  for (int i = 0; i < 3; ++i) flash_eye[i] = float(eye[i]);
  have_flash_eye = true;
  // For the ray-traced draws without a world matrix: world from clip x, y
  // and w (rows 0, 1, 3 of the view-projection; clip z is left out, as
  // vehicle interiors are drawn with another near plane).
  double world_from_clip[4][4], draw_vp[4][4], xyw[4][4];
  if (!Invert(inv_vp, draw_vp)) return;
  for (int c = 0; c < 4; ++c) {
    xyw[0][c] = draw_vp[0][c];
    xyw[1][c] = draw_vp[1][c];
    xyw[2][c] = draw_vp[3][c];
    xyw[3][c] = c == 3 ? 1.0 : 0.0;
  }
  if (!Invert(xyw, world_from_clip)) return;
  // Depth buffer value -> NDC z through the viewport's z transform: fold it
  // into the matrix's z column.
  double zs = cap.viewport_z_scale ? cap.viewport_z_scale : 1.0;
  double zo = cap.viewport_z_offset;
  // A 20e4 float depth target's [0, 2) is stored halved in the host depth
  // buffer (draw_util GetHostViewportInfo, full_float24_in_0_to_1).
  double stored = depth_key.GetDepthFormat() ==
                          xenos::DepthRenderTargetFormat::kD24FS8
                      ? 2.0
                      : 1.0;
  for (int r = 0; r < 4; ++r) {
    inv_vp[r][3] -= inv_vp[r][2] * zo / zs;
    inv_vp[r][2] *= stored / zs;
  }

  double vp[4][4];
  if (!Invert(inv_vp, vp)) return;

  const Settings& s = GetSettings();
  Params p = {};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      p.view_proj[c * 4 + r] = float(vp[r][c]);
  p.ss[0] = s.ss_strength;
  p.ss[1] = s.ss_length;
  p.ss[2] = float(s.ss_steps);
  p.ss[3] = s.ss_thickness;
  p.rt[0] = vk.rt ? 1.0f : 0.0f;
  p.rt[1] = s.rt_distance;
  p.rt[2] = float(vk.rt_instances);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      p.inv_view_proj[c * 4 + r] = float(r < 3 ? inv_vp[r][c] - eye[r] * inv_vp[3][c] : inv_vp[r][c]);
  // Ray-traced light and reflections: last frame's camera for the light kept
  // over frames (none after a jump, a level load or a resize).
  bool traced_light = vk.rt && !save_pass && vk.rt_instances > 0;
  if (traced_light) {
    std::memcpy(p.prev_view_proj, vk.have_prev ? vk.prev_view_proj : p.view_proj,
                sizeof(p.prev_view_proj));
    float jump = std::sqrt(float((eye[0] - vk.prev_camera[0]) * (eye[0] - vk.prev_camera[0]) +
                                 (eye[1] - vk.prev_camera[1]) * (eye[1] - vk.prev_camera[1]) +
                                 (eye[2] - vk.prev_camera[2]) * (eye[2] - vk.prev_camera[2])));
    std::memcpy(p.prev_camera, vk.prev_camera, 12);
    p.prev_camera[3] = vk.have_prev && vk.light_initialized && jump < 20.0f ? 1.0f : 0.0f;
    p.gi[0] = s.gi;
    p.gi[1] = float(s.gi_rays);
    p.gi[2] = s.rt_cell;
    p.gi[3] = float(vk.frame);
    p.gi2[0] = s.gi_distance;
    p.gi2[1] = s.gi_frames;
    static const auto start = std::chrono::steady_clock::now();
    p.refl[0] = s.reflect;
    // Other surfaces only with their materials.
    p.refl[1] = command_processor_.bf_material_buffer() ? s.gloss : 0.0f;
    p.refl[2] = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    p.refl[3] = s.waves;
    std::memcpy(vk.prev_view_proj, p.view_proj, sizeof(vk.prev_view_proj));
    for (int i = 0; i < 3; ++i) vk.prev_camera[i] = float(eye[i]);
    vk.have_prev = true;
  }
  // The slice rows (c0-c10) take object positions in some shaders: shadow =
  // S * W, so world -> shadow is S * W^-1; in others the world position.
  double w_inv[4][4];
  if (!Invert(w, w_inv)) return;
  if (cap.slices_world) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) w_inv[r][c] = r == c ? 1.0 : 0.0;
  }
  for (int i = 0; i < 3; ++i)
    for (int row = 0; row < 3; ++row)
      for (int c = 0; c < 4; ++c) {
        double sum = 0.0;
        for (int k = 0; k < 4; ++k) sum += cap.vs[i * 4 + row][k] * w_inv[k][c];
        p.slices[i * 3 + row][c] = float(sum);
      }
  p.camera[0] = float(eye[0]);
  p.camera[1] = float(eye[1]);
  p.camera[2] = float(eye[2]);
  p.camera[3] = cap.ps[0][0] * 0.5f;  // the lit shaders' exposure
  std::memcpy(p.sun_dir, cap.ps[1], 12);
  p.sun_dir[3] = s.density;
  std::memcpy(p.sun_color, cap.ps[5], 12);
  p.sun_color[3] = s.falloff;
  std::memcpy(p.ambient, cap.ps[3], 12);
  p.ambient[3] = s.base_height;
  p.screen[0] = float(origin_x * scale_x);
  p.screen[1] = float(origin_y * scale_y);
  p.screen[2] = 1.0f / float(width);
  p.screen[3] = 1.0f / float(height);
  p.march[0] = s.max_distance;
  p.march[1] = float(s.steps);
  p.march[3] = float(vk.frame & 63);
  p.strength[0] = s.shafts;
  p.strength[1] = s.ss_map;
  p.strength[2] = s.shadow_bias;
  p.strength[3] = float(zo + zs);  // depth of the far plane
  p.gi2[2] = s.shadows;
  p.gi2[3] = s.gi_contact;
  p.shadow_info[0] = slice_size;
  p.shadow_info[1] = uint32_t(slice_bytes / 4);
  p.shadow_info[2] = tf.endianness == xenos::Endian::k8in32 ? 1 : 0;
  p.shadow_info[3] = s.debug;
  {
    static const auto cloud_start = std::chrono::steady_clock::now();
    p.cloud[0] = s.clouds;
    p.cloud[1] = s.cloud_base;
    p.cloud[2] = s.cloud_thickness;
    p.cloud[3] = std::fmod(
        std::chrono::duration<float>(std::chrono::steady_clock::now() - cloud_start).count(),
        20000.0f);
    p.cloud2[0] = float(s.cloud_steps);
    p.cloud2[1] = s.cloud_density;
    p.cloud2[2] = s.cloud_wind;
    p.cloud2[3] = s.cloud_shadows;
  }
  p.ao[0] = s.ao;
  p.ao[1] = s.ao_radius;
  p.ao[2] = s.ao_direct;
  p.ao[3] = s.ao_distance;
  p.wfx[0] = vk.rt ? s.water_foam : 0.0f;
  p.wfx[1] = s.water_refract;
  p.wfx[2] = s.water_shallow;
  p.wfx[3] = s.foam_width;
  p.detail[0] = s.detail;
  p.detail[1] = s.detail_distance;
  p.detail[2] = s.pebble_size;
  p.detail[3] = s.crack_size;
  if (!save_pass && flash.seen && s.flash > 0.0f) {
    std::memcpy(p.flash, flash.pos, sizeof(flash.pos));
    p.flash[3] = s.flash;
    std::memcpy(p.flash2, flash.color, sizeof(flash.color));
    p.flash2[3] = s.flash_radius;
  }
  p.water[0] = s.eye_height;
  p.water[1] = zs < 0.0 ? 1.0f : 0.0f;
  p.water[2] = s.sky;
  if (!save_pass && bflook::gore) {
    p.gore_info[0] = float(bflook::BuildGore(p.gore, kGoreMax));
    p.gore_info[1] = 0.35f;  // brightness of the blood in the air
  }
  std::memcpy(vk.uniforms_mapping + slot * kSlotSize, &p, sizeof(p));
  ui::vulkan::util::FlushMappedMemoryRange(
      device, vk.uniforms_memory, vk.uniforms_memory_type, slot * kSlotSize,
      vk.uniforms_memory_size, kSlotSize);

  VkDescriptorImageInfo color_info = {VK_NULL_HANDLE, color.view_depth_color(),
                                      VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo depth_info = {
      VK_NULL_HANDLE, depth.view_depth_color(),
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
  VkDescriptorBufferInfo shadow_info = {shared_memory.buffer(), shadow_base,
                                        slice_bytes * 3};
  VkDescriptorBufferInfo uniform_info = {vk.uniforms, slot * kSlotSize,
                                         sizeof(Params)};
  VkDescriptorImageInfo saved_info = {VK_NULL_HANDLE, vk.saved_depth_view,
                                      VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo horizon_info = {vk.horizon, 0, kHorizonSize};
  VkAccelerationStructureKHR tlas = vk.rt ? rt::Tlas() : VK_NULL_HANDLE;
  VkWriteDescriptorSetAccelerationStructureKHR tlas_info = {
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  tlas_info.accelerationStructureCount = 1;
  tlas_info.pAccelerationStructures = &tlas;
  VkDescriptorBufferInfo normals_info = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
  if (vk.rt) normals_info.buffer = rt::Normals(&normals_info.range);
  VkDescriptorBufferInfo cache_info = {vk.cache, 0, kCacheSize};
  // Without materials the cache stands in (and gloss is 0).
  VkBuffer materials = command_processor_.bf_material_buffer();
  const VkDeviceSize materials_size =
      VkDeviceSize(pbr::kMaterialPitch) * pbr::kMaterialRows * 4;
  VkDescriptorBufferInfo material_info = {materials ? materials : vk.cache, 0,
                                          materials ? materials_size : kCacheSize};
  // This frame writes one of the two light images and reads the other.
  uint32_t write_light = vk.light_parity, read_light = vk.light_parity ^ 1;
  VkDescriptorImageInfo light_infos[4] = {
      {VK_NULL_HANDLE, vk.light_view[read_light], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, vk.light_view[write_light], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, vk.light_dist_view[read_light], VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, vk.light_dist_view[write_light], VK_IMAGE_LAYOUT_GENERAL}};
  uint32_t list[kBindings];
  const uint32_t write_count = BindingList(list);
  VkDescriptorImageInfo clouds_info = {VK_NULL_HANDLE, vk.clouds_view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo ao_info = {VK_NULL_HANDLE, vk.ao_view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo refract_info = {VK_NULL_HANDLE, vk.refract_view,
                                        VK_IMAGE_LAYOUT_GENERAL};
  // Indexed by binding, then packed in the variant's order.
  VkWriteDescriptorSet writes[kBindings] = {};
  for (uint32_t i = 0; i < kBindings; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = vk.sets[slot];
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = kBindingTypes[i];
  }
  writes[kCloudBinding].pImageInfo = &clouds_info;
  writes[kAoBinding].pImageInfo = &ao_info;
  writes[kRefractBinding].pImageInfo = &refract_info;
  writes[7].pBufferInfo = &normals_info;
  writes[8].pBufferInfo = &cache_info;
  for (int i = 0; i < 4; ++i) writes[9 + i].pImageInfo = &light_infos[i];
  writes[13].pBufferInfo = &material_info;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[0].pImageInfo = &color_info;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  writes[1].pImageInfo = &depth_info;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &shadow_info;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  writes[3].pBufferInfo = &uniform_info;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[4].pImageInfo = &saved_info;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[5].pBufferInfo = &horizon_info;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  writes[6].pNext = &tlas_info;
  VkWriteDescriptorSet packed[kBindings];
  for (uint32_t i = 0; i < write_count; ++i) packed[i] = writes[list[i]];
  dfn.vkUpdateDescriptorSets(device->device(), write_count, packed, 0, nullptr);

  // Barriers: the shadow map was written by resolves, the targets by draws,
  // the saved depth by the previous pass.
  command_processor_.EndRenderPass();
  if (!save_pass) shared_memory.Use(VulkanSharedMemory::Usage::kRead);
  const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  const VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  command_processor_.PushImageMemoryBarrier(
      color.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
      color.current_stage_mask(), stage, color.current_access_mask(), rw,
      color.current_layout(), VK_IMAGE_LAYOUT_GENERAL);
  color.SetUsage(stage, rw, VK_IMAGE_LAYOUT_GENERAL);
  command_processor_.PushImageMemoryBarrier(
      depth.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                   VK_IMAGE_ASPECT_STENCIL_BIT),
      depth.current_stage_mask(), stage, depth.current_access_mask(),
      VK_ACCESS_SHADER_READ_BIT, depth.current_layout(),
      VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, true,
      GetSampleLocationsInfo(depth_key.msaa_samples, true));
  depth.SetUsage(stage, VK_ACCESS_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
  command_processor_.PushImageMemoryBarrier(
      vk.saved_depth, ui::vulkan::util::InitializeSubresourceRange(),
      vk.saved_initialized ? stage : 0, stage,
      vk.saved_initialized ? rw : 0, rw,
      vk.saved_initialized ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
      VK_QUEUE_FAMILY_IGNORED, false);
  vk.saved_initialized = true;
  command_processor_.PushImageMemoryBarrier(
      vk.clouds, ui::vulkan::util::InitializeSubresourceRange(),
      vk.clouds_initialized ? stage : 0, stage, vk.clouds_initialized ? rw : 0, rw,
      vk.clouds_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  vk.clouds_initialized = true;
  command_processor_.PushImageMemoryBarrier(
      vk.ao, ui::vulkan::util::InitializeSubresourceRange(),
      vk.ao_initialized ? stage : 0, stage, vk.ao_initialized ? rw : 0, rw,
      vk.ao_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  vk.ao_initialized = true;
  command_processor_.PushImageMemoryBarrier(
      vk.refract, ui::vulkan::util::InitializeSubresourceRange(),
      vk.refract_initialized ? stage : 0, stage, vk.refract_initialized ? rw : 0, rw,
      vk.refract_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  vk.refract_initialized = true;
  if (vk.rt) {
    // The light images stay in the general layout (undefined the first time:
    // its history is not used then, prev_camera.w is 0).
    for (int i = 0; i < 2; ++i) {
      for (VkImage image : {vk.light[i], vk.light_dist[i]}) {
        command_processor_.PushImageMemoryBarrier(
            image, ui::vulkan::util::InitializeSubresourceRange(),
            vk.light_initialized ? stage : 0, stage, vk.light_initialized ? rw : 0,
            rw,
            vk.light_initialized ? VK_IMAGE_LAYOUT_GENERAL
                                 : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, false);
      }
    }
  }
  DeferredCommandBuffer& cb = command_processor_.deferred_command_buffer();
  if (!vk.horizon_initialized) {
    vk.horizon_initialized = true;
    command_processor_.SubmitBarriers(true);
    cb.CmdVkFillBuffer(vk.horizon, 0, kHorizonSize, 0);
    command_processor_.PushBufferMemoryBarrier(
        vk.horizon, 0, kHorizonSize, VK_PIPELINE_STAGE_TRANSFER_BIT, stage,
        VK_ACCESS_TRANSFER_WRITE_BIT, rw);
  }
  if (vk.rt && !vk.cache_initialized) {
    vk.cache_initialized = true;
    command_processor_.SubmitBarriers(true);
    cb.CmdVkFillBuffer(vk.cache, 0, kCacheSize, 0);
    command_processor_.PushBufferMemoryBarrier(
        vk.cache, 0, kCacheSize, VK_PIPELINE_STAGE_TRANSFER_BIT, stage,
        VK_ACCESS_TRANSFER_WRITE_BIT, rw);
  }
  if (vk.rt && materials && !save_pass) {
    if (!vk.materials_initialized) {
      vk.materials_initialized = true;
      command_processor_.SubmitBarriers(true);
      cb.CmdVkFillBuffer(materials, 0, materials_size, UINT32_MAX);
      command_processor_.PushBufferMemoryBarrier(
          materials, 0, materials_size, VK_PIPELINE_STAGE_TRANSFER_BIT, stage,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    } else {
      // The draws' atomics.
      command_processor_.PushBufferMemoryBarrier(
          materials, 0, materials_size, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          stage, rw, VK_ACCESS_SHADER_READ_BIT);
    }
  }
  command_processor_.SubmitBarriers(true);
  // The world's acceleration structures, for this frame's fog pass.
  if (save_pass && vk.rt) {
    vk.rt_instances = rt::BuildFrame(command_processor_, cap.depth_target, vp, world_from_clip);
  }

  command_processor_.BindExternalComputePipeline(vk.pipeline);
  cb.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, vk.layout, 0, 1,
                             &vk.sets[slot], 0, nullptr);
  auto dispatch = [&](uint32_t pass, uint32_t groups_x, uint32_t groups_y) {
    cb.CmdVkPushConstants(vk.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                          sizeof(pass), &pass);
    cb.CmdVkDispatch(groups_x, groups_y, 1);
  };
  if (save_pass) {
    dispatch(0, (width + 7) / 8, (height + 7) / 8);
  } else {
    // Measure the horizon colour (each screen column in its own workgroup,
    // then the azimuth bins), then fog towards it.
    VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, rw,
                               rw};
    dispatch(3, 64, 1);
    cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                            nullptr);
    dispatch(2, 1, 1);
    cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                            nullptr);
    if (traced_light) {
      // The ray-traced light at half resolution, then everything else.
      dispatch(4, ((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8);
      cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                              nullptr);
    }
    if (s.clouds > 0.0f) {
      // The clouds at half resolution.
      dispatch(5, ((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8);
      cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                              nullptr);
    }
    if (s.ao > 0.0f) {
      // The ambient occlusion at half resolution.
      dispatch(6, ((width + 1) / 2 + 7) / 8, ((height + 1) / 2 + 7) / 8);
      cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                              nullptr);
    }
    if (s.water_refract > 0.0f) {
      // What is seen through the sea, bent, into a copy pass 1 reads.
      dispatch(7, (width + 7) / 8, (height + 7) / 8);
      cb.CmdVkPipelineBarrier(stage, stage, 0, 1, &barrier, 0, nullptr, 0,
                              nullptr);
    }
    dispatch(1, (width + 7) / 8, (height + 7) / 8);
    if (vk.rt && materials) {
      // Cleared for the next frame's draws.
      command_processor_.PushBufferMemoryBarrier(
          materials, 0, materials_size, stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      command_processor_.SubmitBarriers(true);
      cb.CmdVkFillBuffer(materials, 0, materials_size, UINT32_MAX);
      command_processor_.PushBufferMemoryBarrier(
          materials, 0, materials_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, rw);
    }
  }

  if (save_pass) {
    pending.valid = true;
    pending.cap = cap;
    after_depth_copy = true;
  } else {
    after_depth_copy = false;
    flash.seen = false;
    ++vk.frame;
    if (traced_light) {
      vk.light_initialized = true;
      vk.light_parity ^= 1;
    }
  }
  static int eye_logs = 0;
  if (getenv("BF_SCENEFX_LOG") && !save_pass && (vk.frame % 120) == 0 &&
      eye_logs++ < 20) {
    XELOGI("scenefx: eye {:.2f} {:.2f} {:.2f}, sun {:.3f} {:.3f} {:.3f}, ps c7 {:.2f} {:.2f} {:.2f} (ps {:016X})",
           eye[0], eye[1], eye[2], cap.ps[1][0], cap.ps[1][1], cap.ps[1][2],
           cap.ps[7][0], cap.ps[7][1], cap.ps[7][2], cap.ps_hash);
  }
  static bool logged[2] = {};
  if (!logged[save_pass]) {
    logged[save_pass] = true;
    XELOGI(
        "scenefx: first {} pass {}x{} at {},{} (scale {}x{}), shadow {:08X} "
        "{}x{}, viewport z {} {}",
        save_pass ? "depth" : "fog", width, height, origin_x, origin_y,
        scale_x, scale_y, uint32_t(shadow_base), slice_size, slice_size, zs,
        zo);
  }
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
