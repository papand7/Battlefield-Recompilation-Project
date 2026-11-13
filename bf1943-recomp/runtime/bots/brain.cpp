// The bot brain: what every bot does, as pad input (see bot_api.h). Built into
// the game, and as bot_brain.dll for reloading while a match runs.
//
// Modelled on DICE's own bots (Battlefield 1942 / 2): a strategic layer gives
// every control point a "temperature" (how much the team needs it) and
// spreads the team's bots over the hot ones; each bot is a reactive agent that
// picks the most needed behaviour every tick (engage an enemy, capture its
// point, go round what blocks it, respawn, ride a vehicle) and turns it into pad input.
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
#include <filesystem>
#include <new>
#include <random>
#include <string>
#include <vector>

using namespace botapi;

namespace {

// A file named in UTF-8 (World::nav_path): on Windows through its wide name,
// for folder names outside the ANSI code page.
FILE* OpenUtf8(const char* name, const char* mode) {
#ifdef _WIN32
  std::filesystem::path p(reinterpret_cast<const char8_t*>(name));
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return fopen(name, mode);
#endif
}

// ---- per-bot state (kept in BotIO::mem) ------------------------------------

// A way up from the water onto land (Shore): its last spot in the water, then
// where it was up on land.
struct Exit { Vec wet, up; };

enum BoatState { kNoBoat, kWalkToBoat, kBoard, kDrive, kLeave };

struct Bot {
  int target_point = -1;     // index into points
  Vec target_offset;         // where around the point to stand
  Vec last_pos;
  double last_moved = 0;     // time it last made progress
  // Stuck on foot: a detour, wider and on the other side each time.
  int stuck_level = 0;
  Vec stuck_at;
  double back_until = 0, detour_until = 0;
  float detour_angle = 0, detour_side = 1;
  bool boat_done = false;  // came ashore by boat this life: no boats again
  int avoid_point = -1;  // a flag it couldn't reach, left alone for a while
  double avoid_until = 0;
  // A vehicle stalled far from its flag: reverse and veer off.
  int reverse_tries = 0;
  double reverse_until = 0, veer_until = 0;
  float veer = 0;
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
  bool ride_land = false;  // a jeep or tank (or a plane) rather than a landing craft
  bool ride_air = false;   // a plane
  double spot_t = -100;       // when the learned spot at its flag was last chosen (LearnedSpot)
  int spot_point = -1;
  float threat_yaw = 0;       // where enemies usually come from around here (Threat), and when
  double threat_t = -100;
  bool threat = false;
  uint32_t seen_enemy = 0;  // the last enemy in sight, and until when it counts
  double seen_until = 0;
  float fly_last_y = 0;
  double fly_last_t = 0;
  float fly_vy = 0;
  float fly_last_h = 0, fly_rate = 0;  // the heading a moment ago, its rate of turn (rad/s)
  bool released = false;   // the landing craft is down from its crane
  uint32_t bad_boat = 0;   // gave up on it
  // Scenario
  bool teleported = false;
  double hold_until = 0;  // standing still for the teleport
  char probe_name[16] = {};  // BF_BOT_INPUT_PROBE
  Vec spawned_at;         // where it last spawned
  Vec shore_from;         // its last spot in the water, since shore_t
  double shore_t = 0;
  int exit_tries = 0;
  // A route on the navigation mesh (World::route): its corners, the next one,
  // when it was planned and to where.
  static constexpr int kCorners = 28;
  Vec route[kCorners];
  int route_n = 0, route_i = 0;
  double route_t = 0;
  Vec route_goal;
  Vec nav_stuck_at;  // where it last got stuck on a route
  int exit_phase = 0;  // 1: to the exit's spot in the water, 2: up it
  Vec land_exit;          // a way up worked out from the terrain (LandExit), and when
  double land_exit_t = -100, land_try_t = -100;
  Exit exit_on;
  double exit_since = 0;
  double scenario_since = -1, judge_from = 0;
  float drive_from = -1;  // distance to the flag when it started driving
  bool released_once = false;  // this ride's landing craft is already in the water
  double off_since = 0;        // driving, but not seated since then
  // The advisor (World::ask): the place it picked and until when; asked about
  // what (1 stuck, 2 cover: walked to while fighting); its health a moment ago.
  Vec advice;
  double advice_until = 0, health_t = 0;
  int asked = 0;
  bool advice_cover = false;
  float health_was = 100;
  Vec idle_pos;  // where it has stood since idle_t (within 5 m)
  double idle_t = 0;
  uint32_t fight_asked = 0;  // the enemy it last asked where to fight from (training)
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
  enum Kind { kNone, kWalk, kBoard, kDrive, kFight, kFly } kind = kNone;
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
  // fight <flag>: all moved near it, fighting as usual (no pass: see who dies)
  if (w == "fight") sc.kind = Scenario::kFight, sc.point = atoi(a), sc.near = true;
  // fly <plane> <flag> [near]: up in it, 60 m over the sea, and (as drive) on to the flag
  if (w == "fly") sc.kind = Scenario::kFly, sc.vehicle = a, sc.point = atoi(b), sc.near = near(c);
  if (w == "drive") sc.kind = Scenario::kDrive, sc.vehicle = a, sc.point = atoi(b), sc.near = near(c);
  if (const char* t = getenv("BF_BOT_SCENARIO_TIME")) sc.time_limit = atof(t);
  return sc;
}

// Ways up from the water onto land, learnt from the bots themselves: the last
// spot in the water of a bot that was up on land soon after. Kept in a file
// per map (World::nav_path), so the bots know more every match.
struct Shore {
  float sea = 1e9f;  // the lowest a soldier on foot has stood (in the water: sea level)
  std::vector<Exit> exits;
  std::string path;
  void Load(const char* p) {
    if (!p || path == p) return;
    path = p;
    exits.clear();
    sea = 1e9f;  // (another map)
    if (FILE* f = OpenUtf8(p, "r")) {
      char line[160];
      while (fgets(line, sizeof(line), f)) {
        Exit e;
        if (sscanf(line, "exit %f %f %f %f %f %f", &e.wet.x, &e.wet.y, &e.wet.z, &e.up.x, &e.up.y, &e.up.z) == 6)
          exits.push_back(e);
      }
      fclose(f);
    }
  }
  bool Add(Exit e) {
    for (const Exit& x : exits)
      if (Dist2D(x.wet, e.wet) < 15.0f) return false;
    exits.push_back(e);
    if (FILE* f = OpenUtf8(path.c_str(), "a")) {
      fprintf(f, "exit %.1f %.1f %.1f %.1f %.1f %.1f\n", e.wet.x, e.wet.y, e.wet.z, e.up.x, e.up.y, e.up.z);
      fclose(f);
    }
    return true;
  }
  const Exit* Nearest(Vec p, float range) const {
    const Exit* best = nullptr;
    for (const Exit& e : exits)
      if (float d = Dist2D(e.wet, p); d < range) range = d, best = &e;
    return best;
  }
};
Shore g_shore;

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

  // A way out of the water from the terrain (World::ground): of points on
  // rings 10..200 m around, land (2 m over the sea) reached in a straight line
  // that never climbs more than 0.6 m in a metre (a beach, not a cliff or a
  // sea wall), with a route on from there; the nearest, counting half the way
  // on to the goal. False without one.
  bool LandExit(Vec from, Vec goal, Vec& out) {
    if (!w.ground || g_shore.sea > 1e8f) return false;
    float best = 1e9f;
    for (float r = 10; r <= 200; r += 10)
      for (int k = 0; k < 36; ++k) {
        float a = k * 0.1745f;
        Vec c{from.x + r * std::sin(a), 0, from.z + r * std::cos(a)};
        float h = w.ground(c.x, c.z);
        if (h < g_shore.sea + 2.0f) continue;
        c.y = h;
        float score = r + 0.5f * Dist2D(c, goal);
        if (score >= best) continue;
        bool ok = true;
        float last = w.ground(from.x, from.z);
        for (float s = 1; s <= r && ok; s += 1) {
          float f = s / r, g = w.ground(from.x + (c.x - from.x) * f, from.z + (c.z - from.z) * f);
          ok = g > -1e8f && (g < g_shore.sea - 0.5f || g - last < 0.6f);
          last = g;
        }
        if (!ok) continue;
        if (w.route) {
          Vec corners[4];
          Vec up{c.x, c.y + 0.5f, c.z};
          if (w.route(-1, up, goal, corners, 4) <= 0) continue;
        }
        best = score, out = c;
      }
    return best < 1e9f;
  }

  // ---- what the matches on this map showed (World::learned) ----
  // A spot to hold at a flag: of points 6 to 20 m around it, the one its team
  // killed from most and died at least (kills - 1.5 deaths, within 6 m), with
  // at least 3 of either there. False without one (or without knowledge).
  bool LearnedSpot(Vec flag, int team, Vec& offset) {
    if (!w.learned) return false;
    float best = 0;
    bool found = false;
    for (float r : {6.0f, 12.0f, 20.0f})
      for (int k = 0; k < 8; ++k) {
        float a = k * 0.785f;
        Vec p{flag.x + r * std::sin(a), flag.y, flag.z + r * std::cos(a)};
        float kills = w.learned(2, team, p, 6.0f), deaths = w.learned(1, team, p, 6.0f);
        if (kills + deaths < 3.0f) continue;
        float score = kills - 1.5f * deaths;
        if (score > best) best = score, found = true, offset = {p.x - flag.x, 0, p.z - flag.z};
      }
    return found;
  }
  // Where enemies usually come from around here: of 12 ways, the one with the
  // most enemy players gone through and killed from 15 to 50 m off. False
  // when there is too little to go by.
  bool Threat(Vec at, int team, float& yaw) {
    if (!w.learned) return false;
    int enemy = 3 - team;
    float best = 0;
    for (int k = 0; k < 12; ++k) {
      float a = k * 0.5236f, score = 0;
      for (float r : {15.0f, 30.0f, 50.0f}) {
        Vec p{at.x + r * std::sin(a), at.y, at.z + r * std::cos(a)};
        score += w.learned(0, enemy, p, 8.0f) + 4.0f * w.learned(2, enemy, p, 8.0f);
      }
      if (score > best) best = score, yaw = a;
    }
    return best >= 20.0f;
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

  // The next place on the way to goal: a corner of its route on the
  // navigation mesh (planned again every 5 s or when the goal moves), or the
  // goal itself without a mesh or a way.
  Vec NextOnRoute(BotIO& io, Bot& b, const Soldier& me, Vec goal) {
    if (!w.route) return goal;
    if (b.route_t == 0 || w.now - b.route_t > 5.0 || Dist2D(goal, b.route_goal) > 3.0f) {
      b.route_n = w.route(io.slot, me.pos, goal, b.route, Bot::kCorners);
      b.route_i = 0;
      b.route_t = w.now;
      b.route_goal = goal;
    }
    while (b.route_i < b.route_n && Dist2D(me.pos, b.route[b.route_i]) < 1.5f) ++b.route_i;
    return b.route_i < b.route_n ? b.route[b.route_i] : goal;
  }

  // Strategic layer: temperature per point for a team, then the bot takes the
  // hottest one, cooled by distance and by team mates already heading there.
  int ChoosePoint(const BotIO& io, const Soldier& me) {
    int best = -1;
    float best_score = -1e9f;
    const Bot& self = *reinterpret_cast<const Bot*>(io.mem);
    for (int i = 0; i < w.point_count; ++i) {
      const Point& p = w.points[i];
      if (i == self.avoid_point && w.now < self.avoid_until && w.point_count > 1) continue;
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
  static bool IsPlane(const char* k) {
    for (const char* p : {"Corsair", "Zero", "Aichi", "Val", "Kate", "Dauntless", "Avenger", "Wildcat"})
      if (strstr(k, p)) return true;
    return false;
  }
  // Same sort of vehicle as the one it rides (getting back in).
  static bool SameSort(const Bot& b, const char* k) {
    return b.ride_air ? IsPlane(k) : b.ride_land ? IsLand(k) : IsBoat(k);
  }
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
      if (kind ? !strstr(v.kind, kind) : !(land ? IsLand(v.kind) : IsBoat(v.kind))) continue;  // (a part of its name)
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

  // A plane: full throttle (LT) off the deck, then hold about 160 m over the
  // sea with the pitch (right stick Y: down climbs, as the game's flight
  // controls; BF_PLANE_PITCH_SIGN=1 the other way), turn by banking (right
  // stick X: the roll; the rudder hardly turns it) toward a rate of turn
  // (measured from its heading) for the way to the flag, and circle the flag
  // once there. Never gets out in the air.
  bool Fly(BotIO& io, Bot& b, const Soldier& me, Vec goal, Pad& cmd) {
    static const float sign = getenv("BF_PLANE_PITCH_SIGN") ? float(atof(getenv("BF_PLANE_PITCH_SIGN"))) : -1.0f;
    static const bool turn_lx = getenv("BF_PLANE_TURN") && !strcmp(getenv("BF_PLANE_TURN"), "lx");
    static const float turn_sign = getenv("BF_PLANE_TURN_SIGN") ? float(atof(getenv("BF_PLANE_TURN_SIGN"))) : 1.0f;
    double dt = w.now - b.fly_last_t;
    if (dt > 0.25) {
      if (b.fly_last_t > 0 && dt < 2.0) {
        b.fly_vy = 0.7f * b.fly_vy + 0.3f * float((me.pos.y - b.fly_last_y) / dt);
        b.fly_rate = 0.6f * b.fly_rate + 0.4f * float(Wrap(b.boat_heading - b.fly_last_h) / dt);
      }
      b.fly_last_y = me.pos.y;
      b.fly_last_h = b.boat_heading;
      b.fly_last_t = w.now;
    }
    cmd.triggers = 255u;  // LT: throttle
    if (getenv("BF_PLANE_BOOST")) cmd.buttons |= kLeftThumb, cmd.ly = 1.0f;  // EXPERIMENT
    double t = w.now - b.drive_since;
    if (t < 5.0) return true;  // rolling along the deck
    const float cruise = 380.0f;  // the sea is at about 214
    float want_vy = std::clamp((cruise - me.pos.y) * 0.25f, -8.0f, 12.0f);
    float gain = getenv("BF_PLANE_VY_GAIN") ? float(atof(getenv("BF_PLANE_VY_GAIN"))) : 0.05f;
    cmd.ry = sign * std::clamp((want_vy - b.fly_vy) * gain, -0.5f, 0.6f);
    if (me.pos.y < 240.0f && t > 8.0) cmd.ry = sign * 0.6f;  // too low: climb
    float d = Dist2D(me.pos, goal);
    float want = std::atan2(goal.x - me.pos.x, goal.z - me.pos.z);
    if (d < 150.0f) want += 1.3f;  // there: circle it
    float e = Wrap(want - b.boat_heading);
    // (The roll is a rate: a bank stays when let go, and right stick + turns
    // the heading up, BF_PLANE_ROLL_PROBE.)
    static const float max_rate = getenv("BF_PLANE_RATE") ? float(atof(getenv("BF_PLANE_RATE"))) : 0.15f;
    float want_rate = std::clamp(e * 0.4f, -max_rate, max_rate);
    if (me.pos.y < 320.0f || b.fly_vy < -8.0f) want_rate = 0;  // low or falling: wings level first
    float turn = t < 9.0 ? 0.0f : std::clamp((want_rate - b.fly_rate) * 1.0f, -0.3f, 0.3f) * turn_sign;
    // BF_PLANE_ROLL_PROBE=<amount> (testing): from 20 s, that roll 3 s, level
    // 6 s, the other way 3 s, level; heading and height logged.
    if (const char* pr = getenv("BF_PLANE_ROLL_PROBE"); pr && t > 20.0) {
      float amt = float(atof(pr)), k = float(std::fmod(t - 20.0, 24.0));
      turn = k < 3 ? amt : k < 9 ? 0.0f : k < 12 ? -amt : 0.0f;
      static double nx = 0;
      if (w.now >= nx)
        nx = w.now + 0.5, Log("fly probe: t %.1f roll %.2f heading %.0f rate %.2f height %.0f vy %.1f", t, turn,
                              b.boat_heading * 57.3f, b.fly_rate, me.pos.y, b.fly_vy);
    }
    if (turn_lx) cmd.lx = turn;
    else cmd.rx = turn;
    snprintf(b.probe_name, sizeof(b.probe_name), "fly %.0f", me.pos.y);
    if (getenv("BF_PLANE_TRACE")) {
      static double next = 0;
      if (w.now >= next) {
        next = w.now + 2.0;
        const Vehicle* v = b.boat ? FindVehicle(b.boat) : nullptr;
        Log("fly: bot%d at %.0f,%.0f,%.0f, its plane %s at %.0f,%.0f,%.0f; pad lx %.2f ly %.2f rx %.2f ry %.2f lt %u",
            io.slot + 1, me.pos.x, me.pos.y, me.pos.z, v ? v->kind : "?", v ? v->pos.x : 0, v ? v->pos.y : 0,
            v ? v->pos.z : 0, cmd.lx, cmd.ly, cmd.rx, cmd.ry, cmd.triggers & 0xFF);
      }
    }
    return true;
  }

  // Vehicles, as a few states (Bot::boat_state): walk to one, board it (E;
  // the seat pointer says when it sits in it), drive it (a landing craft is
  // let down from its crane with the right stick up; then the left stick
  // drives and steers) and get out at the flag. True while busy (pad set).
  bool Ride(BotIO& io, Bot& b, const Soldier& me, Pad& cmd) {
    const Vehicle* boat = b.boat ? FindVehicle(b.boat) : nullptr;
    const char* want_kind =
        sc.kind == Scenario::kBoard || sc.kind == Scenario::kDrive || sc.kind == Scenario::kFly ? sc.vehicle.c_str() : nullptr;
    switch (b.boat_state) {
      case kNoBoat: {
        if (w.now < b.no_boat_until || b.target_point < 0 || b.target_point >= w.point_count) return false;
        b.ride_air = false;
        if (want_kind) {
          boat = NearestVehicle(me.pos, 1e9f, io, b, true, false, want_kind);
          if (boat) b.ride_land = !IsBoat(boat->kind), b.ride_air = IsPlane(boat->kind);
        } else {
          // A landing craft from a carrier for a far flag, else a jeep or
          // tank close by for a flag a long walk away.
          float away = Dist2D(me.pos, w.points[b.target_point].pos);
          boat = away > 200.0f && !b.boat_done ? NearestVehicle(me.pos, 60.0f, io, b, true, false) : nullptr;
          b.ride_land = false;
          // One bot in four is a pilot: a plane close by (a carrier's).
          if (!boat && io.slot % 4 == 3) {
            float best = 50.0f;
            for (int i = 0; i < w.vehicle_count; ++i) {
              const Vehicle& v = w.vehicles[i];
              if (!IsPlane(v.kind) || v.obj == b.bad_boat || BoatTaken(v.obj, io)) continue;
              if (float d = Dist3D(v.pos, me.pos); d < best) best = d, boat = &v;
            }
            if (boat) b.ride_land = b.ride_air = true;
          }
          if (!boat && away > 120.0f) {
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
          b.bad_boat = b.boat;  // couldn't get to it: another one next time
          b.boat = 0;
          b.no_boat_until = w.now + 20;
          SetState(io, b, me, kNoBoat);
          return false;
        }
        float dx = boat->pos.x - me.pos.x, dz = boat->pos.z - me.pos.z;
        Vec next = NextOnRoute(io, b, me, boat->pos);  // on the navigation mesh, when there is one
        Steer(cmd, me, std::atan2(next.x - me.pos.x, next.z - me.pos.z), 0.0f);
        Walk(cmd, me, next, 1.0f);
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
          b.reverse_tries = 0;
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
        if (!me.vehicle && b.ride_air) {
          b.boat = 0;
          b.no_boat_until = w.now + 30;
          SetState(io, b, me, kNoBoat);
          return false;
        }
        if (!me.vehicle) {
          if (!b.off_since) b.off_since = w.now;
          if (w.now - b.off_since > 1.5) {
            b.off_since = 0;
            const Vehicle* v = nullptr;
            float best = 15.0f;
            for (int i = 0; i < w.vehicle_count; ++i) {
              const Vehicle& c = w.vehicles[i];
              if (!SameSort(b, c.kind)) continue;
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
        if (b.ride_air) return Fly(io, b, me, goal, cmd);
        float arrive = b.ride_land ? 30.0f : 40.0f;
        // BF_BOT_INPUT_PROBE (testing): seated, every input 4 s in turn, named
        // in the bot's state, to read off the log which one moves the vehicle.
        static const bool probe = getenv("BF_BOT_INPUT_PROBE") != nullptr;
        if (probe) {
          static const char* kTry[] = {"lt", "rt", "ly+", "ly-", "ry+", "ry-", "lb", "rb", "x", "y"};
          int k = int((w.now - b.drive_since) / 4.0) % 10;
          std::string x = kTry[k];
          if (x == "lt") cmd.triggers = 255u;
          if (x == "rt") cmd.triggers = 255u << 8;
          if (x == "ly+") cmd.ly = 1;
          if (x == "ly-") cmd.ly = -1;
          if (x == "ry+") cmd.ry = 1;
          if (x == "ry-") cmd.ry = -1;
          if (x == "lb") cmd.buttons |= 0x0100;
          if (x == "rb") cmd.buttons |= 0x0200;
          if (x == "x") cmd.buttons |= 0x4000;
          if (x == "y") cmd.buttons |= 0x8000;
          snprintf(b.probe_name, sizeof(b.probe_name), "probe %s", kTry[k]);
          return true;
        }
        if (sc.kind == Scenario::kBoard) {  // the test is over once seated: sit still
          return true;
        }
        if (w.now < b.reverse_until) {  // backing off: LB (brake, then reverse), turning
          cmd.buttons |= 0x0100;
          cmd.lx = b.veer > 0 ? -1.0f : 1.0f;
          return true;
        }
        // Stalled far from the flag (a rock, a reef, another craft): back
        // off and go round, twice, before getting out there.
        if (b.released && b.boat_travel > (b.ride_land ? 15.0f : 30.0f) && stalled &&
            Dist2D(pos, goal) > 120.0f && b.reverse_tries < 2) {
          ++b.reverse_tries;
          b.veer = b.reverse_tries % 2 ? 0.8f : -0.8f;
          b.reverse_until = w.now + 2.5;
          b.veer_until = w.now + 9.0;
          b.boat_moved = w.now + 2.5;
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
        // (A jeep or tank: getting in takes a few seconds, wait longer.)
        if (b.released && b.boat_travel < 15.0f && w.now - b.drive_since > (b.ride_land ? 12.0 : 5.0) && stalled) {
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
        float want = std::atan2(goal.x - pos.x, goal.z - pos.z) + (w.now < b.veer_until ? b.veer : 0.0f);
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
            // E gives the driver's seat (the first), whose throttle lets the
            // craft down, though the game can take a while (seen 2 s to 2
            // minutes, the same seat): up to 45 s there, then the other two
            // seats once (A after the last gets out). The throttle in pulses:
            // one held while getting in isn't taken.
            if (t > (b.seat_switches == 0 ? 45.0 : 5.0)) {
              if (++b.seat_switches > 2) {
                b.bad_boat = b.boat;
                SetState(io, b, me, kLeave);
                return true;
              }
              b.release_try = w.now;
              t = 0;
            }
            if (t < 0.3 && b.seat_switches > 0) cmd.buttons |= kA;
            else if (t > 0.6 && std::fmod(t, 1.8) > 0.3) cmd.triggers = 255u;  // LT
            return true;
          }
        }
        cmd.triggers = 255u;                           // LT: throttle
        cmd.lx = std::clamp(-e * 1.5f, -1.0f, 1.0f);  // turning right lowers the heading
        // Not moving: let go of the throttle now and then (one held while
        // getting in isn't taken).
        if (stalled && std::fmod(w.now - b.drive_since, 4.0) < 0.4) cmd.triggers = 0;
        return true;
      }
      case kLeave: {
        if (!b.ride_land) b.boat_done = true;
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
    // Before a teleport: stand still a moment (the bot's game finds its
    // position by value).
    // (Off the spawn point first: standing on it, the spawn point's own
    // position would match too.)
    if (sc.near && !b.teleported && !b.hold_until && Dist3D(me.pos, b.spawned_at) > 4.0f)
      b.hold_until = w.now + 1.5;
    if (sc.near && !b.teleported && !b.hold_until) return;
    if (sc.near && !b.teleported && w.now < b.hold_until) return;
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
      case Scenario::kFly:
        // (seated in the plane it went for: a flying plane's place isn't known)
        done = me.vehicle && b.ride_air && b.boat_state == kDrive && me.pos.y > 275.0f;
        break;
      case Scenario::kDrive: {
        // Driven to the flag, or most of the way (a landing craft stops at
        // the beach).
        float d = sc.point < w.point_count ? Dist2D(me.pos, w.points[sc.point].pos) : 1e9f;
        done = b.drive_from > 0 && (d < 45 || (b.boat_state >= kDrive && b.drive_from - d > 0.6f * b.drive_from));
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
      if (b.alive) b.boat = 0, b.boat_state = kNoBoat, b.teleported = false, b.hold_until = 0, b.boat_done = false,
                   b.stuck_level = 0, b.exit_tries = 0, b.shore_t = 0, b.exit_phase = 0;
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
    if (!b.alive) b.last_pos = me->pos, b.last_moved = w.now, b.spawned_at = me->pos;
    b.alive = true;
    b.team = me->team;
    // The advisor's answer: a way round what blocks it, or cover.
    if (Vec p; w.answer && w.answer(io.slot, &p) && Dist2D(p, me->pos) < 40) {
      b.advice = p, b.advice_cover = b.asked == 2 || b.asked == 5;
      b.advice_until = w.now + 4 + 0.7 * Dist2D(p, me->pos);
      if (!b.advice_cover) b.detour_until = 0;
      Log("bots: bot%d: the advisor's place %.0f,%.0f (%s)", io.slot + 1, p.x, p.z, b.advice_cover ? "cover" : "a way round");
    }
    if (w.now < b.advice_until && Dist2D(b.advice, me->pos) < 1.5f) b.advice_until = 0;
    if (sc.kind != Scenario::kNone) Judge(io, b, *me);
    if (sc.near && b.hold_until && w.now < b.hold_until + 0.5) {  // holding still for the teleport
      snprintf(io.state, sizeof(io.state), "teleport");
      io.pad = cmd;
      return;
    }
    // Behaviour weights (BF1942 style: how much each is needed right now).
    // The nearest enemy in sight (eye to chest; of the three nearest within
    // 70 m), or the one last seen for 1.5 s more (behind a tree a moment).
    const Soldier* enemy = nullptr;
    float enemy_d = 1e9f;
    {
      const Soldier* near[3] = {};
      float near_d[3] = {1e9f, 1e9f, 1e9f};
      for (int i = 0; i < w.soldier_count; ++i) {
        const Soldier& s = w.soldiers[i];
        if (s.team == me->team) continue;
        float d = Dist3D(s.pos, me->pos);
        if (d > 70) continue;
        if (s.obj == b.seen_enemy && w.now < b.seen_until && d < enemy_d) enemy = &s, enemy_d = d;
        for (int k = 0; k < 3; ++k)
          if (d < near_d[k]) {
            for (int m = 2; m > k; --m) near[m] = near[m - 1], near_d[m] = near_d[m - 1];
            near[k] = &s, near_d[k] = d;
            break;
          }
      }
      for (int k = 0; k < 3 && near[k]; ++k) {
        Vec eye{me->pos.x, me->pos.y + 1.6f, me->pos.z}, chest{near[k]->pos.x, near[k]->pos.y + 1.2f, near[k]->pos.z};
        if (w.sight && !w.sight(eye, chest)) continue;
        enemy = near[k], enemy_d = near_d[k];
        b.seen_enemy = enemy->obj, b.seen_until = w.now + 1.5;
        break;
      }
    }
    float w_engage = enemy && enemy_d < 70 && (sc.kind == Scenario::kNone || sc.kind == Scenario::kFight) ? 100 - enemy_d : 0;
    // Losing a fight (under 45 health), or shot by someone it can't see (25
    // or more lost in 3 s): the advisor picks cover.
    if (w.now - b.health_t > 3) b.health_was = me->health, b.health_t = w.now;
    if (w.ask && w.now >= b.advice_until && !me->vehicle && me->health > 0) {
      char what[160] = {};
      if (w_engage > 0 && me->health < (w.training ? 70 : 45))
        snprintf(what, sizeof(what), "It is losing a fight (health %.0f of 100) against the enemy %.0f m away. "
                 "Pick a place out of the enemies' sight.", me->health, enemy_d);
      else if (!enemy && b.health_was - me->health >= (w.training ? 15 : 25))
        snprintf(what, sizeof(what), "It is being shot (health %.0f of 100) and cannot see who shoots. "
                 "Pick a place with cover.", me->health);
      if (what[0] && w.ask(io.slot, 2, what)) b.asked = 2;
    }
    // Training: a medium question too, where to fight a new enemy from.
    if (w.ask && w.training && enemy && enemy->obj != b.fight_asked && enemy_d > 20 && !me->vehicle &&
        w.now >= b.advice_until) {
      b.fight_asked = enemy->obj;
      char what[160];
      snprintf(what, sizeof(what), "An enemy has come into sight %.0f m away. Pick a place to fight it from (cover "
               "with a view of it), or 0 to fight from where it stands.", enemy_d);
      if (w.ask(io.slot, 5, what)) b.asked = 5;
    }
    // Standing about (30 s within 5 m, no enemy, on foot): the advisor picks
    // where to go.
    if (Dist2D(me->pos, b.idle_pos) > 5 || enemy || me->vehicle || !b.idle_t) b.idle_pos = me->pos, b.idle_t = w.now;
    else if (w.now - b.idle_t > (w.training ? 10 : 30) && w.ask) {
      b.idle_t = w.now;
      if (w.ask(io.slot, 4, "It has stood about here for 30 s with no enemy in sight. Pick the place it should go to: "
                            "on towards its flag, or a better spot to hold it from."))
        b.asked = 4;
    }
    if (b.target_point < 0 || b.target_point >= w.point_count ||
        (sc.point < 0 && w.points[b.target_point].owner == me->team && Rand(0, 1) < 0.01f)) {
      if (sc.point >= 0) b.target_point = std::min(sc.point, w.point_count - 1);
      else if (const char* p = getenv("BF_BOT_POINT")) b.target_point = atoi(p);
      else b.target_point = ChoosePoint(io, *me);
      b.target_offset = {Rand(-6, 6), 0, Rand(-6, 6)};
    }
    // Shore: remember where bots get up out of the water.
    if (!me->vehicle) {
      g_shore.sea = std::min(g_shore.sea, me->pos.y);
      if (me->pos.y < g_shore.sea + 1.5f) {
        b.shore_from = me->pos;  // last spot in the water
        b.shore_t = w.now;
      } else if (b.shore_t > 0 && me->pos.y > g_shore.sea + 4.5f && w.now - b.shore_t < 40.0) {
        if (g_shore.Add(Exit{b.shore_from, me->pos}))
          Log("bots: bot%d found a way up from the water at %.0f,%.0f", io.slot + 1, b.shore_from.x, b.shore_from.z);
        b.shore_t = 0;
      }
    }
    // Progress / stuck detection.
    if (Dist2D(me->pos, b.last_pos) > 1.5f) b.last_pos = me->pos, b.last_moved = w.now;
    bool at_point = b.target_point >= 0 && Dist2D(me->pos, w.points[b.target_point].pos) < 12;
    // Landing craft from the carriers; BF_BOT_NO_VEHICLES: none.
    static const bool vehicles_off = getenv("BF_BOT_NO_VEHICLES") != nullptr;
    bool ride = (sc.kind == Scenario::kNone && !vehicles_off) || sc.kind == Scenario::kBoard ||
                sc.kind == Scenario::kDrive || sc.kind == Scenario::kFly;
    if (ride && Ride(io, b, *me, cmd)) {
      static const char* kNames[] = {"foot", "to vehicle", "boarding", "driving", "leaving"};
      snprintf(io.state, sizeof(io.state), "%s", kNames[b.boat_state]);
      if (b.probe_name[0] && b.boat_state == kDrive) snprintf(io.state, sizeof(io.state), "%s", b.probe_name);
      if (b.boat_state != kDrive) b.probe_name[0] = 0;
      io.pad = cmd;
      return;
    }
    // Stuck (no progress for 2.5 s): back off, then a detour at an angle
    // off the direct line. Real progress forgives it; seven failures and it
    // tries another flag.
    if (b.stuck_level && Dist2D(me->pos, b.stuck_at) > 25) b.stuck_level = 0;
    if (!at_point && w_engage <= 0 && w.now - b.last_moved > 2.5 && w.now > b.detour_until) {
      if (!b.stuck_level) b.stuck_at = me->pos;
      ++b.stuck_level;
      // On a route and stuck again where it got stuck before: the navigation
      // mesh is wrong there (as Bad Company's bots): that polygon, 1.5 m on
      // towards the next corner, is off for routes, and a new route.
      if (w.block && b.route_i < b.route_n) {
        if (Dist2D(me->pos, b.nav_stuck_at) < 3.0f) {
          Vec c = b.route[b.route_i];
          float d = std::max(Dist2D(c, me->pos), 0.1f);
          w.block(Vec{me->pos.x + (c.x - me->pos.x) / d * 1.5f, me->pos.y, me->pos.z + (c.z - me->pos.z) / d * 1.5f});
          b.route_t = 0;
        }
        b.nav_stuck_at = me->pos;
      }
      b.last_moved = w.now;
      // Close to the flag it keeps trying: another flag is usually back through the water.
      if (b.stuck_level > 7 && b.target_point >= 0 && Dist2D(me->pos, w.points[b.target_point].pos) < 30)
        b.stuck_level = 1;
      else if (b.stuck_level > 7 && sc.point < 0) {
        b.avoid_point = b.target_point;
        b.avoid_until = w.now + 60;
        b.target_point = -1;
        b.stuck_level = 0;
      }
      // One side for three tries (alternating cancels out: along a shore or
      // a wall it has to keep going one way), each a little longer.
      if (b.stuck_level % 3 == 1) b.detour_side = b.detour_side > 0 ? -1.0f : 1.0f;
      b.back_until = w.now + 0.7;
      b.detour_until = w.now + 0.7 + std::min(3.0 + 1.0 * b.stuck_level, 8.0);
      b.detour_angle = b.detour_side * std::min(1.2f + 0.2f * b.stuck_level, 1.9f);
      // Stuck three times or more: the advisor asked for a better way round
      // (its answer, when it comes, takes over from the detour).
      if (w.ask && b.stuck_level >= (w.training ? 1 : 3) && !me->vehicle &&
          w.ask(io.slot, 1, "It keeps getting stuck here on its way to its flag. "
                         "Pick the place that gets it around what blocks the way."))
        b.asked = 1;
      // At the water's edge: along the shore, longer (a way up is somewhere).
      if (me->pos.y < g_shore.sea + 4.0f) {
        b.detour_angle = b.detour_side * 1.57f;
        b.detour_until = w.now + 0.7 + 10.0;
      }
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
      if (w.now < b.advice_until && b.advice_cover) Walk(cmd, *me, b.advice, 1.0f);  // (to cover, firing)
      snprintf(io.state, sizeof(io.state), "engage");
    } else if (w.now < b.advice_until) {
      // To the advisor's place.
      Steer(cmd, *me, std::atan2(b.advice.x - me->pos.x, b.advice.z - me->pos.z), 0.0f);
      Walk(cmd, *me, b.advice, 1.0f);
      if (!b.advice_cover && std::fmod(w.now, 1.0) < 0.15) cmd.buttons |= kA;  // hop over small things
      snprintf(io.state, sizeof(io.state), b.advice_cover ? "to cover" : "advised");
    } else if (bool wet = b.target_point >= 0 && me->pos.y < g_shore.sea + 1.5f && w.ground &&
                          me->pos.y - w.ground(me->pos.x, me->pos.z) < 4.0f;  // (near a shore: not a carrier's deck)
               wet && (w.now - b.land_exit_t < 8.0 ||
                       (w.now - b.land_try_t > 8.0 &&
                        (b.land_try_t = w.now, LandExit(me->pos, w.points[b.target_point].pos, b.land_exit)) &&
                        (b.land_exit_t = w.now, true)))) {
      // In the water: the way up the terrain shows (LandExit), straight there.
      Vec e = b.land_exit;
      Steer(cmd, *me, std::atan2(e.x - me->pos.x, e.z - me->pos.z), 0.0f);
      Walk(cmd, *me, e, 1.0f);
      if (std::fmod(w.now, 1.0) < 0.15) cmd.buttons |= kA;  // (jump: over a lip at the edge)
      if (Dist2D(e, me->pos) < 3.0f) b.land_exit_t = -100;
      b.stuck_level = 0;
      snprintf(io.state, sizeof(io.state), "to land exit");
    } else if (b.target_point >= 0 && (b.exit_phase ||
                                        (me->pos.y < g_shore.sea + 1.5f && b.stuck_level > 0 && b.exit_tries < 3 &&
                                         g_shore.Nearest(me->pos, 200.0f)))) {
      // In the water and stuck: the way up another bot found, the way it went.
      if (!b.exit_phase) {
        b.exit_on = *g_shore.Nearest(me->pos, 200.0f);
        b.exit_phase = 1;
        b.exit_since = w.now;
        ++b.exit_tries;
      }
      Vec e = b.exit_phase == 1 ? b.exit_on.wet : b.exit_on.up;
      if (b.exit_phase == 1 && Dist2D(e, me->pos) < 4.0f) b.exit_phase = 2, b.exit_since = w.now;
      else if (b.exit_phase == 2 && (Dist2D(e, me->pos) < 5.0f || me->pos.y > g_shore.sea + 4.5f))
        b.exit_phase = 0, b.stuck_level = 0;
      if (w.now - b.exit_since > 40.0) b.exit_phase = 0;  // not getting there
      if (b.exit_phase == 1 && me->pos.y > g_shore.sea + 4.5f) b.exit_phase = 0;  // up on land already
      Steer(cmd, *me, std::atan2(e.x - me->pos.x, e.z - me->pos.z), 0.0f);
      Walk(cmd, *me, e, 1.0f);
      if (std::fmod(w.now, 1.0) < 0.15) cmd.buttons |= kA;
      snprintf(io.state, sizeof(io.state), b.exit_phase == 2 ? "up shore exit" : "to shore exit");
    } else if (b.target_point >= 0 && w.now < b.detour_until) {
      Vec goal = w.points[b.target_point].pos;
      float head = std::atan2(goal.x - me->pos.x, goal.z - me->pos.z);
      if (w.now < b.back_until) {
        Steer(cmd, *me, head, 0.0f);
        cmd.ly = -1.0f;
      } else {
        float dir = head + b.detour_angle;
        Vec to{me->pos.x + 10 * std::sin(dir), me->pos.y, me->pos.z + 10 * std::cos(dir)};
        Steer(cmd, *me, dir, 0.0f);
        Walk(cmd, *me, to, 1.0f);
        if (std::fmod(w.now, 1.0) < 0.15) cmd.buttons |= kA;  // hop over small things
      }
      snprintf(io.state, sizeof(io.state), "detour %d", b.stuck_level);
    } else if (b.target_point >= 0) {
      Vec flag = w.points[b.target_point].pos;
      // Near its flag: the spot the matches showed is good to hold there
      // (chosen again every 30 s, and for each new flag).
      if (Dist2D(me->pos, flag) < 60 && (b.spot_point != b.target_point || w.now - b.spot_t > 30)) {
        b.spot_point = b.target_point, b.spot_t = w.now;
        Vec off;
        if (w.points[b.target_point].owner == me->team && LearnedSpot(flag, me->team, off)) {
          b.target_offset = off;
          if (getenv("BF_BOT_LEARN_TRACE"))
            Log("learned: bot%d holds %.0f,%.0f off flag %d", io.slot + 1, off.x, off.z, b.target_point);
        }
      }
      Vec goal = flag;
      goal.x += b.target_offset.x, goal.z += b.target_offset.z;
      float dx = goal.x - me->pos.x, dz = goal.z - me->pos.z;
      Vec next = NextOnRoute(io, b, *me, goal);
      // Near the flag: looking where enemies usually come from (it walks on
      // all the same), else where it goes.
      if (Dist2D(me->pos, flag) < 60 && w.now - b.threat_t > 3.0)
        b.threat_t = w.now, b.threat = Threat(me->pos, me->team, b.threat_yaw);
      if (b.threat && getenv("BF_BOT_LEARN_TRACE")) {
        static double said = 0;
        if (w.now > said) said = w.now + 10, Log("learned: bot%d watches %.0f deg near flag %d", io.slot + 1, b.threat_yaw * 57.3f, b.target_point);
      }
      bool watch = b.threat && Dist2D(me->pos, flag) < 60;
      Steer(cmd, *me, watch ? b.threat_yaw : std::atan2(next.x - me->pos.x, next.z - me->pos.z), 0.0f);
      Walk(cmd, *me, next, 1.0f);
      if (!watch && !at_point && std::hypot(dx, dz) > 25) cmd.buttons |= kLeftThumb;  // sprint
      snprintf(io.state, sizeof(io.state), at_point ? "at flag %d" : "to flag %d", b.target_point);
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
  g_shore.Load(w->nav_path);
  Brain brain{*w, rng, sc};
  for (int i = 0; i < w->bot_count; ++i) brain.Think(w->bots[i]);
}
