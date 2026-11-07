// bf1943: implementation of the xe::Memory shim (see xenia/memory.h).
#include "xenia/memory.h"
#include "xenia/base/mutex.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bitset>
#include <mutex>
#include <vector>

namespace xe {

namespace {
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kPageCount = Memory::kBufferSize / kPageSize;

struct Callback {
  Memory::PhysicalMemoryInvalidationCallback fn;
  void* context;
};

// Watch state is guarded by Xenia's global critical region (recursive), as in
// Xenia itself: the invalidation callbacks take it, and the GPU thread arms
// watches while holding it, so a separate mutex here deadlocks (lock order).
auto WatchLock() { return global_critical_region::AcquireDirect(); }
std::bitset<kPageCount>* g_watched = nullptr;
std::vector<Callback*> g_callbacks;
Memory* g_memory = nullptr;
PVOID g_veh = nullptr;

void Protect(uint8_t* physical_membase, uint32_t first_page, uint32_t page_count, DWORD protect) {
  DWORD old;
  VirtualProtect(physical_membase + size_t(first_page) * kPageSize, size_t(page_count) * kPageSize,
                 protect, &old);
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
  auto* rec = ep->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 ||
      rec->ExceptionInformation[0] != 1 || !g_memory)
    return EXCEPTION_CONTINUE_SEARCH;
  return g_memory->HandleWriteFault(reinterpret_cast<void*>(rec->ExceptionInformation[1]))
             ? EXCEPTION_CONTINUE_EXECUTION
             : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

Memory* Memory::Current() { return g_memory; }

Memory::Memory(uint8_t* guest_base)
    : virtual_membase_(guest_base), physical_membase_(guest_base + kPhysicalWindow) {
  g_watched = new std::bitset<kPageCount>();
  g_memory = this;
  // First in the chain, ahead of the runtime's fault reporter.
  g_veh = AddVectoredExceptionHandler(1, WatchHandler);
}

Memory::~Memory() {
  if (g_veh) RemoveVectoredExceptionHandler(g_veh);
  g_memory = nullptr;
}

Memory* GetMemory() { return g_memory; }

bool IsPhysicalCommitted(uint32_t physical_address) {
  if (!g_memory) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(g_memory->physical_membase() + (physical_address & 0x1FFFFFFF), &mbi, sizeof(mbi)))
    return false;
  return mbi.State == MEM_COMMIT;
}

void* Memory::RegisterPhysicalMemoryInvalidationCallback(
    PhysicalMemoryInvalidationCallback callback, void* callback_context) {
  auto lock = WatchLock();
  auto* c = new Callback{callback, callback_context};
  g_callbacks.push_back(c);
  return c;
}

void Memory::UnregisterPhysicalMemoryInvalidationCallback(void* handle) {
  auto lock = WatchLock();
  std::erase(g_callbacks, static_cast<Callback*>(handle));
  delete static_cast<Callback*>(handle);
}

void Memory::EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                                 bool enable_invalidation_notifications, bool) {
  if (!enable_invalidation_notifications || !length) return;
  physical_address &= 0x1FFFFFFF;
  uint32_t first = physical_address / kPageSize;
  uint32_t last = std::min<uint32_t>((physical_address + length - 1) / kPageSize, kPageCount - 1);
  auto lock = WatchLock();
  uint32_t run = first;
  for (uint32_t p = first; p <= last + 1; ++p) {
    bool arm = p <= last && !g_watched->test(p);
    if (arm) {
      g_watched->set(p);
      continue;
    }
    if (p > run) Protect(physical_membase_, run, p - run, PAGE_READONLY);
    run = p + 1;
  }
}

void Memory::TriggerWatches(uint32_t physical_address, uint32_t length) {
  if (!length) return;
  physical_address &= 0x1FFFFFFF;
  auto lock = WatchLock();
  uint32_t first = physical_address / kPageSize;
  uint32_t last = std::min<uint32_t>((physical_address + length - 1) / kPageSize, kPageCount - 1);
  for (uint32_t p = first; p <= last; ++p)
    if (g_watched->test(p)) HandleWriteFault(physical_membase_ + size_t(p) * kPageSize);
}

bool Memory::HandleWriteFault(void* host_address) {
  auto* a = static_cast<uint8_t*>(host_address);
  if (a < physical_membase_ || a >= physical_membase_ + kBufferSize) return false;
  uint32_t page = uint32_t((a - physical_membase_) / kPageSize);
  auto lock = WatchLock();
  if (!g_watched->test(page)) {
    // Another thread lifted this watch between our fault and taking the lock:
    // retry the write if the page is writable again.
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(a, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
  }
  // Let every listener drop its copy; each returns the range it is fine to unwatch.
  uint32_t unwatch_start = page * kPageSize, unwatch_end = unwatch_start + kPageSize;
  for (Callback* c : g_callbacks) {
    auto [start, len] = c->fn(c->context, page * kPageSize, kPageSize, false);
    if (len != UINT32_MAX) {
      unwatch_start = std::max(std::min(unwatch_start, start), 0u);
      unwatch_end = std::min(std::max<uint64_t>(unwatch_end, uint64_t(start) + len), uint64_t(kBufferSize));
    }
  }
  // Only lift pages that are actually watched, page by page into runs.
  uint32_t first = unwatch_start / kPageSize, last = (unwatch_end - 1) / kPageSize;
  uint32_t run = first;
  for (uint32_t p = first; p <= last + 1; ++p) {
    bool lift = p <= last && g_watched->test(p);
    if (lift) {
      g_watched->reset(p);
      continue;
    }
    if (p > run) Protect(physical_membase_, run, p - run, PAGE_READWRITE);
    run = p + 1;
  }
  return true;
}

}  // namespace xe
