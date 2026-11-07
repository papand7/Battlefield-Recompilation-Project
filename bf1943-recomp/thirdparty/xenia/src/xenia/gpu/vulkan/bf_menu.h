// bf1943: the front end's menu rows as drawn, for mouse hover (runtime
// xam.cpp). The game's menus (EA APT movies) only take pad input; their solid
// 2D boxes (vertex shaders that transform FMT_32_32_FLOAT positions by c0-c3,
// colour in c8) are collected each frame, and the input code steps the
// highlight (D-pad) to the row under the cursor.

#ifndef XENIA_GPU_VULKAN_BF_MENU_H_
#define XENIA_GPU_VULKAN_BF_MENU_H_

#include <cstdint>
#include <mutex>
#include <vector>

namespace xe {
namespace gpu {
namespace bfmenu {

struct Box {
  float x0, y0, x1, y1;  // 0-1 of the frame, y down
  float r, g, b, a;      // the draw's colour (c8)
};

// The Back prompt's key icon: its cell in a UI atlas (registered by the
// runtime when it redraws the atlas, ui pc_ui.cpp), so the prompt can be
// clicked.
struct Icon {
  uint32_t address, width, height;
  float u0, v0, u1, v1;  // texels
};

inline std::mutex g_mutex;
inline std::vector<Box> g_building, g_done;
inline std::vector<Icon> g_back_icons;
inline std::vector<Box> g_back_building, g_back_done;
inline void AddBackIcon(const Icon& icon) {
  std::lock_guard lock(g_mutex);
  g_back_icons.push_back(icon);
}
// A drawn Back prompt icon: its screen box, the prompt's label to its right
// included.
inline void AddBack(const Box& box) {
  std::lock_guard lock(g_mutex);
  if (g_back_building.size() < 16) g_back_building.push_back(box);
}
inline std::vector<Box> BackBoxes() {
  std::lock_guard lock(g_mutex);
  return g_back_done;
}
// Filled by the command processor during a frame, published at the swap.
inline void AddBox(const Box& box) {
  std::lock_guard lock(g_mutex);
  if (g_building.size() < 512) g_building.push_back(box);
}
inline void EndFrame() {
  std::lock_guard lock(g_mutex);
  g_done.swap(g_building);
  g_building.clear();
  g_back_done.swap(g_back_building);
  g_back_building.clear();
}
// The last finished frame's boxes.
inline std::vector<Box> Boxes() {
  std::lock_guard lock(g_mutex);
  return g_done;
}

}  // namespace bfmenu
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_MENU_H_
