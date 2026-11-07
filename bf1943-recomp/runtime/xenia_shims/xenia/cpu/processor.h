// bf1943 shim: Xenia's Processor only provides memory() to the GPU side here.
#pragma once
#include "xenia/memory.h"
namespace xe { namespace cpu {
typedef uint32_t (*MMIOReadCallback)(void* ppc_context, void* callback_context, uint32_t addr);
typedef void (*MMIOWriteCallback)(void* ppc_context, void* callback_context, uint32_t addr, uint32_t value);
class Processor {
 public:
  explicit Processor(Memory* memory) : memory_(memory) {}
  Memory* memory() const { return memory_; }
 private:
  Memory* memory_;
};
} }
