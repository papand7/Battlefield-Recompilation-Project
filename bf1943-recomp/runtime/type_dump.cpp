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


// BF_FIND_VTABLE=<class>[,<class>...] (e.g. hkRigidBody): the vtables of those
// C++ classes through their MSVC RTTI in the image (".?AV<class>@@" type
// descriptor <- complete object locator +C <- vtable -4), as Bad Company's
// Havok classes were found.
const bool g_find_vtable = [] {
  const char* names = getenv("BF_FIND_VTABLE");
  if (!names) return false;
  std::string list = names;
  std::thread([list] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const uint32_t lo = 0x82000000, hi = 0x82E00000;
    for (size_t p = 0; p <= list.size();) {
      size_t c = list.find(',', p);
      if (c == std::string::npos) c = list.size();
      std::string want = ".?AV" + list.substr(p, c - p) + "@@";
      p = c + 1;
      int found = 0;
      for (uint32_t a = lo; a + want.size() < hi; ++a) {
        if (!IsCommitted(a) || memcmp(G<char>(a), want.c_str(), want.size() + 1) != 0) continue;
        uint32_t td = a - 8;
        ++found;
        for (uint32_t col = lo; col < hi; col += 4) {
          if (!IsCommitted(col) || Ld32(col + 12) != td) continue;
          for (uint32_t v = lo; v < hi; v += 4)
            if (IsCommitted(v) && Ld32(v) == col)
              RT_INFO("find vtable: %s type %08X locator %08X vtable %08X", want.c_str(), td, col, v + 4);
        }
      }
      if (!found) RT_INFO("find vtable: %s not in the image", want.c_str());
    }
    RT_INFO("find vtable: done");
  }).detach();
  return true;
}();


// BF_FIND_SLOT=<function>[,...] (hex): the vtable slots holding these
// functions (virtual calls), to see whose methods they are.
const bool g_find_slot = [] {
  const char* list = getenv("BF_FIND_SLOT");
  if (!list) return false;
  std::string l = list;
  std::thread([l] {
    std::this_thread::sleep_for(std::chrono::seconds(4));
    for (size_t p = 0; p <= l.size();) {
      size_t c = l.find(',', p);
      if (c == std::string::npos) c = l.size();
      uint32_t fn = uint32_t(strtoul(l.substr(p, c - p).c_str(), nullptr, 16));
      p = c + 1;
      for (uint32_t a = 0x82000000; a < 0x82180000; a += 4)
        if (IsCommitted(a) && Ld32(a) == fn) RT_INFO("find slot: %08X at %08X", fn, a);
    }
    RT_INFO("find slot: done");
  }).detach();
  return true;
}();


// BF_DUMP_VT=<vtable hex>,<bytes>: the words of a vtable (its methods).
const bool g_dump_vt = [] {
  const char* e = getenv("BF_DUMP_VT");
  if (!e) return false;
  uint32_t vt = 0, len = 0x100;
  sscanf(e, "%x,%x", &vt, &len);
  std::thread([vt, len] {
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::string out;
    for (uint32_t k = 0; k < len; k += 4) {
      char buf[24];
      snprintf(buf, sizeof(buf), " +%X:%08X", k, Ld32(vt + k));
      out += buf;
    }
    RT_INFO("dump vt %08X:%s", vt, out.c_str());
  }).detach();
  return true;
}();


// BF_FIND_STRINGS=<prefix>: every NUL-terminated string in the image starting
// with it (enum names: their order in memory).
const bool g_find_strings = [] {
  const char* pre = getenv("BF_FIND_STRINGS");
  if (!pre) return false;
  std::string p = pre;
  std::thread([p] {
    std::this_thread::sleep_for(std::chrono::seconds(4));
    for (uint32_t a = 0x82000000; a < 0x82E00000; ++a) {
      if (!IsCommitted(a) || memcmp(G<char>(a), p.c_str(), p.size()) != 0) continue;
      if (G<char>(a)[-1] != 0) continue;
      RT_INFO("find strings: %08X %s", a, std::string(G<char>(a)).substr(0, 60).c_str());
    }
    RT_INFO("find strings: done");
  }).detach();
  return true;
}();

}  // namespace
}  // namespace rt
