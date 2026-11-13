// Bad Company's collision tool (bfbc1-recomp/runtime/bc_bots.cpp) for BF1943,
// for the bots' navigation meshes. Private: the collision comes from the
// player's own copy and stays on that PC; tools/navgen makes nav/<map>.nav from
// it and the terrain (tools/terrain.py), see tools/make_navmesh.py.
//
//   BF_BOT_DUMP_COLLISION=<file.obj> bf1943 --dedicated --host-map <map> ...
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "bot_llm.h"
#include "guest.h"
#include "kernel.h"

using namespace rt;

namespace {

// BF1943's Havok 4.6 classes (Bad Company: 820BDE74, 820BD8A0).
constexpr uint32_t kRigidBody = 0x820B07CC;  // hkRigidBody
constexpr uint32_t kWorld = 0x820B0748;      // hkWorld

// Every live object with this vtable in the heaps.
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    for (uint32_t i = 0; i < 0x1000; i += 4)
      if (Ld32(uint32_t(page) + i) == vtable) out.push_back(uint32_t(page) + i);
  }
  return out;
}
float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// BF_BOT_DUMP_COLLISION=<file.obj> (the navigation tool, private: it is made
// from the player's own copy and stays on that PC): once the level is up,
// every Havok rigid body in memory (BF1943: vtable 820B07CC, found through its RTTI)
// and its shapes, as triangles in map coordinates. A body's transform at +B0
// (rotation columns +B0/+C0/+D0, translation +E0), its shape at +10. The game
// fills in what the level files leave out (placements, mesh pointers), so the
// live world has the whole collision. Shapes (Havok 4.6, no RTTI) told apart
// by their fields: a MOPP tree (+14 its child, +18 its code), an extended mesh
// (+50 triangle subparts: 64 bytes, +10 triangles, +14 vertices, +18 vertex
// stride, +1C vertex count, +24 indices, +34 index stride, +38 1 for 16-bit;
// +58 shape subparts: 64 bytes, +10 child pointer array, +20 translation, +30
// rotation), a convex hull (+40 FourVectors vertices, +4C their count, +50
// plane equations, +54 their count).
static bool Ptr(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && !(a & 3) && IsCommitted(a); }
struct Mat { float c[3][3]; float t[3]; };
static void Apply(const Mat& m, const float v[3], float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = m.c[0][i] * v[0] + m.c[1][i] * v[1] + m.c[2][i] * v[2] + m.t[i];
}
static Mat FromQuat(const float q[4], const float t[3]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat m;
  m.c[0][0] = 1 - 2 * (y * y + z * z), m.c[0][1] = 2 * (x * y + z * w), m.c[0][2] = 2 * (x * z - y * w);
  m.c[1][0] = 2 * (x * y - z * w), m.c[1][1] = 1 - 2 * (x * x + z * z), m.c[1][2] = 2 * (y * z + x * w);
  m.c[2][0] = 2 * (x * z + y * w), m.c[2][1] = 2 * (y * z - x * w), m.c[2][2] = 1 - 2 * (x * x + y * y);
  for (int i = 0; i < 3; ++i) m.t[i] = t[i];
  return m;
}
static Mat Mul(const Mat& a, const Mat& b) {  // a after b
  Mat m;
  for (int j = 0; j < 3; ++j) Apply(Mat{{{a.c[0][0], a.c[0][1], a.c[0][2]}, {a.c[1][0], a.c[1][1], a.c[1][2]},
                                         {a.c[2][0], a.c[2][1], a.c[2][2]}}, {0, 0, 0}}, b.c[j], m.c[j]);
  Apply(a, b.t, m.t);
  return m;
}
struct CollisionOut {
  FILE* f;
  size_t verts = 0, tris = 0;
  float clip_r = 0, clip_x = 0, clip_z = 0;
  std::vector<std::array<float, 9>>* mem = nullptr;
  std::map<uint64_t, std::pair<int, uint32_t>> unhandled;  // shapes left out: (type, vtable) -> count, one of them  // clip_r > 0: only triangles touching that circle
  void Tri(const Mat& m, const float a[3], const float b[3], const float c[3]) {
    float w[3][3];
    Apply(m, a, w[0]), Apply(m, b, w[1]), Apply(m, c, w[2]);
    if (clip_r > 0 && std::hypot(w[0][0] - clip_x, w[0][2] - clip_z) > clip_r &&
        std::hypot(w[1][0] - clip_x, w[1][2] - clip_z) > clip_r && std::hypot(w[2][0] - clip_x, w[2][2] - clip_z) > clip_r)
      return;
    if (mem) {  // kept in memory instead (the bots' line of sight)
      mem->push_back({w[0][0], w[0][1], w[0][2], w[1][0], w[1][1], w[1][2], w[2][0], w[2][1], w[2][2]});
      ++tris;
      return;
    }
    for (auto& v : w) fprintf(f, "v %.3f %.3f %.3f\n", v[0], v[1], v[2]);
    fprintf(f, "f %zu %zu %zu\n", verts + 1, verts + 2, verts + 3);
    verts += 3, ++tris;
  }
};
static void ConvexOut(CollisionOut& out, const Mat& m, uint32_t s) {
  uint32_t vp = Ld32(s + 0x40), n = Ld32(s + 0x4C), pp = Ld32(s + 0x50), np = Ld32(s + 0x54);
  if (!Ptr(vp) || !Ptr(pp) || n < 3 || n > 4096 || np > 4096) return;
  std::vector<std::array<float, 3>> v(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t blk = vp + 48 * (i / 4), j = i % 4;
    v[i] = {LdF(blk + 4 * j), LdF(blk + 16 + 4 * j), LdF(blk + 32 + 4 * j)};
  }
  float size = 0.01f;
  for (auto& q : v)
    for (float c : q) size = std::max(size, std::fabs(c));
  for (uint32_t k = 0; k < np; ++k) {
    float nx = LdF(pp + 16 * k), ny = LdF(pp + 16 * k + 4), nz = LdF(pp + 16 * k + 8), d = LdF(pp + 16 * k + 12);
    std::vector<std::array<float, 3>> on;
    for (auto& q : v)
      if (std::fabs(nx * q[0] + ny * q[1] + nz * q[2] + d) < 0.01f * size + 1e-3f) on.push_back(q);
    if (on.size() < 3) continue;
    float cx = 0, cy = 0, cz = 0;
    for (auto& q : on) cx += q[0], cy += q[1], cz += q[2];
    cx /= on.size(), cy /= on.size(), cz /= on.size();
    float ax[3] = {1, 0, 0};
    if (std::fabs(nx) > 0.9f) ax[0] = 0, ax[1] = 1;
    float ux[3] = {ny * ax[2] - nz * ax[1], nz * ax[0] - nx * ax[2], nx * ax[1] - ny * ax[0]};
    float ul = std::max(std::sqrt(ux[0] * ux[0] + ux[1] * ux[1] + ux[2] * ux[2]), 1e-6f);
    for (float& c : ux) c /= ul;
    float vx[3] = {ny * ux[2] - nz * ux[1], nz * ux[0] - nx * ux[2], nx * ux[1] - ny * ux[0]};
    std::sort(on.begin(), on.end(), [&](const auto& p, const auto& q) {
      auto ang = [&](const auto& r) {
        float d0 = r[0] - cx, d1 = r[1] - cy, d2 = r[2] - cz;
        return std::atan2(d0 * vx[0] + d1 * vx[1] + d2 * vx[2], d0 * ux[0] + d1 * ux[1] + d2 * ux[2]);
      };
      return ang(p) < ang(q);
    });
    for (size_t i = 1; i + 1 < on.size(); ++i) out.Tri(m, on[0].data(), on[i].data(), on[i + 1].data());
  }
}
// A box (half extents at +20) as 12 triangles.
static void BoxOut(CollisionOut& out, const Mat& m, uint32_t s) {
  float h[3] = {LdF(s + 0x20), LdF(s + 0x24), LdF(s + 0x28)};
  if (!(h[0] > 0 && h[1] > 0 && h[2] > 0 && h[0] < 1e4f && h[1] < 1e4f && h[2] < 1e4f)) return;
  float c[8][3];
  for (int i = 0; i < 8; ++i)
    c[i][0] = (i & 1) ? h[0] : -h[0], c[i][1] = (i & 2) ? h[1] : -h[1], c[i][2] = (i & 4) ? h[2] : -h[2];
  static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  for (auto& f : faces) out.Tri(m, c[f[0]], c[f[1]], c[f[2]]), out.Tri(m, c[f[0]], c[f[2]], c[f[3]]);
}
// Havok 4.6 shape types (hkShape +C): 3 sphere, 6 box, 7 capsule, 8 convex
// vertices, 10 list, 11 MOPP tree, 15 extended mesh, 0x18 the terrain's
// sampled heightfield (the terrain comes from its own file).
static void ShapeOut(CollisionOut& out, const Mat& m, uint32_t s, int depth) {
  if (!Ptr(s) || depth > 4) return;
  uint32_t type = Ld32(s + 0xC);
  if (type == 8) {
    ConvexOut(out, m, s);
    return;
  }
  if (type == 6) {
    BoxOut(out, m, s);
    return;
  }
  if (type == 11) {  // MOPP tree: its child
    ShapeOut(out, m, Ld32(s + 0x14), depth + 1);
    return;
  }
  if (type == 10) {  // list: +18 child entries (16 bytes, the shape first), +1C their count
    uint32_t arr = Ld32(s + 0x18), n = Ld32(s + 0x1C);
    for (uint32_t i = 0; Ptr(arr) && i < n && i < 4096; ++i) ShapeOut(out, m, Ld32(arr + 16 * i), depth + 1);
    return;
  }
  if (type != 15) {  // not turned into triangles: noted (type, vtable -> count, an example)
    float o[3] = {0, 0, 0}, w[3];
    if (type == 7) o[0] = LdF(s + 0x20), o[1] = LdF(s + 0x24), o[2] = LdF(s + 0x28);  // a capsule: its first end
    Apply(m, o, w);
    if (out.clip_r > 0 && std::hypot(w[0] - out.clip_x, w[2] - out.clip_z) > out.clip_r) return;
    auto& u = out.unhandled[(uint64_t(type) << 32) | Ld32(s)];
    if (!u.first++) u.second = s;
    return;
  }
  uint32_t tri = Ld32(s + 0x50), ntri = Ld32(s + 0x54), sub = Ld32(s + 0x58), nsub = Ld32(s + 0x5C);
  if (type == 15) {
    for (uint32_t e = 0; Ptr(tri) && e < ntri && e < 100000; ++e) {
      uint32_t t = tri + 64 * e;
      // A triangles subpart (Havok 4.6): +10 triangles, +14 vertices, +18 their
      // stride, +1C their count, +20 the extrusion (a vector), +30 indices, +34
      // their stride, +38 the index type (1: 16-bit).
      uint32_t n = Ld32(t + 0x10), vb = Ld32(t + 0x14), vs = Ld32(t + 0x18), ib = Ld32(t + 0x30), is = Ld32(t + 0x34);
      bool i16 = (Ld32(t + 0x38) >> 24) == 1;
      if (!Ptr(vb) || !Ptr(ib) || n > 1000000 || vs < 12 || is < 6) {
        auto& u = out.unhandled[(uint64_t(0x100 | (Ld32(t + 0x38) >> 24)) << 32) | is];  // skipped: 0x100 | index type, stride
        if (!u.first++) u.second = t;
        continue;
      }
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t idx[3];
        for (int j = 0; j < 3; ++j)
          idx[j] = i16 ? (Ld32(ib + is * k + 2 * j) >> ((j & 1) ? 0 : 16) & 0xFFFF) : Ld32(ib + is * k + 4 * j);
        if (i16) {  // two 16-bit indices per word, big-endian
          uint32_t w0 = Ld32(ib + is * k), w1 = Ld32(ib + is * k + 4);
          idx[0] = w0 >> 16, idx[1] = w0 & 0xFFFF, idx[2] = w1 >> 16;
        }
        float v[3][3];
        for (int j = 0; j < 3; ++j)
          for (int c = 0; c < 3; ++c) v[j][c] = LdF(vb + vs * idx[j] + 4 * c);
        out.Tri(m, v[0], v[1], v[2]);
      }
    }
    for (uint32_t e = 0; Ptr(sub) && e < nsub && e < 100000; ++e) {
      uint32_t t = sub + 64 * e;
      uint32_t arr = Ld32(t + 0x10);
      if (!Ptr(arr)) continue;
      float q[4] = {LdF(t + 0x30), LdF(t + 0x34), LdF(t + 0x38), LdF(t + 0x3C)};
      float tr[3] = {LdF(t + 0x20), LdF(t + 0x24), LdF(t + 0x28)};
      ShapeOut(out, Mul(m, FromQuat(q, tr)), Ld32(arr), depth + 1);
    }
  }
}
static void DumpCollision(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  CollisionOut out{f};
  size_t bodies = 0;
  std::map<uint32_t, std::array<uint32_t, 4>> kinds;
  size_t templates = 0;
  for (uint32_t b : Scan(kRigidBody)) {
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    size_t before = out.tris;
    uint32_t shape = Ld32(b + 0x10);
    // Only bodies in the physics world (hkWorldObject +8, the hkWorld: vtable
    // 820BD8A0); the others are templates loaded with the level's assets.
    uint32_t world = Ld32(b + 8);
    bool in_world = Ptr(world) && Ld32(world) == kWorld;
    if (in_world) ShapeOut(out, m, shape, 0);
    else ++templates;
    ++bodies;
    // What the shapes are: per shape vtable, bodies, triangles, and the child's vtable.
    uint32_t vt = Ptr(shape) ? Ld32(shape) : 0, child = Ptr(shape) ? Ld32(shape + 0x14) : 0;
    auto& k = kinds[vt];
    k[0]++, k[1] += out.tris - before;
    if (Ptr(child)) k[2] = Ld32(child);
    if (k[0] == 1) k[3] = b;
  }
  // Where the world transform is: in the first bodies of each kind, rotation
  // columns (orthonormal, 16 bytes apart) followed by a translation in the map.
  for (auto& [vt, k] : kinds) {
    uint32_t b = k[3];
    std::string hits;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      float c[3][3];
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) c[j][i] = LdF(b + off + 16 * j + 4 * i);
      bool ok = true;
      for (int j = 0; j < 3 && ok; ++j) {
        float l = c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2];
        ok = std::isfinite(l) && std::fabs(l - 1) < 1e-3f;
      }
      for (int i = 0; i < 3 && ok; ++i)
        for (int j = i + 1; j < 3 && ok; ++j)
          ok = std::fabs(c[i][0] * c[j][0] + c[i][1] * c[j][1] + c[i][2] * c[j][2]) < 1e-3f;
      if (!ok) continue;
      float t[3] = {LdF(b + off + 48), LdF(b + off + 52), LdF(b + off + 56)};
      char buf[80];
      snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off, t[0], t[1], t[2]);
      hits += buf;
    }
    RT_INFO("bot nav: kind %08X body %08X transforms:%s", vt, b, hits.substr(0, 400).c_str());
  }
  // A shape of each kind (and its child): its first 0x70 bytes.
  for (auto& [vt, k] : kinds) {
    uint32_t shape = Ld32(k[3] + 0x10);
    for (uint32_t obj : {shape, Ptr(shape) ? Ld32(shape + 0x14) : 0u}) {
      if (!Ptr(obj)) continue;
      std::string w;
      for (uint32_t off = 0; off < 0x70; off += 4) {
        uint32_t u = Ld32(obj + off);
        float fv = LdF(obj + off);
        char buf[32];
        if (std::isfinite(fv) && std::fabs(fv) > 1e-4f && std::fabs(fv) < 1e5f) snprintf(buf, sizeof(buf), " +%X:%.2f", off, fv);
        else snprintf(buf, sizeof(buf), " +%X:%08X", off, u);
        if (u) w += buf;
      }
      RT_INFO("bot nav: kind %08X %s %08X:%s", vt, obj == shape ? "shape" : "child", obj, w.c_str());
    }
  }
  for (auto& [vt, k] : kinds)
    RT_INFO("bot nav: shape vtable %08X: %u bodies, %u triangles, child vtable %08X, e.g. body %08X at %.0f %.0f %.0f", vt,
            k[0], k[1], k[2], k[3], LdF(k[3] + 0xE0), LdF(k[3] + 0xE4), LdF(k[3] + 0xE8));
  fclose(f);
  RT_INFO("bot nav: collision of %zu rigid bodies (%zu not in the world), %zu triangles, to %s", bodies, templates,
          out.tris, path);
}
// The physics world's triangles within r of (x, z), one .obj group per rigid
// body (named by its address, shape vtable and whether in the world).
static void DumpCollisionNear(const char* path, float x, float z, float r) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  CollisionOut out{f};
  out.clip_r = r, out.clip_x = x, out.clip_z = z;
  for (uint32_t b : Scan(kRigidBody)) {
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    uint32_t shape = Ld32(b + 0x10), world = Ld32(b + 8);
    bool in_world = Ptr(world) && Ld32(world) == kWorld;
    fprintf(f, "g body_%08X_vt%08X%s_w%08X_wvt%08X_type%u\n", b, Ptr(shape) ? Ld32(shape) : 0, in_world ? "" : "_out",
            world, Ptr(world) ? Ld32(world) : 0, Ptr(shape) ? Ld32(shape + 0xC) : 0);
    ShapeOut(out, m, shape, 0);
  }
  fclose(f);
  RT_INFO("bot nav: %zu triangles within %.0f m of %.1f %.1f to %s", out.tris, r, x, z, path);
  for (auto& [k, u] : out.unhandled) {
    std::string w;
    for (uint32_t off = 0; off < 0x60 && Ptr(u.second + off); off += 4) {
      char buf[16];
      snprintf(buf, sizeof(buf), " %08X", Ld32(u.second + off));
      w += buf;
    }
    RT_INFO("bot nav: shapes left out: type %u vtable %08X: %d, e.g. %08X:%s", uint32_t(k >> 32), uint32_t(k), u.first,
            u.second, w.c_str());
  }
}

const bool g_dump_collision_started = [] {
  const char* path = getenv("BF_BOT_DUMP_COLLISION");
  if (!path) return false;
  std::string p = path;
  std::thread([p] {
    // When the level's bodies are up and their number stopped changing.
    size_t last = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      size_t n = Scan(kRigidBody).size();
      if (n > 10 && n == last) break;
      last = n;
    }
    DumpCollision(p.c_str());
    // The play area for the navigation mesh: the flags' extent (capture
    // points 820190D0, position *(+14) + 60) and 350 m around it (the carriers).
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (uint32_t cp : Scan(0x820190D0)) {
      uint32_t sp = Ld32(cp + 0x14);
      if (!Ptr(sp)) continue;
      float x = LdF(sp + 0x60), z = LdF(sp + 0x68);
      if (!std::isfinite(x) || !std::isfinite(z) || std::fabs(x) > 4096 || std::fabs(z) > 4096) continue;
      lo[0] = std::min(lo[0], x), lo[1] = std::min(lo[1], z), hi[0] = std::max(hi[0], x), hi[1] = std::max(hi[1], z);
    }
    if (lo[0] < 1e8f)
      RT_INFO("bot nav: play area %.0f %.0f %.0f %.0f", lo[0] - 350, lo[1] - 350, hi[0] + 350, hi[1] + 350);
  }).detach();
  return true;
}();

}  // namespace

// ---- line of sight (the bots' shots and who they see) ------------------------
// The physics world's bodies as triangles (as DumpCollision, the vehicles left
// out: they move), binned once on a 4 m grid by their extent across, a segment
// tested against those in the cells it crosses (Moller-Trumbore), as Bad
// Company's bots. And the terrain, which isn't triangles: its heightfield
// shape (Havok type 0x18, its body at the map's corner, -1024 -1024) points
// at the terrain (+60), whose +10 are its samples, 2048 a row, 1 m apart,
// big-endian u16, height = sample / 65535 * 1000 (+54; the flags stand there).
namespace {
struct SightGrid {
  float x0 = 0, z0 = 0, cell = 4.0f;
  int nx = 0, nz = 0;
  std::vector<std::array<float, 9>> tris;
  std::vector<std::vector<uint32_t>> cells;
  uint32_t hf = 0;  // the terrain's samples
  float hf_x = 0, hf_z = 0, hf_scale = 0;
};
std::atomic<SightGrid*> g_sight{nullptr};  // (an old map's is left, never freed: another thread may be in it)
std::atomic<bool> g_sight_building{false};
std::string g_sight_map;

SightGrid* BuildSight() {
  auto* g = new SightGrid;
  std::vector<std::array<float, 2>> vehicles;
  for (uint32_t v : Scan(0x82013068)) {  // ServerVehicleEntity
    uint32_t body = Ld32(v + 0xF0);
    if (!Ptr(body)) continue;
    float x = LdF(body + 0xF0), z = LdF(body + 0xF8);
    if (std::isfinite(x) && (x != 0 || z != 0)) vehicles.push_back({x, z});
  }
  CollisionOut out{nullptr};
  out.mem = &g->tris;
  for (uint32_t b : Scan(kRigidBody)) {
    uint32_t world = Ld32(b + 8), shape = Ld32(b + 0x10);
    if (!Ptr(world) || Ld32(world) != kWorld || !Ptr(shape)) continue;
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    if (Ld32(shape + 0xC) == 0x18) {  // the terrain
      uint32_t terrain = Ld32(shape + 0x60), samples = Ptr(terrain) ? Ld32(terrain + 0x10) : 0;
      if (Ptr(samples) && IsCommitted(samples + 2048 * 2048 * 2 - 4))
        g->hf = samples, g->hf_x = m.t[0], g->hf_z = m.t[2], g->hf_scale = LdF(shape + 0x54) / 65535.0f;
      continue;
    }
    bool vehicle = false;
    for (auto& v : vehicles) vehicle |= std::hypot(m.t[0] - v[0], m.t[2] - v[1]) < 4.0f;
    if (!vehicle) ShapeOut(out, m, shape, 0);
  }
  // Triangles over 300 m across (the sea's plane, the map's edge walls) left
  // out: they'd be in every cell. The grid: the terrain's square (the maps'
  // play areas are within -1100..1100).
  size_t big = 0;
  g->tris.erase(std::remove_if(g->tris.begin(), g->tris.end(),
                               [&](const std::array<float, 9>& t) {
                                 float w = std::max({t[0], t[3], t[6]}) - std::min({t[0], t[3], t[6]});
                                 float d = std::max({t[2], t[5], t[8]}) - std::min({t[2], t[5], t[8]});
                                 return (w > 300 || d > 300) && ++big;
                               }),
                g->tris.end());
  float lo[2] = {-1100, -1100}, hi[2] = {1100, 1100};
  g->x0 = lo[0], g->z0 = lo[1];
  g->nx = std::max(1, int((hi[0] - lo[0]) / g->cell) + 1), g->nz = std::max(1, int((hi[1] - lo[1]) / g->cell) + 1);
  g->cells.resize(size_t(g->nx) * g->nz);
  for (uint32_t i = 0; i < g->tris.size(); ++i) {
    auto& t = g->tris[i];
    float a0 = std::min({t[0], t[3], t[6]}), a1 = std::max({t[0], t[3], t[6]});
    float b0 = std::min({t[2], t[5], t[8]}), b1 = std::max({t[2], t[5], t[8]});
    int i0 = std::clamp(int((a0 - g->x0) / g->cell), 0, g->nx - 1), i1 = std::clamp(int((a1 - g->x0) / g->cell), 0, g->nx - 1);
    int j0 = std::clamp(int((b0 - g->z0) / g->cell), 0, g->nz - 1), j1 = std::clamp(int((b1 - g->z0) / g->cell), 0, g->nz - 1);
    for (int j = j0; j <= j1; ++j)
      for (int k = i0; k <= i1; ++k) g->cells[size_t(j) * g->nx + k].push_back(i);
  }
  RT_INFO("bot sight: %zu triangles (%zu big ones and %zu vehicles left out), %d x %d cells of %.0f m, terrain %s", g->tris.size(),
          big, vehicles.size(), g->nx, g->nz, g->cell, g->hf ? "found" : "not found");
  return g;
}
bool SegmentHitsTri(const float o[3], const float d[3], const std::array<float, 9>& t) {
  float e1[3] = {t[3] - t[0], t[4] - t[1], t[5] - t[2]}, e2[3] = {t[6] - t[0], t[7] - t[1], t[8] - t[2]};
  float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
  float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-9f) return false;
  float inv = 1.0f / det, s[3] = {o[0] - t[0], o[1] - t[1], o[2] - t[2]};
  float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return false;
  float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
  float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return false;
  float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  return tt > 0.02f && tt < 0.98f;  // not at either end (where the soldiers are)
}
}  // namespace

namespace rt {
// The map's line of sight, built (in the background, once its bodies stopped
// changing in number) when the map is new.
void SightForMap(const std::string& map) {
  if (map.empty() || map == g_sight_map || g_sight_building.exchange(true)) return;
  std::thread([map] {
    size_t last = 0;
    for (;;) {
      size_t n = Scan(kRigidBody).size();
      if (n > 10 && n == last) break;
      last = n;
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    g_sight = BuildSight();
    g_sight_map = map;
    g_sight_building = false;
  }).detach();
}
float TerrainHeight(float x, float z) {
  SightGrid* g = g_sight;
  if (!g || !g->hf) return -1e9f;
  int col = int(x - g->hf_x), row = int(z - g->hf_z);
  if (col < 0 || row < 0 || col >= 2048 || row >= 2048) return -1e9f;
  return Ld16(g->hf + 2 * uint32_t(row * 2048 + col)) * g->hf_scale;
}
// Whether the map's sight (its collision and terrain) is built.
bool SightReadyFor(const std::string& map) { return !g_sight_building && g_sight && g_sight_map == map; }
// The highest surface of the map's collision (buildings, walls, rocks, ships)
// over n x n pixels of mpp metres, the first one's corner at x0, z0 (rows go
// south): NAN where there is none. Each triangle filled from above, and its
// edges drawn too (walls stand edge-on from above). False before it is built.
bool CollisionTops(float x0, float z0, float mpp, int n, std::vector<float>& top) {
  SightGrid* g = g_sight;
  if (!g) return false;
  botllm::TopsOfTriangles(g->tris, x0, z0, mpp, n, top);
  return true;
}
// Clear from a to b (true too before the map's is built).
bool InSight(const float a[3], const float b[3]) {
  SightGrid* g = g_sight;
  if (!g) return true;
  float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  float len = std::hypot(d[0], d[2]);
  // The ground: every metre along, the segment over the terrain (1 m from
  // either end: where they stand).
  if (g->hf)
    for (float s = 1.0f; s < len - 1.0f; s += 1.0f) {
      float f = s / len;
      if (a[1] + d[1] * f < TerrainHeight(a[0] + d[0] * f, a[2] + d[2] * f) - 0.3f) return false;
    }
  // (each triangle once: a stamp per thread, the bots' and the server's)
  thread_local std::vector<uint32_t> stamp;
  thread_local uint32_t visit = 0;
  thread_local const SightGrid* stamped = nullptr;
  if (stamped != g) stamp.assign(g->tris.size(), 0), stamped = g, visit = 0;
  ++visit;
  int steps = int(len / (g->cell * 0.25f)) + 1, last = -1;
  for (int k = 0; k <= steps; ++k) {
    float f = float(k) / steps;
    int i = int((a[0] + d[0] * f - g->x0) / g->cell), j = int((a[2] + d[2] * f - g->z0) / g->cell);
    if (i < 0 || j < 0 || i >= g->nx || j >= g->nz) continue;
    int c = j * g->nx + i;
    if (c == last) continue;
    last = c;
    for (uint32_t ti : g->cells[size_t(c)]) {
      if (stamp[ti] == visit) continue;
      stamp[ti] = visit;
      if (SegmentHitsTri(a, d, g->tris[ti])) return false;
    }
  }
  return true;
}
}  // namespace rt
