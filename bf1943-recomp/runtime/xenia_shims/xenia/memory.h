// bf1943 shim for Xenia's xe::Memory, as used by its GPU code. Guest memory is
// the runtime's 4 GB space; guest physical memory is the window at guest
// 0xA0000000 (the runtime's physical heap), so physical address P lives at
// g_base + 0xA0000000 + (P & 0x1FFFFFFF).
//
// CPU-write detection for GPU caches (EnablePhysicalMemoryAccessCallbacks) uses
// page protection and a vectored exception handler, like Xenia does.
#pragma once
#include <cstdint>
#include <utility>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"      // the real memory.h brings these in
#include "xenia/base/threading.h"

namespace xe {

enum : uint32_t { kMemoryAllocationReserve = 1 << 0, kMemoryAllocationCommit = 1 << 1 };
enum : uint32_t { kMemoryProtectRead = 1 << 0, kMemoryProtectWrite = 1 << 1 };
enum : uint32_t { kSystemHeapVirtual = 1 << 0, kSystemHeapPhysical = 1 << 1 };

struct HeapAllocationInfo {
  uint32_t base_address;
  uint32_t allocation_base;
  uint32_t allocation_protect;
  uint32_t allocation_size;
  uint32_t region_size;
  uint32_t state;
  uint32_t protect;
};

// Is guest physical address P backed by committed memory? (memory_shim.cc)
bool IsPhysicalCommitted(uint32_t physical_address);

class VirtualHeap {
 public:
  // Used before writing GPU results back to guest memory: one committed page
  // at a time is enough for the caller's range check.
  bool QueryRegionInfo(uint32_t address, HeapAllocationInfo* info) {
    uint32_t page = address & ~0xFFFu;
    bool committed = IsPhysicalCommitted(page);
    *info = {page, page, kMemoryProtectRead | kMemoryProtectWrite, 0x1000, 0x1000,
             committed ? kMemoryAllocationCommit : 0u, kMemoryProtectRead | kMemoryProtectWrite};
    // Extend over following committed pages so multi-page writes pass.
    while (committed && info->region_size < 0x1000000 &&
           IsPhysicalCommitted(page + info->region_size))
      info->region_size += 0x1000;
    return true;
  }
  // The GPU only asks whether ranges are accessible; everything we hand out is.
  xe::memory::PageAccess QueryRangeAccess(uint32_t, uint32_t) {
    return xe::memory::PageAccess::kReadWrite;
  }
  uint32_t page_size() const { return 0x1000; }
  uint32_t heap_base() const { return 0; }
  uint32_t heap_size() const { return 0x20000000; }
};

class Memory {
 public:
  static constexpr uint32_t kPhysicalWindow = 0xA0000000;
  // Size of the guest physical address space the GPU's shared memory mirrors.
  static constexpr uint32_t kBufferSize = 0x20000000;

  explicit Memory(uint8_t* guest_base);
  static Memory* Current();  // the one instance (bf1943)
  ~Memory();

  uint8_t* virtual_membase() const { return virtual_membase_; }
  uint8_t* physical_membase() const { return physical_membase_; }

  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + guest_address);
  }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }
  uint32_t HostToGuestVirtual(const void* host_address) const {
    return uint32_t(static_cast<const uint8_t*>(host_address) - virtual_membase_);
  }
  uint32_t GetPhysicalAddress(uint32_t address) const { return address & 0x1FFFFFFF; }

  VirtualHeap* GetPhysicalHeap() { return &physical_heap_; }

  // Guest allocations for Xenia's own structures (XMA context array). The
  // runtime installs the allocator; it always returns physical-heap memory.
  static inline uint32_t (*system_heap_alloc)(uint32_t size, uint32_t alignment) = nullptr;
  uint32_t SystemHeapAlloc(uint32_t size, uint32_t alignment = 0x20,
                           uint32_t /*system_heap_flags*/ = kSystemHeapVirtual) {
    return system_heap_alloc ? system_heap_alloc(size, alignment) : 0;
  }
  void SystemHeapFree(uint32_t) {}
  // MMIO is routed by the runtime (PPC_MM_STORE_U32 hook), not by Xenia.
  template <typename R, typename W>
  void AddVirtualMappedRange(uint32_t, uint32_t, uint32_t, void*, R, W) {}

  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);

  typedef void (*PhysicalMemoryReadCallback)(void* context_ptr, uint32_t physical_address_start,
                                             uint32_t length);
  void* RegisterPhysicalMemoryReadCallback(PhysicalMemoryReadCallback, void*) { return nullptr; }
  void UnregisterPhysicalMemoryReadCallback(void*) {}

  // Arms write watches on the pages of [physical_address, +length). The first
  // CPU write to a watched page calls the invalidation callbacks and unwatches it.
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

  // Called by the runtime's exception handler for a write fault at host_address.
  // Returns true if it was a watched page (now unwatched; retry the access).
  bool HandleWriteFault(void* host_address);

  // Call before the runtime itself writes guest physical memory that may be
  // watched (for example NtReadFile into a texture buffer).
  void TriggerWatches(uint32_t physical_address, uint32_t length);

 private:
  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
  VirtualHeap physical_heap_;
};

// The runtime's single instance (created when the Vulkan GPU starts).
Memory* GetMemory();

}  // namespace xe
