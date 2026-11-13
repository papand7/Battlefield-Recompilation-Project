// The bot brain's interface. bots.cpp (the host) reads the server's soldiers,
// flags and vehicles into a World and asks the brain (brain.cpp) for every
// bot's pad. Plain structs and one C function, so the brain can also be built
// as bot_brain.dll and reloaded while a match runs (BF_BOT_BRAIN=<dll>, see
// docs/DEVELOPERS.md): its state lives in BotIO::mem, kept by the host.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace botapi {

constexpr int kVersion = 8;      // bump when a struct here changes
constexpr int kMaxBots = 16;
constexpr int kMemSize = 1024;   // per-bot brain state
constexpr float kPi = 3.14159265f;

// Pad buttons (XInput's bits).
enum : uint32_t { kDpadRight = 0x0008, kLeftThumb = 0x0040, kA = 0x1000, kB = 0x2000 };

// The behaviour settings the bots tune by self-play (runtime/bot_tuning.h):
// each with its default (the hand-made brain's) and the range tried.
enum Tune {
  kEngageRange,     // enemies fought within (m)
  kCloseIn,         // farther than this (m), it walks at the enemy while firing
  kAdvance,         // how fast it walks in then (stick, 0-1)
  kStrafePeriod,    // seconds between strafe changes (on average)
  kStrafeAmount,    // how hard it strafes (stick, 0-1)
  kAimTolerance,    // it fires when its aim is within this (radians)
  kFlagMates,       // a flag is worth this much less per teammate going there
  kFlagDistance,    // ... and this much less per metre away
  kFlagDefend,      // an own flag is worth this much more per enemy near it
  kFlagNeutral,     // a neutral flag's worth
  kFlagEnemy,       // an enemy flag's worth
  kSprintDistance,  // farther than this from where it goes (m): it sprints
  kTuneCount
};
struct TuneInfo { const char* name; float def, lo, hi; };
inline constexpr TuneInfo kTuneInfo[kTuneCount] = {
    {"engage_range", 70, 40, 110},   {"close_in", 30, 10, 60},       {"advance", 0.6f, 0, 1},
    {"strafe_period", 1.2f, 0.3f, 3}, {"strafe_amount", 1, 0, 1},     {"aim_tolerance", 0.06f, 0.02f, 0.15f},
    {"flag_mates", 12, 0, 30},       {"flag_distance", 0.04f, 0, 0.15f}, {"flag_defend", 25, 0, 60},
    {"flag_neutral", 60, 20, 100},   {"flag_enemy", 45, 20, 100},    {"sprint_distance", 25, 5, 60},
};

struct Vec { float x = 0, y = 0, z = 0; };
struct Soldier {
  uint32_t obj;
  int team;          // 1 US, 2 Japan
  Vec pos;
  float yaw, pitch;  // radians; forward = (sin yaw, cos yaw) in x/z
  uint32_t vehicle;  // the vehicle it sits in (its seat), 0 on foot
  float health;      // 0-100
};
struct Point { uint32_t obj; int owner; Vec pos; };
struct Vehicle { uint32_t obj; char kind[32]; Vec pos; };
struct Pad {
  float lx, ly, rx, ry;  // -1..1 (ly + forward, ry + look up)
  uint32_t buttons;
  uint32_t triggers;     // lt | rt << 8
};

struct BotIO {
  int slot;
  uint32_t soldier;     // in: its soldier, 0 while dead or not identified yet
  Pad pad;              // out
  char state[24];       // out: what it is doing (logs, BF_BOT_LOG)
  bool teleport;        // out: put the soldier at `to` (scenarios)
  Vec to;
  const float* tune;    // in: its behaviour settings (kTuneCount; null: the defaults)
  uint32_t mem_tag;     // the brain's state: reset when the tag isn't its own
  alignas(8) unsigned char mem[kMemSize];
};

struct World {
  int version;
  double now;  // seconds
  const Soldier* soldiers; int soldier_count;
  const Point* points; int point_count;
  const Vehicle* vehicles; int vehicle_count;
  BotIO* bots; int bot_count;
  bool points_changed;   // the flags were found again: targets are stale
  const char* scenario;  // BF_BOT_SCENARIO, or null
  int result;            // out: 1 the scenario passed, -1 failed
  char result_text[160];
  void (*log)(const char* text);
  const char* nav_path;  // the brain's notes on this map (ways up from the water), kept across matches
  // The map's navigation mesh (runtime/bots/nav.cpp; null without one): up to
  // max corners of a route for that bot from a to b (0: no way), and a place a
  // bot couldn't get onto (off for routes from then on).
  int (*route)(int slot, Vec a, Vec b, Vec* corners, int max);
  void (*block)(Vec at);
  // Whether nothing (the map's collision, its terrain) is between a and b:
  // an eye and a chest. Null without it.
  bool (*sight)(Vec a, Vec b);
  // The terrain's height there (-1e9 off it or not known yet). Null without it.
  float (*ground)(float x, float z);
  // What the matches on this map showed (bot_knowledge.h): the sum within
  // radius of `at` of what (0 where players of that team went, 1 where they
  // died, 2 where they killed from). Null without it.
  float (*learned)(int what, int team, Vec at, float radius);
  // The advisor (bot_llm.h, --bot-llm; else what it learned on this map): a
  // small vision model shown the bot's surroundings with places around it to
  // walk to, asked which one solves the situation (kind 1: stuck, 2: cover,
  // 4: standing idle; true: asked, one question at a time). Its answer comes later: true once,
  // with the place. The host judges what came of it and learns from that.
  bool (*ask)(int slot, int kind, const char* situation);
  bool (*answer)(int slot, Vec* place);
  // --train: a server only bots play on; asking about medium situations too
  // (kind 5: where to fight a new enemy from).
  bool training;
};

using ThinkFn = void (*)(World*);

inline float Wrap(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}
inline float Setting(const BotIO& io, Tune t) { return io.tune ? io.tune[t] : kTuneInfo[t].def; }
inline float Dist2D(Vec a, Vec b) { return std::hypot(a.x - b.x, a.z - b.z); }
inline float Dist3D(Vec a, Vec b) { return std::hypot(Dist2D(a, b), a.y - b.y); }

// Turn the view towards a yaw and pitch (right stick).
inline void Steer(Pad& out, const Soldier& me, float want_yaw, float want_pitch) {
  float e = Wrap(want_yaw - me.yaw);
  out.rx = std::clamp(-e * 2.0f, -1.5f, 1.5f);
  float ep = want_pitch - me.pitch;
  float ry = std::clamp(ep * 2.0f, -1.5f, 1.5f);
  if (std::fabs(ep) > 0.03f && std::fabs(ry) < 0.35f) ry = ry < 0 ? -0.35f : 0.35f;  // small pitch inputs don't move it
  out.ry = ry;
}

// Walk towards a target, whatever way the soldier faces (left stick).
inline void Walk(Pad& out, const Soldier& me, Vec target, float speed) {
  float dx = target.x - me.pos.x, dz = target.z - me.pos.z;
  float len = std::hypot(dx, dz);
  if (len < 0.5f) { out.lx = out.ly = 0; return; }
  dx /= len, dz /= len;
  float fx = std::sin(me.yaw), fz = std::cos(me.yaw);  // forward
  float rxv = -fz, rzv = fx;                            // right
  out.ly = std::clamp((dx * fx + dz * fz) * speed, -1.0f, 1.0f);
  out.lx = std::clamp((dx * rxv + dz * rzv) * speed, -1.0f, 1.0f);
}

}  // namespace botapi

#ifdef BF_BOT_BRAIN_DLL
#define BOT_BRAIN_API extern "C" __declspec(dllexport)
#else
#define BOT_BRAIN_API extern "C"
#endif
BOT_BRAIN_API void BotBrainThink(botapi::World* w);
BOT_BRAIN_API int BotBrainVersion();
