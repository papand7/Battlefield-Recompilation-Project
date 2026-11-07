// Win32 surface + swapchain. Recreated on resize / VK_ERROR_OUT_OF_DATE_KHR.
#pragma once

#include "vk_device.h"

#include <vector>

namespace bf::vk {

class Swapchain {
 public:
  // `hwnd` / `hinstance` are HWND / HINSTANCE (kept opaque to avoid windows.h here).
  bool Init(Device* dev, void* hinstance, void* hwnd, bool vsync);
  void Shutdown();

  // Acquires the next image. Returns false if the swapchain must be (and was)
  // recreated or the window is minimised; the caller should skip the frame.
  bool Acquire(uint32_t frame_slot, uint32_t* image_index);
  // Presents `image_index`; the caller's submit must signal render_done(image_index).
  bool Present(uint32_t image_index);

  VkSemaphore acquire_semaphore(uint32_t frame_slot) const { return acquire_[frame_slot]; }
  VkSemaphore render_done(uint32_t image_index) const { return render_done_[image_index]; }
  VkImage image(uint32_t i) const { return images_[i]; }
  VkImageView view(uint32_t i) const { return views_[i]; }
  VkFormat format() const { return format_; }
  VkExtent2D extent() const { return extent_; }

 private:
  bool Create();
  void DestroySwapchainObjects();

  Device* dev_ = nullptr;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  bool vsync_ = true;
  std::vector<VkImage> images_;
  std::vector<VkImageView> views_;
  std::vector<VkSemaphore> acquire_;      // per frame slot
  std::vector<VkSemaphore> render_done_;  // per swapchain image (safe reuse on present)
};

}  // namespace bf::vk
