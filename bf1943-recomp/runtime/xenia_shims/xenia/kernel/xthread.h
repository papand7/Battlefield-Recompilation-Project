// bf1943 shim: Xenia's host-thread wrapper, as a plain std::thread.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

class XHostThread : public xe::threading::WaitHandle {
 public:
  XHostThread(KernelState*, uint32_t /*stack_size*/, uint32_t /*flags*/,
              std::function<int()> fn, XProcess* = nullptr)
      : fn_(std::move(fn)) {}
  ~XHostThread() { if (thread_.joinable()) thread_.join(); }
  void set_name(std::string name) { name_ = std::move(name); }
  void Create();
  void Wait(int, int, int, void*) { if (thread_.joinable()) thread_.join(); }
  XHostThread* thread() { return this; }
  void* native_handle() const override { return const_cast<std::thread&>(thread_).native_handle(); }
  void Resume() {}
  void set_can_debugger_suspend(bool) {}
  bool IsCurrent() const { return std::this_thread::get_id() == id_; }

 private:
  std::function<int()> fn_;
  std::string name_;
  std::thread thread_;
  std::thread::id id_;
};

template <typename T>
using object_ref = std::unique_ptr<T>;

struct XThread {
  static bool IsInThread(const XHostThread* t) { return t && t->IsCurrent(); }
  static bool IsInThread() { return false; }
};

}  // namespace kernel
}  // namespace xe
