#include "vk_frame.h"

#include "vk_log.h"

namespace bf::vk {

bool FrameRing::Init(Device* dev) {
  dev_ = dev;
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  sci.pNext = &type;
  BF_VK_CHECK(vkCreateSemaphore(dev_->device(), &sci, nullptr, &timeline_));

  for (auto& s : slots_) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = dev_->queue_family();
    BF_VK_CHECK(vkCreateCommandPool(dev_->device(), &pci, nullptr, &s.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    BF_VK_CHECK(vkAllocateCommandBuffers(dev_->device(), &ai, &s.cmd));
  }
  return true;
}

bool FrameRing::WaitValue(uint64_t v) {
  if (v == 0) return true;
  VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wi.semaphoreCount = 1;
  wi.pSemaphores = &timeline_;
  wi.pValues = &v;
  BF_VK_CHECK(vkWaitSemaphores(dev_->device(), &wi, UINT64_MAX));
  return true;
}

VkCommandBuffer FrameRing::Begin() {
  slot_ = uint32_t(frame_ % kFramesInFlight);
  Slot& s = slots_[slot_];
  if (!WaitValue(s.done_value)) return VK_NULL_HANDLE;
  vkResetCommandPool(dev_->device(), s.pool, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(s.cmd, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
  return s.cmd;
}

bool FrameRing::Submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
  Slot& s = slots_[slot_];
  BF_VK_CHECK(vkEndCommandBuffer(s.cmd));

  VkSemaphoreSubmitInfo waits{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  waits.semaphore = wait;
  waits.stageMask = wait_stage;
  VkSemaphoreSubmitInfo signals[2]{};
  signals[0] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signals[0].semaphore = timeline_;
  signals[0].value = ++submitted_;
  signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  signals[1] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signals[1].semaphore = signal;
  signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = s.cmd;

  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.waitSemaphoreInfoCount = wait ? 1 : 0;
  si.pWaitSemaphoreInfos = &waits;
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  si.signalSemaphoreInfoCount = signal ? 2 : 1;
  si.pSignalSemaphoreInfos = signals;
  BF_VK_CHECK(vkQueueSubmit2(dev_->queue(), 1, &si, VK_NULL_HANDLE));
  s.done_value = submitted_;
  ++frame_;
  return true;
}

bool FrameRing::WaitIdle() { return WaitValue(submitted_); }

void FrameRing::Shutdown() {
  if (!dev_ || !dev_->device()) return;
  WaitIdle();
  for (auto& s : slots_)
    if (s.pool) vkDestroyCommandPool(dev_->device(), s.pool, nullptr), s = {};
  if (timeline_) vkDestroySemaphore(dev_->device(), timeline_, nullptr), timeline_ = VK_NULL_HANDLE;
}

}  // namespace bf::vk
