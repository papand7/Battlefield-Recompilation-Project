// Force-included (/FI) when building XenonRecomp tools with MSVC cl.exe.
// Upstream XenonRecomp targets Clang; the *tools* (not generated output) only
// use these GCC/Clang builtins, so map them to MSVC intrinsics.
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#include <intrin.h>
#define __builtin_bswap16(x) _byteswap_ushort(x)
#define __builtin_bswap32(x) _byteswap_ulong(x)
#define __builtin_bswap64(x) _byteswap_uint64(x)
static __inline int __builtin_ffsll(long long v)
{
    unsigned long idx;
    return _BitScanForward64(&idx, (unsigned long long)v) ? (int)idx + 1 : 0;
}
#endif
