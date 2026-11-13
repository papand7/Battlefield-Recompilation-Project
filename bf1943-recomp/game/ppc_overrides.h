// Force-included into the recompiled code. Checks every indirect call so a bad
// target is reported with its guest address instead of jumping to null.
#pragma once
#include <cstdint>
#include <immintrin.h>  // _cvtsh_ss for vupkd3d128 half floats

// mftb: the recompiler emits __rdtsc(). Guest code expects the console's
// 49.875 MHz timebase (some code hard-codes it), so scale the host TSC. The
// scale and offset are swapped together (one pointer): --speed changes the
// rate while the game runs, without a jump (memory.cpp, SetTimeSpeed).
struct BfClock { double scale; int64_t offset; };
extern "C" const BfClock* g_bf_clock;
inline uint64_t BfMftb() {
  const BfClock* c = __atomic_load_n(&g_bf_clock, __ATOMIC_ACQUIRE);
  return uint64_t(int64_t(double(__builtin_ia32_rdtsc()) * c->scale) + c->offset);
}
#define __rdtsc() BfMftb()
struct PPCContext;
extern "C" [[noreturn]] void PPCBadIndirectCall(PPCContext& ctx, uint32_t target);
#define PPC_CALL_INDIRECT_FUNC(x)                                                     \
  do {                                                                                \
    uint32_t bf_target_ = (x);                                                        \
    PPCFunc* bf_fn_ = (bf_target_ - PPC_CODE_BASE < PPC_CODE_SIZE)                    \
                          ? PPC_LOOKUP_FUNC(base, bf_target_) : nullptr;              \
    if (!bf_fn_) [[unlikely]] PPCBadIndirectCall(ctx, bf_target_);                    \
    bf_fn_(ctx, base);                                                                \
  } while (0)

// Stores the recompiler marks as MMIO (followed by eieio). The XMA register block
// at 0x7FEA0000 is forwarded to the audio runtime and the GPU block at 0x7FC80000
// (D3D's ring-buffer kick) to the command processor; everything else is plain memory.
extern "C" void RtMmioWrite32(uint32_t address, uint32_t value);
extern "C" void RtGpuMmioWrite32(uint32_t address, uint32_t value);
#define PPC_MM_STORE_U32(x, y)                                                        \
  do {                                                                                \
    uint32_t bf_a_ = (x), bf_v_ = (y);                                                \
    PPC_STORE_U32(bf_a_, bf_v_);                                                      \
    if ((bf_a_ & 0xFFFF0000u) == 0x7FEA0000u) [[unlikely]] RtMmioWrite32(bf_a_, bf_v_); \
    else if ((bf_a_ & 0xFFFF0000u) == 0x7FC80000u) [[unlikely]]                       \
      RtGpuMmioWrite32(bf_a_, bf_v_);                                                 \
  } while (0)
