// Shared runtime declarations: guest memory access, guest heaps, logging and the
// macros used to implement Xbox 360 kernel/XAM imports for the recompiled code.
#pragma once
#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>

#include "posix_compat.h"  // Win32 names on Linux (empty on Windows)

namespace rt {

// ---- guest memory ----------------------------------------------------------
// The 4 GB guest address space lives at g_base. Pages are committed on demand
// (image, heaps, stacks); everything else faults, which catches wild pointers.
extern uint8_t* g_base;

bool MemoryInit();
void Commit(uint32_t guest, uint32_t size);
void SnapshotMemory(const char* path);  // DEBUG
void StartWriteWatch(uint32_t addr, uint32_t size, double at);  // DEBUG
bool IsCommitted(uint32_t guest);  // is the 4 KB page backed?

template <class T = uint8_t>
inline T* G(uint32_t guest) { return reinterpret_cast<T*>(g_base + guest); }
inline uint32_t ToGuest(const void* host) {
  return uint32_t(reinterpret_cast<const uint8_t*>(host) - g_base);
}

inline uint16_t Ld16(uint32_t a) { return __builtin_bswap16(*G<uint16_t>(a)); }
inline uint32_t Ld32(uint32_t a) { return __builtin_bswap32(*G<uint32_t>(a)); }
inline uint64_t Ld64(uint32_t a) { return __builtin_bswap64(*G<uint64_t>(a)); }
inline void St8(uint32_t a, uint8_t v) { *G<uint8_t>(a) = v; }
inline void St16(uint32_t a, uint16_t v) { *G<uint16_t>(a) = __builtin_bswap16(v); }
inline void St32(uint32_t a, uint32_t v) { *G<uint32_t>(a) = __builtin_bswap32(v); }
inline void St64(uint32_t a, uint64_t v) { *G<uint64_t>(a) = __builtin_bswap64(v); }
// Store only if the guest passed a non-null out pointer.
inline void Out32(uint32_t a, uint32_t v) { if (a) St32(a, v); }

// First-fit allocator over a guest address range; commits what it hands out.
class GuestHeap {
 public:
  GuestHeap(const char* name, uint32_t lo, uint32_t hi, uint32_t granularity);
  uint32_t Alloc(uint32_t size, uint32_t align = 0);
  // Fixed-address allocation (NtAllocateVirtualMemory with a base). 0 on failure.
  uint32_t AllocAt(uint32_t addr, uint32_t size);
  void Free(uint32_t addr);
  uint32_t SizeOf(uint32_t addr);
  bool Contains(uint32_t addr) const { return addr >= lo_ && addr < hi_; }
  uint32_t used_bytes() const { return used_bytes_; }

 private:
  struct Impl;
  Impl* impl_;
  const char* name_;
  uint32_t lo_, hi_, gran_;
  uint32_t used_bytes_ = 0;
};

extern GuestHeap* g_virtual_heap;   // 0x40000000..: NtAllocateVirtualMemory
extern GuestHeap* g_system_heap;    // pools, XamAlloc, stacks, kernel objects
extern GuestHeap* g_physical_heap;  // 0xA0000000..: MmAllocatePhysicalMemoryEx

// ---- logging ---------------------------------------------------------------
enum class Log { Info, Warn, Error, Trace };
void Logf(Log level, const char* fmt, ...);
extern bool g_trace_imports;
#define RT_INFO(...) ::rt::Logf(::rt::Log::Info, __VA_ARGS__)
#define RT_WARN(...) ::rt::Logf(::rt::Log::Warn, __VA_ARGS__)
#define RT_ERROR(...) ::rt::Logf(::rt::Log::Error, __VA_ARGS__)
#define RT_TRACE(...) do { if (::rt::g_trace_imports) ::rt::Logf(::rt::Log::Trace, __VA_ARGS__); } while (0)
[[noreturn]] void Fatal(const char* fmt, ...);

// ---- guest calls -----------------------------------------------------------
PPCFunc* LookupFunc(uint32_t guest);
// Calls a guest function on the current guest thread. Integer args go in r3..r10.
uint64_t GuestCall(PPCContext& ctx, uint32_t func, std::initializer_list<uint64_t> args);

// ---- formatting (sprintf, DbgPrint, ...) -----------------------------------
// Reads printf-style varargs from guest registers r[first_gpr].. then the stack.
std::string GuestFormat(PPCContext& ctx, uint32_t fmt, int first_gpr);
std::string GuestFormatVa(uint32_t fmt, uint32_t va_list);

// ---- misc ------------------------------------------------------------------
std::string ReadAnsiString(uint32_t ansi_string_ptr);  // ANSI_STRING*
std::string ReadCString(uint32_t ptr);
uint64_t HostTicksPerSecond();  // host rdtsc rate; also sets the mftb scale
// --speed: the guest's clocks (mftb, the kernel's) run this many times as
// fast from now on, carrying on from where they are; GuestSeconds: the
// guest's time since the start, at those speeds.
void SetTimeSpeed(double speed);
double GuestSeconds();
constexpr uint64_t kGuestTimebase = 49875000;  // console mftb / KeQueryPerformanceFrequency rate

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
constexpr uint32_t STATUS_PENDING = 0x00000103;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_NOT_SUPPORTED = 0xC00000BB;
constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_FUNCTION_FAILED = 1627;

}  // namespace rt

// An import implementation. Arguments are in ctx.rN, the result goes in ctx.r3.
#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)
#define ARG32(n) (ctx.r##n.u32)
#define ARG64(n) (ctx.r##n.u64)
#define RETURN(v) do { ctx.r3.u64 = uint64_t(v); return; } while (0)
