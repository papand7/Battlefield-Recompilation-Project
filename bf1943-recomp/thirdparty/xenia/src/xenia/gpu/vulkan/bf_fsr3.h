// bf1943: AMD FSR 3 temporal upscaling (thirdparty/fidelityfx, the FSR SDK
// 2.4 API; amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll
// next to the exe, loaded at run time, so the game runs without them).
//
// The game draws its scene, bloom, tone mapping and HUD at one resolution, so
// the frame is upscaled at the end: the image right after the game's tone
// mapping pass (before the HUD), with the scene's depth and motion vectors
// (scene_fx), to the window's size; the HUD goes back on top unscaled-blurry
// (what the final frame has over the pre-HUD one).

#ifndef XENIA_GPU_VULKAN_BF_FSR3_H_
#define XENIA_GPU_VULKAN_BF_FSR3_H_

#include <atomic>
#include <cstdint>

#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}  // namespace vulkan
}  // namespace ui
namespace gpu {
namespace vulkan {
class VulkanCommandProcessor;
}  // namespace vulkan
namespace bffsr {

// The FSR DLLs found and loaded (once).
bool Available();

// On (the UPSCALING setting; BF_FSR3=1): the 3D draws of a frame are moved by
// the jitter (render pixels, set at each swap) until the game's tone mapping
// pass; past_tonemap is set at that pass and cleared at the swap.
inline std::atomic<bool> enabled{false};
// The upscaler: false FSR 3, true DLSS (bf_dlss.h; FSR 3 if DLSS cannot start).
inline std::atomic<bool> dlss{false};
inline float jitter_x = 0.0f, jitter_y = 0.0f;
inline bool past_tonemap = false;
// The last swap was upscaled (DLSS or FSR 3): the scene effects then take
// fewer samples with noise that changes every frame, which the upscaler
// averages over its jittered frames.
inline std::atomic<bool> active{false};
// The window's client size (the upscale's output), from the runtime.
inline std::atomic<uint32_t> display_width{0}, display_height{0};
inline std::atomic<float> sharpness{0.3f};

// At the swap: the whole upscale recorded into the command processor's
// commands (the pre-HUD frame unpacked, FSR 3, the HUD back on top), from the
// front buffer final_view (render size, shader-read-only). Returns the
// display-size result (shader-read-only), or VK_NULL_HANDLE (not this frame:
// the swap goes on as without FSR). Also sets the next frame's jitter.
// The result's size goes to width and height.
VkImageView Record(vulkan::VulkanCommandProcessor& cp, VkImageView final_view,
                   uint32_t& width, uint32_t& height);

// One frame's inputs, all images in VK_IMAGE_LAYOUT_GENERAL (render size
// unless said otherwise).
struct Frame {
  VkImage color = VK_NULL_HANDLE;   // the pre-HUD frame
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkImage depth = VK_NULL_HANDLE;   // R32_SFLOAT
  VkImage motion = VK_NULL_HANDLE;  // R16G16_SFLOAT, in pixels
  VkImage output = VK_NULL_HANDLE;  // display size, storage-capable
  VkFormat output_format = VK_FORMAT_UNDEFINED;
  // Their views (DLSS).
  VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE;
  VkImageView motion_view = VK_NULL_HANDLE, output_view = VK_NULL_HANDLE;
  bool use_dlss = false;
  uint32_t render_width = 0, render_height = 0;
  uint32_t display_width = 0, display_height = 0;
  float jitter_x = 0.0f, jitter_y = 0.0f;  // pixels
  float frame_ms = 16.7f;
  float near_plane = 0.1f, far_plane = 10000.0f, fov_y = 1.0f;
  bool reset = false;
  float sharpness = 0.3f;
};

// Records the upscale into cb. False if FSR is not available or the context
// could not be made (it is remade when the sizes change).
bool Dispatch(const ui::vulkan::VulkanDevice* device, VkCommandBuffer cb, const Frame& frame);

// The jitter for frame index n (Halton 2/3, the phase count FSR asks for at
// these sizes), in render pixels, each -0.5...0.5.
void Jitter(uint32_t n, uint32_t render_width, uint32_t display_width, float& x, float& y);

void Shutdown();

}  // namespace bffsr
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_FSR3_H_
