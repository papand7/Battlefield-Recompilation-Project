// bf1943 shim for Xenia's GraphicsSystem: same interface as far as the command
// processor and the Vulkan backend use it, wired to the bf1943 runtime instead
// of Xenia's emulator (interrupts and vsync stay in runtime/gpu.cpp).
#ifndef XENIA_GPU_GRAPHICS_SYSTEM_H_
#define XENIA_GPU_GRAPHICS_SYSTEM_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

#include "xenia/cpu/processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

namespace xe {
namespace gpu {

class CommandProcessor;

class GraphicsSystem {
 public:
  virtual ~GraphicsSystem();
  virtual std::string name() const = 0;

  Memory* memory() const { return memory_; }
  cpu::Processor* processor() const { return processor_; }
  kernel::KernelState* kernel_state() const { return kernel_state_; }
  ui::GraphicsProvider* provider() const { return provider_.get(); }
  ui::Presenter* presenter() const { return presenter_.get(); }

  virtual X_STATUS Setup(cpu::Processor* processor, kernel::KernelState* kernel_state,
                         ui::WindowedAppContext* app_context, bool with_presentation);
  virtual void Shutdown();
  void OnHostGpuLossFromAnyThread(bool is_responsible);

  RegisterFile* register_file() { return register_file_; }
  CommandProcessor* command_processor() const { return command_processor_.get(); }

  virtual void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  virtual void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
  virtual void SetInterruptCallback(uint32_t callback, uint32_t user_data);
  // Called by the command processor (PM4 INTERRUPT); forwarded to the runtime.
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);
  virtual void ClearCaches();

  // bf1943: hooks into the runtime.
  std::function<void(uint32_t source, uint32_t cpu)> on_interrupt;
  void WriteRegister(uint32_t index, uint32_t value);

  std::pair<uint32_t, uint32_t> GetScaledAspectRatio() const {
    return {scaled_aspect_x_, scaled_aspect_y_};
  }
  void SetScaledAspectRatio(uint32_t x, uint32_t y) {
    scaled_aspect_x_ = x;
    scaled_aspect_y_ = y;
  }
  bool is_paused() const { return false; }

 protected:
  GraphicsSystem();
  virtual std::unique_ptr<CommandProcessor> CreateCommandProcessor() = 0;

  Memory* memory_ = nullptr;
  cpu::Processor* processor_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;
  ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<ui::GraphicsProvider> provider_;
  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  RegisterFile* register_file_;
  std::unique_ptr<CommandProcessor> command_processor_;
  uint32_t scaled_aspect_x_ = 16;
  uint32_t scaled_aspect_y_ = 9;

 private:
  std::unique_ptr<ui::Presenter> presenter_;
  std::atomic_flag host_gpu_loss_reported_;
};

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_GRAPHICS_SYSTEM_H_
