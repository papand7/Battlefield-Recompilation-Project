// Bot AI for --bots: the host's server decides, the bot clients press buttons.
//
// Modelled on DICE's own bots (Battlefield 1942 / 2): a strategic layer gives
// every control point a "temperature" (how much the team needs it) and
// spreads the team's bots over the hot ones; each bot is a reactive agent that
// picks the most needed behaviour every tick (engage an enemy, capture its
// point, get unstuck, respawn) and turns it into pad input. The director runs
// in the host process, which owns the server and so sees every soldier and
// control point; the bot processes read their pad from shared memory.
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
// A soldier object exists while its player is alive. Local games name every
// player "Player1", so the director finds which soldier is which bot by
// probing: it tips one bot's view up and watches whose pitch moves.
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
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace rt {

namespace {

constexpr uint32_t kSoldierVtable = 0x82013A40;
constexpr uint32_t kCapturePointVtable = 0x820190D0;
constexpr uint32_t kVehicleVtable = 0x82013068;
constexpr int kMaxBots = 16;
constexpr float kPi = 3.14159265f;

struct BotSlot {
  volatile float lx, ly, rx, ry;  // -1..1 (ly + = forward, ry + = look up)
  volatile uint32_t buttons;      // XINPUT_GAMEPAD_* bits
  volatile uint32_t triggers;     // lt | rt << 8
  volatile uint32_t active;
};
struct BotShared {
  uint32_t magic;
  BotSlot slots[kMaxBots];
};
constexpr uint32_t kMagic = 0xB0751943;

BotShared* g_shared = nullptr;
int g_my_slot = -1;  // in a bot process

BotShared* MapShared(const std::string& name, bool create) {
#ifndef _WIN32
  int fd = shm_open(name.c_str(), create ? O_CREAT | O_RDWR : O_RDWR, 0600);
  if (fd < 0) return nullptr;
  if (create && ftruncate(fd, sizeof(BotShared)) != 0) {
    close(fd);
    return nullptr;
  }
  void* m = mmap(nullptr, sizeof(BotShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return nullptr;
  auto* p = static_cast<BotShared*>(m);
  if (create) {
    memset(p, 0, sizeof(*p));
    p->magic = kMagic;
  }
  return p;
#else
  HANDLE h = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(BotShared), name.c_str())
                    : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!h) return nullptr;
  auto* p = static_cast<BotShared*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BotShared)));
  if (p && create) {
    memset(p, 0, sizeof(*p));
    p->magic = kMagic;
  }
  return p;
#endif
}

float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
bool Readable(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a) && IsCommitted(a + 0x400); }
float Wrap(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

struct Vec { float x = 0, y = 0, z = 0; };
float Dist2D(Vec a, Vec b) { return std::hypot(a.x - b.x, a.z - b.z); }

struct Soldier {
  uint32_t obj;
  int team;
  Vec pos;
  float yaw, pitch;
};
struct Point {
  uint32_t obj;
  int owner;
  Vec pos;
};
struct Vehicle {
  uint32_t obj;
  std::string kind;
  Vec pos;
};

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

bool ReadSoldier(uint32_t o, Soldier& s) {
  if (!Readable(o) || Ld32(o) != kSoldierVtable) return false;
  s.obj = o;
  s.team = int(Ld32(o + 0x48));
  s.pos = {LdF(o + 0x180), LdF(o + 0x184), LdF(o + 0x188)};
  uint32_t aim = Ld32(o + 0x130);
  if (!Readable(aim)) return false;
  s.yaw = LdF(aim + 0x134);
  s.pitch = LdF(aim + 0x138);
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
  v.kind.assign(n, strnlen(n, 32));
  v.pos = {LdF(body + 0xF0), LdF(body + 0xF4), LdF(body + 0xF8)};
  return std::isfinite(v.pos.x) && std::fabs(v.pos.x) < 1e5f;
}

// A soldier's current seat (entry component): its own while on foot, a
// vehicle's when in one. The seat's +2C points at its vehicle + 190.
constexpr uint32_t kSoldierSeat = 0x350, kSeatVehicle = 0x2C, kVehicleSeatLink = 0x190;
uint32_t SeatVehicle(uint32_t soldier) {
  uint32_t seat = Ld32(soldier + kSoldierSeat);
  if (!Readable(seat)) return 0;
  uint32_t link = Ld32(seat + kSeatVehicle);
  if (!Readable(link)) return 0;
  uint32_t v = link - kVehicleSeatLink;
  return Readable(v) && Ld32(v) == kVehicleVtable ? v : 0;
}

// ---- one bot ---------------------------------------------------------------

struct Bot {
  int slot;
  uint32_t soldier = 0;      // mapped soldier object (0 = dead / not found)
  int target_point = -1;     // index into points
  Vec target_offset;         // where around the point to stand
  Vec last_pos;
  double last_moved = 0;     // time it last made progress
  double unstuck_until = 0;
  float unstuck_dir = 1;
  double dead_since = 0;
  double next_spawn_press = 0;
  float strafe = 0;
  double next_strafe = 0;
  // Landing craft: the one it is going for or in, since when, its heading.
  uint32_t boat = 0;
  bool in_boat = false;
  double boat_since = 0, next_enter_press = 0;
  Vec boat_last;
  double boat_moved = 0;
  float boat_heading = 0;
  bool spawn_choice = false;
  int team = 0;
  // Seat test: after pressing E, a view sweep that the soldier doesn't
  // follow means it sits in the boat.
  double sweep_until = 0;
  float sweep_yaw = 0;
  float boat_travel = 0, boat_travel3d = 0;
  int seat_switches = 0;
  Vec boat_start;
  int boat_state = 0, board_tries = 0;
  bool ride_land = false;  // a jeep or tank rather than a landing craft
  bool released = false;   // the landing craft is down from its crane
  double board_cycle = 0;
  Vec board_pos;
  double release_try = 0;
  double boat_lost_since = 0;
  double no_boat_until = 0, drive_since = 0;
  double next_seat_switch = 0, leaving_since = 0;
  uint32_t bad_boat = 0;  // gave up on it
};

struct Director {
  std::vector<Bot> bots;
  std::vector<Soldier> soldiers;
  std::vector<Point> points;
  std::vector<Vehicle> vehicles;
  std::mt19937 rng{1943};
  double now = 0;
  // probing
  int probe_slot = -1;
  double probe_until = 0;
  float probe_dir = -1.0f;
  std::vector<std::pair<uint32_t, float>> probe_pitch;  // unmapped soldiers before the probe

  float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

  void Refresh(bool rescan) {
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
    std::vector<Point> fresh;
    for (uint32_t o : point_objs) {
      Point p;
      if (ReadPoint(o, p)) fresh.push_back(p);
    }
    if (fresh.size() != points.size()) {
      points = fresh;
      for (Bot& b : bots) b.target_point = -1;
    } else {
      points = fresh;
    }
    for (Bot& b : bots)
      if (b.soldier && !Find(b.soldier)) b.soldier = 0, b.dead_since = now;
  }

  const Vehicle* FindVehicle(uint32_t obj) const {
    for (const Vehicle& v : vehicles)
      if (v.obj == obj) return &v;
    return nullptr;
  }
  bool BoatTaken(uint32_t obj, const Bot& me) const {
    for (const Bot& b : bots)
      if (&b != &me && b.boat == obj) return true;
    return false;
  }
  // Nearest flag to a position.
  float NearestPoint(Vec p) const {
    float best = 1e9f;
    for (const Point& q : points) best = std::min(best, Dist2D(p, q.pos));
    return best;
  }

  const Soldier* Find(uint32_t obj) const {
    for (const Soldier& s : soldiers)
      if (s.obj == obj) return &s;
    return nullptr;
  }
  bool Mapped(uint32_t obj) const {
    for (const Bot& b : bots)
      if (b.soldier == obj) return true;
    return false;
  }

  // Which soldier belongs to which bot: tip one unmapped bot's view up and
  // watch which unmapped soldier's pitch rises.
  void Probe() {
    if (probe_slot >= 0) {
      if (now < probe_until) return;
      for (auto& [obj, before] : probe_pitch) {
        const Soldier* s = Find(obj);
        // (Up or down: a view already at the top can't go further up.)
        if (s && std::fabs(s->pitch - before) > 0.25f) {
          for (Bot& b : bots)
            if (b.slot == probe_slot) {
              b.soldier = obj;
              b.last_pos = s->pos;
              b.last_moved = now;
              RT_INFO("bots: bot %d is soldier %08X (team %d)", probe_slot + 1, obj, s->team);
            }
          break;
        }
      }
      probe_slot = -1;
      return;
    }
    probe_pitch.clear();
    for (const Soldier& s : soldiers)
      if (!Mapped(s.obj)) probe_pitch.push_back({s.obj, s.pitch});
    if (probe_pitch.empty()) return;
    // Next bot without a soldier, round robin.
    static int next = 0;
    for (size_t i = 0; i < bots.size(); ++i) {
      Bot& b = bots[(next + i) % bots.size()];
      if (b.soldier) continue;
      next = int((next + i + 1) % bots.size());
      probe_slot = b.slot;
      probe_until = now + 0.8;
      probe_dir = -probe_dir;  // alternate, so a view stuck at one end moves
      return;
    }
  }

  // Strategic layer: temperature per point for a team, then the bot takes the
  // hottest one, cooled by distance and by team mates already heading there.
  int ChoosePoint(const Bot& b, const Soldier& me) {
    int best = -1;
    float best_score = -1e9f;
    for (size_t i = 0; i < points.size(); ++i) {
      const Point& p = points[i];
      float temp = p.owner == me.team ? 10.0f : (p.owner == 0 ? 60.0f : 45.0f);
      // Enemy soldiers near a point we own make it hot to defend.
      int threats = 0;
      for (const Soldier& s : soldiers)
        if (s.team != me.team && Dist2D(s.pos, p.pos) < 40) ++threats;
      if (p.owner == me.team) temp += 25.0f * threats;
      int mates = 0;
      for (const Bot& o : bots)
        if (&o != &b && o.target_point == int(i)) ++mates;
      float score = temp - 12.0f * mates - Dist2D(me.pos, p.pos) / 25.0f + Rand(0, 5);
      if (score > best_score) best_score = score, best = int(i);
    }
    return best;
  }

  void Steer(BotSlot& out, const Soldier& me, float want_yaw, float want_pitch) {
    float e = Wrap(want_yaw - me.yaw);
    float rx = std::clamp(-e * 2.0f, -1.5f, 1.5f);
    float ep = want_pitch - me.pitch;
    float ry = std::clamp(ep * 2.0f, -1.5f, 1.5f);
    if (std::fabs(ep) > 0.03f && std::fabs(ry) < 0.35f) ry = ry < 0 ? -0.35f : 0.35f;  // small pitch inputs don't move it
    out.rx = rx;
    out.ry = ry;
  }

  // Walk towards target (world), whatever way the bot faces.
  void Walk(BotSlot& out, const Soldier& me, Vec target, float speed) {
    float dx = target.x - me.pos.x, dz = target.z - me.pos.z;
    float len = std::hypot(dx, dz);
    if (len < 0.5f) { out.lx = out.ly = 0; return; }
    dx /= len, dz /= len;
    float fx = std::sin(me.yaw), fz = std::cos(me.yaw);  // forward
    float rxv = -fz, rzv = fx;                            // right
    out.ly = std::clamp((dx * fx + dz * fz) * speed, -1.0f, 1.0f);
    out.lx = std::clamp((dx * rxv + dz * rzv) * speed, -1.0f, 1.0f);
  }

  void Think(Bot& b) {
    BotSlot& out = g_shared->slots[b.slot];
    BotSlot cmd{};
    cmd.active = 1;
    if (probe_slot == b.slot) {  // being identified: look up or down, nothing else
      cmd.ry = probe_dir;
      Commit(out, cmd);
      return;
    }
    const Soldier* me = b.soldier ? Find(b.soldier) : nullptr;
    if (!me) {
      // Dead or on the spawn screen. When the team holds a flag, first step
      // the spawn choice off the carrier (the spawn screen lists the carrier,
      // then the held flags), then confirm.
      b.boat = 0, b.boat_state = 0;
      int team = 0;
      for (const Bot& o : bots)
        if (const Soldier* s = o.soldier ? Find(o.soldier) : nullptr) team = s->team;
      bool held = false;
      for (const Point& q : points) held |= q.owner != 0 && q.owner == b.team;
      if (now >= b.next_spawn_press) {
        if (held && !b.spawn_choice) {
          cmd.buttons = XINPUT_GAMEPAD_DPAD_RIGHT;
          b.spawn_choice = true;
          b.next_spawn_press = now + 0.6;
        } else {
          cmd.buttons = XINPUT_GAMEPAD_A;
          b.spawn_choice = false;
          b.next_spawn_press = now + 2.0;
        }
      }
      (void)team;
      Commit(out, cmd);
      return;
    }
    b.team = me->team;
    // Behaviour weights (BF1942 style: how much each is needed right now).
    const Soldier* enemy = nullptr;
    float enemy_d = 1e9f;
    for (const Soldier& s : soldiers) {
      if (s.team == me->team) continue;
      float d = std::hypot(Dist2D(s.pos, me->pos), s.pos.y - me->pos.y);
      if (d < enemy_d) enemy_d = d, enemy = &s;
    }
    float w_engage = enemy && enemy_d < 70 ? 100 - enemy_d : 0;
    if (b.target_point < 0 || b.target_point >= int(points.size()) ||
        points[b.target_point].owner == me->team && Rand(0, 1) < 0.01f) {
      b.target_point = getenv("BF_BOT_POINT") ? atoi(getenv("BF_BOT_POINT")) : ChoosePoint(b, *me);  // DEBUG override
      b.target_offset = {Rand(-6, 6), 0, Rand(-6, 6)};
    }
    // Progress / stuck detection.
    if (Dist2D(me->pos, b.last_pos) > 1.5f) b.last_pos = me->pos, b.last_moved = now;
    bool at_point = b.target_point >= 0 && Dist2D(me->pos, points[b.target_point].pos) < 12;
    if (!at_point && now - b.last_moved > 2.5 && now > b.unstuck_until) {
      b.unstuck_until = now + 1.2;
      b.unstuck_dir = Rand(0, 1) < 0.5f ? -1.0f : 1.0f;
      b.last_moved = now;
    }

    // Landing craft: far from every flag (on a carrier), with one nearby that
    // no other bot has, get in, drive to the target flag's shore, get out.
    // (Vehicles are not reliable yet: off unless BF_BOT_VEHICLES is set.)
    static const bool vehicles_on = getenv("BF_BOT_VEHICLES") != nullptr;
    if (vehicles_on && Boat(b, *me, cmd)) {
      Commit(out, cmd);
      return;
    }
    if (w_engage > 0) {
      // Aim at the chest, strafe now and then, fire when on target.
      float dx = enemy->pos.x - me->pos.x, dz = enemy->pos.z - me->pos.z;
      float want_yaw = std::atan2(dx, dz);
      float want_pitch = std::atan2(enemy->pos.y + 1.2f - (me->pos.y + 1.6f), std::hypot(dx, dz));
      Steer(cmd, *me, want_yaw, want_pitch);
      if (std::fabs(Wrap(want_yaw - me->yaw)) < 0.06f && std::fabs(want_pitch - me->pitch) < 0.08f)
        cmd.triggers |= 255u << 8;  // right trigger: fire
      if (now > b.next_strafe) b.strafe = Rand(-1, 1), b.next_strafe = now + Rand(0.6f, 1.8f);
      cmd.lx = b.strafe;
      cmd.ly = enemy_d > 30 ? 0.6f : 0.0f;
    } else if (b.target_point >= 0) {
      Vec goal = points[b.target_point].pos;
      goal.x += b.target_offset.x, goal.z += b.target_offset.z;
      float dx = goal.x - me->pos.x, dz = goal.z - me->pos.z;
      Steer(cmd, *me, std::atan2(dx, dz), 0.0f);
      Walk(cmd, *me, goal, 1.0f);
      if (!at_point && std::hypot(dx, dz) > 25) cmd.buttons |= XINPUT_GAMEPAD_LEFT_THUMB;  // sprint
    }
    if (now < b.unstuck_until) {  // jump and side-step
      cmd.buttons |= XINPUT_GAMEPAD_A;
      cmd.lx = b.unstuck_dir;
      cmd.ly = 0.5f;
    }
    Commit(out, cmd);
  }

  // Landing craft, as a few states (Bot::boat_state): walk to one, board it
  // (E, then a view sweep the soldier doesn't follow means seated), drive it
  // (a landing craft is let down from its crane with the right stick up;
  // then the left stick drives and steers), and get out at the flag. True while busy (cmd set).
  enum BoatState { kNoBoat, kWalkToBoat, kBoard, kDrive, kLeave };
  // any_kind: a landing craft let down from its crane is a new vehicle whose
  // kind doesn't read as one.
  static bool IsBoat(const std::string& k) { return k == "LCVP" || k == "Daihatsu"; }
  static bool IsLand(const std::string& k) {
    return k == "Willys" || k == "Kurogane" || k == "Sherman" || k == "chi-ha" || k == "Chi-Ha";
  }
  const Vehicle* NearestBoat(Vec p, float range, const Bot& b, bool free_only, bool any_kind = false,
                             bool land = false) const {
    const Vehicle* best = nullptr;
    for (const Vehicle& v : vehicles) {
      if (!any_kind && !(land ? IsLand(v.kind) : IsBoat(v.kind))) continue;
      if (land && std::fabs(v.pos.y - p.y) > 4.0f) continue;
      if (v.obj == b.bad_boat || (free_only && BoatTaken(v.obj, b))) continue;
      float d = std::hypot(Dist2D(v.pos, p), v.pos.y - p.y);
      if (d < range) range = d, best = &v;
    }
    return best;
  }
  bool Boat(Bot& b, const Soldier& me, BotSlot& cmd) {
    const Vehicle* boat = b.boat ? FindVehicle(b.boat) : nullptr;
    auto set_state = [&](int s) {
      if (getenv("BF_BOAT_TRACE"))  // debugging: state changes
        RT_INFO("boat: bot%d %d -> %d at %.0f,%.0f,%.0f travel %.0f tries %d", b.slot + 1, b.boat_state, s, me.pos.x,
                me.pos.y, me.pos.z, b.boat_travel, b.board_tries);
      b.boat_state = s;
      b.boat_since = now;
    };
    switch (b.boat_state) {
      case kNoBoat: {
        if (now < b.no_boat_until || b.target_point < 0 || b.target_point >= int(points.size())) return false;
        float away = Dist2D(me.pos, points[b.target_point].pos);
        // A landing craft from a carrier for a far flag, else a jeep or tank
        // close by for a flag a long walk away.
        boat = away > 200.0f ? NearestBoat(me.pos, 60.0f, b, true) : nullptr;
        b.ride_land = false;
        if (!boat && away > 120.0f) {
          boat = NearestBoat(me.pos, 35.0f, b, true, false, true);
          b.ride_land = boat != nullptr;
        }
        if (!boat) return false;
        b.boat = boat->obj;
        b.board_tries = 0;
        set_state(kWalkToBoat);
        [[fallthrough]];
      }
      case kWalkToBoat: {
        if (!boat || now - b.boat_since > 25) {
          b.boat = 0;
          b.no_boat_until = now + 20;
          set_state(kNoBoat);
          return false;
        }
        float dx = boat->pos.x - me.pos.x, dz = boat->pos.z - me.pos.z;
        Steer(cmd, me, std::atan2(dx, dz), 0.0f);
        Walk(cmd, me, boat->pos, 1.0f);
        if (std::hypot(dx, dz) < 4.0f) {
          set_state(kBoard);
          b.board_cycle = now - 5.0;  // judge (not aboard) and start a cycle now
          b.board_pos = me.pos;
          b.board_tries = 0;
        }
        return true;
      }
      case kBoard: {
        // A 3 s cycle: aim at it (E only works looking at it), E, then the
        // throttle (a landing craft: the right stick up, which lets it down;
        // a jeep or tank: the left stick). Aboard once it has dropped with a
        // landing craft, or moves faster than on foot.
        // (E may seat it as the gunner, who can't drive: every other try also
        // switches seats, A, before the throttle.)
        double t = now - b.board_cycle;
        if (t > 4.0) {
          float went = Dist2D(me.pos, b.board_pos);
          bool aboard = went > 6.0f || (!b.ride_land && me.pos.y < b.board_pos.y - 3.0f);
          if (aboard) {
            set_state(kDrive);
            b.drive_since = now;
            b.released = true;
            b.boat_last = me.pos;
            b.boat_moved = now;
            b.boat_travel = 0;
            b.seat_switches = 0;
            return true;
          }
          if (++b.board_tries > 8) {
            b.bad_boat = b.boat;
            b.boat = 0;
            b.no_boat_until = now + 10;
            set_state(kNoBoat);
            return false;
          }
          b.board_cycle = now;
          b.board_pos = me.pos;
          t = 0;
        }
        if (t < 1.4) {
          if (boat) {
            float dx = boat->pos.x - me.pos.x, dz = boat->pos.z - me.pos.z;
            float pitch = std::atan2(boat->pos.y + 0.5f - (me.pos.y + 1.6f), std::max(std::hypot(dx, dz), 0.5f));
            Steer(cmd, me, std::atan2(dx, dz), pitch);
          }
          if (t >= 0.8 && t < 1.1) cmd.buttons |= XINPUT_GAMEPAD_B;
        } else if (t >= 1.6 && t < 1.9) {
          if (b.board_tries % 2 == 1) cmd.buttons |= XINPUT_GAMEPAD_A;
        } else if (t >= 2.2) {
          if (b.ride_land) cmd.ly = 1.0f;
          else cmd.ry = 1.0f;
        }
        return true;
      }
      case kDrive: {
        // The soldier rides with the boat: its position is the boat's (the
        // landing craft let down from its crane is a new vehicle, found only
        // at the next object scan).
        Vec pos = me.pos;
        if (b.target_point < 0 || b.target_point >= int(points.size())) return false;
        Vec goal = points[b.target_point].pos;
        float moved = Dist2D(pos, b.boat_last);
        if (moved > 1.0f) {
          b.boat_travel += moved;
          b.boat_heading = std::atan2(pos.x - b.boat_last.x, pos.z - b.boat_last.z);
          b.boat_last = pos;
          b.boat_moved = now;
        }
        bool stalled = now - b.boat_moved > 4.0;
        float arrive = b.ride_land ? 30.0f : 40.0f;
        if ((b.boat_travel > (b.ride_land ? 15.0f : 30.0f) && stalled) || Dist2D(pos, goal) < arrive ||
            now - b.drive_since > 150) {
          set_state(kLeave);
          return true;
        }
        // Not going anywhere after a while: perhaps the gunner's seat.
        if ((b.ride_land || b.released) && b.boat_travel < 3.0f && now - b.drive_since > 15.0 && stalled) {
          if (b.seat_switches >= 3) {
            b.bad_boat = b.boat;
            set_state(kLeave);
            return true;
          }
          ++b.seat_switches;
          b.boat_moved = now;
          cmd.buttons |= XINPUT_GAMEPAD_A;
          return true;
        }
        float want = std::atan2(goal.x - pos.x, goal.z - pos.z);
        float e = Wrap(want - b.boat_heading);
        // A landing craft first: the right stick straight up (nothing else)
        // lets it down from its crane. Then the left stick drives and steers,
        // with the right stick up as throttle too.
        if (!b.ride_land && !b.released) {
          // Down once the soldier has dropped with it; else perhaps the
          // gunner's seat (only the driver lets it down): switch seats (A).
          if (me.pos.y < b.boat_start.y - 3.0f) {
            b.released = true;
            b.boat_last = me.pos;
            b.boat_moved = now;
            b.boat_travel = 0;
          } else {
            double t = now - b.release_try;
            if (t > 4.0) {
              if (++b.seat_switches > 4) {
                b.bad_boat = b.boat;
                set_state(kLeave);
                return true;
              }
              b.release_try = now;
              t = 0;
            }
            if (t < 0.3 && b.seat_switches > 0) cmd.buttons |= XINPUT_GAMEPAD_A;
            else if (t > 0.6) cmd.ry = 1.0f;
            return true;
          }
        }
        cmd.ly = 1.0f;
        cmd.ry = 1.0f;
        cmd.lx = std::clamp(-e * 1.5f, -1.0f, 1.0f);  // turning right lowers the heading
        cmd.rx = 0.0f;
        return true;
      }
      case kLeave: {
        double t = std::fmod(now - b.boat_since, 1.2);
        if (t < 0.3) cmd.buttons |= XINPUT_GAMEPAD_B;  // E: get out
        if (now - b.boat_since > 3.0) {
          b.boat = 0;
          b.no_boat_until = now + 60;
          set_state(kNoBoat);
          return false;
        }
        return true;
      }
    }
    return false;
  }

  // BF_SELF_TEST=<vehicle kind> (testing, with --bots 1): the host's own
  // player walks to the nearest vehicle of that kind, gets in (the seat
  // pointer says when), then tries each input for 5 s, logging how the
  // vehicle moves, so its controls can be read off the log.
  void SelfTest() {
    static int state = 0, step = 0;
    static bool seated_flag = false;
    static double since = 0, next_log = 0;
    static Vec start;
    const int slot = kMaxBots - 1;
    BotSlot& out = g_shared->slots[slot];
    BotSlot cmd{};
    cmd.active = 1;
    // Our soldier: the one no bot has (once the bots are found).
    const Soldier* me = nullptr;
    // (Or the only soldier while no bot is found: the host spawns first.)
    static uint32_t mine = 0;
    bool all_found = true;
    for (const Bot& b : bots) all_found &= b.soldier != 0;
    // Ours: the server soldier nearest the camera (the bots' are elsewhere).
    float eye[3];
    if (!mine && xgpu::CameraEye(eye)) {
      float best_d = 5.0f;
      for (const Soldier& so : soldiers) {
        float d = std::hypot(so.pos.x - eye[0], so.pos.y - eye[1], so.pos.z - eye[2]);
        if (d < best_d) best_d = d, mine = so.obj;
      }
    }
    (void)all_found;
    me = mine ? Find(mine) : nullptr;
    if (mine && !me) mine = 0;  // died
    // Idle once seated (BF_SELF_TEST_IDLE): the keyboard has the pad, even if
    // the soldier isn't found while in a vehicle.
    if (state >= 1 && getenv("BF_SELF_TEST_IDLE")) {
      out.active = 0;
      if (me && now >= next_log) {
        next_log = now + 1.0;
        Vec p = me->pos;
        for (uint32_t c : Scan(0x82007518)) {
          uint32_t q = Ld32(c + 0x220), cv = Readable(q) ? Ld32(q + 0x130) : 0;
          if (Readable(cv) && Ld32(cv) == 0x82006DC8) p = {LdF(c + 0x3D0), LdF(c + 0x3D4), LdF(c + 0x3D8)};
        }
        RT_INFO("selftest: idle at %.1f,%.1f,%.1f yaw %.2f", p.x, p.y, p.z, me->yaw);
      }
      return;
    }
    if (!me) {  // spawn
      cmd.buttons = (int(now * 2) % 4 == 0) ? XINPUT_GAMEPAD_A : 0;
      Commit(out, cmd);
      return;
    }
    std::string want = getenv("BF_SELF_TEST");
    uint32_t in = SeatVehicle(me->obj);
    const Vehicle* target = nullptr;
    float best = 1e9f;
    for (const Vehicle& v : vehicles)
      if (v.kind == want && Dist2D(v.pos, me->pos) < best) best = Dist2D(v.pos, me->pos), target = &v;
    if (state == 0 && since == 0) since = now;
    if (state == 0 && now >= next_log) {
      next_log = now + 2.0;
      uint32_t seat = Ld32(me->obj + kSoldierSeat), link = Readable(seat) ? Ld32(seat + kSeatVehicle) : 0;
      RT_INFO("selftest: walking at %.0f,%.0f,%.0f, %s %.0f m, seat vehicle %08X (seat %08X vt %08X link %08X self %08X)",
              me->pos.x, me->pos.y, me->pos.z, want.c_str(), target ? best : -1.0f, in, seat,
              Readable(seat) ? Ld32(seat) : 0, link, me->obj);
    }
    if (state == 0) {
      if (seated_flag) {
        seated_flag = false;
        state = 1, since = now, step = 0;
        start = me->pos;
        RT_INFO("selftest: seated in %s at %.0f,%.0f,%.0f", want.c_str(), me->pos.x, me->pos.y, me->pos.z);
      } else if (in) {
        state = 1, since = now, step = 0;
        const Vehicle* v = FindVehicle(in);
        start = v ? v->pos : me->pos;
        RT_INFO("selftest: in %s", v ? v->kind.c_str() : "?");
      } else if (target) {
        float dx = target->pos.x - me->pos.x, dz = target->pos.z - me->pos.z;
        float pitch = std::atan2(target->pos.y + 0.5f - (me->pos.y + 1.6f), std::max(std::hypot(dx, dz), 0.5f));
        if (std::hypot(dx, dz) > 3.0f && now - since < 20) {
          Steer(cmd, *me, std::atan2(dx, dz), pitch);
          Walk(cmd, *me, target->pos, 0.8f);
        } else {
          // Close: sweep the aim over the vehicle and press E; 0.5 s after
          // each E, push the view sideways: if it doesn't turn, seated.
          static const float kYaw[] = {0, 0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f};
          static const float kPitch[] = {-0.5f, -0.25f, 0.0f, 0.2f};
          static float test_yaw = 0;
          int k = int(now / 1.6);
          double t = std::fmod(now, 1.6);
          if (t < 0.6) {
            Steer(cmd, *me, std::atan2(dx, dz) + kYaw[k % 7], kPitch[(k / 7) % 4]);
            if (t > 0.4) cmd.buttons |= XINPUT_GAMEPAD_B;
          } else if (t < 1.0) {
            test_yaw = me->yaw;
          } else if (t < 1.5) {
            cmd.rx = 1.0f;
          } else if (std::fabs(Wrap(me->yaw - test_yaw)) < 0.01f) {
            seated_flag = true;
          }
        }
      }
      Commit(out, cmd);
      return;
    }
    // Steps: 2 s nothing, then 5 s of one input.
    static const char* kSteps[] = {"ly+", "ly-", "lx+", "lx-", "ry+", "ry-", "rx+", "rt", "lt", "a"};
    const int n = int(sizeof(kSteps) / sizeof(kSteps[0]));
    double t = now - since;
    if (t > 7.0) {
      since = now, t = 0;
      if (++step >= n) step = n;
    }
    if (getenv("BF_SELF_TEST_IDLE")) step = n;  // the keyboard drives (BF_TEST_KEY)
    bool idle = getenv("BF_SELF_TEST_IDLE") != nullptr;
    if (step < n && t > 2.0) {
      std::string k = kSteps[step];
      if (k == "ly+") cmd.ly = 1;
      if (k == "ly-") cmd.ly = -1;
      if (k == "lx+") cmd.lx = 1;
      if (k == "lx-") cmd.lx = -1;
      if (k == "ry+") cmd.ry = 1;
      if (k == "ry-") cmd.ry = -1;
      if (k == "rx+") cmd.rx = 1;
      if (k == "rt") cmd.triggers = 255u << 8;
      if (k == "lt") cmd.triggers = 255u;
      if (k == "a") cmd.buttons = XINPUT_GAMEPAD_A;
    }
    if (now >= next_log) {
      next_log = now + 1.0;
      const Vehicle* v = in ? FindVehicle(in) : nullptr;
      Vec p = v ? v->pos : me->pos;  // the soldier rides with it
      for (uint32_t c : Scan(0x82007518)) {
        uint32_t q = Ld32(c + 0x220), cv = Readable(q) ? Ld32(q + 0x130) : 0;
        if (Readable(cv) && Ld32(cv) == 0x82006DC8) p = {LdF(c + 0x3D0), LdF(c + 0x3D4), LdF(c + 0x3D8)};
      }
      RT_INFO("selftest: step %s t %.0f seat %s veh %.1f,%.1f,%.1f yaw %.2f", step < n ? kSteps[step] : "done", t,
              in ? (v ? v->kind.c_str() : "vehicle") : "foot", p.x, p.y, p.z, me->yaw);
    }
    if (idle) {
      out.active = 0;  // the keyboard has the pad now
      return;
    }
    Commit(out, cmd);
  }

  static void Commit(BotSlot& out, const BotSlot& cmd) {
    out.lx = cmd.lx, out.ly = cmd.ly, out.rx = cmd.rx, out.ry = cmd.ry;
    out.buttons = cmd.buttons;
    out.triggers = cmd.triggers;
    out.active = 1;
  }
};

#ifdef _WIN32
std::string SharedName() { return "Local\\bf1943-bots-" + std::to_string(GetCurrentProcessId()); }
#else
std::string SharedName() { return "/bf1943-bots-" + std::to_string(GetCurrentProcessId()); }
#endif

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
std::string StartBotDirector(int count) {
  std::string name = SharedName();
  g_shared = MapShared(name, true);
  if (!g_shared) {
    RT_WARN("bots: no shared memory for the AI");
    return "";
  }
  std::thread([count] {
    Director d;
    for (int i = 0; i < std::min(count, kMaxBots - 1); ++i) d.bots.push_back(Bot{i});
    if (getenv("BF_SELF_TEST")) g_my_slot = kMaxBots - 1;  // the host's own player
    auto t0 = std::chrono::steady_clock::now();
    double last_scan = -10, last_log = 0;
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
      d.now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (g_in_menus) continue;
      bool rescan = d.now - last_scan > 2.0;
      if (rescan) last_scan = d.now;
      d.Refresh(rescan);
      if (d.points.empty()) continue;
      if (getenv("BF_SELF_TEST")) d.SelfTest();
      d.Probe();
      for (Bot& b : d.bots) d.Think(b);
      if (getenv("BF_BOT_TRACE")) {  // DEBUG
        static double last_trace = 0;
        if (d.now - last_trace > 2.0) {
          last_trace = d.now;
          for (const Bot& b : d.bots) {
            const Soldier* me = b.soldier ? d.Find(b.soldier) : nullptr;
            const BotSlot& o = g_shared->slots[b.slot];
            const Vehicle* bv = b.boat ? d.FindVehicle(b.boat) : nullptr;
            if (me) RT_INFO("bot%d: %.1f,%.1f,%.1f yaw %.2f pitch %.2f  lx %.2f ly %.2f rx %.2f ry %.2f btn %X fire %d boat %s %.1f,%.1f,%.1f%s",
                            b.slot + 1, me->pos.x, me->pos.y, me->pos.z, me->yaw, me->pitch, o.lx, o.ly, o.rx, o.ry,
                            o.buttons, int(o.triggers >> 8 != 0), bv ? bv->kind.c_str() : "-", bv ? bv->pos.x : 0.f,
                            bv ? bv->pos.y : 0.f, bv ? bv->pos.z : 0.f, b.boat_state == 3 ? " DRIVE" : "");
            else RT_INFO("bot%d: no soldier", b.slot + 1);
          }
        }
      }
      if (d.now - last_log > 10) {
        last_log = d.now;
        std::string s;
        for (const Bot& b : d.bots) {
          const Soldier* me = b.soldier ? d.Find(b.soldier) : nullptr;
          char buf[128];
          if (me) snprintf(buf, sizeof(buf), " [%d: team %d at %.0f,%.0f,%.0f -> point %d%s]", b.slot + 1, me->team,
                           me->pos.x, me->pos.y, me->pos.z, b.target_point,
                           b.boat_state == 3 ? " driving" : b.boat_state == 2 ? " boarding" : b.boat_state == 1 ? " to boat" : b.boat_state == 4 ? " leaving" : "");
          else snprintf(buf, sizeof(buf), " [%d: no soldier]", b.slot + 1);
          s += buf;
        }
        RT_INFO("bots: %zu soldiers, %zu points;%s", d.soldiers.size(), d.points.size(), s.c_str());
      }
    }
  }).detach();
  return name;
}

// Bot process: read pad input from the host's director.
void SetBotSlot(const std::string& shared_name, int slot) {
  g_shared = MapShared(shared_name, false);
  g_my_slot = g_shared && g_shared->magic == kMagic ? slot : -1;
  RT_INFO("bots: AI input %s (slot %d)", g_my_slot >= 0 ? "connected" : "unavailable", slot);
}

// Bot process: look speed in right-stick units (see MouseLookAxis), or false.
bool BotLookAxis(int axis, float& value) {
  if (g_my_slot < 0 || !g_shared || !g_shared->slots[g_my_slot].active) return false;
  value = axis == 0 ? g_shared->slots[g_my_slot].rx : g_shared->slots[g_my_slot].ry;
  return true;
}

bool AddBotInput(XINPUT_GAMEPAD& g) {
  if (g_my_slot < 0 || !g_shared) return false;
  const BotSlot& s = g_shared->slots[g_my_slot];
  if (!s.active) return false;
  auto axis = [](float v) { return SHORT(std::clamp(v, -1.0f, 1.0f) * 32767.0f); };
  g.sThumbLX = axis(s.lx);
  g.sThumbLY = axis(s.ly);
  // Looking goes through the unclamped mouse path (BotLookAxis), without
  // the stick's deadzone; the stick only marks the pad as active.
  if (s.rx || s.ry) g.sThumbRX ^= 1;
  g.wButtons |= WORD(s.buttons);
  g.bLeftTrigger = BYTE(s.triggers & 0xFF);
  g.bRightTrigger = BYTE((s.triggers >> 8) & 0xFF);
  return true;
}

}  // namespace rt
