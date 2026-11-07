// Frames in flight: one command pool per frame, a single timeline semaphore
// for CPU/GPU pacing (no per-frame VkFences), sync2 submission.
#pragma once

#include "vk_device.h"

#include <array>

namespace bf::vk {

class FrameRing {
 public:
  static constexpr uint32_t kFramesInFlight = 2;

  bool Init(Device* dev);
  void Shutdown();

  // Waits until the GPU finished the frame that last used this slot, resets
  // its pool and returns a command buffer in the recording state.
  VkCommandBuffer Begin();
  // Ends recording and submits. `wait`/`signal` are optional binary
  // semaphores (swapchain acquire / present).
  bool Submit(VkSemaphore wait = VK_NULL_HANDLE,
              VkPipelineStageFlags2 wait_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VkSemaphore signal = VK_NULL_HANDLE);
  // Blocks until all submitted work is complete.
  bool WaitIdle();

  uint32_t slot() const { return slot_; }
  uint64_t submitted() const { return submitted_; }
  VkSemaphore timeline() const { return timeline_; }

 private:
  struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint64_t done_value = 0;  // timeline value that marks this slot free
  };
  bool WaitValue(uint64_t v);

  Device* dev_ = nullptr;
  std::array<Slot, kFramesInFlight> slots_{};
  VkSemaphore timeline_ = VK_NULL_HANDLE;
  uint64_t submitted_ = 0;
  uint32_t slot_ = 0;
  uint64_t frame_ = 0;
};

}  // namespace bf::vk
