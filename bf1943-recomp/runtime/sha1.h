// SHA-1 (FIPS 180-1), for XeCryptSha where the host has no BCrypt (Linux).
#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>

namespace rt {

class Sha1 {
 public:
  void Update(const uint8_t* p, size_t n) {
    total_ += n;
    while (n) {
      size_t take = std::min(n, size_t(64) - used_);
      memcpy(block_ + used_, p, take);
      used_ += take, p += take, n -= take;
      if (used_ == 64) Block(), used_ = 0;
    }
  }
  void Finish(uint8_t out[20]) {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    Update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) Update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    Update(len, 8);
    for (int i = 0; i < 5; ++i)
      for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(h_[i] >> (24 - 8 * j));
  }

 private:
  static uint32_t Rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
  void Block() {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(block_[i * 4]) << 24 | uint32_t(block_[i * 4 + 1]) << 16 | uint32_t(block_[i * 4 + 2]) << 8 |
             block_[i * 4 + 3];
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = Rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = Rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
  }
  uint32_t h_[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint8_t block_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};

}  // namespace rt
