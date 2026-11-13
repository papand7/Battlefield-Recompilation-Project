// xboxkrnl.exe imports: memory, synchronisation, critical sections, Rtl helpers,
// time, module queries and the kernel data exports.
#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <immintrin.h>
#include "sha1.h"
#endif

#include <chrono>
#include <thread>
#include <unordered_map>

#include <xex.h>

namespace rt {
std::string g_guest_args;
uint32_t SetTimer(uint32_t timer, uint32_t due_ptr, int32_t period_ms);
uint32_t CancelTimer(uint32_t timer);

namespace {
uint32_t g_timestamp_bundle = 0;

// Zero-filled guest block, never freed.
uint32_t Block(uint32_t size, uint32_t align = 0x10) {
  uint32_t a = g_system_heap->Alloc(size, align);
  memset(G(a), 0, size);
  return a;
}
}  // namespace

uint32_t ResolveVariableImport(const char* module, const char* name, uint32_t ordinal) {
  std::string n = name;
  if (n.rfind("__imp__", 0) == 0) n = n.substr(7);
  uint32_t a = 0;
  if (n == "KeTimeStampBundle") {
    a = g_timestamp_bundle = Block(0x18);
  } else if (n == "XboxHardwareInfo") {
    a = Block(0x10);
    St32(a, 0x00000000);  // flags
    St8(a + 4, 6);        // hardware threads
  } else if (n == "XboxKrnlVersion") {
    a = Block(8);
    St16(a, 2);
    St16(a + 2, 0);
    St16(a + 4, 17559);
    St16(a + 6, 0);
  } else if (n == "XexExecutableModuleHandle") {
    a = Block(4);
    St32(a, g_module.ldr_entry);
  } else if (n == "ExConsoleGameRegion") {
    a = Block(4);
    St32(a, 0xFFFFFFFF);
  } else if (n == "ExLoadedCommandLine") {
    // The CRT splits this into argc/argv; the engine reads -listen, -level,
    // -Client.ServerIp and other -Table.Member settings from it.
    std::string cmd = "default.xex " + g_guest_args;
    a = Block(uint32_t(cmd.size() + 0x10));
    memcpy(G(a), cmd.data(), cmd.size());
  } else if (n == "VdGpuClockInMHz") {
    a = Block(4);
    St32(a, 500);
  } else if (n == "VdHSIOCalibrationLock") {
    a = Block(0x1C);
    St32(a + 0x10, 0xFFFFFFFF);
  } else {
    a = Block(0x100);  // object types, debug data, ...: zeroed placeholder
  }
  RT_INFO("data import %s!%s (#%u) -> %08X", module, name, ordinal, a);
  return a;
}

// The guest's clocks at --speed: from when first read, g_time_speed times as
// fast as the host's.
uint64_t GuestFileTime() {
  static const uint64_t ft0 = [] {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  }();
  static const double g0 = GuestSeconds();
  return ft0 + uint64_t((GuestSeconds() - g0) * 1e7);
}
uint64_t GuestTickCount64() {
  static const uint64_t ms0 = GetTickCount64();
  static const double g0 = GuestSeconds();
  return ms0 + uint64_t((GuestSeconds() - g0) * 1000.0);
}

// --speed: g_time_speed_target once a level has loaded (open 20 s, no file
// opened for 8 s), 1x from the next level's open (its load) on.
static void SpeedControl() {
  if (g_time_speed_target == 1.0) return;
  int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
  static int64_t fast_since = 0;
  int64_t level = g_level_open_ms.load();
  if (g_time_speed != 1.0) {
    if (g_in_menus || level > fast_since) SetTimeSpeed(1.0);
  } else if (!g_in_menus && level && now - level > 20000 && now - g_file_open_ms.load() > 8000) {
    fast_since = now;
    SetTimeSpeed(g_time_speed_target);
  }
}

void StartKernelServices() {
  std::thread([] {
    SetThreadDescription(GetCurrentThread(), L"kernel timestamp");
    auto start = std::chrono::steady_clock::now();
    for (;;) {
      if (g_timestamp_bundle) {
        auto up = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(GuestSeconds()));
        St64(g_timestamp_bundle + 0, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(up).count() / 100));
        St64(g_timestamp_bundle + 8, GuestFileTime());
        St32(g_timestamp_bundle + 16, uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(up).count()));
      }
      static int tick = 0;
      if (++tick % 250 == 0) SpeedControl();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }).detach();
}

}  // namespace rt

using namespace rt;

// ---- memory -----------------------------------------------------------------
// NtAllocateVirtualMemory(base*, size*, type, protect, debug)
GUEST_IMPORT(NtAllocateVirtualMemory) {
  uint32_t base_ptr = ARG32(3), size_ptr = ARG32(4), type = ARG32(5);
  uint32_t want = Ld32(base_ptr), size = Ld32(size_ptr);
  uint32_t page = (type & 0x20000000) ? 0x10000 : 0x1000;  // MEM_LARGE_PAGES
  size = (size + page - 1) & ~(page - 1);
  uint32_t addr = 0;
  if (want) {
    addr = g_virtual_heap->AllocAt(want, size);
    if (!addr) addr = want & ~0xFFFu;  // committing inside an earlier reservation
  } else {
    addr = g_virtual_heap->Alloc(size, 0x10000);
  }
  if (!addr) RETURN(STATUS_NO_MEMORY);
  Commit(addr, size);
  if (!want) memset(G(addr), 0, size);
  St32(base_ptr, addr);
  St32(size_ptr, size);
  RT_TRACE("NtAllocateVirtualMemory(%08X, %X, %X) -> %08X", want, size, type, addr);
  RETURN(STATUS_SUCCESS);
}

// NtFreeVirtualMemory(base*, size*, type, debug)
GUEST_IMPORT(NtFreeVirtualMemory) {
  uint32_t addr = Ld32(ARG32(3));
  if (ARG32(5) & 0x8000) g_virtual_heap->Free(addr);  // MEM_RELEASE; decommit is a no-op
  RETURN(STATUS_SUCCESS);
}

// NtQueryVirtualMemory(base, MEMORY_BASIC_INFORMATION*, debug)
GUEST_IMPORT(NtQueryVirtualMemory) {
  uint32_t addr = ARG32(3), info = ARG32(4);
  memset(G(info), 0, 0x1C);
  St32(info + 0, addr & ~0xFFFu);   // BaseAddress
  St32(info + 4, addr & ~0xFFFu);   // AllocationBase
  St32(info + 8, 4);                // AllocationProtect: PAGE_READWRITE
  St32(info + 12, 0x1000);          // RegionSize
  St32(info + 16, 0x1000);          // State: MEM_COMMIT
  St32(info + 20, 4);               // Protect
  St32(info + 24, 0x20000);         // Type: MEM_PRIVATE
  RETURN(STATUS_SUCCESS);
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min, max, alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx) {
  uint32_t size = ARG32(4), align = std::max<uint32_t>(ARG32(8), 0x1000);
  uint32_t a = g_physical_heap->Alloc(size, align);
  if (a) memset(G(a), 0, size);
  RT_TRACE("MmAllocatePhysicalMemoryEx(%X, align %X, protect %X) -> %08X", size, align, ARG32(5), a);
  RETURN(a);
}

GUEST_IMPORT(MmFreePhysicalMemory) {
  g_physical_heap->Free(ARG32(4));
  RETURN(0);
}

GUEST_IMPORT(MmGetPhysicalAddress) { RETURN(ARG32(3) & 0x1FFFFFFF); }
GUEST_IMPORT(MmQueryAddressProtect) { RETURN(4); }

GUEST_IMPORT(ExAllocatePool) {
  uint32_t a = g_system_heap->Alloc(ARG32(3), 0x10);
  RETURN(a);
}
GUEST_IMPORT(ExAllocatePoolTypeWithTag) {
  uint32_t a = g_system_heap->Alloc(ARG32(3), 0x10);
  RETURN(a);
}
GUEST_IMPORT(ExFreePool) {
  g_system_heap->Free(ARG32(3));
  RETURN(0);
}

// XamAlloc(flags, size, out*)
GUEST_IMPORT(XamAlloc) {
  uint32_t a = g_system_heap->Alloc(ARG32(4), 0x10);
  if (a) memset(G(a), 0, ARG32(4));
  Out32(ARG32(5), a);
  RETURN(a ? ERROR_SUCCESS : 14 /*ERROR_OUTOFMEMORY*/);
}
GUEST_IMPORT(XamFree) {
  if (ARG32(3)) g_system_heap->Free(ARG32(3));
  RETURN(ERROR_SUCCESS);
}

// ---- dispatcher objects (pointer forms) ------------------------------------------
GUEST_IMPORT(KeInitializeSemaphore) {
  InitDispatcherHeader(ARG32(3), kSemaphore, int32_t(ARG32(4)));
  St32(ARG32(3) + 0x10, ARG32(5));
  RETURN(0);
}
GUEST_IMPORT(KeSetEvent) { RETURN(SetEvent(ARG32(3))); }
GUEST_IMPORT(KeResetEvent) { RETURN(ResetEvent(ARG32(3))); }
GUEST_IMPORT(KeReleaseSemaphore) { RETURN(ReleaseSemaphore(ARG32(3), int32_t(ARG32(5)), 0)); }

// KeWaitForSingleObject(object, reason, mode, alertable, timeout*)
GUEST_IMPORT(KeWaitForSingleObject) {
  uint32_t o = ARG32(3);
  RETURN(WaitMultiple(&o, 1, false, ARG32(7), ARG32(6) != 0));
}

// KeWaitForMultipleObjects(count, objects*, wait_type, reason, mode, alertable, timeout*, blocks)
GUEST_IMPORT(KeWaitForMultipleObjects) {
  uint32_t n = ARG32(3);
  std::vector<uint32_t> objs(n);
  for (uint32_t i = 0; i < n; ++i) objs[i] = Ld32(ARG32(4) + i * 4);
  RETURN(WaitMultiple(objs.data(), n, ARG32(5) == 0, ARG32(9), ARG32(8) != 0));
}

// ---- dispatcher objects (handle forms) --------------------------------------------
static uint32_t NewObject(uint8_t type, int32_t state, uint32_t size = 0x20) {
  uint32_t o = g_system_heap->Alloc(size, 0x10);
  memset(G(o), 0, size);
  InitDispatcherHeader(o, type, state);
  return o;
}

static void WarnIfNamed(const char* what, uint32_t attrs) {
  if (attrs && Ld32(attrs + 4))
    RT_WARN("%s: named object '%s' created unnamed", what, ReadAnsiString(Ld32(attrs + 4)).c_str());
}

// NtCreateEvent(handle*, attrs, type, initial)
GUEST_IMPORT(NtCreateEvent) {
  WarnIfNamed("NtCreateEvent", ARG32(4));
  uint32_t o = NewObject(ARG32(5) == 0 ? kNotificationEvent : kSynchronizationEvent, ARG32(6) ? 1 : 0);
  St32(ARG32(3), CreateHandle(o));
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtSetEvent) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(4), SetEvent(o));
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtClearEvent) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  ResetEvent(o);
  RETURN(STATUS_SUCCESS);
}
// NtCreateSemaphore(handle*, attrs, count, limit)
GUEST_IMPORT(NtCreateSemaphore) {
  WarnIfNamed("NtCreateSemaphore", ARG32(4));
  uint32_t o = NewObject(kSemaphore, int32_t(ARG32(5)));
  St32(o + 0x10, ARG32(6));
  St32(ARG32(3), CreateHandle(o));
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtReleaseSemaphore) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  uint32_t prev = ReleaseSemaphore(o, int32_t(ARG32(4)), 0);
  if (prev & 0x80000000u) RETURN(STATUS_SEMAPHORE_LIMIT_EXCEEDED);
  Out32(ARG32(5), prev);
  RETURN(STATUS_SUCCESS);
}
// NtCreateMutant(handle*, attrs, initial_owner)
GUEST_IMPORT(NtCreateMutant) {
  WarnIfNamed("NtCreateMutant", ARG32(4));
  uint32_t o = NewObject(kMutant, 1);
  if (ARG32(5)) {
    St32(o + 4, 0);
    St32(o + 0x18, CurrentKThread());
  }
  St32(ARG32(3), CreateHandle(o));
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtReleaseMutant) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  uint32_t prev = ReleaseMutant(o, false, false);
  if (prev & 0x80000000u) RETURN(STATUS_MUTANT_NOT_OWNED);
  Out32(ARG32(4), prev);
  RETURN(STATUS_SUCCESS);
}
// NtCreateTimer(handle*, attrs, type)
GUEST_IMPORT(NtCreateTimer) {
  uint32_t o = NewObject(ARG32(5) == 0 ? kNotificationTimer : kSynchronizationTimer, 0, 0x30);
  St32(ARG32(3), CreateHandle(o));
  RETURN(STATUS_SUCCESS);
}
// NtSetTimerEx(handle, due*, apc_routine, apc_mode, apc_context, resume, period_ms, prev*)
GUEST_IMPORT(NtSetTimerEx) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  if (ARG32(5)) RT_WARN("NtSetTimerEx: APC routine %08X ignored", ARG32(5));
  Out32(ARG32(10), SetTimer(o, ARG32(4), int32_t(ARG32(9))));
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtCancelTimer) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(4), CancelTimer(o));
  RETURN(STATUS_SUCCESS);
}

// NtWaitForSingleObjectEx(handle, mode, alertable, timeout*)
GUEST_IMPORT(NtWaitForSingleObjectEx) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  RETURN(WaitMultiple(&o, 1, false, ARG32(6), ARG32(5) != 0));
}
// NtWaitForMultipleObjectsEx(count, handles*, wait_type, mode, alertable, timeout*)
GUEST_IMPORT(NtWaitForMultipleObjectsEx) {
  uint32_t n = ARG32(3);
  std::vector<uint32_t> objs(n);
  for (uint32_t i = 0; i < n; ++i)
    if (!(objs[i] = HandleToObject(Ld32(ARG32(4) + i * 4)))) RETURN(STATUS_INVALID_HANDLE);
  RETURN(WaitMultiple(objs.data(), n, ARG32(5) == 0, ARG32(8), ARG32(7) != 0));
}

GUEST_IMPORT(NtClose) { RETURN(CloseHandle(ARG32(3)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE); }
// NtDuplicateObject(handle, out*, options)
GUEST_IMPORT(NtDuplicateObject) {
  uint32_t n = DuplicateHandle(ARG32(3));
  if (!n) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(4), n);
  if (ARG32(5) & 1) CloseHandle(ARG32(3));  // DUPLICATE_CLOSE_SOURCE
  RETURN(STATUS_SUCCESS);
}
// ObReferenceObjectByHandle(handle, type, out*)
GUEST_IMPORT(ObReferenceObjectByHandle) {
  uint32_t o = HandleToObject(ARG32(3));
  if (!o) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(5), o);
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(ObDereferenceObject) { RETURN(0); }
GUEST_IMPORT(ObReferenceObject) { RETURN(0); }
GUEST_IMPORT(ObIsTitleObject) { RETURN(1); }
GUEST_IMPORT(ObCreateSymbolicLink) {
  RT_INFO("ObCreateSymbolicLink(%s -> %s)", ReadAnsiString(ARG32(3)).c_str(), ReadAnsiString(ARG32(4)).c_str());
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(ObDeleteSymbolicLink) { RETURN(STATUS_SUCCESS); }

// ---- critical sections and spinlocks ------------------------------------------------
// X_RTL_CRITICAL_SECTION: header (16), +0x10 lock_count (-1 = free),
// +0x14 recursion_count, +0x18 owning_thread.
static std::atomic<uint32_t>& Word(uint32_t a) { return *reinterpret_cast<std::atomic<uint32_t>*>(G(a)); }

GUEST_IMPORT(RtlInitializeCriticalSection) {
  uint32_t cs = ARG32(3);
  memset(G(cs), 0, 0x1C);
  St8(cs, 1);  // synchronization event type, as the XDK does
  St32(cs + 0x10, 0xFFFFFFFF);
  RETURN(0);
}
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) {
  uint32_t cs = ARG32(3);
  memset(G(cs), 0, 0x1C);
  St8(cs, 1);
  St32(cs + 0x10, 0xFFFFFFFF);
  RETURN(STATUS_SUCCESS);
}

static bool TryEnter(uint32_t cs, uint32_t self) {
  if (Ld32(cs + 0x18) == self) {
    St32(cs + 0x14, Ld32(cs + 0x14) + 1);
    return true;
  }
  uint32_t expected = 0xFFFFFFFF;  // -1 is the same in either byte order
  if (Word(cs + 0x10).compare_exchange_strong(expected, 0)) {
    St32(cs + 0x18, self);
    St32(cs + 0x14, 1);
    return true;
  }
  return false;
}

// A contended section blocks in WaitOnAddress on its lock word and Leave wakes it, so
// a waiter resumes as soon as the owner leaves (a plain sleep would last ~1 ms).
// Waiters are counted per address hash so an uncontended Leave skips the wake call.
static std::atomic<uint32_t> g_cs_waiters[256];
static std::atomic<uint32_t>& CsWaiters(uint32_t cs) { return g_cs_waiters[(cs >> 3) & 255]; }

GUEST_IMPORT(RtlEnterCriticalSection) {
  uint32_t cs = ARG32(3), self = CurrentKThread();
  for (int spin = 0; spin < 128; ++spin) {
    if (TryEnter(cs, self)) RETURN(0);
    if (spin < 64) _mm_pause();
    else std::this_thread::yield();
  }
  CsWaiters(cs).fetch_add(1);
  while (!TryEnter(cs, self)) {
    uint32_t held = 0;  // lock_count 0 = held once; anything else re-checks at once
    WaitOnAddress(G(cs + 0x10), &held, 4, 2);
  }
  CsWaiters(cs).fetch_sub(1);
  RETURN(0);
}
GUEST_IMPORT(RtlTryEnterCriticalSection) { RETURN(TryEnter(ARG32(3), CurrentKThread()) ? 1 : 0); }
GUEST_IMPORT(RtlLeaveCriticalSection) {
  uint32_t cs = ARG32(3);
  uint32_t rec = Ld32(cs + 0x14) - 1;
  St32(cs + 0x14, rec);
  if (rec == 0) {
    St32(cs + 0x18, 0);
    Word(cs + 0x10).store(0xFFFFFFFF);
    if (CsWaiters(cs).load()) WakeByAddressSingle(G(cs + 0x10));
  }
  RETURN(0);
}

static void SpinAcquire(uint32_t lock) {
  uint32_t self = __builtin_bswap32(CurrentKThread() | 1);
  for (int spin = 0;; ++spin) {
    uint32_t expected = 0;
    if (Word(lock).compare_exchange_weak(expected, self)) return;
    if (spin < 64) _mm_pause();
    else std::this_thread::yield();
  }
}
GUEST_IMPORT(KfAcquireSpinLock) { SpinAcquire(ARG32(3)); RETURN(0); }
GUEST_IMPORT(KfReleaseSpinLock) { Word(ARG32(3)).store(0); RETURN(0); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql) { SpinAcquire(ARG32(3)); RETURN(0); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { Word(ARG32(3)).store(0); RETURN(0); }
GUEST_IMPORT(KeRaiseIrqlToDpcLevel) { RETURN(0); }
GUEST_IMPORT(KfLowerIrql) { RETURN(0); }
GUEST_IMPORT(KeEnterCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeLeaveCriticalRegion) { RETURN(0); }
GUEST_IMPORT(KeLockL2) { RETURN(0); }
GUEST_IMPORT(KeUnlockL2) { RETURN(0); }

// ---- Rtl ----------------------------------------------------------------------------
GUEST_IMPORT(RtlInitAnsiString) {
  uint32_t dst = ARG32(3), src = ARG32(4);
  uint16_t len = src ? uint16_t(strlen(G<const char>(src))) : 0;
  St16(dst, len);
  St16(dst + 2, src ? len + 1 : 0);
  St32(dst + 4, src);
  RETURN(0);
}
GUEST_IMPORT(RtlFillMemoryUlong) {
  uint32_t dst = ARG32(3), len = ARG32(4), v = ARG32(5);
  for (uint32_t i = 0; i + 4 <= len; i += 4) St32(dst + i, v);
  RETURN(0);
}
GUEST_IMPORT(RtlCompareMemoryUlong) {
  uint32_t src = ARG32(3), len = ARG32(4), v = ARG32(5), i = 0;
  while (i + 4 <= len && Ld32(src + i) == v) i += 4;
  RETURN(i);
}
GUEST_IMPORT(RtlNtStatusToDosError) {
  uint32_t s = ARG32(3);
  if (s == STATUS_SUCCESS) RETURN(0);
  switch (s) {
    case STATUS_OBJECT_NAME_NOT_FOUND: RETURN(2);   // ERROR_FILE_NOT_FOUND
    case STATUS_OBJECT_PATH_NOT_FOUND: RETURN(3);   // ERROR_PATH_NOT_FOUND
    case STATUS_ACCESS_DENIED: RETURN(5);
    case STATUS_INVALID_HANDLE: RETURN(6);
    case STATUS_NO_MEMORY: RETURN(8);
    case STATUS_END_OF_FILE: RETURN(38);            // ERROR_HANDLE_EOF
    case STATUS_NOT_SUPPORTED: RETURN(50);
    case STATUS_INVALID_PARAMETER: RETURN(87);
    case STATUS_OBJECT_NAME_COLLISION: RETURN(183); // ERROR_ALREADY_EXISTS
    case STATUS_NO_MORE_FILES: RETURN(ERROR_NO_MORE_FILES);
    case STATUS_PENDING: RETURN(ERROR_IO_PENDING);
    case STATUS_TIMEOUT: RETURN(1460);              // ERROR_TIMEOUT
    default: RETURN(317);                           // ERROR_MR_MID_NOT_FOUND
  }
}
GUEST_IMPORT(RtlUpcaseUnicodeChar) { RETURN(towupper(wchar_t(ARG32(3)))); }
GUEST_IMPORT(RtlCompareStringN) {
  // (str1, len1, str2, len2, case_insensitive)
  std::string a(G<const char>(ARG32(3)), ARG32(4)), b(G<const char>(ARG32(5)), ARG32(6));
  int r = ARG32(7) ? _stricmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str());
  RETURN(int64_t(r));
}
// RtlMultiByteToUnicodeN(dst, dst_bytes, written*, src, src_bytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN) {
  uint32_t n = std::min(ARG32(4) / 2, ARG32(7));
  for (uint32_t i = 0; i < n; ++i) St16(ARG32(3) + i * 2, *G<uint8_t>(ARG32(6) + i));
  Out32(ARG32(5), n * 2);
  RETURN(STATUS_SUCCESS);
}
// RtlUnicodeToMultiByteN(dst, dst_bytes, written*, src, src_bytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN) {
  uint32_t n = std::min(ARG32(4), ARG32(7) / 2);
  for (uint32_t i = 0; i < n; ++i) {
    uint16_t c = Ld16(ARG32(6) + i * 2);
    St8(ARG32(3) + i, c < 0x100 ? uint8_t(c) : '?');
  }
  Out32(ARG32(5), n);
  RETURN(STATUS_SUCCESS);
}
// RtlTimeToTimeFields(time*, fields*): fields are be16 year, month, day, hour, minute, second, ms, weekday
GUEST_IMPORT(RtlTimeToTimeFields) {
  uint64_t t = Ld64(ARG32(3));
  FILETIME ft{DWORD(t), DWORD(t >> 32)};
  SYSTEMTIME st;
  FileTimeToSystemTime(&ft, &st);
  uint32_t f = ARG32(4);
  St16(f + 0, st.wYear); St16(f + 2, st.wMonth); St16(f + 4, st.wDay); St16(f + 6, st.wHour);
  St16(f + 8, st.wMinute); St16(f + 10, st.wSecond); St16(f + 12, st.wMilliseconds);
  St16(f + 14, st.wDayOfWeek);
  RETURN(0);
}
GUEST_IMPORT(RtlTimeFieldsToTime) {
  uint32_t f = ARG32(3);
  SYSTEMTIME st{Ld16(f), Ld16(f + 2), Ld16(f + 14), Ld16(f + 4), Ld16(f + 6), Ld16(f + 8), Ld16(f + 10), Ld16(f + 12)};
  FILETIME ft;
  if (!SystemTimeToFileTime(&st, &ft)) RETURN(0);
  St64(ARG32(4), (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
  RETURN(1);
}
// RtlImageXexHeaderField(xex_header, key) -> value or pointer
GUEST_IMPORT(RtlImageXexHeaderField) {
  uint32_t hdr = ARG32(3), key = ARG32(4);
  uint32_t count = Ld32(hdr + 0x14);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t e = hdr + 0x18 + i * 8;
    if (Ld32(e) != key) continue;
    uint32_t v = Ld32(e + 4);
    if ((key & 0xFF) == 0) RETURN(v);
    if ((key & 0xFF) == 1) RETURN(e + 4);
    RETURN(hdr + v);
  }
  RETURN(0);
}

// ---- strings / debug output ---------------------------------------------------------
GUEST_IMPORT(sprintf) {
  std::string s = GuestFormat(ctx, ARG32(4), 2);
  memcpy(G(ARG32(3)), s.c_str(), s.size() + 1);
  RETURN(s.size());
}
GUEST_IMPORT(_snprintf) {
  std::string s = GuestFormat(ctx, ARG32(5), 3);
  uint32_t cap = ARG32(4);
  if (cap == 0) RETURN(-1);
  size_t n = std::min<size_t>(s.size(), cap);
  memcpy(G(ARG32(3)), s.data(), n);
  if (n < cap) St8(ARG32(3) + uint32_t(n), 0);
  RETURN(s.size() < cap ? int64_t(s.size()) : -1);
}
GUEST_IMPORT(_vsnprintf) {
  std::string s = GuestFormatVa(ARG32(5), ARG32(6));
  uint32_t cap = ARG32(4);
  if (cap == 0) RETURN(-1);
  size_t n = std::min<size_t>(s.size(), cap);
  memcpy(G(ARG32(3)), s.data(), n);
  if (n < cap) St8(ARG32(3) + uint32_t(n), 0);
  RETURN(s.size() < cap ? int64_t(s.size()) : -1);
}
GUEST_IMPORT(DbgPrint) {
  std::string s = GuestFormat(ctx, ARG32(3), 1);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  RT_INFO("DbgPrint: %s", s.c_str());
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(DbgBreakPoint) {
  RT_WARN("DbgBreakPoint");
  RETURN(0);
}
GUEST_IMPORT(KeBugCheck) { Fatal("KeBugCheck(%08X)", ARG32(3)); }
GUEST_IMPORT(KeBugCheckEx) { Fatal("KeBugCheckEx(%08X, %08X, %08X, %08X, %08X)", ARG32(3), ARG32(4), ARG32(5), ARG32(6), ARG32(7)); }
// EXCEPTION_RECORD (32-bit): code, flags, record, address, num_params, info[].
GUEST_IMPORT(RtlRaiseException) {
  uint32_t rec = ARG32(3), code = Ld32(rec);
  if (code == 0x406D1388 && Ld32(rec + 0x14) == 0x1000) {  // MSVC SetThreadName
    std::string name = ReadCString(Ld32(rec + 0x18));
    uint32_t tid = Ld32(rec + 0x1C);
    RT_INFO("thread %u named '%s'", tid == 0xFFFFFFFF ? CurrentThreadId() : tid, name.c_str());
    if (tid == 0xFFFFFFFF || tid == CurrentThreadId()) {
      std::wstring w(name.begin(), name.end());
      SetThreadDescription(GetCurrentThread(), w.c_str());
    }
    RETURN(0);
  }
  Fatal("RtlRaiseException(record %08X, code %08X)", rec, code);
}
GUEST_IMPORT(HalReturnToFirmware) {
  RT_INFO("HalReturnToFirmware(%u): exiting", ARG32(3));
  fflush(nullptr);
  ExitProcess(0);
}

// ---- time ---------------------------------------------------------------------------
GUEST_IMPORT(KeQueryPerformanceFrequency) { RETURN(kGuestTimebase); }
GUEST_IMPORT(KeQuerySystemTime) {
  St64(ARG32(3), GuestFileTime());
  RETURN(0);
}

// ---- modules ------------------------------------------------------------------------
// XexGetModuleHandle(name, handle*)
GUEST_IMPORT(XexGetModuleHandle) {
  std::string name = ReadCString(ARG32(3));
  if (ARG32(3) == 0 || _stricmp(name.c_str(), "default.xex") == 0) {
    Out32(ARG32(4), g_module.ldr_entry);
    RETURN(STATUS_SUCCESS);
  }
  RT_WARN("XexGetModuleHandle(%s): not found", name.c_str());
  Out32(ARG32(4), 0);
  RETURN(STATUS_NOT_SUPPORTED | 0);
}
GUEST_IMPORT(XexGetProcedureAddress) {
  RT_WARN("XexGetProcedureAddress(%08X, %u): unsupported", ARG32(3), ARG32(4));
  Out32(ARG32(5), 0);
  RETURN(STATUS_NOT_SUPPORTED);
}
// XexLoadImage(name, flags, min_version, handle*): no extra modules on this host.
// BF1943 loads xam.xex only for the optional Party API.
GUEST_IMPORT(XexLoadImage) {
  RT_INFO("XexLoadImage(%s): not available", ReadCString(ARG32(3)).c_str());
  Out32(ARG32(6), 0);
  RETURN(STATUS_NO_SUCH_FILE);
}
GUEST_IMPORT(XexUnloadImage) { RETURN(STATUS_SUCCESS); }
GUEST_IMPORT(XexCheckExecutablePrivilege) { RETURN(0); }
GUEST_IMPORT(ExRegisterTitleTerminateNotification) { RETURN(0); }

// ExGetXConfigSetting(category, setting, buffer, size, required*)
GUEST_IMPORT(ExGetXConfigSetting) {
  uint16_t cat = uint16_t(ARG32(3)), id = uint16_t(ARG32(4));
  uint32_t buf = ARG32(5), size = ARG32(6);
  uint32_t value = 0, len = 4;
  bool known = true;
  if (cat == 3 && id == 9) value = 1;            // user language: English
  else if (cat == 3 && id == 10) value = 0x00400000;  // video flags: widescreen
  else if (cat == 3) value = 0;
  else if (cat == 2 && id == 2) { value = 0xFF; len = 2; }  // game region
  else known = false;
  if (!known) RT_WARN("ExGetXConfigSetting(%u, %u): unknown, returning 0", cat, id);
  if (buf && size >= len) {
    if (len == 4) St32(buf, value);
    else St16(buf, uint16_t(value));
  }
  Out32(ARG32(7), len);
  RETURN(STATUS_SUCCESS);
}

// XeCryptSha(in1, size1, in2, size2, in3, size3, out, out_size): SHA-1 of the three inputs.
GUEST_IMPORT(XeCryptSha) {
  uint8_t digest[20] = {};
  const uint32_t in[3][2] = {{ARG32(3), ARG32(4)}, {ARG32(5), ARG32(6)}, {ARG32(7), ARG32(8)}};
#ifdef _WIN32
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0) == 0) {
    for (auto& [p, n] : in)
      if (p && n) BCryptHashData(h, G<uint8_t>(p), n, 0);
    BCryptFinishHash(h, digest, 20, 0);
    BCryptDestroyHash(h);
  }
#else
  rt::Sha1 sha;
  for (auto& [p, n] : in)
    if (p && n) sha.Update(G<uint8_t>(p), n);
  sha.Finish(digest);
#endif
  if (ARG32(9)) memcpy(G(ARG32(9)), digest, std::min<uint32_t>(ARG32(10), 20));
  RETURN(0);
}
