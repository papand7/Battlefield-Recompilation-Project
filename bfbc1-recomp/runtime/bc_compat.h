// Bad Company's runtime on Windows and Linux: reading guest memory that can be
// freed under the reading thread (heap scans and watchers off the game's
// threads). False where it isn't mapped any more, instead of a crash.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
inline bool SafeRead(void* dst, const void* src, size_t n) {
  SIZE_T got = 0;
  return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
#else
#include <sys/uio.h>
#include <unistd.h>
inline bool SafeRead(void* dst, const void* src, size_t n) {
  iovec local{dst, n}, remote{const_cast<void*>(src), n};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
}
#endif
