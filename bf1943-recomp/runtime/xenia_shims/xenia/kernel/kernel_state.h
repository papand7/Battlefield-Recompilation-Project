// bf1943 shim: the parts of Xenia's KernelState its GPU code touches.
#pragma once
#include <cstdint>
#include "xenia/memory.h"

namespace xe {
class Emulator;
namespace cpu { class Processor; }
namespace kernel {

struct XProcess {};

class UserModule {
 public:
  uint32_t title_id() const { return title_id_; }
  uint32_t title_id_ = 0;
};

class KernelState {
 public:
  explicit KernelState(Memory* memory) : memory_(memory) {}
  Memory* memory() const { return memory_; }
  XProcess* GetIdleProcess() { return &idle_process_; }
  UserModule* GetExecutableModule() { return &module_; }
  Emulator* emulator() const { return nullptr; }
  cpu::Processor* processor() const { return nullptr; }
  void set_title_id(uint32_t id) { module_.title_id_ = id; }

 private:
  Memory* memory_;
  XProcess idle_process_;
  UserModule module_;
};

}  // namespace kernel
}  // namespace xe
