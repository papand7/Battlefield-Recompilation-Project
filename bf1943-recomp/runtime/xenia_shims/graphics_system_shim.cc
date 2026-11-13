// bf1943: implementation of the GraphicsSystem shim and the XHostThread shim.
#include "xenia/gpu/graphics_system.h"

#include "xenia/base/logging.h"
#include "xenia/gpu/command_processor.h"

#ifdef _WIN32  // (Linux: linux_shim.cc)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace xe {
namespace kernel {
void XHostThread::Create() {
  thread_ = std::thread([this] {
    id_ = std::this_thread::get_id();
    std::wstring w(name_.begin(), name_.end());
    SetThreadDescription(GetCurrentThread(), w.c_str());
    fn_();
  });
  while (id_ == std::thread::id()) std::this_thread::yield();
}
}  // namespace kernel
}  // namespace xe
#endif  // _WIN32

namespace xe {

namespace gpu {

GraphicsSystem::GraphicsSystem() : register_file_(new RegisterFile()) {}
GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(cpu::Processor* processor, kernel::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context, bool with_presentation) {
  memory_ = processor->memory();
  processor_ = processor;
  kernel_state_ = kernel_state;
  app_context_ = app_context;
  if (with_presentation && provider_) {
    presenter_ = provider_->CreatePresenter(
        [this](bool is_responsible, bool) { OnHostGpuLossFromAnyThread(is_responsible); });
  }
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    XELOGE("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }
  presenter_.reset();
  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread(bool) {
  if (host_gpu_loss_reported_.test_and_set()) return;
  XELOGE("Host GPU lost");
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (on_interrupt) on_interrupt(source, cpu);
}

void GraphicsSystem::ClearCaches() { command_processor_->ClearCaches(); }

void GraphicsSystem::WriteRegister(uint32_t index, uint32_t value) {
  switch (index) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    default:
      register_file_->values[index] = value;
      break;
  }
}

}  // namespace gpu
}  // namespace xe
