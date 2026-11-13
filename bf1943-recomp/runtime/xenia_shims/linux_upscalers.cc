// Linux: FSR 3 and DLSS come as Windows libraries only (bf_fsr3.cc loads
// amd_fidelityfx_loader_vk.dll, bf_dlss.cc NVIDIA's NGX), so here they are
// not available: the presenter's FSR 1 / CAS / bilinear output is used.
// (Only with them in the tree: nothing calls these otherwise.)
#if !defined(_WIN32) && __has_include("xenia/gpu/vulkan/bf_dlss.h")
#include "xenia/gpu/vulkan/bf_dlss.h"
#include "xenia/gpu/vulkan/bf_fsr3.h"

namespace xe::gpu {
namespace bffsr {
bool Available() { return false; }
VkImageView Record(vulkan::VulkanCommandProcessor&, VkImageView final_view, uint32_t&, uint32_t&) {
  return final_view;
}
bool Dispatch(const ui::vulkan::VulkanDevice*, VkCommandBuffer, const Frame&) { return false; }
void Jitter(uint32_t, uint32_t, uint32_t, float& x, float& y) { x = y = 0.0f; }
void Shutdown() {}
}  // namespace bffsr
namespace bfdlss {
void Prepare() {}
bool Available(const ui::vulkan::VulkanDevice*) { return false; }
bool Evaluate(const ui::vulkan::VulkanDevice*, VkCommandBuffer, const bffsr::Frame&) { return false; }
void Jitter(uint32_t, uint32_t, uint32_t, float& x, float& y) { x = y = 0.0f; }
void Shutdown(const ui::vulkan::VulkanDevice*) {}
}  // namespace bfdlss
}  // namespace xe::gpu
#endif  // !_WIN32 && bf_dlss.h
