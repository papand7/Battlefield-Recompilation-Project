// bf1943 blood on hit: see bf_gore.h.

#include "xenia/gpu/vulkan/bf_gore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace bflook {

namespace {

struct Event {
  float feet[3];
  float damage;
  int type;  // 0 hit, 1 death, 2 death with gibs
  float push[3];
};

enum Kind { kPuff = 0, kChunk = 1, kSplat = 2, kDrop = 3, kGroundChunk = 4 };

struct Particle {
  float pos[3], vel[3];
  float size;  // metres
  float age = 0.0f, life;  // life 0: stays (splats, chunks on the ground)
  int kind;
  int seed;
  float floor_y;  // where it lands (the soldier's feet)
};

// A soldier who came apart: their skinned draws' bones, frozen at the
// moment of death, split into pieces that fly and land.
struct Piece {
  float center[3];  // at the moment of death
  float offset[3] = {}, vel[3] = {};
  float axis[3] = {0, 1, 0};
  float spin = 0.0f, angle = 0.0f;
  bool landed = false;
  float trail = 0.0f;  // seconds to the next drop of blood
  float m[3][4];       // this frame's rigid motion (world -> world)
};
struct Frozen {
  uint64_t key;
  std::vector<int> regs;                      // first constant of each bone
  std::vector<std::array<float, 12>> bones;   // their rows at death
  std::vector<int> piece;
};
struct Corpse {
  uint64_t serial;
  float feet[3], push[3];
  float track[3];  // the middle of the matched bones, last seen
  float age = 0.0f, unseen = 0.0f;
  std::vector<Piece> pieces;
  std::vector<Frozen> frozen;
};

std::mutex event_lock;
std::vector<Event> events;

// GPU thread only.
float camera_vs[19][4];
bool have_camera = false;
std::chrono::steady_clock::time_point camera_time;
std::vector<Particle> particles;
std::vector<Corpse> corpses;
std::mt19937 rng(1943);
// RestoreBones: the constants OverrideBones changed.
std::vector<std::pair<int, std::array<float, 4>>> saved;
// The last match of OverrideBones.
uint64_t last_match = 0;
int last_piece[256];
uint64_t next_serial = 1;

constexpr size_t kMaxStaying = 160;  // splats and chunks kept on the ground

float Rand(float a, float b) {
  return a + (b - a) * std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

bool Invert(const double m[4][4], double out[4][4]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    if (std::abs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r][c] = a[r][c + 4];
  return true;
}

Particle& Add(int kind, const float* at, float size, float life, float floor_y) {
  Particle p;
  std::memcpy(p.pos, at, sizeof(p.pos));
  std::memset(p.vel, 0, sizeof(p.vel));
  p.size = size;
  p.life = life;
  p.kind = kind;
  p.seed = int(Rand(0.0f, 63.99f));
  p.floor_y = floor_y;
  particles.push_back(p);
  return particles.back();
}

void Spawn(const Event& e, const double eye[3]) {
  float chest[3] = {e.feet[0], e.feet[1] + Rand(1.0f, 1.45f), e.feet[2]};
  // Away from the camera (the shot's exit side), flattened.
  float away[3] = {chest[0] - float(eye[0]), 0.0f, chest[2] - float(eye[2])};
  float len = std::sqrt(away[0] * away[0] + away[2] * away[2]);
  if (len > 0.01f) {
    away[0] /= len;
    away[2] /= len;
  }
  float strength = std::clamp(e.damage / 100.0f, 0.1f, 1.0f);
  bool gib = e.type == 2;
  float floor_y = e.feet[1];
  Add(kPuff, chest, gib ? 1.6f : 0.3f + 0.45f * strength, gib ? 0.9f : 0.45f, floor_y);
  int drops = gib ? 14 : 2 + int(strength * 5.0f);
  for (int i = 0; i < drops; ++i) {
    Particle& p = Add(kDrop, chest, Rand(0.03f, 0.06f), 2.0f, floor_y);
    float spread = gib ? 4.0f : 1.5f;
    p.vel[0] = away[0] * Rand(1.0f, 3.0f) + Rand(-spread, spread);
    p.vel[1] = Rand(0.5f, gib ? 6.0f : 2.5f);
    p.vel[2] = away[2] * Rand(1.0f, 3.0f) + Rand(-spread, spread);
  }
  if (gib) {
    for (int i = 0; i < 7; ++i) {
      Particle& p = Add(kChunk, chest, Rand(0.07f, 0.16f), 4.0f, floor_y);
      float angle = Rand(0.0f, 6.2832f), speed = Rand(2.0f, 7.0f);
      p.vel[0] = std::cos(angle) * speed + e.push[0] * 3.0f;
      p.vel[1] = Rand(3.0f, 8.0f) + e.push[1] * 2.0f;
      p.vel[2] = std::sin(angle) * speed + e.push[2] * 3.0f;
    }
    Corpse c;
    c.serial = next_serial++;
    std::memcpy(c.feet, e.feet, sizeof(c.feet));
    std::memcpy(c.push, e.push, sizeof(c.push));
    c.track[0] = e.feet[0];
    c.track[1] = e.feet[1] + 1.0f;
    c.track[2] = e.feet[2];
    corpses.push_back(std::move(c));
    if (corpses.size() > 8) corpses.erase(corpses.begin());
  }
  if (e.type >= 1) {
    float feet[3] = {e.feet[0], e.feet[1] + 0.02f, e.feet[2]};
    Add(kSplat, feet, gib ? 1.8f : 0.9f, 0.0f, floor_y);
  }
}

// Pieces from the bones at the moment of death: up to six groups of nearby
// bones (k-means from the extremes: head, hands, feet), each flying off.
void MakePieces(Corpse& c, const std::vector<std::array<float, 3>>& points) {
  size_t n = points.size();
  size_t k = std::min<size_t>(6, n / 2);
  std::vector<std::array<float, 3>> centers;
  // Farthest-point start: the highest bone, then each farthest from the rest.
  size_t top = 0;
  for (size_t i = 1; i < n; ++i)
    if (points[i][1] > points[top][1]) top = i;
  centers.push_back(points[top]);
  auto d2 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
  };
  while (centers.size() < k) {
    size_t best = 0;
    float best_d = -1.0f;
    for (size_t i = 0; i < n; ++i) {
      float d = 1e30f;
      for (auto& ce : centers) d = std::min(d, d2(points[i], ce));
      if (d > best_d) best_d = d, best = i;
    }
    centers.push_back(points[best]);
  }
  for (int iter = 0; iter < 8; ++iter) {
    std::vector<std::array<float, 4>> sum(k, {0, 0, 0, 0});
    for (auto& p : points) {
      size_t best = 0;
      for (size_t j = 1; j < k; ++j)
        if (d2(p, centers[j]) < d2(p, centers[best])) best = j;
      for (int a = 0; a < 3; ++a) sum[best][a] += p[a];
      sum[best][3] += 1.0f;
    }
    for (size_t j = 0; j < k; ++j)
      if (sum[j][3] > 0)
        for (int a = 0; a < 3; ++a) centers[j][a] = sum[j][a] / sum[j][3];
  }
  float middle[3] = {};
  for (auto& ce : centers)
    for (int a = 0; a < 3; ++a) middle[a] += ce[a] / float(k);
  for (auto& ce : centers) {
    Piece p;
    std::memcpy(p.center, ce.data(), sizeof(p.center));
    float out[3] = {ce[0] - middle[0], 0.0f, ce[2] - middle[2]};
    float l = std::sqrt(out[0] * out[0] + out[2] * out[2]);
    if (l < 0.05f) {
      float a = Rand(0.0f, 6.2832f);
      out[0] = std::cos(a);
      out[2] = std::sin(a);
    } else {
      out[0] /= l;
      out[2] /= l;
    }
    // The torso (nearest the middle) moves least.
    float speed = l < 0.25f ? Rand(0.5f, 1.5f) : Rand(2.5f, 6.0f);
    for (int a = 0; a < 3; ++a) p.vel[a] = out[a] * speed + c.push[a] * Rand(1.5f, 4.0f);
    p.vel[1] += Rand(2.0f, 5.0f) * (l < 0.25f ? 0.4f : 1.0f);
    float ax[3] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)};
    float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-4f;
    for (int a = 0; a < 3; ++a) p.axis[a] = ax[a] / al;
    p.spin = Rand(4.0f, 12.0f) * (l < 0.25f ? 0.3f : 1.0f);
    c.pieces.push_back(p);
  }
}

void StepCorpses(float dt) {
  for (Corpse& c : corpses) {
    c.age += dt;
    c.unseen += dt;
    for (Piece& p : c.pieces) {
      if (!p.landed) {
        p.vel[1] -= 9.8f * dt;
        for (int a = 0; a < 3; ++a) p.offset[a] += p.vel[a] * dt;
        p.angle += p.spin * dt;
        float pos[3] = {p.center[0] + p.offset[0], p.center[1] + p.offset[1], p.center[2] + p.offset[2]};
        // Blood trailing behind it.
        p.trail -= dt;
        if (p.trail <= 0.0f) {
          p.trail = 0.07f;
          Particle& d = Add(kDrop, pos, Rand(0.04f, 0.07f), 1.5f, c.feet[1]);
          for (int a = 0; a < 3; ++a) d.vel[a] = p.vel[a] * 0.3f + Rand(-0.5f, 0.5f);
        }
        if (pos[1] <= c.feet[1] + 0.12f && p.vel[1] < 0.0f) {
          p.landed = true;
          float at[3] = {pos[0], c.feet[1] + 0.02f, pos[2]};
          Add(kSplat, at, Rand(0.5f, 0.9f), 0.0f, c.feet[1]);
        }
      }
      // Rotation about the axis (Rodrigues), then the move.
      float s = std::sin(p.angle), co = std::cos(p.angle), t = 1.0f - co;
      float x = p.axis[0], y = p.axis[1], z = p.axis[2];
      float r[3][3] = {{t * x * x + co, t * x * y - s * z, t * x * z + s * y},
                       {t * x * y + s * z, t * y * y + co, t * y * z - s * x},
                       {t * x * z - s * y, t * y * z + s * x, t * z * z + co}};
      for (int i = 0; i < 3; ++i) {
        float moved = p.center[i] + p.offset[i];
        for (int j = 0; j < 3; ++j) {
          p.m[i][j] = r[i][j];
          moved -= r[i][j] * p.center[j];
        }
        p.m[i][3] = moved;
      }
    }
  }
  // Not drawn for a while (the game removed the body): forget it.
  corpses.erase(std::remove_if(corpses.begin(), corpses.end(),
                               [](const Corpse& c) { return c.unseen > 5.0f && c.age > 5.0f; }),
                corpses.end());
}

}  // namespace

void AddHit(const float feet[3], float damage) {
  std::lock_guard<std::mutex> lock(event_lock);
  events.push_back({{feet[0], feet[1], feet[2]}, damage, 0, {0, 0, 0}});
}

void AddDeath(const float feet[3], float damage, bool gib, const float push[3]) {
  std::lock_guard<std::mutex> lock(event_lock);
  events.push_back({{feet[0], feet[1], feet[2]}, damage, gib ? 2 : 1, {push[0], push[1], push[2]}});
}

void SetCamera(const float vs[19][4]) {
  std::memcpy(camera_vs, vs, sizeof(camera_vs));
  have_camera = true;
  camera_time = std::chrono::steady_clock::now();
}

uint32_t BuildGore(float (*out)[4], uint32_t max) {
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
  last = now;

  std::vector<Event> new_events;
  {
    std::lock_guard<std::mutex> lock(event_lock);
    new_events.swap(events);
  }
  // No world view for long (a level loading): a new start.
  if (!have_camera || now - camera_time > std::chrono::seconds(20)) {
    particles.clear();
    corpses.clear();
    return 0;
  }

  // world -> clip = WVP * W^-1; the eye is where clip (0, 0, 1, 0) maps back.
  double wvp[4][4], w[4][4] = {}, w_inv[4][4], wvp_inv[4][4], vp[4][4], inv_vp[4][4];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      wvp[r][c] = camera_vs[12 + r][c];
      w[r][c] = r < 3 ? camera_vs[16 + r][c] : (c == 3 ? 1.0 : 0.0);
    }
  if (!Invert(w, w_inv) || !Invert(wvp, wvp_inv)) return 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double a = 0.0, b = 0.0;
      for (int k = 0; k < 4; ++k) {
        a += wvp[r][k] * w_inv[k][c];
        b += w[r][k] * wvp_inv[k][c];
      }
      vp[r][c] = a;
      inv_vp[r][c] = b;
    }
  double eye[3] = {inv_vp[0][2] / inv_vp[3][2], inv_vp[1][2] / inv_vp[3][2],
                   inv_vp[2][2] / inv_vp[3][2]};

  static const bool self = std::getenv("BF_GORE_SELF") != nullptr;
  for (const Event& e : new_events) {
    // The player's own soldier stands under the eye: its hits are the game's
    // red vignette.
    double dx = e.feet[0] - eye[0], dz = e.feet[2] - eye[2];
    if (!self && dx * dx + dz * dz < 2.5 * 2.5 && std::abs(e.feet[1] - eye[1]) < 3.0) continue;
    Spawn(e, eye);
  }

  StepCorpses(dt);

  // Gravity; flying chunks landing stay as chunks on the ground with a splat,
  // drops leave a small splat now and then.
  std::vector<Particle> landed;
  for (Particle& p : particles) {
    p.age += dt;
    if (p.kind == kSplat) p.vel[0] = std::min(1.0f, p.vel[0] + dt * 2.0f);  // spreading
    if (p.kind != kChunk && p.kind != kDrop) continue;
    p.vel[1] -= 9.8f * dt;
    for (int i = 0; i < 3; ++i) p.pos[i] += p.vel[i] * dt;
    if (p.pos[1] <= p.floor_y + p.size * 0.5f && p.vel[1] < 0.0f) {
      Particle s = p;
      s.pos[1] = p.floor_y + 0.02f;
      s.age = 0.0f;
      s.life = 0.0f;
      s.vel[0] = 0.0f;
      if (p.kind == kChunk) {
        s.kind = kGroundChunk;
        landed.push_back(s);
        s.kind = kSplat;
        s.size = p.size * Rand(2.5f, 4.0f);
        landed.push_back(s);
      } else if (Rand(0.0f, 1.0f) < 0.1f) {
        s.kind = kSplat;
        s.size = Rand(0.1f, 0.25f);
        landed.push_back(s);
      }
      p.age = p.life;
    }
  }
  particles.erase(std::remove_if(particles.begin(), particles.end(),
                                 [](const Particle& p) { return p.life > 0.0f && p.age >= p.life; }),
                  particles.end());
  particles.insert(particles.end(), landed.begin(), landed.end());
  // Only so much stays on the ground: the oldest drops of blood go first,
  // then the oldest of the rest; and only so much flies.
  auto trim = [](size_t keep, bool staying, float below) {
    size_t n = 0;
    for (const Particle& p : particles) n += (p.life == 0.0f) == staying && p.size < below;
    for (auto it = particles.begin(); n > keep && it != particles.end();) {
      if ((it->life == 0.0f) == staying && it->size < below) {
        it = particles.erase(it);
        --n;
      } else {
        ++it;
      }
    }
  };
  size_t staying = 0;
  for (const Particle& p : particles) staying += p.life == 0.0f;
  if (staying > kMaxStaying) {
    size_t small = 0;
    for (const Particle& p : particles) small += p.life == 0.0f && p.size < 0.3f;
    trim(small - std::min(small, staying - kMaxStaying), true, 0.3f);
    trim(kMaxStaying, true, 1e9f);
  }
  trim(400, false, 1e9f);

  // What is in front of the camera and not too far: the flying ones first
  // (newest first), then what lies on the ground (nearest first).
  struct Item {
    const Particle* p;
    double dist;
  };
  std::vector<Item> order;
  for (const Particle& p : particles) {
    double clip[4];
    for (int r = 0; r < 4; ++r)
      clip[r] = vp[r][0] * p.pos[0] + vp[r][1] * p.pos[1] + vp[r][2] * p.pos[2] + vp[r][3];
    double margin = p.size * 2.0;
    if (clip[3] < -margin) continue;
    double w_abs = std::max(std::abs(clip[3]), 0.01);
    double reach = 1.0 + margin * 2.0 / w_abs;
    if (clip[3] > 0.0 && (std::abs(clip[0] / w_abs) > reach || std::abs(clip[1] / w_abs) > reach)) continue;
    double dx = p.pos[0] - eye[0], dy = p.pos[1] - eye[1], dz = p.pos[2] - eye[2];
    double d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d > 250.0) continue;
    order.push_back({&p, d});
  }
  std::stable_sort(order.begin(), order.end(), [](const Item& a, const Item& b) {
    bool a_ground = a.p->life == 0.0f, b_ground = b.p->life == 0.0f;
    if (a_ground != b_ground) return b_ground;
    return a_ground ? a.dist < b.dist : a.p->age < b.p->age;
  });
  static const bool log = std::getenv("BF_GORE_LOG") != nullptr;
  static int log_frame = 0;
  if (log && ++log_frame % 120 == 0) {
    XELOGI("gore: {} particles, {} in view, {} bodies, eye {:.1f} {:.1f} {:.1f}", particles.size(), order.size(),
           corpses.size(), eye[0], eye[1], eye[2]);
    for (const Item& item : order) {
      if (item.p->life == 0.0f) {
        XELOGI("gore:   ground {} at {:.1f} {:.2f} {:.1f} size {:.2f}", item.p->kind, item.p->pos[0], item.p->pos[1],
               item.p->pos[2], item.p->size);
        break;
      }
    }
  }
  // Flying ones take at most 5/8 of the room, so what lies on the ground
  // stays drawn during a burst.
  uint32_t n = 0, flying = 0;
  for (const Item& item : order) {
    if (n >= max) break;
    const Particle* p = item.p;
    if (p->life > 0.0f && ++flying > max * 5 / 8) continue;
    out[n * 2][0] = p->pos[0];
    out[n * 2][1] = p->pos[1];
    out[n * 2][2] = p->pos[2];
    out[n * 2][3] = p->size;
    out[n * 2 + 1][0] = float(p->kind);
    out[n * 2 + 1][1] = float(p->seed);
    out[n * 2 + 1][2] = p->life > 0.0f ? std::min(p->age / p->life, 0.999f) : 0.0f;
    out[n * 2 + 1][3] = p->kind == kSplat ? std::max(p->vel[0], 0.0f) : 1.0f;
    ++n;
  }
  return n;
}

int BoneBase(const char* disassembly) {
  std::string d = disassembly;
  for (int base = 0; base + 2 < 256; ++base) {
    auto has = [&](int n) { return d.find("c[" + std::to_string(n) + "+a0]") != std::string::npos; };
    if (has(base) && has(base + 1) && has(base + 2)) return base;
  }
  return -1;
}

bool OverrideBones(uint64_t key, float* constants, int base) {
  if (corpses.empty() || base < 0) return false;
  // The draw's bones: rigid 3x4 rows (c[k], c[k+1], c[k+2]: rotation and
  // translation) from the base on, until one is not.
  struct Bone {
    int reg;
    float t[3];
  };
  std::vector<Bone> bones;
  for (int k = base; k + 2 < 256; k += 3) {
    const float* r = constants + k * 4;
    bool rigid = true;
    for (int i = 0; i < 3 && rigid; ++i) {
      float l = r[i * 4] * r[i * 4] + r[i * 4 + 1] * r[i * 4 + 1] + r[i * 4 + 2] * r[i * 4 + 2];
      rigid = l > 0.8f && l < 1.2f;
    }
    for (int i = 0; i < 3 && rigid; ++i) {
      int j = (i + 1) % 3;
      float d = r[i * 4] * r[j * 4] + r[i * 4 + 1] * r[j * 4 + 1] + r[i * 4 + 2] * r[j * 4 + 2];
      rigid = std::abs(d) < 0.1f;
    }
    if (!rigid) break;
    bones.push_back({k, {r[3], r[7], r[11]}});
  }
  if (bones.size() < 3) return false;
  bool changed = false;
  for (Corpse& c : corpses) {
    // This soldier's bones: near where the body was last seen.
    std::vector<const Bone*> mine;
    float mid[3] = {};
    for (const Bone& b : bones) {
      float dx = b.t[0] - c.track[0], dy = b.t[1] - c.track[1], dz = b.t[2] - c.track[2];
      if (dx * dx + dy * dy + dz * dz > 2.2f * 2.2f) continue;
      mine.push_back(&b);
      for (int a = 0; a < 3; ++a) mid[a] += b.t[a];
    }
    // A soldier's skeleton is all there.
    if (mine.size() < 3 || mine.size() * 5 < bones.size() * 4) continue;
    for (int a = 0; a < 3; ++a) mid[a] /= float(mine.size());
    // Another soldier standing nearby has their middle elsewhere.
    float dx = mid[0] - c.track[0], dz = mid[2] - c.track[2];
    if (dx * dx + dz * dz > 1.2f * 1.2f) continue;
    std::memcpy(c.track, mid, sizeof(mid));
    c.unseen = 0.0f;
    auto it = std::find_if(c.frozen.begin(), c.frozen.end(), [key](const Frozen& f) { return f.key == key; });
    if (it == c.frozen.end()) {
      if (c.pieces.empty()) {
        if (mine.size() < 8) continue;  // the pieces come from a whole body
        std::vector<std::array<float, 3>> points;
        for (const Bone* b : mine) points.push_back({b->t[0], b->t[1], b->t[2]});
        MakePieces(c, points);
        XELOGI("gore: soldier at {:.1f} {:.1f} {:.1f} comes apart: {} bones from c{}, {} pieces (mesh {:016X})",
               c.feet[0], c.feet[1], c.feet[2], mine.size(), base, c.pieces.size(), key);
      }
      Frozen f;
      f.key = key;
      for (const Bone* b : mine) {
        std::array<float, 12> rows;
        std::memcpy(rows.data(), constants + b->reg * 4, sizeof(rows));
        f.regs.push_back(b->reg);
        f.bones.push_back(rows);
        int best = 0;
        float best_d = 1e30f;
        for (size_t j = 0; j < c.pieces.size(); ++j) {
          float x = b->t[0] - c.pieces[j].center[0], y = b->t[1] - c.pieces[j].center[1],
                z = b->t[2] - c.pieces[j].center[2];
          float d = x * x + y * y + z * z;
          if (d < best_d) best_d = d, best = int(j);
        }
        f.piece.push_back(best);
      }
      c.frozen.push_back(std::move(f));
      it = c.frozen.end() - 1;
    }
    last_match = c.serial * 0x9E3779B97F4A7C15ull ^ key;
    std::fill(std::begin(last_piece), std::end(last_piece), -1);
    for (size_t i = 0; i < it->regs.size(); ++i) last_piece[it->regs[i]] = it->piece[i];
    // Bone' = piece motion * bone at death.
    for (size_t i = 0; i < it->regs.size(); ++i) {
      const Piece& p = c.pieces[it->piece[i]];
      const float* b = it->bones[i].data();
      float* dst = constants + it->regs[i] * 4;
      for (int row = 0; row < 3; ++row) {
        saved.push_back({it->regs[i] + row, {dst[row * 4], dst[row * 4 + 1], dst[row * 4 + 2], dst[row * 4 + 3]}});
      }
      float nb[12];
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
          float v = p.m[row][0] * b[col] + p.m[row][1] * b[4 + col] + p.m[row][2] * b[8 + col];
          if (col == 3) v += p.m[row][3];
          nb[row * 4 + col] = v;
        }
      }
      std::memcpy(dst, nb, sizeof(nb));
    }
    changed = true;
  }
  return changed;
}

uint64_t LastMatch() { return last_match; }

int LastPiece(int reg) { return reg >= 0 && reg < 256 ? last_piece[reg] : -1; }

bool BoneFetch(const char* disassembly, int* vf, int* offset, int* stride, int* weights) {
  // Lines like "vfetch_full r0.x___, r0.x, vf0, Offset=8,
  // DataFormat=FMT_8_8_8_8, Stride=9, NumFormat=integer"; a vfetch_mini takes
  // the stride of the last full fetch.
  std::string d = disassembly;
  int last_stride = 0, last_vf = -1;
  size_t at = 0;
  while (at < d.size()) {
    size_t end = d.find(char(10), at);
    if (end == std::string::npos) end = d.size();
    std::string line = d.substr(at, end - at);
    at = end + 1;
    size_t f = line.find("vfetch_");
    if (f == std::string::npos) continue;
    int line_vf = last_vf;
    size_t v = line.find(", vf");
    if (v != std::string::npos) line_vf = std::atoi(line.c_str() + v + 4);
    size_t s = line.find("Stride=");
    if (s != std::string::npos) last_stride = std::atoi(line.c_str() + s + 7);
    last_vf = line_vf;
    // FMT_8_8_8_8 itself, not FMT_8_8_8_8_AS_16_16_16_16.
    size_t fmt = line.find("FMT_8_8_8_8");
    if (fmt == std::string::npos || (fmt + 11 < line.size() && line[fmt + 11] == '_')) continue;
    if (line.find("NumFormat=integer") == std::string::npos) continue;
    size_t o = line.find("Offset=");
    int line_offset = o != std::string::npos ? std::atoi(line.c_str() + o + 7) : 0;
    if (*vf < 0) {
      *vf = line_vf;
      *offset = line_offset;
      *stride = last_stride;
      *weights = -1;
    } else if (line_vf == *vf) {
      *weights = line_offset;
      break;
    }
  }
  return *vf >= 0 && *stride > 0;
}

void RestoreBones(float* constants) {
  // Backwards: a register saved twice gets its first (the game's) value.
  for (auto it = saved.rbegin(); it != saved.rend(); ++it)
    std::memcpy(constants + it->first * 4, it->second.data(), sizeof(float) * 4);
  saved.clear();
}

}  // namespace bflook
}  // namespace gpu
}  // namespace xe
