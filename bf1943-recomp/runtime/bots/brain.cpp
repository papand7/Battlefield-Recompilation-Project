// The bot brain: what every bot does, as pad input (see bot_api.h). Built into
// the game, and as bot_brain.dll for reloading while a match runs.
//
// Modelled on DICE's own bots (Battlefield 1942 / 2): a strategic layer gives
// every control point a "temperature" (how much the team needs it) and
// spreads the team's bots over the hot ones; each bot is a reactive agent that
// picks the most needed behaviour every tick (engage an enemy, capture its
// point, get unstuck, respawn, ride a vehicle) and turns it into pad input.
//
// Scenarios (BF_BOT_SCENARIO) make every bot do one thing and say whether it
// worked, for testing:
//   walk <flag>            walk to flag number <flag>
//   board <kind>           get into the nearest vehicle of that kind
//   drive <kind> <flag>    get into one and drive it to the flag
// Add "near" to start next to the vehicle or 15 m from the flag (teleported).
#include "bot_api.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>

using namespace botapi;

namespace {

// ---- per-bot state (kept in BotIO::mem) ------------------------------------

enum BoatState { kNoBoat, kWalkToBoat, kBoard, kDrive, kLeave };

struct Bot {
  int target_point = -1;     // index into points
  Vec target_offset;         // where around the point to stand
  Vec last_pos;
  double last_moved = 0;     // time it last made progress
  double unstuck_until = 0;
  float unstuck_dir = 1;
  double next_spawn_press = 0;
  float strafe = 0;
  double next_strafe = 0;
  bool spawn_choice = false;
  int team = 0;
  bool alive = false;
  // Vehicle: the one it is going for or in, since when, its heading.
  uint32_t boat = 0;
  int boat_state = kNoBoat, board_tries = 0, seat_switches = 0;
  double boat_since = 0, boat_moved = 0, drive_since = 0, board_cycle = 0, release_try = 0;
  double no_boat_until = 0;
  Vec boat_last, board_pos, boat_start;
  float boat_heading = 0, boat_travel = 0;
  bool ride_land = false;  // a jeep or tank rather than a landing craft
  bool released = false;   // the landing craft is down from its crane
  uint32_t bad_boat = 0;   // gave up on it
  // Scenario
  bool teleported = false;
  double scenario_since = -1, judge_from = 0;
  float drive_from = -1;  // distance to the flag when it started driving
  bool released_once = false;  // this ride's landing craft is already in the water
  double off_since = 0;        // driving, but not seated since then
};
constexpr uint32_t kTag = 0xB7A10000u + sizeof(Bot);
static_assert(sizeof(Bot) <= kMemSize, "Bot state too big for BotIO::mem");

Bot& State(BotIO& io) {
  if (io.mem_tag != kTag) {
    new (io.mem) Bot{};
    io.mem_tag = kTag;
  }
  return *reinterpret_cast<Bot*>(io.mem);
}

// ---- the scenario ------------------------------------------------------------

struct Scenario {
  enum Kind { kNone, kWalk, kBoard, kDrive } kind = kNone;
  std::string vehicle;
  int point = -1;
  bool near = false;
  double time_limit = 180;
};
Scenario ParseScenario(const char* s) {
  Scenario sc;
  if (!s || !*s) return sc;
  char word[32] = {}, a[32] = {}, b[32] = {}, c[32] = {};
  sscanf(s, "%31s %31s %31s %31s", word, a, b, c);
  std::string w = word;
  auto near = [&](const char* x) { return strcmp(x, "near") == 0; };
  if (w == "walk") sc.kind = Scenario::kWalk, sc.point = atoi(a), sc.near = near(b);
  if (w == "board") sc.kind = Scenario::kBoard, sc.vehicle = a, sc.near = near(b);
  if (w == "drive") sc.kind = Scenario::kDrive, sc.vehicle = a, sc.point = atoi(b), sc.near = near(c);
  if (const char* t = getenv("BF_BOT_SCENARIO_TIME")) sc.time_limit = atof(t);
  return sc;
}

// ---- the brain ---------------------------------------------------------------

struct Brain {
  World& w;
  std::mt19937& rng;
  Scenario sc;

  float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
  void Log(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (w.log) w.log(buf);
  }

  const Soldier* Find(uint32_t obj) const {
    for (int i = 0; i < w.soldier_count; ++i)
      if (w.soldiers[i].obj == obj) return &w.soldiers[i];
    return nullptr;
  }
  const Vehicle* FindVehicle(uint32_t obj) const {
    for (int i = 0; i < w.vehicle_count; ++i)
      if (w.vehicles[i].obj == obj) return &w.vehicles[i];
    return nullptr;
  }
  bool BoatTaken(uint32_t obj, const BotIO& me) const {
    for (int i = 0; i < w.bot_count; ++i) {
      BotIO& o = w.bots[i];
      if (&o != &me && o.mem_tag == kTag && reinterpret_cast<const Bot*>(o.mem)->boat == obj) return true;
    }
    return false;
  }

  // Strategic layer: temperature per point for a team, then the bot takes the
  // hottest one, cooled by distance and by team mates already heading there.
  int ChoosePoint(const BotIO& io, const Soldier& me) {
    int best = -1;
    float best_score = -1e9f;
    for (int i = 0; i < w.point_count; ++i) {
      const Point& p = w.points[i];
      float temp = p.owner == me.team ? 10.0f : (p.owner == 0 ? 60.0f : 45.0f);
      // Enemy soldiers near a point we own make it hot to defend.
      int threats = 0;
      for (int k = 0; k < w.soldier_count; ++k)
        if (w.soldiers[k].team != me.team && Dist2D(w.soldiers[k].pos, p.pos) < 40) ++threats;
      if (p.owner == me.team) temp += 25.0f * threats;
      int mates = 0;
      for (int k = 0; k < w.bot_count; ++k) {
        const BotIO& o = w.bots[k];
        if (&o != &io && o.mem_tag == kTag && reinterpret_cast<const Bot*>(o.mem)->target_point == i) ++mates;
      }
      float score = temp - 12.0f * mates - Dist2D(me.pos, p.pos) / 25.0f + Rand(0, 5);
      if (score > best_score) best_score = score, best = i;
    }
    return best;
  }

  static bool IsBoat(const char* k) { return !strcmp(k, "LCVP") || !strcmp(k, "Daihatsu"); }
  static bool IsLand(const char* k) {
    return !strcmp(k, "Willys") || !strcmp(k, "Kurogane") || !strcmp(k, "Sherman") || !strcmp(k, "chi-ha") ||
           !strcmp(k, "Chi-Ha");
  }
  // The nearest vehicle: of a kind (kind set), else a landing craft or (land)
  // a jeep or tank on about the same level.
  const Vehicle* NearestVehicle(Vec p, float range, const BotIO& io, const Bot& b, bool free_only, bool land,
                                const char* kind = nullptr) const {
    const Vehicle* best = nullptr;
    for (int i = 0; i < w.vehicle_count; ++i) {
      const Vehicle& v = w.vehicles[i];
      if (kind ? strcmp(v.kind, kind) != 0 : !(land ? IsLand(v.kind) : IsBoat(v.kind))) continue;
      if (!kind && land && std::fabs(v.pos.y - p.y) > 4.0f) continue;
      if (v.obj == b.bad_boat || (free_only && BoatTaken(v.obj, io))) continue;
      float d = Dist3D(v.pos, p);
      if (d < range) range = d, best = &v;
    }
    return best;
  }

  void SetState(BotIO& io, Bot& b, const Soldier& me, int s) {
    if (getenv("BF_BOAT_TRACE"))
      Log("boat: bot%d %d -> %d at %.0f,%.0f,%.0f travel %.0f tries %d", io.slot + 1, b.boat_state, s, me.pos.x,
          me.pos.y, me.pos.z, b.boat_travel, b.board_tries);
    b.boat_state = s;
    b.boat_since = w.now;
  }

  // Vehicles, as a few states (Bot::boat_state): walk to one, board it (E;
  // the seat pointer says when it sits in it), drive it (a landing craft is
  // let down from its crane with the right stick up; then the left stick
  // drives and steers) and get out at the flag. True while busy (pad set).
  bool Ride(BotIO& io, Bot& b, const Soldier& me, Pad& cmd) {
    const Vehicle* boat = b.boat ? FindVehicle(b.boat) : nullptr;
    const char* want_kind = sc.kind == Scenario::kBoard || sc.kind == Scenario::kDrive ? sc.vehicle.c_str() : nullptr;
    switch (b.boat_state) {
      case kNoBoat: {
        if (w.now < b.no_boat_until || b.target_point < 0 || b.target_point >= w.point_count) return false;
        if (want_kind) {
          boat = NearestVehicle(me.pos, 1e9f, io, b, true, false, want_kind);
          if (boat) b.ride_land = !IsBoat(boat->kind);
        } else {
          // A landing craft from a carrier for a far flag, else a jeep or
          // tank close by for a flag a long walk away.
          float away = Dist2D(me.pos, w.points[b.target_point].pos);
          boat = away > 200.0f ? NearestVehicle(me.pos, 60.0f, io, b, true, false) : nullptr;
          b.ride_land = false;
          // (Jeeps and tanks are untested: only with BF_BOT_VEHICLES.)
          static const bool land_on = getenv("BF_BOT_VEHICLES") != nullptr;
          if (!boat && land_on && away > 120.0f) {
            boat = NearestVehicle(me.pos, 35.0f, io, b, true, true);
            b.ride_land = boat != nullptr;
          }
        }
        if (!boat) return false;
        b.boat = boat->obj;
        b.board_tries = 0;
        b.released_once = false;
        b.boat_start = me.pos;
        SetState(io, b, me, kWalkToBoat);
        [[fallthrough]];
      }
      case kWalkToBoat: {
        if (!boat || (!want_kind && w.now - b.boat_since > 25)) {
          b.boat = 0;
          b.no_boat_until = w.now + 20;
          SetState(io, b, me, kNoBoat);
          return false;
        }
        float dx = boat->pos.x - me.pos.x, dz = boat->pos.z - me.pos.z;
        Steer(cmd, me, std::atan2(dx, dz), 0.0f);
        Walk(cmd, me, boat->pos, 1.0f);
        if (std::hypot(dx, dz) > 25) cmd.buttons |= kLeftThumb;  // sprint
        if (std::hypot(dx, dz) < 4.0f) {
          SetState(io, b, me, kBoard);
          b.board_cycle = w.now;
          b.board_pos = me.pos;
          b.board_tries = 0;
        }
        return true;
      }
      case kBoard: {
        // Aboard once the seat says so. (Not by moving or dropping: that is
        // also a soldier fallen off the carrier's deck.)
        if (me.vehicle != 0) {
          SetState(io, b, me, kDrive);
          b.drive_since = w.now;
          b.release_try = w.now;  // the throttle in this seat first (no seat switch yet)
          if (b.target_point >= 0 && b.target_point < w.point_count)
            b.drive_from = Dist2D(me.pos, w.points[b.target_point].pos);
          b.released = b.ride_land || b.released_once;
          b.off_since = 0;
          b.boat_last = me.pos;
          b.boat_start = me.pos;
          b.boat_moved = w.now;
          b.boat_travel = 0;
          b.seat_switches = 0;
          return true;
        }
        // E only works looking at the vehicle's entry: sweep the aim over it
        // (as the self-test does), an angle every 1.2 s with E on each, 7
        // headings x 4 pitches, then give up on it.
        static const float kYaw[] = {0, 0.35f, -0.35f, 0.7f, -0.7f, 1.05f, -1.05f};
        static const float kPitch[] = {-0.5f, -0.25f, 0.0f, 0.2f};
        double t = w.now - b.board_cycle;
        if (t > 1.2) {
          b.board_cycle = w.now;
          t = 0;
          if (++b.board_tries >= 28 || !boat) {
            b.bad_boat = b.boat;
            b.boat = 0;
            b.no_boat_until = w.now + 10;
            SetState(io, b, me, kNoBoat);
            return false;
          }
        }
        if (!boat) return true;
        float dx = boat->pos.x - me.pos.x, dz = boat->pos.z - me.pos.z;
        int k = b.board_tries;
        Steer(cmd, me, std::atan2(dx, dz) + kYaw[k % 7], kPitch[(k / 7) % 4]);
        if (t > 0.6 && t < 0.85) cmd.buttons |= kB;
        return true;
      }
      case kDrive: {
        // The soldier rides with it: its position is the vehicle's (a landing
        // craft let down from its crane is a new vehicle, found only at the
        // next object scan).
        Vec pos = me.pos;
        if (b.target_point < 0 || b.target_point >= w.point_count) return false;
        // Out of the seat (a seat switch can leave it standing in the craft):
        // get back in, the nearest one (a landing craft let down from its
        // crane is a new vehicle).
        if (!me.vehicle) {
          if (!b.off_since) b.off_since = w.now;
          if (w.now - b.off_since > 1.5) {
            b.off_since = 0;
            const Vehicle* v = nullptr;
            float best = 15.0f;
            for (int i = 0; i < w.vehicle_count; ++i) {
              const Vehicle& c = w.vehicles[i];
              if (!(b.ride_land ? IsLand(c.kind) : IsBoat(c.kind))) continue;
              if (float d = Dist3D(c.pos, me.pos); d < best) best = d, v = &c;
            }
            if (!v || ++b.board_tries > 40) {
              SetState(io, b, me, kLeave);
              return true;
            }
            b.boat = v->obj;
            b.board_cycle = w.now;
            b.board_pos = me.pos;
            SetState(io, b, me, kBoard);
            return true;
          }
        } else {
          b.off_since = 0;
        }
        Vec goal = w.points[b.target_point].pos;
        float moved = Dist2D(pos, b.boat_last);
        if (moved > 1.0f) {
          b.boat_travel += moved;
          b.boat_heading = std::atan2(pos.x - b.boat_last.x, pos.z - b.boat_last.z);
          b.boat_last = pos;
          b.boat_moved = w.now;
        }
        bool stalled = w.now - b.boat_moved > 4.0;
        float arrive = b.ride_land ? 30.0f : 40.0f;
        if (sc.kind == Scenario::kBoard) {  // the test is over once seated: sit still
          return true;
        }
        if ((b.boat_travel > (b.ride_land ? 15.0f : 30.0f) && stalled) || Dist2D(pos, goal) < arrive ||
            w.now - b.drive_since > 150) {
          SetState(io, b, me, kLeave);
          return true;
        }
        // Not going anywhere: perhaps not the driver's seat (the craft let
        // down while it sat in another, or E gave it the gunner's): switch
        // seats (A) until it moves. (The drop itself moves it a few metres.)
        if (b.released && b.boat_travel < 15.0f && w.now - b.drive_since > 5.0 && stalled) {
          if (b.seat_switches >= 6) {
            b.bad_boat = b.boat;
            SetState(io, b, me, kLeave);
            return true;
          }
          ++b.seat_switches;
          b.boat_moved = w.now;
          cmd.buttons |= kA;
          return true;
        }
        float want = std::atan2(goal.x - pos.x, goal.z - pos.z);
        float e = Wrap(want - b.boat_heading);
        // The controls (the game's default vehicle layout): LT throttle, LB
        // brake, the left stick steers. A landing craft on a carrier first
        // hangs from its crane: the driver's throttle lets it down.
        if (!b.released) {
          // Down once the soldier has dropped with it; else perhaps the
          // gunner's seat (only the driver lets it down): switch seats (A).
          if (me.pos.y < b.boat_start.y - 3.0f) {
            b.released = true;
            b.released_once = true;
            b.drive_since = w.now;
            b.boat_last = me.pos;
            b.boat_moved = w.now;
            b.boat_travel = 0;
            b.seat_switches = 0;
          } else {
            double t = w.now - b.release_try;
            // The first seat E gives is the driver's, and a crane can take a
            // while to let a craft down: 30 s there before trying the others.
            if (t > (b.seat_switches == 0 ? 30.0 : 5.0)) {
              if (++b.seat_switches > 3) {
                b.bad_boat = b.boat;
                SetState(io, b, me, kLeave);
                return true;
              }
              b.release_try = w.now;
              t = 0;
            }
            if (t < 0.3 && b.seat_switches > 0) cmd.buttons |= kA;
            else if (t > 0.6) cmd.triggers = 255u;  // LT
            return true;
          }
        }
        cmd.triggers = 255u;                           // LT: throttle
        cmd.lx = std::clamp(-e * 1.5f, -1.0f, 1.0f);  // turning right lowers the heading
        return true;
      }
      case kLeave: {
        double t = std::fmod(w.now - b.boat_since, 1.2);
        if (t < 0.3) cmd.buttons |= kB;  // E: get out
        if (w.now - b.boat_since > 3.0) {
          b.boat = 0;
          b.no_boat_until = w.now + 60;
          SetState(io, b, me, kNoBoat);
          return false;
        }
        return true;
      }
    }
    return false;
  }

  // Scenario bookkeeping for one bot: teleport, pass or fail.
  void Judge(BotIO& io, Bot& b, const Soldier& me) {
    if (b.scenario_since < 0) b.scenario_since = w.now;
    if (sc.near && !b.teleported && !sc.vehicle.empty()) {
      if (const Vehicle* v = NearestVehicle(me.pos, 1e9f, io, b, true, false, sc.vehicle.c_str())) {
        io.teleport = true;
        io.to = {v->pos.x + 4.0f, v->pos.y + 1.0f, v->pos.z};
        b.teleported = true;
        Log("scenario: bot%d to the %s at %.0f,%.0f,%.0f", io.slot + 1, v->kind, v->pos.x, v->pos.y, v->pos.z);
      }
    } else if (sc.near && !b.teleported && sc.point >= 0 && sc.point < w.point_count) {
      const Vec& p = w.points[sc.point].pos;
      io.teleport = true;
      io.to = {p.x + 15.0f, p.y + 2.0f, p.z};
      b.teleported = true;
      Log("scenario: bot%d to 15 m from flag %d", io.slot + 1, sc.point);
    }
    if (io.teleport) b.judge_from = w.now + 3.0;  // where it lands shows a little later
    if (w.result || w.now < b.judge_from) return;
    const Vehicle* in = me.vehicle ? FindVehicle(me.vehicle) : nullptr;
    bool done = false;
    switch (sc.kind) {
      case Scenario::kWalk:
        done = sc.point < w.point_count && Dist2D(me.pos, w.points[sc.point].pos) < 15;
        break;
      case Scenario::kBoard:
        done = in && sc.vehicle == in->kind;
        break;
      case Scenario::kDrive: {
        // Driven to the flag, or most of the way (a landing craft stops at
        // the beach).
        float d = sc.point < w.point_count ? Dist2D(me.pos, w.points[sc.point].pos) : 1e9f;
        done = b.boat_state >= kDrive && (d < 45 || (b.drive_from > 0 && b.drive_from - d > 0.6f * b.drive_from));
        break;
      }
      default:
        break;
    }
    if (done) {
      w.result = 1;
      snprintf(w.result_text, sizeof(w.result_text), "PASS: bot%d %s in %.0f s", io.slot + 1, w.scenario,
               w.now - b.scenario_since);
    } else if (w.now - b.scenario_since > sc.time_limit) {
      w.result = -1;
      snprintf(w.result_text, sizeof(w.result_text), "FAIL: bot%d %s, %.0f s, at %.0f,%.0f,%.0f, state %s", io.slot + 1,
               w.scenario, sc.time_limit, me.pos.x, me.pos.y, me.pos.z, io.state);
    }
  }

  void Think(BotIO& io) {
    Bot& b = State(io);
    Pad cmd{};
    io.teleport = false;
    if (w.points_changed) b.target_point = -1;
    const Soldier* me = io.soldier ? Find(io.soldier) : nullptr;
    if (!me) {
      // Dead or on the spawn screen. When the team holds a flag, first step
      // the spawn choice off the carrier (the spawn screen lists the carrier,
      // then the held flags), then confirm.
      if (b.alive) b.boat = 0, b.boat_state = kNoBoat, b.teleported = false;
      b.alive = false;
      bool held = false;
      for (int i = 0; i < w.point_count; ++i) held |= w.points[i].owner != 0 && w.points[i].owner == b.team;
      if (w.now >= b.next_spawn_press) {
        if (held && !b.spawn_choice && sc.kind == Scenario::kNone) {
          cmd.buttons = kDpadRight;
          b.spawn_choice = true;
          b.next_spawn_press = w.now + 0.6;
        } else {
          cmd.buttons = kA;
          b.spawn_choice = false;
          b.next_spawn_press = w.now + 2.0;
        }
      }
      snprintf(io.state, sizeof(io.state), "spawning");
      io.pad = cmd;
      return;
    }
    if (!b.alive) b.last_pos = me->pos, b.last_moved = w.now;
    b.alive = true;
    b.team = me->team;
    if (sc.kind != Scenario::kNone) Judge(io, b, *me);
    // Behaviour weights (BF1942 style: how much each is needed right now).
    const Soldier* enemy = nullptr;
    float enemy_d = 1e9f;
    for (int i = 0; i < w.soldier_count; ++i) {
      const Soldier& s = w.soldiers[i];
      if (s.team == me->team) continue;
      float d = Dist3D(s.pos, me->pos);
      if (d < enemy_d) enemy_d = d, enemy = &s;
    }
    float w_engage = enemy && enemy_d < 70 && sc.kind == Scenario::kNone ? 100 - enemy_d : 0;
    if (b.target_point < 0 || b.target_point >= w.point_count ||
        (sc.point < 0 && w.points[b.target_point].owner == me->team && Rand(0, 1) < 0.01f)) {
      if (sc.point >= 0) b.target_point = std::min(sc.point, w.point_count - 1);
      else if (const char* p = getenv("BF_BOT_POINT")) b.target_point = atoi(p);
      else b.target_point = ChoosePoint(io, *me);
      b.target_offset = {Rand(-6, 6), 0, Rand(-6, 6)};
    }
    // Progress / stuck detection.
    if (Dist2D(me->pos, b.last_pos) > 1.5f) b.last_pos = me->pos, b.last_moved = w.now;
    bool at_point = b.target_point >= 0 && Dist2D(me->pos, w.points[b.target_point].pos) < 12;
    // Landing craft from the carriers; BF_BOT_NO_VEHICLES: none.
    static const bool vehicles_off = getenv("BF_BOT_NO_VEHICLES") != nullptr;
    bool ride = !vehicles_off || sc.kind == Scenario::kBoard || sc.kind == Scenario::kDrive;
    if (ride && Ride(io, b, *me, cmd)) {
      static const char* kNames[] = {"foot", "to vehicle", "boarding", "driving", "leaving"};
      snprintf(io.state, sizeof(io.state), "%s", kNames[b.boat_state]);
      io.pad = cmd;
      return;
    }
    if (!at_point && w.now - b.last_moved > 2.5 && w.now > b.unstuck_until) {
      b.unstuck_until = w.now + 1.2;
      b.unstuck_dir = Rand(0, 1) < 0.5f ? -1.0f : 1.0f;
      b.last_moved = w.now;
    }
    if (w_engage > 0) {
      // Aim at the chest, strafe now and then, fire when on target.
      float dx = enemy->pos.x - me->pos.x, dz = enemy->pos.z - me->pos.z;
      float want_yaw = std::atan2(dx, dz);
      float want_pitch = std::atan2(enemy->pos.y + 1.2f - (me->pos.y + 1.6f), std::hypot(dx, dz));
      Steer(cmd, *me, want_yaw, want_pitch);
      if (std::fabs(Wrap(want_yaw - me->yaw)) < 0.06f && std::fabs(want_pitch - me->pitch) < 0.08f)
        cmd.triggers |= 255u << 8;  // right trigger: fire
      if (w.now > b.next_strafe) b.strafe = Rand(-1, 1), b.next_strafe = w.now + Rand(0.6f, 1.8f);
      cmd.lx = b.strafe;
      cmd.ly = enemy_d > 30 ? 0.6f : 0.0f;
      snprintf(io.state, sizeof(io.state), "engage");
    } else if (b.target_point >= 0) {
      Vec goal = w.points[b.target_point].pos;
      goal.x += b.target_offset.x, goal.z += b.target_offset.z;
      float dx = goal.x - me->pos.x, dz = goal.z - me->pos.z;
      Steer(cmd, *me, std::atan2(dx, dz), 0.0f);
      Walk(cmd, *me, goal, 1.0f);
      if (!at_point && std::hypot(dx, dz) > 25) cmd.buttons |= kLeftThumb;  // sprint
      snprintf(io.state, sizeof(io.state), at_point ? "at flag %d" : "to flag %d", b.target_point);
    }
    if (w.now < b.unstuck_until) {  // jump and side-step
      cmd.buttons |= kA;
      cmd.lx = b.unstuck_dir;
      cmd.ly = 0.5f;
      snprintf(io.state, sizeof(io.state), "unstuck");
    }
    io.pad = cmd;
  }
};

}  // namespace

BOT_BRAIN_API int BotBrainVersion() { return kVersion; }

BOT_BRAIN_API void BotBrainThink(World* w) {
  static std::mt19937 rng{1943};
  static std::string last_scenario;
  static Scenario sc;
  std::string now_scenario = w->scenario ? w->scenario : "";
  if (now_scenario != last_scenario) sc = ParseScenario(w->scenario), last_scenario = now_scenario;
  Brain brain{*w, rng, sc};
  for (int i = 0; i < w->bot_count; ++i) brain.Think(w->bots[i]);
}
