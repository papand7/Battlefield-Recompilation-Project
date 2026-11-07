// Linux (and other POSIX) builds: the few Win32 names the runtime uses all
// over, as thin wrappers, so most files need no #ifdef. Included by kernel.h
// and guest.h users through platform.h; empty on Windows.
#pragma once
#ifndef _WIN32

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>
#include <linux/futex.h>
#include <setjmp.h>
#include <strings.h>

using DWORD = uint32_t;
using HANDLE = void*;
using BOOL = int;

inline void ExitProcess(unsigned code) {
  fflush(nullptr);
  _exit(int(code));
}
constexpr int FALSE = 0, TRUE = 1;
constexpr DWORD INFINITE = 0xFFFFFFFFu;
constexpr int MAX_PATH = 260;
constexpr DWORD ERROR_OUTOFMEMORY = 14, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122,
                ERROR_INVALID_NAME = 123, ERROR_MR_MID_NOT_FOUND = 317, ERROR_TIMEOUT = 1460;

// Times: FILETIME is 100 ns ticks since 1601 (UTC).
struct FILETIME {
  DWORD dwLowDateTime, dwHighDateTime;
};
struct SYSTEMTIME {
  uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
constexpr uint64_t kFileTimeUnixOffset = 11644473600ull;  // seconds from 1601 to 1970
inline void GetSystemTimeAsFileTime(FILETIME* ft) {
  timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  uint64_t v = (uint64_t(t.tv_sec) + kFileTimeUnixOffset) * 10000000ull + uint64_t(t.tv_nsec) / 100;
  ft->dwLowDateTime = DWORD(v);
  ft->dwHighDateTime = DWORD(v >> 32);
}
inline BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
  uint64_t v = (uint64_t(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
  time_t secs = time_t(v / 10000000ull) - time_t(kFileTimeUnixOffset);
  tm g{};
  if (!gmtime_r(&secs, &g)) return 0;
  st->wYear = uint16_t(g.tm_year + 1900), st->wMonth = uint16_t(g.tm_mon + 1), st->wDayOfWeek = uint16_t(g.tm_wday);
  st->wDay = uint16_t(g.tm_mday), st->wHour = uint16_t(g.tm_hour), st->wMinute = uint16_t(g.tm_min);
  st->wSecond = uint16_t(g.tm_sec), st->wMilliseconds = uint16_t((v / 10000ull) % 1000);
  return 1;
}
inline BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
  tm g{};
  g.tm_year = st->wYear - 1900, g.tm_mon = st->wMonth - 1, g.tm_mday = st->wDay;
  g.tm_hour = st->wHour, g.tm_min = st->wMinute, g.tm_sec = st->wSecond;
  time_t secs = timegm(&g);
  if (secs == time_t(-1)) return 0;
  uint64_t v = (uint64_t(secs) + kFileTimeUnixOffset) * 10000000ull + uint64_t(st->wMilliseconds) * 10000ull;
  ft->dwLowDateTime = DWORD(v);
  ft->dwHighDateTime = DWORD(v >> 32);
  return 1;
}

// Thread names (SetThreadDescription(GetCurrentThread(), L"name")).
inline HANDLE GetCurrentThread() { return nullptr; }
inline long SetThreadDescription(HANDLE, const wchar_t* name) {
  char n[16];
  size_t i = 0;
  for (; name[i] && i < 15; ++i) n[i] = char(name[i]);
  n[i] = 0;
  pthread_setname_np(pthread_self(), n);
  return 0;
}
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }

// WaitOnAddress / WakeByAddressSingle (4-byte values) on a futex.
inline BOOL WaitOnAddress(volatile void* address, void* compare, size_t size, DWORD ms) {
  if (size != 4) {
    sched_yield();
    return 1;
  }
  timespec t{time_t(ms / 1000), long(ms % 1000) * 1000000L};
  syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, *static_cast<uint32_t*>(compare), ms == 0xFFFFFFFFu ? nullptr : &t,
          nullptr, 0);
  return 1;
}
inline void WakeByAddressSingle(void* address) { syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0); }
inline void WakeByAddressAll(void* address) { syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, nullptr, nullptr, 0); }

// A guarded call that may fault (SEH __try on Windows): the SIGSEGV handler
// jumps back here while it is set (main.cpp).
namespace rt {
extern thread_local sigjmp_buf* g_fault_guard;
}

// MSVC byte-swap intrinsics.
inline uint16_t _byteswap_ushort(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t _byteswap_ulong(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }

inline DWORD GetCurrentThreadId() { return DWORD(syscall(SYS_gettid)); }
inline DWORD GetCurrentProcessId() { return DWORD(getpid()); }
inline void Sleep(DWORD ms) {
  timespec t{time_t(ms / 1000), long(ms % 1000) * 1000000L};
  nanosleep(&t, nullptr);
}
inline void SwitchToThread() { sched_yield(); }
inline DWORD GetLastError() { return DWORD(errno); }
inline unsigned timeBeginPeriod(unsigned) { return 0; }
inline uint64_t GetTickCount64() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}

#endif  // !_WIN32
