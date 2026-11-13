// bf1943 looks for the final frame (shaders/bf_look.xesli), applied in the
// swap's gamma pass: set by the runtime (--look; --blood is bf_gore.h).

#ifndef XENIA_GPU_VULKAN_BF_LOOK_H_
#define XENIA_GPU_VULKAN_BF_LOOK_H_

#include <atomic>

namespace xe {
namespace gpu {
namespace bflook {

// 0 the game's own, 1 toon, 2 noir, 3 ww2, 4 vivid.
inline std::atomic<int> look{0};
// The picture's BRIGHTNESS (-0.3...0.3, 0 the game's) and GAMMA (0.7...1.3,
// 1 the game's; above 1 brighter midtones), on the final frame.
inline std::atomic<float> brightness{0.0f}, gamma{1.0f};

}  // namespace bflook
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_LOOK_H_
