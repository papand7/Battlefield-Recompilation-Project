// Bot AI for --bots: the bots are players on the host's own server
// (bot_players.cpp), driven by the bot brain.
//
// The director runs in the host process, which owns the server and so sees
// every soldier, control point and vehicle; it hands them to the bot brain
// (bots/brain.cpp: what each bot does) and passes each bot's pad on to its
// player (SetBotPlayerInput). The brain can be a DLL reloaded while the match
// runs (BF_BOT_BRAIN); BF_BOT_SCENARIO and BF_BOT_LOG are for testing (see
// bots/brain.cpp and tools/bot_paths.py).
//
// Server state (found with --snap-mem / --watch-write, see README):
//   ServerSoldierEntity: vtable 82013A40 (getType 822ED958), team +48 (1 US,
//     2 Japan), position +180, aim component *(+130): yaw +134, pitch +138
//     (radians; forward = (sin yaw, cos yaw) in x/z, turning right lowers yaw).
//   ServerCapturePointEntity: vtable 820190D0, owner team +80, position
//     *(+14) + 60.
//   ServerVehicleEntity: vtable 82013068 (getType 822EE748; found through the
//     type records: *(typeinfo + 4) -> {name, ...}), its kind's name
//     *(*(+14) + C) ("LCVP", "Corsair", "Zero", "Willys", "Bofors"...),
//     position *(+F0) + F0 (*(+EC) + 310 is where it was made: a landing
//     craft's crane).
// A soldier object exists while its player is alive.
#include "bot_knowledge.h"
#include "bot_tuning.h"
#include "bot_llm.h"
#include "bot_players.h"
#include "bots/bot_api.h"
#include "bots/nav.h"
#include "guest.h"
#include "kernel.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include "posix_input.h"
// Reads of our own memory that fail (instead of faulting) where nothing is mapped.
using SIZE_T = size_t;
inline HANDLE GetCurrentProcess() { return nullptr; }
inline BOOL ReadProcessMemory(HANDLE, const void* addr, void* buf, size_t n, size_t* got) {
  iovec local{buf, n}, remote{const_cast<void*>(addr), n};
  ssize_t r = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
  if (got) *got = r > 0 ? size_t(r) : 0;
  return r == ssize_t(n);
}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace rt {

extern std::mutex g_host_map_mutex;  // game_patches.cpp

namespace {

using botapi::Pad;
using botapi::Point;
using botapi::Soldier;
using botapi::Vec;
using botapi::Vehicle;
using botapi::Wrap;
using botapi::Dist2D;
using botapi::Dist3D;

constexpr uint32_t kSoldierVtable = 0x82013A40;
constexpr uint32_t kCapturePointVtable = 0x820190D0;
constexpr uint32_t kVehicleVtable = 0x82013068;
constexpr int kMaxBots = botapi::kMaxBots;

float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
bool Readable(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a) && IsCommitted(a + 0x400); }

// Every live object with this vtable in the heaps.
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = _byteswap_ulong(vtable);
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i)
      if (p[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}

// A soldier's own entry component (+350; its +2C points at the soldier's
// +190). It stays the same in a vehicle: being seated shows in the byte at
// +34C instead, 1 on foot and 0 in a vehicle (seen with BF_SEAT_DUMP while a
// bot got in and out of a landing craft).
constexpr uint32_t kSoldierSeat = 0x350, kSeatVehicle = 0x2C, kSoldierOnFoot = 0x34C;
constexpr uint32_t kSeated = 1;  // Soldier::vehicle until Refresh finds which

bool ReadSoldier(uint32_t o, Soldier& s) {
  if (!Readable(o) || Ld32(o) != kSoldierVtable) return false;
  s.obj = o;
  s.team = int(Ld32(o + 0x48));
  s.pos = {LdF(o + 0x180), LdF(o + 0x184), LdF(o + 0x188)};
  uint32_t aim = Ld32(o + 0x130);
  if (!Readable(aim)) return false;
  s.yaw = LdF(aim + 0x134);
  s.pitch = LdF(aim + 0x138);
  s.vehicle = (Ld32(o + kSoldierOnFoot) >> 24) == 0 ? kSeated : 0;
  float health = LdF(o + 0x3C);
  s.health = std::isfinite(health) && health >= 0 && health <= 1000 ? health : 100;
  return std::isfinite(s.pos.x) && std::isfinite(s.yaw) && (s.team == 1 || s.team == 2);
}

bool ReadPoint(uint32_t o, Point& p) {
  if (!Readable(o) || Ld32(o) != kCapturePointVtable) return false;
  uint32_t spatial = Ld32(o + 0x14);
  if (!Readable(spatial)) return false;
  p.obj = o;
  p.owner = int(Ld32(o + 0x80));
  p.pos = {LdF(spatial + 0x60), LdF(spatial + 0x64), LdF(spatial + 0x68)};
  return std::isfinite(p.pos.x);
}

bool ReadVehicle(uint32_t o, Vehicle& v) {
  if (!Readable(o) || Ld32(o) != kVehicleVtable) return false;
  uint32_t data = Ld32(o + 0x14), body = Ld32(o + 0xF0);
  if (!Readable(data) || !Readable(body)) return false;
  uint32_t name = Ld32(data + 0xC);
  if (!Readable(name)) return false;
  v.obj = o;
  const char* n = G<char>(name);
  size_t len = strnlen(n, sizeof(v.kind) - 1);
  memcpy(v.kind, n, len);
  v.kind[len] = 0;
  v.pos = {LdF(body + 0xF0), LdF(body + 0xF4), LdF(body + 0xF8)};
  // Jeeps, tanks, planes and guns don't keep it there (0,0,0): where it was
  // made, *(+EC) + 310, is where it stands until someone moves it (and a
  // seated bot's own position is its vehicle's).
  if (v.pos.x == 0 && v.pos.y == 0 && v.pos.z == 0) {
    uint32_t made = Ld32(o + 0xEC);
    if (Readable(made)) v.pos = {LdF(made + 0x310), LdF(made + 0x314), LdF(made + 0x318)};
  }
  if (v.pos.x == 0 && v.pos.y == 0 && v.pos.z == 0) return false;
  return std::isfinite(v.pos.x) && std::fabs(v.pos.x) < 1e5f;
}

// Scenarios: move a soldier. Its position lives in its physics (and copies
// of it): every 16-byte aligned position vector where it stands (the
// physics' format, within 5 cm) moves there. Done on the server, which has
// the say (a move in the bot's own game is put back), and in the bot's game.
size_t MoveVectorsNear(Vec pos, Vec to, const char* who) {
  auto f = [](uint32_t raw) {
    float v;
    raw = _byteswap_ulong(raw);
    memcpy(&v, &raw, 4);
    return v;
  };
  std::vector<std::pair<uint32_t, Vec>> hits;
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; i += 4) {
      float x = f(w[i]), y = f(w[i + 1]), z = f(w[i + 2]);
      if (std::fabs(x - pos.x) < 0.05f && std::fabs(z - pos.z) < 0.05f && std::fabs(y - pos.y) < 0.05f)
        hits.push_back({uint32_t(page) + i * 4, Vec{x, y, z}});
    }
  }
  static const bool trace = getenv("BF_TELEPORT_SCAN") != nullptr;
  for (auto& [a, v] : hits) {
    if (trace) RT_INFO("teleport (%s): %08X %.2f,%.2f,%.2f w %08X", who, a, v.x, v.y, v.z, Ld32(a + 12));
    float t[3] = {to.x + v.x - pos.x, to.y + v.y - pos.y, to.z + v.z - pos.z};
    for (int i = 0; i < 3; ++i) {
      uint32_t u;
      memcpy(&u, &t[i], 4);
      St32(a + 4 * i, u);
    }
  }
  RT_INFO("bots: teleport (%s): %zu position vectors near %.1f,%.1f,%.1f moved", who, hits.size(), pos.x, pos.y,
          pos.z);
  return hits.size();
}

// ---- the brain: built in, or bot_brain.dll reloaded when it changes ----------

void BrainLog(const char* text) { RT_INFO("%s", text); }

// The navigation mesh for the brain (World::route, World::block).
nav::Memory g_nav_memory[kMaxBots];
int BrainRoute(int slot, Vec a, Vec b, Vec* corners, int max) {
  float fa[3] = {a.x, a.y, a.z}, fb[3] = {b.x, b.y, b.z};
  static float buf[256 * 3];
  int n = nav::Route(fa, fb, buf, std::min(max, 256), slot >= 0 && slot < kMaxBots ? &g_nav_memory[slot] : nullptr);
  for (int i = 0; i < n; ++i) corners[i] = Vec{buf[3 * i], buf[3 * i + 1], buf[3 * i + 2]};
  return n;
}
float BrainGround(float x, float z) { return TerrainHeight(x, z); }
float BrainLearned(int what, int team, Vec at, float radius) {
  return knowledge::Get(knowledge::What(std::clamp(what, 0, 2)), team, at.x, at.z, radius);
}
bool BrainSight(Vec a, Vec b) {
  float fa[3] = {a.x, a.y, a.z}, fb[3] = {b.x, b.y, b.z};
  return InSight(fa, fb);
}
void BrainBlock(Vec at) {
  float p[3] = {at.x, at.y, at.z};
  nav::Block(p);
}

struct BrainModule {
  botapi::ThinkFn think = &BotBrainThink;
#ifdef _WIN32
  // BF_BOT_BRAIN=<path of bot_brain.dll>: loaded from a copy (so the build can
  // write the DLL), again whenever the file changes.
  std::filesystem::file_time_type stamp{};
  HMODULE module = nullptr;
  int loads = 0;
  bool reloaded = false;  // the scenario is judged afresh
  double next_check = 0;
  void Poll(double now) {
    static const char* path = getenv("BF_BOT_BRAIN");
    if (!path || now < next_check) return;
    next_check = now + 0.5;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec || t == stamp) return;
    // Settled: the linker is done writing it.
    if (std::filesystem::file_time_type::clock::now() - t < std::chrono::milliseconds(500)) return;
    std::filesystem::path copy = std::filesystem::temp_directory_path() /
                                 ("bot_brain_" + std::to_string(GetCurrentProcessId()) + "_" +
                                  std::to_string(++loads) + ".dll");
    std::filesystem::copy_file(path, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return;
    stamp = t;
    HMODULE m = LoadLibraryW(copy.c_str());
    auto fn = m ? reinterpret_cast<botapi::ThinkFn>(GetProcAddress(m, "BotBrainThink")) : nullptr;
    auto ver = m ? reinterpret_cast<int (*)()>(GetProcAddress(m, "BotBrainVersion")) : nullptr;
    if (!fn || !ver || ver() != botapi::kVersion) {
      RT_WARN("bots: %s is not a usable brain (version %d, want %d)", path, ver ? ver() : -1, botapi::kVersion);
      if (m) FreeLibrary(m);
      return;
    }
    think = fn;
    if (module) FreeLibrary(module);
    module = m;
    reloaded = true;
    RT_INFO("bots: brain loaded from %s (#%d)", path, loads);
  }
#else
  bool reloaded = false;
  void Poll(double) {}
#endif
};

// ---- the director: the host's view of the match --------------------------------

struct Director {
  std::vector<botapi::BotIO> bots;
  std::vector<Soldier> soldiers;
  std::vector<Point> points;
  std::vector<Vehicle> vehicles;
  double now = 0;  // (bots[i] is bot player i, bot_players.cpp)

  // Reads the match; true when the flags changed.
  bool Refresh(bool rescan) {
    static std::vector<uint32_t> soldier_objs, point_objs, vehicle_objs;
    if (rescan) {
      soldier_objs = Scan(kSoldierVtable);
      point_objs = Scan(kCapturePointVtable);
      vehicle_objs = Scan(kVehicleVtable);
    }
    vehicles.clear();
    for (uint32_t o : vehicle_objs) {
      Vehicle v;
      if (ReadVehicle(o, v)) vehicles.push_back(v);
    }
    soldiers.clear();
    for (uint32_t o : soldier_objs) {
      Soldier s;
      if (ReadSoldier(o, s)) soldiers.push_back(s);
    }
    // A seated soldier sits at its vehicle: the nearest one.
    for (Soldier& s : soldiers) {
      if (s.vehicle != kSeated) continue;
      float best = 8.0f;
      for (const Vehicle& v : vehicles)
        if (float dd = botapi::Dist3D(s.pos, v.pos); dd < best) best = dd, s.vehicle = v.obj;
    }
    std::vector<Point> fresh;
    for (uint32_t o : point_objs) {
      Point p;
      if (ReadPoint(o, p)) fresh.push_back(p);
    }
    bool changed = fresh.size() != points.size();
    points = fresh;
    for (botapi::BotIO& b : bots)
      if (b.soldier && !Find(b.soldier)) b.soldier = 0;
    return changed;
  }

  const Vehicle* FindVehicle(uint32_t obj) const {
    for (const Vehicle& v : vehicles)
      if (v.obj == obj) return &v;
    return nullptr;
  }
  const Soldier* Find(uint32_t obj) const {
    for (const Soldier& s : soldiers)
      if (s.obj == obj) return &s;
    return nullptr;
  }
};

// The advisor for the brain (World::ask, World::answer; bot_llm.h): the bot's
// surroundings, 80 m across, with up to 6 places 6 to 25 m off to pick from.
// (The director's thread only.)
// A place it learned (botllm::Recall) is answered at once, without asking;
// each answer taken is judged (JudgeAdvice) and learned from.
const Director* g_advised = nullptr;
struct Advice {
  std::vector<Vec> places;  // offered
  uint32_t soldier = 0;
  int kind = 0;
  float at[3] = {}, dir = 0;
  bool recalled = false;  // an answer ready: `place`
  Vec place;
  // Judging the one taken: until when (0: none), the case.
  double judge_until = 0;
  int64_t judge_case = 0;
  float judge_place[3] = {};
} g_advice[kMaxBots];
bool BrainAsk(int slot, int kind, const char* situation) {
  if (!g_advised || slot < 0 || slot >= kMaxBots || slot >= int(g_advised->bots.size())) return false;
  const Soldier* me = g_advised->Find(g_advised->bots[size_t(slot)].soldier);
  if (!me) return false;
  Advice& a = g_advice[slot];
  if (a.judge_until || a.recalled) return false;  // (one piece of advice at a time)
  // Which way: where it faces (stuck), or the nearest enemy (cover).
  float dir = me->yaw, nearest = 1e9f;
  if (kind == 2 || kind == 5)
    for (const Soldier& s : g_advised->soldiers)
      if (s.team != me->team && s.health > 0 && botapi::Dist2D(s.pos, me->pos) < nearest)
        nearest = botapi::Dist2D(s.pos, me->pos), dir = std::atan2(s.pos.x - me->pos.x, s.pos.z - me->pos.z);
  float at[3] = {me->pos.x, me->pos.y, me->pos.z}, learned[3];
  if (botllm::Recall(kind, at, dir, learned) && std::hypot(learned[0] - at[0], learned[2] - at[2]) > 3.0f) {
    a.kind = kind, a.soldier = me->obj, a.dir = dir, a.recalled = true, a.place = Vec{learned[0], learned[1], learned[2]};
    memcpy(a.at, at, sizeof(at));
    return true;
  }
  if (!botllm::CanAsk(slot)) return false;
  float found[6 * 3];
  int n = nav::Places(at, 6.0f, 25.0f, found, 6);
  if (n < 2) return false;
  using botllm::Marker;
  botllm::MapView m;
  m.cx = me->pos.x, m.cz = me->pos.z, m.y = me->pos.y, m.half = 40, m.kind = kind;
  m.ground = [](float x, float z) {
    float h = TerrainHeight(x, z);
    return h < -1e8f ? NAN : h;
  };
  m.walkable = [](float x, float y, float z) { return nav::Walkable(x, y, z); };
  // The eye view: from its eyes, toward the enemy when it is about a fight.
  m.sight = [](const float* a, const float* b) { return InSight(a, b); };
  m.eye[0] = me->pos.x, m.eye[1] = me->pos.y + 1.6f, m.eye[2] = me->pos.z;
  m.eye_yaw = kind == 2 || kind == 5 ? dir : me->yaw;
  m.markers.push_back({Marker::kSelf, me->pos.x, me->pos.z, me->yaw});
  for (const Soldier& s : g_advised->soldiers)
    if (s.obj != me->obj && s.health > 0 && std::fabs(s.pos.x - m.cx) < m.half && std::fabs(s.pos.z - m.cz) < m.half)
      m.markers.push_back({s.team == me->team ? Marker::kFriend : Marker::kEnemy, s.pos.x, s.pos.z});
  for (const Vehicle& v : g_advised->vehicles)
    if (std::fabs(v.pos.x - m.cx) < m.half && std::fabs(v.pos.z - m.cz) < m.half)
      m.markers.push_back({Marker::kVehicle, v.pos.x, v.pos.z});
  for (const Point& p : g_advised->points)  // (flags: the enemy's orange, the rest yellow)
    m.markers.push_back({p.owner != 0 && p.owner != me->team ? Marker::kObjectiveEnemy : Marker::kObjective, p.pos.x, p.pos.z});
  std::vector<Vec> places;
  for (int k = 0; k < n; ++k) {
    const float* p = &found[3 * k];
    places.push_back(Vec{p[0], p[1], p[2]});
    // Cover: the nearest enemy, and whether one of the (up to 4) within 70 m sees it.
    char note[96] = {};
    if (kind == 2 || kind == 5) {
      float nearest = 1e9f;
      bool seen = false;
      int looked = 0;
      for (const Soldier& s : g_advised->soldiers) {
        if (s.team == me->team || s.health <= 0) continue;
        float d = std::hypot(s.pos.x - p[0], s.pos.z - p[2]);
        nearest = std::min(nearest, d);
        float eye[3] = {s.pos.x, s.pos.y + 1.6f, s.pos.z}, chest[3] = {p[0], p[1] + 1.2f, p[2]};
        if (d < 70.0f && looked < 4) ++looked, seen |= InSight(eye, chest);
      }
      if (nearest < 1e8f)
        snprintf(note, sizeof(note), "%.0f m from the nearest enemy, %s", nearest, seen ? "an enemy can see it" : "no enemy sees it");
    }
    m.markers.push_back({Marker::kPlace, p[0], p[2], 0, k + 1, note});
  }
  if (!botllm::Ask(slot, situation, m, n)) return false;
  a.places = places, a.soldier = me->obj, a.kind = kind, a.dir = dir, a.recalled = false;
  memcpy(a.at, at, sizeof(at));
  return true;
}
bool BrainAnswer(int slot, Vec* place) {
  if (slot < 0 || slot >= kMaxBots || !g_advised || slot >= int(g_advised->bots.size())) return false;
  Advice& a = g_advice[slot];
  int k = 0;
  int64_t id = 0;
  if (a.recalled) {
    a.recalled = false, *place = a.place;
  } else if (botllm::Answer(slot, &k, nullptr, &id) && k >= 1 && k <= int(a.places.size())) {
    *place = a.places[size_t(k - 1)];
  } else {
    return false;
  }
  if (g_advised->bots[size_t(slot)].soldier != a.soldier) return false;
  a.judge_until = g_advised->now + (a.kind == 2 ? 12.0 : a.kind == 5 ? 15.0 : 20.0), a.judge_case = id;
  a.judge_place[0] = place->x, a.judge_place[1] = place->y, a.judge_place[2] = place->z;
  return true;
}
// What came of it: a way round worked if the bot got over 12 m from where it
// was stuck within 20 s; cover, if it was still alive 12 s later; moving on
// from standing idle, if it was over 5 m on and alive 20 s later.
void JudgeAdvice(const Director& d) {
  for (int slot = 0; slot < kMaxBots && slot < int(d.bots.size()); ++slot) {
    Advice& a = g_advice[slot];
    if (!a.judge_until) continue;
    const Soldier* me = d.bots[size_t(slot)].soldier == a.soldier ? d.Find(a.soldier) : nullptr;
    bool died = !me || me->health <= 0;
    bool moved = me && std::hypot(me->pos.x - a.at[0], me->pos.z - a.at[2]) > (a.kind == 4 ? 5.0f : 12.0f);
    bool fight = a.kind == 2 || a.kind == 5;  // (judged by surviving)
    if (died && !fight) a.judge_until = 0;  // (no telling)
    else if (died || d.now >= a.judge_until || (a.kind == 1 && moved)) {
      botllm::Learn(a.kind, a.at, a.dir, a.judge_place, fight ? !died : moved, a.judge_case);
      a.judge_until = 0;
    }
  }
}

// BF_BOT_LOG=<file.csv>: every bot's track twice a second, with the flags and
// vehicles now and then (tools/bot_paths.py draws it).
struct TraceLog {
  FILE* f = nullptr;
  double next_row = 0, next_map = 0;
  TraceLog() {
    if (const char* p = getenv("BF_BOT_LOG")) {
      f = fopen(p, "w");
      if (f) fprintf(f, "t,bot,team,x,y,z,vehicle,state\n");
    }
  }
  void Write(const Director& d, bool flags_changed, const char* result) {
    if (!f) return;
    if (flags_changed || d.now >= next_map) {
      next_map = d.now + 10;
      for (size_t i = 0; i < d.points.size(); ++i)
        fprintf(f, "#flag,%zu,%.1f,%.1f,%d\n", i, d.points[i].pos.x, d.points[i].pos.z, d.points[i].owner);
      for (const Vehicle& v : d.vehicles) fprintf(f, "#vehicle,%s,%.1f,%.1f\n", v.kind, v.pos.x, v.pos.z);
    }
    if (result) fprintf(f, "#result,%s\n", result);
    if (d.now >= next_row) {
      next_row = d.now + 0.5;
      for (const botapi::BotIO& b : d.bots) {
        const Soldier* s = b.soldier ? d.Find(b.soldier) : nullptr;
        if (!s) continue;
        const Vehicle* v = s->vehicle ? d.FindVehicle(s->vehicle) : nullptr;
        fprintf(f, "%.2f,%d,%d,%.1f,%.1f,%.1f,%s,%s\n", d.now, b.slot + 1, s->team, s->pos.x, s->pos.y, s->pos.z,
                v ? v->kind : (s->vehicle ? "?" : ""), b.state);
      }
    }
    fflush(f);
  }
};


// ---- the local player's vehicle (for the PC vehicle keys) -------------------
//
// Client side, so it works joined as well as hosting:
//   ClientSoldierEntity: vtable 82007518, position +3D0 (x, y, z); its vehicle
//     (ClientVehicleEntity, vtable 82006DC8) *(*(+220) + 130), 0 on foot.
//   ClientVehicleEntity: its kind's name *(*(+14) + C), as the server's.
// The player entity doesn't point at its soldier, so ours is the one nearest
// the camera (xgpu::CameraEye). Read with ReadProcessMemory: a level can be
// unloaded under this thread.
constexpr uint32_t kClientSoldierVtable = 0x82007518, kClientVehicleVtable = 0x82006DC8;
std::atomic<int> g_local_vehicle{rt::kOnFoot};
std::atomic<int64_t> g_vehicle_asked{0};

bool SafeLd32(uint32_t a, uint32_t& v) {
  uint32_t raw;
  if (!ReadProcessMemory(GetCurrentProcess(), g_base + a, &raw, 4, nullptr)) return false;
  v = _byteswap_ulong(raw);
  return true;
}
bool SafeLdF(uint32_t a, float& f) {
  uint32_t u;
  if (!SafeLd32(a, u)) return false;
  memcpy(&f, &u, 4);
  return true;
}
std::string SafeString(uint32_t a) {
  char buf[33] = {};
  SIZE_T got = 0;
  ReadProcessMemory(GetCurrentProcess(), g_base + a, buf, 32, &got);
  return std::string(buf, strnlen(buf, got));
}
std::vector<uint32_t> SafeScan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = _byteswap_ulong(vtable);
  static uint32_t page_words[1024];
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    if (!ReadProcessMemory(GetCurrentProcess(), g_base + page, page_words, 0x1000, nullptr)) continue;
    for (int i = 0; i < 1024; ++i)
      if (page_words[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}

int VehicleClass(const std::string& kind) {
  static const char* kPlanes[] = {"Corsair", "Zero", "Aichi", "Val", "Kate", "Dauntless", "Avenger", "Wildcat"};
  static const char* kGuns[] = {"Bofors", "HMG", "AA", "Type96", "Flak"};
  for (const char* p : kPlanes)
    if (kind.find(p) != std::string::npos) return rt::kFlying;
  for (const char* g : kGuns)
    if (kind.rfind(g, 0) == 0) return rt::kGunner;
  return rt::kDriving;
}

void WatchVehicle() {
  std::vector<uint32_t> soldiers;
  auto last_scan = std::chrono::steady_clock::time_point{};
  std::string last_kind;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto now = std::chrono::steady_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    float eye[3];
    // Only while the game asks (in a level) and draws a level view.
    if (now_ms - g_vehicle_asked.load() > 1000 || !xgpu::CameraEye(eye)) {
      g_local_vehicle = rt::kOnFoot;
      soldiers.clear();
      continue;
    }
    bool stale = false;
    for (uint32_t c : soldiers) {
      uint32_t vt = 0;
      if (!SafeLd32(c, vt) || vt != kClientSoldierVtable) stale = true;
    }
    // A full heap scan: at most every 3 s (soldiers come and go), 1 s when one went.
    auto since = now - last_scan;
    if ((stale && since > std::chrono::seconds(1)) || since > std::chrono::seconds(3) ||
        (soldiers.empty() && since > std::chrono::seconds(1))) {
      soldiers = SafeScan(kClientSoldierVtable);
      last_scan = now;
    }
    // Ours: the nearest to the eye (the camera rides behind a vehicle).
    uint32_t mine = 0;
    float best = 60.0f;
    static const bool debug = getenv("BF_VEH_DEBUG") != nullptr;
    static int64_t next_debug = 0;
    bool log_now = debug && now_ms >= next_debug;
    if (log_now) next_debug = now_ms + 2000;
    for (uint32_t c : soldiers) {
      float x, y, z;
      if (!SafeLdF(c + 0x3D0, x) || !SafeLdF(c + 0x3D4, y) || !SafeLdF(c + 0x3D8, z)) continue;
      float d = std::sqrt((x - eye[0]) * (x - eye[0]) + (y - eye[1]) * (y - eye[1]) + (z - eye[2]) * (z - eye[2]));
      if (d < best) best = d, mine = c;
      if (log_now) {
        uint32_t l = 0, vv = 0;
        SafeLd32(c + 0x220, l);
        if (l) SafeLd32(l + 0x130, vv);
        RT_INFO("vehicle keys: eye %.1f,%.1f,%.1f soldier %08X at %.1f,%.1f,%.1f d %.1f veh %08X", eye[0], eye[1],
                eye[2], c, x, y, z, d, vv);
      }
    }

    int kind_class = rt::kOnFoot;
    std::string kind;
    uint32_t link = 0, v = 0, vt = 0, data = 0, name = 0;
    if (mine && SafeLd32(mine + 0x220, link) && link && SafeLd32(link + 0x130, v) && v && SafeLd32(v, vt) &&
        vt == kClientVehicleVtable) {
      if (SafeLd32(v + 0x14, data) && data && SafeLd32(data + 0xC, name) && name) kind = SafeString(name);
      kind_class = VehicleClass(kind);
    }
    if (kind != last_kind) {
      RT_INFO("vehicle keys: %s", kind.empty() ? "on foot" : kind.c_str());
      last_kind = kind;
    }
    // Steady for 0.45 s before it counts (a frame's camera can be elsewhere).
    static int pending = -1, seen = 0;
    if (kind_class != pending) pending = kind_class, seen = 0;
    if (++seen >= 3) g_local_vehicle = kind_class;
  }
}

}  // namespace

int LocalVehicle() {
  static const bool off = getenv("BF_NO_VEHICLE_KEYS") != nullptr;
  if (off) return kOnFoot;
  static std::once_flag started;
  std::call_once(started, [] { std::thread(WatchVehicle).detach(); });
  g_vehicle_asked = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  return g_local_vehicle.load(std::memory_order_relaxed);
}

// Host: start the director (once the bots are launched).
void StartBotDirector() {
  static bool started = false;
  if (started) return;
  started = true;
  std::thread([] {
    Director d;
    BrainModule brain;
    TraceLog trace;
    const char* scenario = getenv("BF_BOT_SCENARIO");
    int result = 0;
    char result_text[160] = {};
    double result_at = 0;
    const double g0 = GuestSeconds();  // (the guest's time: --speed)
    double last_scan = -10, last_log = 0;
    while (true) {
      // (--speed: thinking as often in the game's time, its clock as fast)
      std::this_thread::sleep_for(std::chrono::milliseconds(std::max(5, int(33 / g_time_speed))));
      d.now = GuestSeconds() - g0;
      brain.Poll(d.now);
      if (brain.reloaded) {
        brain.reloaded = false;
        result = 0;
        // BF_BOT_RESET_ON_RELOAD: every bot starts over with the new brain.
        if (getenv("BF_BOT_RESET_ON_RELOAD"))
          for (botapi::BotIO& b : d.bots) b.mem_tag = 0;
      }
      if (g_in_menus) continue;
      std::string map;
      {
        std::lock_guard lock(g_host_map_mutex);  // (the dedicated server's rotation changes it)
        map = g_host_map;
      }
      bool rescan = d.now - last_scan > 2.0;
      if (rescan) last_scan = d.now;
      // Bot players (bot_players.cpp): an entry each, their soldier as it is.
      while (int(d.bots.size()) < std::min(BotPlayerCount(), kMaxBots)) {
        botapi::BotIO io{};
        io.slot = int(d.bots.size());
        d.bots.push_back(io);
      }
      bool flags_changed = d.Refresh(rescan);
      for (botapi::BotIO& b : d.bots) b.soldier = BotPlayerSoldier(b.slot);
      // Self-play tuning (bot_tuning.h): each bot's settings; a flag taken
      // counts for its team's bots within 25 m of it.
      tuning::UseMap(map, g_training);
      for (botapi::BotIO& b : d.bots) b.tune = tuning::For(b.slot);
      {
        static std::vector<int> owners;
        if (flags_changed || owners.size() != d.points.size()) owners.clear();
        for (size_t i = 0; i < d.points.size(); ++i) {
          const Point& p = d.points[i];
          if (i < owners.size() && owners[i] != p.owner && p.owner > 0)
            for (const botapi::BotIO& b : d.bots)
              if (const Soldier* s = b.soldier ? d.Find(b.soldier) : nullptr)
                if (s->team == p.owner && botapi::Dist2D(s->pos, p.pos) < 25) tuning::Capture(b.slot);
          if (i < owners.size()) owners[i] = p.owner;
          else owners.push_back(p.owner);
        }
      }
      tuning::Update(d.now, int(d.bots.size()));
      if (d.points.empty()) continue;

      botapi::World w{};
      w.version = botapi::kVersion;
      w.now = d.now;
      w.soldiers = d.soldiers.data(), w.soldier_count = int(d.soldiers.size());
      w.points = d.points.data(), w.point_count = int(d.points.size());
      w.vehicles = d.vehicles.data(), w.vehicle_count = int(d.vehicles.size());
      w.bots = d.bots.data(), w.bot_count = int(d.bots.size());
      w.points_changed = flags_changed;
      w.scenario = scenario;
      w.result = result;
      w.log = BrainLog;
      // (UTF-8, as the brain opens it: path::string() throws on Windows for a
      // folder name outside the ANSI code page)
      const auto nav_u8 = (g_user_root / ("bot_nav_" + map + ".txt")).u8string();
      const std::string nav(nav_u8.begin(), nav_u8.end());
      w.nav_path = nav.c_str();
      bool have_mesh = nav::Ready(map);  // (loads a new map's once)
      if (have_mesh) {  // its main piece: the one the flags are on
        std::vector<float> flags;
        for (const Point& p : d.points) flags.insert(flags.end(), {p.pos.x, p.pos.y, p.pos.z});
        nav::ChooseMain(flags.data(), int(d.points.size()));
      }
      w.route = have_mesh ? BrainRoute : nullptr;
      w.block = have_mesh ? BrainBlock : nullptr;
      SightForMap(map);
      w.sight = BrainSight;
      w.ground = BrainGround;
      w.learned = BrainLearned;
      // The advisor (or what it taught on this map).
      g_advised = &d;
      botllm::UseMap(map);
      JudgeAdvice(d);
      w.ask = have_mesh ? BrainAsk : nullptr;
      w.answer = have_mesh ? BrainAnswer : nullptr;
      w.training = g_training;
      // Training: the run's record (bot_llm.h): the whole level drawn once a
      // map (the flags' extent and 150 m round), and every bot's track.
      static std::string recorded;
      // (once the map's collision is in: its terrain and structures are drawn from it)
      if (g_training && have_mesh && recorded != map && SightReadyFor(map)) {
        recorded = map;
        float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f, ys = 0;
        for (const Point& p : d.points)
          x0 = std::min(x0, p.pos.x), x1 = std::max(x1, p.pos.x), z0 = std::min(z0, p.pos.z), z1 = std::max(z1, p.pos.z),
          ys += p.pos.y;
        botllm::MapView level;
        level.cx = (x0 + x1) / 2, level.cz = (z0 + z1) / 2, level.half = std::max(x1 - x0, z1 - z0) / 2 + 350.0f;
        level.y = ys / float(d.points.size());
        level.ground = [](float x, float z) {
          float h = TerrainHeight(x, z);
          return h < -1e8f ? NAN : h;
        };
        level.walkable = [](float x, float y, float z) { return nav::Walkable(x, y, z); };
        level.tops = [](float x0, float z0, float mpp, int n, std::vector<float>& top) {
          return CollisionTops(x0, z0, mpp, n, top);
        };
        for (const Point& p : d.points) level.markers.push_back({botllm::Marker::kObjective, p.pos.x, p.pos.z});
        botllm::SetLevelMap(map, level);
      }
      for (const botapi::BotIO& b : d.bots)
        if (const Soldier* s = b.soldier ? d.Find(b.soldier) : nullptr) botllm::Track(b.slot, s->team, s->pos.x, s->pos.z);
      // Learning (bot_knowledge.h): where the real players go, once a second
      // (not the bots: what they do is what is being improved); saved each minute.
      knowledge::UseMap(map);
      static double next_visit = 0, next_save = 0;
      if (d.now >= next_visit) {
        next_visit = d.now + 1.0;
        for (const Soldier& s : d.soldiers) {
          bool bot = false;
          for (const botapi::BotIO& b : d.bots) bot |= b.soldier == s.obj;
          static const bool from_bots = getenv("BF_BOT_LEARN_FROM_BOTS") != nullptr;  // (testing)
          if (!bot || from_bots) knowledge::Add(knowledge::kVisits, s.team, s.pos.x, s.pos.z);
        }
      }
      if (d.now >= next_save) next_save = d.now + 60.0, knowledge::Save();
      brain.think(&w);
      for (botapi::BotIO& b : d.bots) {
        const Pad& pad = b.pad;
        SetBotPlayerInput(b.slot, pad.lx, pad.ly, pad.rx, pad.ry, pad.buttons, pad.triggers);
        const Soldier* s = b.soldier ? d.Find(b.soldier) : nullptr;
        SetBotPlayerVehicle(b.slot, s && s->vehicle != kSeated ? s->vehicle : 0);
        // (its soldier is the server's own: moving it there is all)
        if (b.teleport && s) MoveVectorsNear(s->pos, b.to, "server");
      }
      const char* new_result = nullptr;
      if (w.result && !result) {
        result = w.result;
        snprintf(result_text, sizeof(result_text), "%s", w.result_text);
        result_at = d.now;
        new_result = result_text;
        RT_INFO("scenario: %s", result_text);
        if (const char* p = getenv("BF_BOT_RESULT"))
          if (FILE* f = fopen(p, "w")) fprintf(f, "%s\n", result_text), fclose(f);
      }
      trace.Write(d, flags_changed, new_result);
      // DEBUG BF_SEAT_DUMP: the soldier's seat fields each second, to see them change on E.
      static double next_dump = 0;
      if (getenv("BF_SEAT_DUMP") && d.now >= next_dump) {
        next_dump = d.now + 1.0;
        for (const botapi::BotIO& b : d.bots) {
          const Soldier* s = b.soldier ? d.Find(b.soldier) : nullptr;
          if (!s) continue;
          std::string words;
          for (uint32_t off = 0x330; off < 0x380; off += 4) {
            char w8[12];
            snprintf(w8, sizeof(w8), " %08X", Ld32(s->obj + off));
            words += w8;
          }
          uint32_t seat = Ld32(s->obj + kSoldierSeat);
          RT_INFO("seat: bot%d at %.1f,%.1f,%.1f seat %08X->+2C %08X | 330:%s", b.slot + 1, s->pos.x, s->pos.y,
                  s->pos.z, seat, Readable(seat) ? Ld32(seat + kSeatVehicle) : 0, words.c_str());
        }
      }
      // BF_BOT_SCENARIO_EXIT: end the match (and with it the bots) once judged.
      if (result && getenv("BF_BOT_SCENARIO_EXIT") && d.now - result_at > 1.0) {
        fflush(stdout);
        std::_Exit(result > 0 ? 0 : 3);
      }
      if (d.now - last_log > 10) {
        last_log = d.now;
        std::string s;
        for (const botapi::BotIO& b : d.bots) {
          const Soldier* me = b.soldier ? d.Find(b.soldier) : nullptr;
          char buf[128];
          if (me) snprintf(buf, sizeof(buf), " [%d: team %d at %.0f,%.0f,%.0f, %s]", b.slot + 1, me->team, me->pos.x,
                           me->pos.y, me->pos.z, b.state);
          else snprintf(buf, sizeof(buf), " [%d: no soldier]", b.slot + 1);
          s += buf;
        }
        RT_INFO("bots: %zu soldiers, %zu points;%s", d.soldiers.size(), d.points.size(), s.c_str());
      }
    }
  }).detach();
}

// The pad's game hook (xam.cpp; Bad Company's adds its automation there):
// nothing for BF1943 (its bots are players on the server, not pads).
bool AddBotInput(XINPUT_GAMEPAD&) { return false; }

}  // namespace rt
