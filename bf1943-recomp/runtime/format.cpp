// printf-style formatting with guest arguments (kernel sprintf, _snprintf,
// _vsnprintf, DbgPrint). Xbox 360 varargs are 64-bit slots: r3..r10, then the
// parameter area at r1+0x50. Doubles travel in the integer slots as raw bits.
#include "guest.h"

#include <cstring>

namespace rt {

namespace {
struct ArgReader {
  virtual ~ArgReader() = default;
  virtual uint64_t Next() = 0;
};

struct RegArgs : ArgReader {
  PPCContext& ctx;
  int index;
  explicit RegArgs(PPCContext& c, int first) : ctx(c), index(first) {}
  uint64_t Next() override {
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    int i = index++;
    if (i < 8) return regs[i]->u64;
    return Ld64(ctx.r1.u32 + 0x50 + (i - 8) * 8);
  }
};

// va_list form: the guest va_list points at an array of 64-bit slots.
struct VaListArgs : ArgReader {
  uint32_t ptr;
  explicit VaListArgs(uint32_t p) : ptr(p) {}
  uint64_t Next() override {
    uint64_t v = Ld64(ptr);
    ptr += 8;
    return v;
  }
};

std::string Format(uint32_t fmt_ptr, ArgReader& args) {
  std::string out;
  const char* f = G<const char>(fmt_ptr);
  while (*f) {
    if (*f != '%') { out += *f++; continue; }
    const char* spec_start = f++;
    if (*f == '%') { out += '%'; ++f; continue; }
    std::string spec = "%";
    while (*f && strchr("-+ #0", *f)) spec += *f++;
    if (*f == '*') { spec += std::to_string(int32_t(args.Next())); ++f; }
    while (*f >= '0' && *f <= '9') spec += *f++;
    if (*f == '.') {
      spec += *f++;
      if (*f == '*') { spec += std::to_string(int32_t(args.Next())); ++f; }
      while (*f >= '0' && *f <= '9') spec += *f++;
    }
    int longs = 0;
    for (;; ++f) {
      if (*f == 'l') ++longs;
      else if (*f == 'h') {}
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { longs = 2; f += 2; }
      else if (*f == 'L' || *f == 'z' || *f == 'j' || *f == 't') longs = 2;
      else break;
    }
    char conv = *f ? *f++ : 0;
    char buf[512];
    switch (conv) {
      case 'd': case 'i': {
        uint64_t v = args.Next();
        snprintf(buf, sizeof(buf), (spec + "lld").c_str(), longs >= 2 ? int64_t(v) : int64_t(int32_t(v)));
        out += buf;
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        uint64_t v = args.Next();
        if (longs < 2) v = uint32_t(v);
        snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), (unsigned long long)v);
        out += buf;
        break;
      }
      case 'p': {
        snprintf(buf, sizeof(buf), "%08X", uint32_t(args.Next()));
        out += buf;
        break;
      }
      case 'c': out += char(args.Next()); break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        uint64_t bits = args.Next();
        double d;
        memcpy(&d, &bits, 8);
        snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
        out += buf;
        break;
      }
      case 's': case 'S': {
        uint32_t p = uint32_t(args.Next());
        bool is_wide = (conv == 's' && longs > 0) || (conv == 'S' && longs == 0);
        std::string s;
        if (!p) s = "(null)";
        else if (is_wide) {
          for (uint32_t a = p; Ld16(a); a += 2) s += char(Ld16(a) & 0x7F);
        } else s = G<const char>(p);
        snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
        out += buf;
        break;
      }
      case 'n': args.Next(); break;
      default: out.append(spec_start, f); break;
    }
  }
  return out;
}
}  // namespace

std::string GuestFormat(PPCContext& ctx, uint32_t fmt, int first_gpr) {
  RegArgs args(ctx, first_gpr);
  return Format(fmt, args);
}

std::string GuestFormatVa(uint32_t fmt, uint32_t va_list) {
  VaListArgs args(va_list);
  return Format(fmt, args);
}

}  // namespace rt
