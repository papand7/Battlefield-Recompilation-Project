// Swapchain smoke test: opens a Win32 window, presents N frames with an
// animated clear (via vkCmdClearColorImage on the swapchain image), exits.
// Passes if every frame presented and validation reported no errors.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "vk_device.h"
#include "vk_frame.h"
#include "vk_log.h"
#include "vk_swapchain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace bf::vk;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
  int frames_to_run = 120;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames_to_run = std::atoi(argv[++i]);

  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"bf1943_vk";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"BF1943 Vulkan swapchain test",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1280,
                            720, nullptr, nullptr, inst, nullptr);

  DeviceOptions opts;
  opts.validation = true;
  Device dev;
  FrameRing ring;
  Swapchain sc;
  if (!dev.Init(opts) || !ring.Init(&dev) || !sc.Init(&dev, inst, hwnd, true)) return 2;

  int presented = 0;
  bool quit = false;
  for (int f = 0; f < frames_to_run * 4 && presented < frames_to_run && !quit; ++f) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) quit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    VkCommandBuffer cmd = ring.Begin();
    uint32_t idx;
    if (!sc.Acquire(ring.slot(), &idx)) {
      // Keep the ring balanced: submit the (empty) command buffer.
      ring.Submit(VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE);
      continue;
    }
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = sc.image(idx);
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    float t = presented / 60.0f;
    VkClearColorValue c{{0.5f + 0.5f * std::sin(t), 0.3f, 0.5f + 0.5f * std::cos(t), 1.0f}};
    vkCmdClearColorImage(cmd, sc.image(idx), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1,
                         &b.subresourceRange);
    b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(cmd, &dep);
    if (!ring.Submit(sc.acquire_semaphore(ring.slot()), VK_PIPELINE_STAGE_2_CLEAR_BIT,
                     sc.render_done(idx)) ||
        !sc.Present(idx))
      break;
    ++presented;
  }
  ring.WaitIdle();
  sc.Shutdown();
  ring.Shutdown();
  uint32_t errors = dev.validation_errors();
  dev.Shutdown();
  DestroyWindow(hwnd);
  bool ok = presented == frames_to_run && errors == 0;
  std::printf("presented %d/%d frames, validation errors: %u\n%s\n", presented, frames_to_run,
              errors, ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
