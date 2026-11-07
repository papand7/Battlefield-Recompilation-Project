// bf1943 HDR output: an HDR10 (BT.2020, PQ) swapchain where the display has
// one (VulkanPresenter), and the swap's gamma pass (shaders/bf_look_apply.xesli)
// turning the game's SDR frame into it. Set by the runtime (--hdr).

#ifndef XENIA_UI_VULKAN_BF_HDR_H_
#define XENIA_UI_VULKAN_BF_HDR_H_

#include <atomic>

namespace xe {
namespace ui {
namespace bfhdr {

// Asked for (before the window opens).
inline std::atomic<bool> requested{false};
// The current swapchain is HDR10: the gamma pass encodes PQ only then.
inline std::atomic<bool> active{false};
// Nits of the game's white (menus, HUD, lit surfaces) and of the brightest
// highlights the frame's top end is stretched to.
inline std::atomic<float> paper_white{200.0f};
inline std::atomic<float> peak{1000.0f};
// No stretch (everything at most the game's white): the front end, whose
// near-white clouds and text otherwise went to the peak and glared.
inline std::atomic<bool> no_stretch{true};

}  // namespace bfhdr
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_BF_HDR_H_
