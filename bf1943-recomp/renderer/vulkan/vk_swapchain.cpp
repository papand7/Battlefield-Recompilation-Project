#include "vk_swapchain.h"

#include "vk_frame.h"
#include "vk_log.h"

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

namespace bf::vk {

bool Swapchain::Init(Device* dev, void* hinstance, void* hwnd, bool vsync) {
  dev_ = dev;
  vsync_ = vsync;
#ifdef _WIN32
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = static_cast<HINSTANCE>(hinstance);
  sci.hwnd = static_cast<HWND>(hwnd);
  BF_VK_CHECK(vkCreateWin32SurfaceKHR(dev_->instance(), &sci, nullptr, &surface_));
#else
  (void)hinstance, (void)hwnd;
  BF_VK_ERROR("only Win32 surfaces are implemented");
  return false;
#endif
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(dev_->physical(), dev_->queue_family(), surface_,
                                       &supported);
  if (!supported) {
    BF_VK_ERROR("graphics queue family cannot present to this surface");
    return false;
  }
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  acquire_.resize(FrameRing::kFramesInFlight);
  for (auto& s : acquire_) BF_VK_CHECK(vkCreateSemaphore(dev_->device(), &semi, nullptr, &s));
  return Create();
}

bool Swapchain::Create() {
  VkSurfaceCapabilitiesKHR caps;
  BF_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev_->physical(), surface_, &caps));
  if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;  // minimised
  extent_ = caps.currentExtent;

  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev_->physical(), surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(dev_->physical(), surface_, &n, formats.data());
  // Xenos output is gamma-encoded already (PWL gamma applied by the game /
  // our resolve), so present through a UNORM, not SRGB, format.
  VkSurfaceFormatKHR fmt = formats[0];
  for (const auto& f : formats)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      fmt = f;
      break;
    }
  format_ = fmt.format;

  vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical(), surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(dev_->physical(), surface_, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync_) {
    for (auto m : modes)
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    if (mode == VK_PRESENT_MODE_FIFO_KHR)
      for (auto m : modes)
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
  }

  uint32_t count = std::max(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

  VkSwapchainKHR old = swapchain_;
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface_;
  ci.minImageCount = count;
  ci.imageFormat = fmt.format;
  ci.imageColorSpace = fmt.colorSpace;
  ci.imageExtent = extent_;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = old;
  BF_VK_CHECK(vkCreateSwapchainKHR(dev_->device(), &ci, nullptr, &swapchain_));
  if (old) {
    DestroySwapchainObjects();
    vkDestroySwapchainKHR(dev_->device(), old, nullptr);
  }

  vkGetSwapchainImagesKHR(dev_->device(), swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(dev_->device(), swapchain_, &n, images_.data());
  views_.resize(n);
  render_done_.resize(n);
  VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images_[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    BF_VK_CHECK(vkCreateImageView(dev_->device(), &vi, nullptr, &views_[i]));
    BF_VK_CHECK(vkCreateSemaphore(dev_->device(), &semi, nullptr, &render_done_[i]));
  }
  BF_VK_INFO("swapchain %ux%u, %u images, present mode %d", extent_.width, extent_.height, n,
             int(mode));
  return true;
}

void Swapchain::DestroySwapchainObjects() {
  for (auto v : views_) vkDestroyImageView(dev_->device(), v, nullptr);
  for (auto s : render_done_) vkDestroySemaphore(dev_->device(), s, nullptr);
  views_.clear();
  render_done_.clear();
  images_.clear();
}

bool Swapchain::Acquire(uint32_t frame_slot, uint32_t* image_index) {
  if (!swapchain_) {
    vkDeviceWaitIdle(dev_->device());
    Create();  // recreated (or still minimised); skip this frame either way
    return false;
  }
  VkResult r = vkAcquireNextImageKHR(dev_->device(), swapchain_, UINT64_MAX,
                                     acquire_[frame_slot], VK_NULL_HANDLE, image_index);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    vkDeviceWaitIdle(dev_->device());
    Create();
    return false;
  }
  return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
}

bool Swapchain::Present(uint32_t image_index) {
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &render_done_[image_index];
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain_;
  pi.pImageIndices = &image_index;
  VkResult r = vkQueuePresentKHR(dev_->queue(), &pi);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    vkDeviceWaitIdle(dev_->device());
    Create();
    return true;
  }
  return r == VK_SUCCESS;
}

void Swapchain::Shutdown() {
  if (!dev_ || !dev_->device()) return;
  vkDeviceWaitIdle(dev_->device());
  DestroySwapchainObjects();
  for (auto s : acquire_) vkDestroySemaphore(dev_->device(), s, nullptr);
  acquire_.clear();
  if (swapchain_) vkDestroySwapchainKHR(dev_->device(), swapchain_, nullptr);
  if (surface_) vkDestroySurfaceKHR(dev_->instance(), surface_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  surface_ = VK_NULL_HANDLE;
}

}  // namespace bf::vk
