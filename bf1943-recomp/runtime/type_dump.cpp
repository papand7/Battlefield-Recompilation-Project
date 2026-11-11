// DEBUG BF_TYPE_DUMP=<file>: every class with an engine type name, "vtable
// getType name" per line (both games: for finding one game's functions in
// the other, as Bad Company's bot functions in BF1943).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "guest.h"
#include "kernel.h"

namespace rt {
namespace {
// A vtable's +8 is getType: lis / addi r3 / blr; the type's +4 points at a
// record whose first word is the name.
const bool g_type_dump = [] {
  const char* path = getenv("BF_TYPE_DUMP");
  if (!path) return false;
  std::string p = path;
  std::thread([p] {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    FILE* f = fopen(p.c_str(), "w");
    if (!f) return;
    int n = 0;
    for (uint32_t v = 0x82000000; v < 0x82180000; v += 4) {
      uint32_t fn = Ld32(v + 8);
      if (fn < 0x82180000 || fn >= 0x82D00000 || (fn & 3)) continue;
      uint32_t i0 = Ld32(fn), i1 = Ld32(fn + 4), i2 = Ld32(fn + 8);
      // lis rX, hi / addi r3, rX, lo / blr (BF1943: r11; Bad Company: r3)
      uint32_t rx = (i0 >> 21) & 31;
      if ((i0 & 0xFC1F0000) != 0x3C000000 || (i1 & 0xFFE00000) != 0x38600000 || ((i1 >> 16) & 31) != rx ||
          i2 != 0x4E800020)
        continue;
      uint32_t type = (i0 << 16) + uint32_t(int32_t(int16_t(i1 & 0xFFFF)));
      if (type < 0x82000000 || type >= 0x83000000) continue;
      uint32_t rec = Ld32(type + 4);
      if (rec < 0x82000000 || rec >= 0x83000000) continue;
      uint32_t name = Ld32(rec);
      if (name < 0x82000000 || name >= 0x83000000) continue;
      const char* s = G<char>(name);
      size_t len = strnlen(s, 96);
      bool ok = len > 2;
      for (size_t k = 0; k < len && ok; ++k) ok = s[k] > 0x20 && s[k] < 0x7F;
      if (!ok) continue;
      fprintf(f, "%08X %08X %.*s\n", v, fn, int(len), s);
      ++n;
    }
    fclose(f);
    RT_INFO("type dump: %d classes to %s", n, p.c_str());
  }).detach();
  return true;
}();

}  // namespace
}  // namespace rt
