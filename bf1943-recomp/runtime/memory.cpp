// Guest address space, guest heaps, logging and guest-call helpers.
#include "guest.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bitset>
#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <utility>

namespace rt {

uint8_t* g_base = nullptr;
GuestHeap* g_virtual_heap = nullptr;
GuestHeap* g_system_heap = nullptr;
GuestHeap* g_physical_heap = nullptr;
bool g_trace_imports = false;

namespace {
constexpr uint64_t kPage = 0x1000;
std::mutex g_commit_mutex;
std::bitset<(1ull << 32) / kPage>* g_committed = nullptr;  // one bit per 4 KB page
}

// Physical RAM (512 MB) is one section mapped at 0xA0000000 (64 KB pages) and
// 0xC0000000 (16 MB pages): both views alias the same memory, as on the console,
// where D3D reads resources back through either. Pages are committed through
// the 0xA0000000 view (SEC_RESERVE), which commits them for both.
// (The 0xE0000000 view has a 4 KB offset on the console and is not mapped.)
uint8_t* ReservePhysicalViews(uint64_t reserve) {
  using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
  using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
  HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
  auto alloc2 = kb ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2")) : nullptr;
  auto map3 = kb ? reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3")) : nullptr;
  if (!alloc2 || !map3) return nullptr;
  constexpr uint64_t kPhys = 0x20000000;
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE,
                                      0, DWORD(kPhys), nullptr);
  if (!section) return nullptr;
  auto* base = static_cast<uint8_t*>(alloc2(nullptr, nullptr, reserve,
                                            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
  if (!base) return nullptr;
  // Split the placeholder into [0,A0) [A0,C0) [C0,E0) [E0,end).
  bool ok = VirtualFree(base + 0xA0000000ull, kPhys, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
            VirtualFree(base + 0xC0000000ull, kPhys, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
  for (uint64_t view : {0xA0000000ull, 0xC0000000ull})
    ok = ok && map3(section, GetCurrentProcess(), base + view, 0, kPhys, MEM_REPLACE_PLACEHOLDER,
                    PAGE_READWRITE, nullptr, 0) == base + view;
  // The rest stays as placeholders; turn them into ordinary reservations.
  for (auto [lo, hi] : {std::pair<uint64_t, uint64_t>{0, 0xA0000000ull},
                        {0xE0000000ull, reserve}})
    ok = ok && alloc2(nullptr, base + lo, hi - lo, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER,
                      PAGE_NOACCESS, nullptr, 0) == base + lo;
  if (!ok) {
    RT_WARN("memory: physical views not mapped (error %lu), 0xC0000000 view unavailable", GetLastError());
    return nullptr;  // leaks the attempt; the caller falls back to one reservation
  }
  return base;
}

bool MemoryInit() {
  // 4 GB plus room after the top so a guest access that straddles 0xFFFFFFFF
  // does not touch host memory. Reserve only: pages are committed on demand.
  constexpr uint64_t kReserve = PPC_MEMORY_SIZE + 0x10000;
  g_base = ReservePhysicalViews(kReserve);
  if (!g_base) g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS));
  if (!g_base) return false;
  if (reinterpret_cast<uintptr_t>(g_base) & 0x1F) return false;  // PPC_FUNC_PROLOGUE assumes it
  g_committed = new std::bitset<(1ull << 32) / kPage>();

  g_virtual_heap = new GuestHeap("virtual", 0x40000000, 0x70000000, 0x1000);
  g_system_heap = new GuestHeap("system", 0x70000000, 0x7F000000, 0x10);
  g_physical_heap = new GuestHeap("physical", 0xA0000000, 0xC0000000, 0x1000);
  // GPU register block (0x7FC80000). Plain memory until the GPU side exists.
  Commit(0x7FC80000, 0x20000);
  // Audio (XMA) register block, plain memory until audio exists.
  Commit(0x7FEA0000, 0x10000);
  return true;
}

// DEBUG --snap-mem: committed pages of the heaps and the image, as
// (u32 address, 4 KB) records, to find game state by comparing snapshots.
void SnapshotMemory(const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  for (uint64_t a = 0x40000000; a < 0xC0000000; a += kPage) {
    if (a >= 0x90000000 && a < 0xA0000000) continue;
    if (!(*g_committed)[a / kPage]) continue;
    uint32_t a32 = uint32_t(a);
    fwrite(&a32, 4, 1, f);
    fwrite(g_base + a, kPage, 1, f);
  }
  fclose(f);
}

void Commit(uint32_t guest, uint32_t size) {
  if (!size) return;
  if (guest >= 0xC0000000 && guest < 0xE0000000) guest -= 0x20000000;  // alias of 0xA0000000
  uint64_t first = guest / kPage, last = (uint64_t(guest) + size - 1) / kPage;
  std::lock_guard lock(g_commit_mutex);
  uint64_t run = first;
  for (uint64_t p = first; p <= last + 1; ++p) {
    bool need = p <= last && !g_committed->test(p);
    if (need) continue;
    if (p > run) {
      if (!VirtualAlloc(g_base + run * kPage, (p - run) * kPage, MEM_COMMIT, PAGE_READWRITE))
        Fatal("commit failed at %08llX (+%llX), error %lu", run * kPage, (p - run) * kPage,
              GetLastError());
      for (uint64_t q = run; q < p; ++q) g_committed->set(q);
    }
    run = p + 1;
  }
}

bool IsCommitted(uint32_t guest) {
  if (guest >= 0xC0000000 && guest < 0xE0000000) guest -= 0x20000000;
  return g_committed->test(guest / kPage);
}

// ---- GuestHeap ---------------------------------------------------------------
struct GuestHeap::Impl {
  std::mutex mutex;
  std::map<uint32_t, uint32_t> free;  // addr -> size
  std::map<uint32_t, uint32_t> used;  // addr -> size
};

GuestHeap::GuestHeap(const char* name, uint32_t lo, uint32_t hi, uint32_t granularity)
    : impl_(new Impl), name_(name), lo_(lo), hi_(hi), gran_(granularity) {
  impl_->free[lo] = hi - lo;
}

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t GuestHeap::Alloc(uint32_t size, uint32_t align) {
  if (align < gran_) align = gran_;
  size = AlignUp(size ? size : 1, gran_);
  std::lock_guard lock(impl_->mutex);
  for (auto it = impl_->free.begin(); it != impl_->free.end(); ++it) {
    uint32_t start = AlignUp(it->first, align);
    uint64_t end = uint64_t(it->first) + it->second;
    if (uint64_t(start) + size > end) continue;
    uint32_t block = it->first, block_size = it->second;
    impl_->free.erase(it);
    if (start > block) impl_->free[block] = start - block;
    if (uint64_t(start) + size < end) impl_->free[start + size] = uint32_t(end - (start + size));
    impl_->used[start] = size;
    used_bytes_ += size;
    Commit(start, size);
    return start;
  }
  RT_ERROR("%s heap: out of memory (size %X, in use %X)", name_, size, used_bytes_);
  return 0;
}

uint32_t GuestHeap::AllocAt(uint32_t addr, uint32_t size) {
  size = AlignUp(size, gran_);
  addr &= ~(gran_ - 1);
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->free.upper_bound(addr);
  if (it == impl_->free.begin()) return 0;
  --it;
  uint64_t end = uint64_t(it->first) + it->second;
  if (uint64_t(addr) + size > end) return 0;
  uint32_t block = it->first;
  impl_->free.erase(it);
  if (addr > block) impl_->free[block] = addr - block;
  if (uint64_t(addr) + size < end) impl_->free[addr + size] = uint32_t(end - (addr + size));
  impl_->used[addr] = size;
  used_bytes_ += size;
  Commit(addr, size);
  return addr;
}

void GuestHeap::Free(uint32_t addr) {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->used.find(addr);
  if (it == impl_->used.end()) {
    RT_WARN("%s heap: free of unknown block %08X", name_, addr);
    return;
  }
  uint32_t size = it->second;
  impl_->used.erase(it);
  used_bytes_ -= size;
  // Insert and merge with neighbours. Pages stay committed (cheap reuse).
  auto [ins, ok] = impl_->free.emplace(addr, size);
  auto next = std::next(ins);
  if (next != impl_->free.end() && ins->first + ins->second == next->first) {
    ins->second += next->second;
    impl_->free.erase(next);
  }
  if (ins != impl_->free.begin()) {
    auto prev = std::prev(ins);
    if (prev->first + prev->second == ins->first) {
      prev->second += ins->second;
      impl_->free.erase(ins);
    }
  }
}

uint32_t GuestHeap::SizeOf(uint32_t addr) {
  std::lock_guard lock(impl_->mutex);
  auto it = impl_->used.find(addr);
  return it == impl_->used.end() ? 0 : it->second;
}

// ---- logging -----------------------------------------------------------------
static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;

void Logf(Log level, const char* fmt, ...) {
  static const char* kTag[] = {"I", "W", "E", "T"};
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  std::lock_guard lock(g_log_mutex);
  if (!g_log_file) g_log_file = fopen("bf1943.log", "w");
  unsigned tid = GetCurrentThreadId();
  if (level != Log::Trace) fprintf(stderr, "[%s %5u] %s\n", kTag[int(level)], tid, buf);
  if (g_log_file) {
    fprintf(g_log_file, "[%s %5u] %s\n", kTag[int(level)], tid, buf);
    fflush(g_log_file);
  }
}

void Fatal(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Logf(Log::Error, "FATAL: %s", buf);
  fflush(nullptr);
  ExitProcess(1);
}

// ---- guest calls ---------------------------------------------------------------
PPCFunc* LookupFunc(uint32_t guest) {
  if (guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
  return PPC_LOOKUP_FUNC(g_base, guest);
}

uint64_t GuestCall(PPCContext& ctx, uint32_t func, std::initializer_list<uint64_t> args) {
  PPCFunc* fn = LookupFunc(func);
  if (!fn) Fatal("GuestCall: no function at %08X", func);
  PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (uint64_t a : args) regs[i++]->u64 = a;
  // Keep the caller's frame (and anything it keeps below r1) intact.
  uint64_t saved_r1 = ctx.r1.u64;
  ctx.r1.u64 = (saved_r1 - 0x200) & ~0xFull;
  fn(ctx, g_base);
  ctx.r1.u64 = saved_r1;
  return ctx.r3.u64;
}

// ---- strings -----------------------------------------------------------------
std::string ReadCString(uint32_t ptr) {
  if (!ptr) return {};
  return std::string(G<const char>(ptr));
}

std::string ReadAnsiString(uint32_t p) {
  if (!p) return {};
  uint16_t len = Ld16(p);
  uint32_t buf = Ld32(p + 4);
  if (!buf) return {};
  return std::string(G<const char>(buf), len);
}

}  // namespace rt
extern "C" double g_bf_timebase_scale = 0;
namespace rt {

uint64_t HostTicksPerSecond() {
  static const uint64_t rate = [] {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    uint64_t t0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    QueryPerformanceCounter(&b);
    uint64_t t1 = __rdtsc();
    double secs = double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
    uint64_t r = uint64_t(double(t1 - t0) / secs);
    g_bf_timebase_scale = double(kGuestTimebase) / double(r);
    return r;
  }();
  return rate;
}

}  // namespace rt
