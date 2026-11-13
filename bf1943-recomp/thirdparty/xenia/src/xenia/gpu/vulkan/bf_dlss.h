// bf1943: NVIDIA DLSS super resolution (thirdparty/dlss, the DLSS SDK 310;
// nvngx_dlss.dll next to the exe). The same frame path as FSR 3 (bf_fsr3.h):
// the pre-HUD frame, the scene's depth and motion vectors, upscaled to the
// window's size, the HUD back on top; only the upscale itself differs.

#ifndef XENIA_GPU_VULKAN_BF_DLSS_H_
#define XENIA_GPU_VULKAN_BF_DLSS_H_

#include <atomic>
#include <cstdint>

#include "xenia/gpu/vulkan/bf_fsr3.h"

namespace xe {
namespace gpu {
namespace bfdlss {

// The UPSCALING setting is DLSS (BF_DLSS=1): set before the GPU starts, so the
// Vulkan device is created with the extensions NGX asks for.
void Prepare();

// NGX started on this device and DLSS supported (tried once).
bool Available(const ui::vulkan::VulkanDevice* device);

// Records the upscale into cb (the feature is made, or remade when the sizes
// change, on the way). False if DLSS is not available.
bool Evaluate(const ui::vulkan::VulkanDevice* device, VkCommandBuffer cb, const bffsr::Frame& frame);

// The jitter for frame index n (Halton 2/3, 8 x the scale squared phases), in
// render pixels, each -0.5...0.5.
void Jitter(uint32_t n, uint32_t render_width, uint32_t display_width, float& x, float& y);

void Shutdown(const ui::vulkan::VulkanDevice* device);

}  // namespace bfdlss
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_DLSS_H_
