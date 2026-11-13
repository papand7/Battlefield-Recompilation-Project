// Game patches: strong definitions of chosen weak sub_ symbols from the
// recompiled code.
#include "kernel.h"
#include "xenia_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <excpt.h>
#endif
#include <string>
#include <thread>
#include <vector>

namespace rt {
uint32_t g_max_fps = 120;  // --max-fps (0 = no limit), applied in VdSwap
bool g_lan_host = false;   // --host
bool g_vsync = false;      // --vsync
std::string g_host_map;    // --host-map: host this level from the main menu
std::mutex g_host_map_mutex;  // (taken to change it in a level's run: the bots' director reads it)
bool g_log_ui = false;     // --log-ui: log the menus' callbacks to the game
std::string g_ui_call;     // --ui-call name:args: fire this menu callback instead
std::string g_log_settings;  // --log-settings Table.Member,...
}  // namespace rt

using namespace rt;

#define GAME_PATCH(addr)                      \
  extern "C" PPC_FUNC(__imp__sub_##addr);     \
  PPC_FUNC(sub_##addr)

static void HostFromMainLoop(PPCContext& ctx, uint8_t* base);
static void LogSettings(PPCContext& ctx, uint8_t* base);

// Game.MaxFps getter (returns the setting's float in f1), read by the frame
// timer sub_822C7388. With a limit, the timer runs Game.MaxFps simulation ticks
// per second, but every tick lasts 1/rate (timer +44, set up in sub_822C7270),
// so a limit above the rate makes the game run fast. With 0 (no limit) it ticks
// at +44 and measures real frame time. So the client's timer gets 0 and VdSwap (gpu.cpp)
// caps the frame rate at --max-fps. The server's timer (mode 1: one tick per
// loop, paced by sleeping 1/Game.MaxFps) keeps 30, else it spins.
static thread_local uint32_t t_timer_mode;
GAME_PATCH(822C4830) {
  HostFromMainLoop(ctx, base);
  LogSettings(ctx, base);
  __imp__sub_822C4830(ctx, base);
  ctx.f1.f64 = t_timer_mode == 1 ? 30.0 : 0.0;
}

// Frame timer setup: r3 = timer, r4 = tick rate (30), r5 = mode. Every
// simulation tick lasts 1/rate (worked out here), so the client's timers (made
// on the main thread at boot) tick at the frame rate: movement then updates
// every frame instead of every 4th at 120 fps. The server's timer (made later
// on its loading thread) stays at 30. Up to 4 ticks a frame (+56, game: 2)
// keep game time right when the frame rate dips below half the tick rate.
static uint32_t g_client_thread;
GAME_PATCH(822C7270) {
  if (!g_client_thread) g_client_thread = CurrentThreadId();
  bool client = CurrentThreadId() == g_client_thread;
  if (client) ctx.r4.u64 = g_max_fps ? std::clamp(g_max_fps, 30u, 240u) : 120u;
  uint32_t timer = ctx.r3.u32;
  __imp__sub_822C7270(ctx, base);
  if (client) St32(timer + 56, 4);
}

// Frame timer update (r3 = timer; +40 mode, +44 tick rate, +48 ticks).
GAME_PATCH(822C7388) {
  t_timer_mode = Ld32(ctx.r3.u32 + 40);
  __imp__sub_822C7388(ctx, base);
  t_timer_mode = 0;
}

// Sets the frame limiter's mode (r4: 2 = vblank-synced with Game.VsyncEnable,
// else 0/1 = timed), called from the render loop at 823974BC.
GAME_PATCH(822BBCF8) {
  if (!g_vsync) ctx.r4.u64 = 0;
  __imp__sub_822BBCF8(ctx, base);
}

// D3D queues a flip: r3 = front buffer | interval << 8 | immediate threshold %.
// The flip happens once the vblank count reaches last flip + interval (the game
// asks for 2, i.e. 30 fps at 60 Hz). Interval 0 flips at once.
GAME_PATCH(82160160) {
  if (!g_vsync) ctx.r3.u64 &= ~uint64_t(0xF00);
  __imp__sub_82160160(ctx, base);
}

// CRT startup step run just before the entry point checks the "parse the command
// line" flag at 82CCB968, which retail never sets, so main always got argc 0.
// Set it when --args gave the game a command line (ExLoadedCommandLine).
GAME_PATCH(827E44F8) {
  __imp__sub_827E44F8(ctx, base);
  if (!g_guest_args.empty()) St32(0x82CCB968, 1);
}

// ClientExplosionEntity's constructor (r3 the new object; vtable 820068D8).
// BF_EXPLOSION_LOG: 0.3 s later, every plausible world position (x, z within
// 3 km, y within -100...800) in the object and in what its first 0x200 bytes
// point to, to find where the explosion's position is kept.
// FIELD OF VIEW (g_fov, horizontal degrees at 16:9; the game's own is 55
// vertical = 85.6 horizontal). The player's camera reaches the view through
// the camera manager's frame, sub_82294A58(manager): its first call of the
// view's SetFov (sub_82583248, f1 = vertical radians, at 0x82294CAC) is the
// camera's; a second one (0x82294CF4) only overrides it. sub_82294818 hands
// the same angle to another view (sub_82623DB8). Each is widened by one ratio
// of half-angle tangents, so a sight's zoom narrows as much as before. The
// manager keeps a FOV controller at +0xE0 (+0x28 the camera's own angle, +0x30
// the angle now, a zoom included): aiming is now below own (AIM SENSITIVITY).
namespace {
double WiderFov(double radians) {
  const float h = rt::g_fov;
  if (h <= 0.0f || !(radians > 0.0 && radians < 3.0)) return radians;
  static const double kGame = std::tan(85.6 * 0.5 * 3.14159265358979 / 180.0);
  double k = std::tan(double(std::clamp(h, 60.0f, 120.0f)) * 0.5 * 3.14159265358979 / 180.0) / kGame;
  return std::min(2.0 * std::atan(std::tan(radians * 0.5) * k), 2.6);
}
float GuestFloat(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
void SetGuestFloat(uint32_t a, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  St32(a, u);
}
}  // namespace

// (No link register here: the camera manager's frame, sub_82294A58(manager),
// and sub_82294818 are marked while they run; the manager's first SetFov is
// the camera's, at 0x82294CAC, the second an override at 0x82294CF4.)
thread_local uint32_t t_camera_manager = 0;
thread_local int t_camera_setfov = 0;
thread_local bool t_second_view = false;
GAME_PATCH(82294A58) {
  const uint32_t manager = ctx.r3.u32, before = t_camera_manager;
  const int calls = t_camera_setfov;
  t_camera_manager = manager, t_camera_setfov = 0;
  __imp__sub_82294A58(ctx, base);
  t_camera_manager = before, t_camera_setfov = calls;
}
GAME_PATCH(82294818) {
  const bool before = t_second_view;
  t_second_view = true;
  __imp__sub_82294818(ctx, base);
  t_second_view = before;
}

GAME_PATCH(82583248) {
  if (t_camera_manager && t_camera_setfov++ == 0) {
    const uint32_t manager = t_camera_manager;
    const uint32_t controller = manager ? Ld32(manager + 0xE0) : 0;
    if (controller) {
      float now = GuestFloat(controller + 0x30), own = GuestFloat(controller + 0x28);
      bool aiming = own > 1.0f && now < own - 0.5f;
      static const bool trace = std::getenv("BF_FOV_TRACE") != nullptr;
      if (trace && aiming != rt::g_aiming) RT_INFO("fov: aiming %s (%.1f of %.1f degrees)", aiming ? "on" : "off", now, own);
      rt::g_aiming = aiming;
    }
    ctx.f1.f64 = WiderFov(ctx.f1.f64);
  }
  __imp__sub_82583248(ctx, base);
}

GAME_PATCH(82623DB8) {
  if (t_second_view) ctx.f1.f64 = WiderFov(ctx.f1.f64);
  __imp__sub_82623DB8(ctx, base);
}

// Physics effects (debris, falling trees): sub_8253EDF8(world, f1, f2) runs
// once a tick with the tick's time in f1 and the frame's in f2, which only a
// second, smoothed physics system inside uses. With ticks and frames apart
// (the server's 30 Hz) that system ran at the wrong speed; it gets the tick's.
GAME_PATCH(8253EDF8) {
  if (ctx.f1.f64 > 0.0) ctx.f2.f64 = ctx.f1.f64;
  __imp__sub_8253EDF8(ctx, base);
}

// HUD and menu animations above 30 fps: the UI's update steps its Flash
// player one movie frame a call, sub_82954388(player, 33, 1), once a game
// frame, and the movies are authored for 30 frames a second (at 120 fps they
// played 4x fast). The player is stepped as many times as a 30 Hz clock has
// ticked since the last step (none on most frames, two at most after a hitch).
GAME_PATCH(82954388) {
  if (ctx.r4.u32 != 33 || rt::g_max_fps == 30) {
    __imp__sub_82954388(ctx, base);
    return;
  }
  static const auto start = std::chrono::steady_clock::now();
  static int64_t stepped = -1;  // movie frames played (on the 30 Hz clock)
  const int64_t due = int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 30.0);
  if (stepped < 0 || due - stepped > 2) stepped = due - (stepped < 0 ? 1 : 2);
  const PPCContext call = ctx;
  for (; stepped < due; ++stepped) {
    ctx = call;
    __imp__sub_82954388(ctx, base);
  }
}

// A player whose game was closed stays on the server: the network host made
// by sub_823BD870(host, server) keeps how long a client may stay silent (+556
// while loading, +560 in a match, seconds) and the game can set both to
// 172800 (two days). Kept to 180 s and 30 s, a vanished player is dropped as
// one who leaves.
GAME_PATCH(823BD870) {
  const uint32_t host = ctx.r3.u32;
  __imp__sub_823BD870(ctx, base);
  if (!host) return;
  float loading = GuestFloat(host + 556), ingame = GuestFloat(host + 560);
  if (!(loading <= 180.0f)) SetGuestFloat(host + 556, 180.0f);
  if (!(ingame <= 30.0f)) SetGuestFloat(host + 560, 30.0f);
  RT_INFO("server: a silent player is dropped after %.0f s in the match, %.0f s loading (the game's %.0f and %.0f)",
          GuestFloat(host + 560), GuestFloat(host + 556), ingame, loading);
}

// The game's own music and voice volumes (PC settings): the sound system (the
// object at 82CC2200) keeps them at +504 and +508 with a changed flag at +513
// (its setters 826F1A08 / 826F1A18, vtable slots 28 and 29, called by name
// from 822736B8). 100 % is the game's own level (read when the object is
// first seen); set again every few seconds (a level may reset them).
namespace {
void AudioTick(PPCContext& ctx) {
  (void)ctx;
  static uint32_t system_seen = 0;
  static float game_music = 1.0f, game_voice = 1.0f, music = -1.0f, voice = -1.0f;
  static int ticks = 0;
  uint32_t system = Ld32(0x82CC2200);
  if (!system) return;
  auto load = [](uint32_t a) {
    uint32_t u = Ld32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
  };
  auto store = [](uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(a, u);
  };
  if (system != system_seen) {
    system_seen = system;
    float m = load(system + 504), v = load(system + 508);
    game_music = m >= 0.0f && m <= 4.0f ? m : 1.0f;
    game_voice = v >= 0.0f && v <= 4.0f ? v : 1.0f;
    RT_INFO("sound: the game's music %.2f, voice %.2f (sound system %08X)", game_music, game_voice, system);
    ticks = 500;
  }
  if (++ticks < 500 && music == rt::g_music_volume && voice == rt::g_voice_volume) return;  // ~2.7 s
  ticks = 0;
  music = rt::g_music_volume, voice = rt::g_voice_volume;
  store(system + 504, game_music * music);
  store(system + 508, game_voice * voice);
  *G<uint8_t>(system + 513) = 1;
}
struct AudioTickInstall {
  AudioTickInstall() { rt::g_audio_tick = AudioTick; }
} g_audio_tick_install;
}  // namespace

// SKIP INTRO: the startup sequence (the publisher's and studio's logos, the
// rating and island cards) is stepped by sub_824364D8(sequence, target, f1 =
// the frame's time); until the title screen it gets 20 times the time.
GAME_PATCH(824364D8) {
  if (rt::g_skip_intro && !rt::g_title_passed.load(std::memory_order_relaxed)) ctx.f1.f64 *= 20.0;
  __imp__sub_824364D8(ctx, base);
}

// A lost connection: sub_8237CE40(connection, reason) closes the game's
// connection to its server (reason 0 a normal leave, 4 kicked, other values
// an error). The reason is kept for the main menu's notice (pc_settings.cpp).
GAME_PATCH(8237CE40) {
  const uint32_t reason = ctx.r4.u32;
  RT_INFO("net: the connection to the server closed (reason %u)", reason);
  rt::g_disconnect_reason = int(reason);
  rt::g_disconnect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  __imp__sub_8237CE40(ctx, base);
}

// Measured with bots (BF_MOTION_LOG): characters near the camera are posed
// and move on nearly every frame; far ones are posed on only about one frame
// in three (the game's own animation level of detail), and this hook only runs
// when a pose is built, so it cannot fill the frames between: the smoothing
// below changed nothing measurable and is off (BF_SMOOTH_PLAYERS=1 to try it).
// SMOOTH OTHER PLAYERS: sub_826D41B8 builds a skeleton's pose (r10 its pose
// buffer, the same one each frame for a character) from its root transform in
// r9 (a 4x4 matrix, the position at +48/+52/+56). Characters whose position
// comes over the network change it on only some frames (about 70 % at 100
// fps, measured), holding still on the others and then jumping. Each such
// character is drawn between its last two distinct positions by time, at most
// one update behind; ones that move every frame (the player's own, anything
// the game steps each frame) are left alone, and so are teleports and long
// gaps. The root is changed for this call only and put back after it.
// BF_MOTION_LOG: every 10 s, for each moving character, on how many frames
// the game's position and the drawn one changed.
namespace {
struct Sample {
  double t;
  float p[3];
};
struct Motion {
  std::deque<Sample> history;  // the last distinct positions, oldest first
  double interval = 0.01;      // seconds between them (smoothed)
  float out[3] = {};
  uint64_t frame = 0;
  int held = 0;  // held frames seen lately (decays): smoothing on while above 0
  // BF_MOTION_LOG
  uint64_t frames = 0, moved_in = 0, moved_out = 0;
  double distance = 0, since = 0;
};
std::mutex g_motion_mutex;
std::map<uint32_t, Motion> g_motion;

double Seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

GAME_PATCH(826D41B8) {
  const uint32_t root = ctx.r9.u32;
  const bool character = rt::g_smooth_players && root && (root < 0x82000000 || root >= 0x83000000) && ctx.r4.u32 == 0;
  static const bool log = std::getenv("BF_MOTION_LOG") != nullptr;
  if (!character) {
    __imp__sub_826D41B8(ctx, base);
    return;
  }
  float p[3] = {GuestFloat(root + 48), GuestFloat(root + 52), GuestFloat(root + 56)};
  float draw[3] = {p[0], p[1], p[2]};
  const uint64_t frame = rt::g_frame_count.load();
  const double now = Seconds();
  {
    std::lock_guard lock(g_motion_mutex);
    Motion& m = g_motion[ctx.r10.u32];
    const bool new_frame = m.frame != frame;
    const float* last = m.history.empty() ? nullptr : m.history.back().p;
    const float step = last ? std::hypot(p[0] - last[0], p[2] - last[2]) + std::abs(p[1] - last[1]) : 0.0f;
    if (new_frame) {
      if (!last || step > 4.0f || now - m.history.back().t > 0.25) {  // a teleport or a long gap: start over
        m.history.clear();
        m.history.push_back({now, {p[0], p[1], p[2]}});
      } else if (step > 0.0005f) {
        m.interval += (std::clamp(now - m.history.back().t, 0.001, 0.25) - m.interval) * 0.1;
        m.history.push_back({now, {p[0], p[1], p[2]}});
        if (m.history.size() > 8) m.history.pop_front();
        m.held = std::max(0, m.held - 1);
      } else if (m.frames) {
        m.held = std::min(m.held + 8, 240);  // a held frame: smoothing on for a while
      }
    }
    // Drawn where it was a little more than one update ago, between the two
    // positions around that moment (never ahead of the newest).
    if (m.held > 0 && m.history.size() >= 2) {
      const double at = now - std::clamp(m.interval * 1.25, 0.004, 0.1);
      const Sample* a = &m.history.front();
      const Sample* b = a;
      for (const Sample& smp : m.history) {
        if (smp.t <= at) a = &smp;
        if (smp.t >= at) {
          b = &smp;
          break;
        }
        b = &smp;
      }
      const double t = b->t > a->t ? std::clamp((at - a->t) / (b->t - a->t), 0.0, 1.0) : 1.0;
      for (int i = 0; i < 3; ++i) draw[i] = float(a->p[i] + (b->p[i] - a->p[i]) * t);
    }
    if (new_frame) {
      if (log) {
        const float out_step = std::hypot(draw[0] - m.out[0], draw[2] - m.out[2]) + std::abs(draw[1] - m.out[1]);
        if (!m.frames) m.since = now;
        else if (step < 4.0f) m.distance += step, m.moved_in += step > 0.0005f, m.moved_out += out_step > 0.0005f;
        ++m.frames;
      }
      memcpy(m.out, draw, sizeof(draw));
      m.frame = frame;
    }
    if (log) {
      static double last = now;
      if (now - last > 10.0) {
        last = now;
        for (auto& [key, k] : g_motion) {
          double s = now - k.since;
          if (k.frames > 100 && s > 1 && k.distance / s > 0.05)
            RT_INFO("motion: character %08X: the game's position changed on %llu of %llu frames, the drawn one on %llu "
                    "(%.1f m/s)", key, (unsigned long long)k.moved_in, (unsigned long long)k.frames,
                    (unsigned long long)k.moved_out, k.distance / s);
          k.frames = k.moved_in = k.moved_out = 0, k.distance = 0;
        }
      }
    }
    if (g_motion.size() > 512) g_motion.clear();
  }
  const bool changed = draw[0] != p[0] || draw[1] != p[1] || draw[2] != p[2];
  if (changed) SetGuestFloat(root + 48, draw[0]), SetGuestFloat(root + 52, draw[1]), SetGuestFloat(root + 56, draw[2]);
  __imp__sub_826D41B8(ctx, base);
  if (changed) SetGuestFloat(root + 48, p[0]), SetGuestFloat(root + 52, p[1]), SetGuestFloat(root + 56, p[2]);
}

// CAMERA SHAKE: a shake (from a blast or a hit) is three curves, yaw, roll and
// pitch, at shaker+0x50/+0x54/+0x58, each with its value now at curve+8;
// sub_82287B40(shaker) turns them into this frame's camera rotation. For that
// call the values are scaled (g_camera_shake) and afterwards set back, so the
// curves themselves run as the game made them.
GAME_PATCH(82287B40) {
  const float scale = rt::g_camera_shake;
  const uint32_t shaker = ctx.r3.u32;
  if (scale == 1.0f || !shaker) {
    __imp__sub_82287B40(ctx, base);
    return;
  }
  uint32_t curves[3];
  float kept[3];
  for (int i = 0; i < 3; ++i) {
    curves[i] = Ld32(shaker + 0x50 + 4 * i);
    if (curves[i]) kept[i] = GuestFloat(curves[i] + 8), SetGuestFloat(curves[i] + 8, kept[i] * scale);
  }
  __imp__sub_82287B40(ctx, base);
  for (int i = 0; i < 3; ++i)
    if (curves[i]) SetGuestFloat(curves[i] + 8, kept[i]);
}

// MOTION BLUR off: the view's effects for the renderer are gathered by
// sub_8228C070(manager, camera), which takes the first-person camera's blur
// (from explosions, bullets, low health) from sub_82282A80 and the manager's
// own (+0x100, used when above 0); and sub_825A38D8 is the motion blur pass.
// Off, the blur reads 0 in that gathering and the pass doesn't run.
thread_local bool t_view_effects = false;
GAME_PATCH(8228C070) {
  const uint32_t manager = ctx.r3.u32;
  if (rt::g_motion_blur || !manager) {
    __imp__sub_8228C070(ctx, base);
    return;
  }
  const float own = GuestFloat(manager + 0x100);
  SetGuestFloat(manager + 0x100, 0.0f);
  t_view_effects = true;
  __imp__sub_8228C070(ctx, base);
  t_view_effects = false;
  SetGuestFloat(manager + 0x100, own);
}
GAME_PATCH(82282A80) {
  __imp__sub_82282A80(ctx, base);
  if (t_view_effects) ctx.f1.f64 = 0.0;
}
GAME_PATCH(825A38D8) {
  if (rt::g_motion_blur) __imp__sub_825A38D8(ctx, base);
}

// Vehicle guns and turrets aimed by the mouse. Each turning part (a tank's
// turret and gun, a mounted gun) is stepped by sub_822ADE38(helper, input)
// twice a tick: helper+4 is its physics body (rotation quaternion +0x10,
// angular velocity +0x70, turning axis +0x90), helper+0x18 its data (the input
// it reads at +0x34: 6 the right stick's x, 5 its y; +0x44 set when the input
// is used as it is, else squared; with byte +0x2C set, limits in degrees at
// +0x14 and +0x18). The stick gives the body a push, so a pushed stick turns it
// at a speed, not to an angle.
//
// Here the mouse sets where each part should point (0.066 degrees a count
// times the mouse sensitivity), and every step works out the
// stick for it: a wanted turning speed from how far off the part is (faster
// when far, easing in at the end, never beyond what a full stick gives), and
// the stick from how much faster or slower than that the part turns now. The
// game gets that stick through the right stick it reads (xam.cpp), so the
// server sees ordinary input. Planes keep the mouse's stick push.
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTopSpeed = 1.2;  // radians a second taken as a part's turn at full stick
struct AimedPart {
  double aim = 0;       // where the mouse wants it (radians, the part's own angle)
  bool mouse = false;   // the mouse has it (else the aim follows the part)
  double stick = 0;     // the last stick sent
  std::chrono::steady_clock::time_point last{};
};
std::mutex g_aim_mutex;
std::map<uint32_t, AimedPart> g_aimed;  // by helper
double g_aim_pending[2] = {};           // mouse radians not handed to a part yet (x, y)
float g_aim_stick[8] = {};
std::chrono::steady_clock::time_point g_aim_stick_at[8] = {};

double Wrap(double a) { return std::remainder(a, 2 * kPi); }
// The part's angle about its axis (signed by the axis), from its quaternion.
double PartAngle(uint32_t body) {
  double w = std::clamp<double>(GuestFloat(body + 0x1C), -1.0, 1.0);
  double along = GuestFloat(body + 0x10) * GuestFloat(body + 0x90) + GuestFloat(body + 0x14) * GuestFloat(body + 0x94) +
                 GuestFloat(body + 0x18) * GuestFloat(body + 0x98);
  return Wrap(std::copysign(2.0 * std::acos(w), along));
}
double PartSpeed(uint32_t body) {
  return GuestFloat(body + 0x70) * GuestFloat(body + 0x90) + GuestFloat(body + 0x74) * GuestFloat(body + 0x94) +
         GuestFloat(body + 0x78) * GuestFloat(body + 0x98);
}
bool InGuest(uint32_t a) { return a >= 0x10000 && a < 0xF0000000; }

bool AimStick(float* x, float* y) {
  std::lock_guard lock(g_aim_mutex);
  const auto now = std::chrono::steady_clock::now();
  const bool have_x = now - g_aim_stick_at[6] < std::chrono::milliseconds(150);
  const bool have_y = now - g_aim_stick_at[5] < std::chrono::milliseconds(150);
  if (!have_x && !have_y) return false;
  *x = have_x ? g_aim_stick[6] : 0.0f;
  *y = have_y ? g_aim_stick[5] : 0.0f;
  return true;
}
const bool g_aim_stick_installed = (rt::g_vehicle_mouse_stick = AimStick, true);
}  // namespace

GAME_PATCH(822ADE38) {
  const uint32_t helper = ctx.r3.u32;
  const uint32_t body = InGuest(helper) ? Ld32(helper + 4) : 0, data = InGuest(helper) ? Ld32(helper + 0x18) : 0;
  const uint32_t axis = InGuest(data) ? Ld32(data + 0x34) : 0;
  if (InGuest(body) && (axis == 5 || axis == 6)) {
    std::lock_guard lock(g_aim_mutex);
    const auto now = std::chrono::steady_clock::now();
    double dx = 0, dy = 0, idle = 0;
    const bool looking = rt::TakeVehicleMouse(dx, dy, idle);
    const double per_count = 0.066 * kPi / 180.0 * std::clamp<double>(rt::g_mouse_sensitivity, 0.05, 10.0);
    g_aim_pending[0] += dx * per_count;
    g_aim_pending[1] += dy * per_count * (rt::g_mouse_invert ? -1.0 : 1.0);
    AimedPart& part = g_aimed[helper];
    const double angle = PartAngle(body), speed = PartSpeed(body);
    // A part not stepped for a while (another seat, a respawn): the mouse
    // takes it again from where it points.
    if (now - part.last > std::chrono::seconds(1)) part.mouse = false, g_aim_pending[0] = g_aim_pending[1] = 0;
    part.last = now;
    if (!looking) part.mouse = false;
    if (!part.mouse) part.aim = angle;
    // Mouse right (+x) and down (+y) turn the part's angle down, as the stick does.
    double& pending = g_aim_pending[axis == 6 ? 0 : 1];
    if (looking && pending != 0) part.mouse = true, part.aim -= pending, pending = 0;
    if (part.mouse) {
      if (Ld32(data + 0x2C) >> 24) {  // a part with limits
        double lo = GuestFloat(data + 0x14) * kPi / 180.0, hi = GuestFloat(data + 0x18) * kPi / 180.0;
        if (lo < hi) part.aim = std::clamp(part.aim, lo, hi);
      }
      double off = Wrap(part.aim - angle);
      off = std::clamp(off, -0.6, 0.6), part.aim = angle + off;  // (a part held at a stop doesn't wind up)
      // The speed it should turn at: full when a quarter radian or more off,
      // easing in below; then the stick that changes its speed toward it
      // (positive stick turns the angle down, so the signs flip).
      const double wanted = kTopSpeed * std::clamp(off / 0.25, -1.0, 1.0);
      double v = -(wanted / kTopSpeed) - 0.5 * (wanted - speed) / kTopSpeed;
      if (std::fabs(off) < 0.003 && std::fabs(speed) < 0.03) v = 0;
      v = std::clamp(v, -1.0, 1.0);
      if (!(Ld32(data + 0x44) >> 24)) v = std::copysign(std::sqrt(std::fabs(v)), v);  // the game squares it
      // Too small a push leaves a part standing: at least enough to move it.
      if (std::fabs(off) > 0.006 && std::fabs(speed) < 0.02 && std::fabs(v) < 0.45) v = off > 0 ? -0.45 : 0.45;
      part.stick = v;
      g_aim_stick[axis] = float(v), g_aim_stick_at[axis] = now;
    }
    if (g_aimed.size() > 64) g_aimed.clear();
  }
  __imp__sub_822ADE38(ctx, base);
}

GAME_PATCH(82205B20) {
  uint32_t obj = ctx.r3.u32;
  __imp__sub_82205B20(ctx, base);
  // Its world position at +0x10, set right after: smoke, dust and debris.
  std::thread([obj] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    float pos[3];
    for (int i = 0; i < 3; ++i) {
      uint32_t u = Ld32(obj + 0x10 + 4 * i);
      memcpy(&pos[i], &u, 4);
    }
    if (std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) &&
        (std::abs(pos[0]) > 1.0f || std::abs(pos[2]) > 1.0f))
      xgpu::ExplosionAt(pos);
  }).detach();
  static const bool log = std::getenv("BF_EXPLOSION_LOG") != nullptr;
  if (!log) return;
  RT_INFO("explosion: new %08X", obj);
  std::thread([obj] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto vec = [](uint32_t a, float* v) {
      for (int i = 0; i < 3; ++i) {
        uint32_t u = Ld32(a + 4 * i);
        memcpy(&v[i], &u, 4);
      }
      return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) &&
             std::abs(v[0]) < 3000 && std::abs(v[2]) < 3000 && v[1] > -100 && v[1] < 800 &&
             (std::abs(v[0]) > 1 || std::abs(v[2]) > 1);
    };
    std::string out;
    float v[3];
    for (uint32_t o = 0; o < 0x200; o += 4) {
      if (vec(obj + o, v)) out += "  +" + std::to_string(o) + ":" + std::to_string(int(v[0])) + "," + std::to_string(int(v[1])) + "," + std::to_string(int(v[2]));
      uint32_t p = Ld32(obj + o);
      if (p >= 0x40000000 && p < 0x80000000 && IsCommitted(p & ~0xFFFu)) {
        for (uint32_t q = 0; q < 0x200; q += 4) {
          if (!IsCommitted((p + q) & ~0xFFFu)) break;
          if (vec(p + q, v)) out += "  +" + std::to_string(o) + "->+" + std::to_string(q) + ":" + std::to_string(int(v[0])) + "," + std::to_string(int(v[1])) + "," + std::to_string(int(v[2]));
        }
      }
    }
    RT_INFO("explosion %08X:%s", obj, out.c_str());
  }).detach();
}

// Host a LAN game (--host): the local client and server normally talk through
// in-memory sockets. Starting a game (here) with the "loopback" flag (196) instead
// of "in-memory" (199) makes the client use real UDP to 127.0.0.1.
GAME_PATCH(8238FB78) {
  if (g_lan_host) {
    St8(ctx.r3.u32 + 196, 1);
    St8(ctx.r3.u32 + 199, 0);
  }
  __imp__sub_8238FB78(ctx, base);
}

// Server message handler; message 39/16 creates the server. Field 44 picks its
// backend: 0 = local (in-memory sockets), 1 = multiplayer (real UDP, port 1003);
// byte 41 binds it to 127.0.0.1 only, cleared so LAN players can reach it.
GAME_PATCH(82413BC0) {
  bool create = ctx.r4.u32 == 39 && ctx.r5.u32 == 16;
  if (create) RT_INFO("server: create, backend %u, localhost only %u", Ld32(ctx.r6.u32 + 44), *G<uint8_t>(ctx.r6.u32 + 41));
  if (g_lan_host && create) {
    St32(ctx.r6.u32 + 44, 1);
    St8(ctx.r6.u32 + 41, 0);
  }
  bool multiplayer = create && Ld32(ctx.r6.u32 + 44) == 1;
  __imp__sub_82413BC0(ctx, base);
  // Online (--online-server): list it on Arcadia so Quick Match finds it.
  if (multiplayer) RegisterHostedGame(uint16_t(1003 + g_port_offset), g_host_map);
}

// UI callback dispatch (ActionScript fscommand): r3 = UI manager, r4 = callback
// name, r5 = its argument string (comma-separated). hostMultiplayer and
// hostMultiplayerLocal (ids 14/15, table at 82020168) take a level name, e.g.
// "Wake_island_s", and start hosting it; the local one sets the handler's "local"
// flag. No menu calls them, so --host-map fires one from the main loop (see the
// MaxFps getter) once the title is past: offline after the first numDialogs(0),
// online once the Theater login has answered (g_online_ready, net.cpp).
static uint32_t g_ui_manager;
static std::atomic<int> g_dialogs{-1};
bool rt::CustomMenus() {
  // Not without a window (a dedicated server); BF_STOCK_MENUS keeps the game's.
  static const bool stock = std::getenv("BF_STOCK_MENUS") != nullptr;
  return !stock && g_present;
}
int rt::GameDialogs() { return g_dialogs; }
const char* const rt::g_game_pc_text[] = {nullptr};
// --blood (gpu.cpp): ServerSoldierEntity and the client soldier.
const rt::SoldierLayout rt::g_blood_server = {0x82013A40u, 0x820139F0u, 0x180, 0x280, 0x120};
const rt::SoldierLayout rt::g_blood_client = {0x82007518u, 0x820074F0u, 0x5D0, 0, 0};
static int g_host_state;  // 0 = before startPressed, 1 = waiting, 2 = due, 3 = done
GAME_PATCH(8246D918) {
  const char* name = G<char>(ctx.r4.u32);
  if (g_log_ui) RT_INFO("ui: %s(%s)", name, ctx.r5.u32 ? G<char>(ctx.r5.u32) : "");
  g_ui_manager = ctx.r3.u32;
  if (!strcmp(name, "numDialogs") && ctx.r5.u32) g_dialogs = std::atoi(G<char>(ctx.r5.u32));
  if ((!g_host_map.empty() || !g_ui_call.empty()) && g_host_state < 2) {
    if (!strcmp(name, "startPressed")) g_host_state = 1;
    if (g_host_state == 1 && g_online_server.empty() && !strcmp(name, "numDialogs") && ctx.r5.u32 &&
        !strcmp(G<char>(ctx.r5.u32), "0"))
      g_host_state = 2;
  }
  __imp__sub_8246D918(ctx, base);
}

// In-level menus (Esc's Frontline Report, the spawn screen): r3 = the client's
// menu handler, r4 = show. It pushes or pops the "Menu" input context (r3+60);
// the front-end flag (g_in_menus) does not cover these, so the mouse works the
// menu way while it is set (xam.cpp).
GAME_PATCH(823CD3E8) {
  g_level_menu = (ctx.r4.u32 & 0xFF) != 0;
  if (g_log_ui) RT_INFO("ui: level menu %s", g_level_menu ? "shown" : "hidden");
  __imp__sub_823CD3E8(ctx, base);
}

// HOST GAME / JOIN GAME (lan.cpp): host g_host_map from the main loop now.
void rt::RequestHost() { g_host_state = 2; }

bool rt::g_dedicated = false;
bool rt::g_auto_start = false;
std::vector<std::string> rt::g_map_rotation;

static void HostFromMainLoop(PPCContext& ctx, uint8_t* base) {
  // Dedicated server: back in the front end after a round, host the next map.
  static bool was_in_level = false;
  if (g_dedicated && g_host_state == 3) {
    if (!g_in_menus) was_in_level = true;
    if (was_in_level && g_in_menus && g_dialogs == 0) {
      was_in_level = false;
      if (!g_map_rotation.empty()) {
        size_t i = 0;
        while (i < g_map_rotation.size() && g_map_rotation[i] != g_host_map) ++i;
        std::lock_guard lock(g_host_map_mutex);  // (the bots' director reads it)
        g_host_map = g_map_rotation[(i + 1) % g_map_rotation.size()];
      }
      RT_INFO("dedicated server: round over, hosting %s", g_host_map.c_str());
      g_host_state = 2;
    }
  }
  if (g_host_state == 1 && !g_online_server.empty() && g_online_ready) g_host_state = 2;
  if (g_host_state != 2 || !g_ui_manager) return;
  g_host_state = 3;
  // Online (--online-server): the non-local host registers the game with the
  // EA Theater server; otherwise a local/LAN game.
  std::string cb = g_online_server.empty() ? "hostMultiplayerLocal" : "hostMultiplayer", arg = g_host_map;
  if (!g_ui_call.empty()) {  // --ui-call name:args (testing)
    size_t colon = g_ui_call.find(':');
    cb = g_ui_call.substr(0, colon);
    arg = colon == std::string::npos ? "" : g_ui_call.substr(colon + 1);
  }
  uint32_t strs = g_system_heap->Alloc(512, 16);
  snprintf(G<char>(strs), 64, "%s", cb.c_str());
  snprintf(G<char>(strs + 64), 448, "%s", arg.c_str());
  RT_INFO("host: %s(%s)%s", cb.c_str(), arg.c_str(), g_lan_host ? " on the LAN" : "");
  PPCContext c = ctx;
  c.r3.u64 = g_ui_manager;
  c.r4.u64 = strs;
  c.r5.u64 = strs + 64;
  sub_8246D918(c, base);
}

// Pad axis read (vtable +28): r3 = pad, r4 = input; returns 0..1 in f1. Inputs
// 4/5 = right stick right/left, 6/7 = up/down. Pad +8 is the user index + 1.
// Mouse look is added here, unclamped (see MouseLookAxis).
GAME_PATCH(821C7FD8) {
  uint32_t input = ctx.r4.u32, pad = ctx.r3.u32;
  __imp__sub_821C7FD8(ctx, base);
  if (input < 4 || input > 7 || Ld32(pad + 8) != 1) return;
  float v = MouseLookAxis(input >= 6);
  if (input & 1) v = -v;
  if (v > 0) ctx.f1.f64 = std::max(ctx.f1.f64, double(v));
}


PPC_EXTERN_FUNC(sub_821F3248);

// --log-settings: sub_821F3248(settings registry at *82CA18B0, "Table.Member")
// returns a pointer to the setting's field (0 if unknown). Read from the main
// loop about every 2 s once the boot is past; a setting is logged when its
// first 4 bytes change, as int, float and byte (the type isn't known here).
// Only registered tables are found (Game, Render, Client...); WorldRender isn't
// one: its object (header 8203F9EC, Shadowmap Width +A8, Height +78, slices
// +84) is made per level, default 1024x1024, 3 slices, view distance 100.
// The lookup faults while the setting's table doesn't exist yet (WorldRender
// in the menus): treat that as "not found" and try again later.
static uint32_t FindSetting(PPCContext& ctx, uint8_t* base, uint32_t registry, uint32_t name) {
  PPCContext c = ctx;
  c.r3.u64 = registry;
  c.r4.u64 = name;
#ifdef _WIN32
  __try {
    sub_821F3248(c, base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
#else
  sigjmp_buf guard;
  if (sigsetjmp(guard, 1) != 0) {
    rt::g_fault_guard = nullptr;
    return 0;
  }
  rt::g_fault_guard = &guard;
  sub_821F3248(c, base);
  rt::g_fault_guard = nullptr;
#endif
  return c.r3.u32;
}

static void LogSettings(PPCContext& ctx, uint8_t* base) {
  static uint32_t calls;
  static std::vector<std::pair<std::string, uint64_t>> last;
  if (g_log_settings.empty() || ++calls < 1200 || calls % 240) return;
  if (last.empty()) {
    size_t p = 0;
    while (p <= g_log_settings.size()) {
      size_t c = g_log_settings.find(',', p);
      if (c == std::string::npos) c = g_log_settings.size();
      if (c > p) last.push_back({g_log_settings.substr(p, c - p), ~0ull});
      p = c + 1;
    }
  }
  uint32_t registry = Ld32(0x82CA18B0);
  if (!registry) return;
  static uint32_t name = g_system_heap->Alloc(128, 16);
  for (auto& [setting, prev] : last) {
    snprintf(G<char>(name), 128, "%s", setting.c_str());
    uint32_t field = FindSetting(ctx, base, registry, name);
    uint64_t v = field ? Ld32(field) : 0xFFFFFFFF00000000ull;
    if (v == prev) continue;
    prev = v;
    if (!field) { RT_INFO("setting %s: not found", setting.c_str()); continue; }
    uint32_t u = Ld32(field);
    float f;
    memcpy(&f, &u, 4);
    RT_INFO("setting %s @%08X = %d / %g / byte %u", setting.c_str(), field, int(u), f, *G<uint8_t>(field));
  }
}

// Is this level unlocked (r3 = levels manager, r4 = level name)? Looks the name
// up in a set filled from the online unlocks. Coral Sea (Air Superiority) was
// unlocked for everyone by a community goal in 2009, and the main menu only
// offers it when this says yes (its extern.hasCoralSea getter), so always yes.
GAME_PATCH(823CC050) {
  bool coral = ctx.r4.u32 && !strcmp(G<char>(ctx.r4.u32), "coral_sea");
  __imp__sub_823CC050(ctx, base);
  if (coral) ctx.r3.u64 = 1;
}

// Shadow settings (--shadowmap, --shadow-distance). The engine keeps several
// copies of the WorldRender settings (object header 8203F9EC: shadowmap width
// +A8, height +78, slices +84, view distance +AC): --args only changes the
// global one, while the level brings its own (1024, 70 m) that parts of the
// renderer still read, so shadows swim and fade. After a level starts loading,
// every copy found in guest memory gets the same values, a few times while
// the level's objects are created.
namespace rt {
uint32_t g_shadow_size = 2048;
float g_shadow_distance = 300;
std::string g_worldrender;  // --worldrender "Field=value,..."
std::string g_overgrowth;   // --overgrowth "Field=value,..."
std::string g_emitter;      // --emitter "Field=value,..."
float g_fov = 0.0f;         // --fov, the FIELD OF VIEW setting
std::atomic<bool> g_aiming{false};
float g_lod = 4.0f;         // --lod N: N times the vegetation detail (1 = the game's)
}
namespace {
// WorldRender fields (type info at 82D32xxx): offset and type (f = float,
// i = int, b = bool).
struct WorldRenderField { const char* name; uint32_t offset; char type; };
const WorldRenderField kWorldRenderFields[] = {
    {"ShadowMinScreenArea", 0x98, 'f'},          {"ShadowViewportScale", 0x8C, 'f'},
    {"ShadowmapMinFov", 0x90, 'f'},              {"ShadowmapFixedMovementEnable", 0xD8, 'b'},
    {"ShadowmapFixedDepthEnable", 0xD9, 'b'},    {"ShadowmapSizeZScale", 0x9C, 'f'},
    {"ShadowmapWidth", 0xA8, 'i'},               {"ShadowmapHeight", 0x78, 'i'},
    {"ShadowmapSliceCount", 0x84, 'i'},          {"ShadowmapSliceSchemeWeight", 0x80, 'f'},
    {"ShadowmapSliceOverlapFactor", 0xB4, 'f'},  {"ShadowmapFirstSliceScale", 0xB0, 'f'},
    {"ShadowmapViewDistance", 0xAC, 'f'},        {"ShadowmapExtrusionLength", 0xA0, 'f'},
    {"ShadowmapMinScreenArea", 0xA4, 'f'},       {"CascadedBoxShadowmapEnable", 0xDA, 'b'},
    {"CascadedBoxShadowmapClipMethodEnable", 0xDD, 'b'},
    {"ColoredShadowmapSlicesEnable", 0xCF, 'b'}, {"MinScreenArea", 0x88, 'f'},
};
// OvergrowthSettings (vegetation: palms, bushes; vtable 82040568, 0x74
// bytes, field table at 82D33780).
const WorldRenderField kOvergrowthFields[] = {
    {"GlobalLodScale", 0x10, 'f'},           {"HiLodEnable", 0x68, 'b'},
    {"AdaptiveLodEnable", 0x6D, 'b'},        {"AdaptiveLodMVertexBudget", 0x0C, 'f'},
    {"AdaptiveLodMinModifier", 0x34, 'f'},   {"AdaptiveLodMaxModifier", 0x2C, 'f'},
    {"AdaptiveLodMargin", 0x48, 'f'},        {"BillboardsEnable", 0x6F, 'b'},
    {"OnlyBillboardsEnable", 0x67, 'b'},     {"AlmostOnlyBillboardsEnable", 0x66, 'b'},
    {"MaxBillboardCount", 0x18, 'i'},        {"BillboardScale", 0x1C, 'f'},
    {"UseMaxShadowLOD", 0x64, 'b'},
};
template <size_t N>
const WorldRenderField* FindField(const WorldRenderField (&fields)[N], const std::string& name) {
  for (const auto& f : fields)
    if (name == f.name) return &f;
  return nullptr;
}
const WorldRenderField* FindWorldRenderField(const std::string& name) {
  return FindField(kWorldRenderFields, name);
}
std::string WorldRenderValue(uint32_t o, const WorldRenderField& f) {
  char buf[48];
  if (f.type == 'b') snprintf(buf, sizeof(buf), "%u", *G<uint8_t>(o + f.offset));
  else if (f.type == 'i') snprintf(buf, sizeof(buf), "%d", int(Ld32(o + f.offset)));
  else {
    uint32_t u = Ld32(o + f.offset);
    float v;
    memcpy(&v, &u, 4);
    snprintf(buf, sizeof(buf), "%g", v);
  }
  return buf;
}
void SetWorldRenderValue(uint32_t o, const WorldRenderField& f, const std::string& value) {
  if (f.type == 'b') *G<uint8_t>(o + f.offset) = (value == "1" || value == "true") ? 1 : 0;
  else if (f.type == 'i') St32(o + f.offset, uint32_t(atoi(value.c_str())));
  else {
    float v = float(atof(value.c_str()));
    uint32_t u;
    memcpy(&u, &v, 4);
    St32(o + f.offset, u);
  }
}
}  // namespace
// EmitterSystemSettings (vtable 8203E120; its fields from the reflection
// table, found through the type info at 82D5E4DC): the game's particle
// renderer. The 360 draws particles at half resolution and simply shaded.
const WorldRenderField kEmitterFields[] = {
    {"QuadEnableRendering", 0x63, 'b'},       {"QuadSimpleRenderingEnable", 0x65, 'b'},
    {"QuadEnableOverdrawRendering", 0x6A, 'b'}, {"QuadEnableSorting", 0x6F, 'b'},
    {"QuadEnableProjectionScaleCulling", 0x71, 'b'}, {"QuadEnableZOcclusion", 0x6C, 'b'},
    {"QuadHalfResEnable", 0x6E, 'b'},         {"QuadHalfResSlice1ThresholdHigh", 0x0C, 'f'},
    {"QuadHalfResSlice2ThresholdHigh", 0x1C, 'f'}, {"QuadGroupsJoinNone", 0x59, 'b'},
    {"QuadTechnique", 0x2C, 'i'},             {"QuadKeptParticlesAlphaBoost", 0x20, 'f'},
    {"QuadProjectionScaleRampEnd", 0x30, 'f'}, {"MeshDrawCountLimit", 0x44, 'i'},
};
// Every EmitterSystemSettings copy gets --emitter (logged before and after).
void ApplyEmitter(int pass) {
  const std::string& settings = rt::g_emitter;
  if (settings.empty()) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x80 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x8203E120) continue;
      uint8_t half = *G<uint8_t>(o + 0x6E), simple = *G<uint8_t>(o + 0x65);
      if (half > 1 || simple > 1) continue;
      ++found;
      if (pass == 0 || pass == 29) {
        std::string all;
        for (const auto& f : kEmitterFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
        RT_INFO("emitter %08X:%s", o, all.c_str());
      }
      for (size_t p = 0; p < settings.size();) {
        size_t comma = settings.find(',', p);
        if (comma == std::string::npos) comma = settings.size();
        std::string item = settings.substr(p, comma - p);
        p = comma + 1;
        size_t eq = item.find('=');
        if (item.empty()) continue;
        const WorldRenderField* f = eq == std::string::npos ? nullptr : FindField(kEmitterFields, item.substr(0, eq));
        if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
        else if (pass == 0) RT_WARN("emitter: unknown setting %s", item.c_str());
      }
    }
  }
  if (pass == 0 || pass == 29) RT_INFO("emitter: %d copies set", found);
}
void rt::ApplyEmitterNow() {
  if (g_in_menus) return;  // the level's settings object is made with the level
  std::thread([] { ApplyEmitter(1); }).detach();
}
// Every OvergrowthSettings copy gets --overgrowth (logged before and after).
void ApplyOvergrowth(int pass) {
  // The engine's GlobalLodScale shrinks the vegetation LOD distances: 1 at
  // Wake Island, 0.25 shows the far palms and bushes at no measured cost,
  // 0.1 costs a third of the frame rate for little more, 4 removes them.
  std::string settings;
  if (rt::g_lod > 0 && rt::g_lod != 1.0f) settings = "GlobalLodScale=" + std::to_string(1.0f / rt::g_lod) + ",";
  settings += rt::g_overgrowth;
  if (settings.empty()) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x74 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x82040568) continue;
      uint32_t u = Ld32(o + 0x10);
      float scale;
      memcpy(&scale, &u, 4);
      if (!(scale > 0.001f && scale < 1000.0f)) continue;
      ++found;
      if (pass == 0 || pass == 29) {
        std::string all;
        for (const auto& f : kOvergrowthFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
        RT_INFO("overgrowth %08X:%s", o, all.c_str());
      }
      for (size_t p = 0; p < settings.size();) {
        size_t comma = settings.find(',', p);
        if (comma == std::string::npos) comma = settings.size();
        std::string item = settings.substr(p, comma - p);
        p = comma + 1;
        size_t eq = item.find('=');
        if (item.empty()) continue;
        const WorldRenderField* f = eq == std::string::npos ? nullptr : FindField(kOvergrowthFields, item.substr(0, eq));
        if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
        else if (pass == 0) RT_WARN("overgrowth: unknown setting %s", item.c_str());
      }
    }
  }
  if (pass == 0 || pass == 29) RT_INFO("overgrowth: %d copies set", found);
}
void rt::ApplyShadowSettings() {
  std::thread([] {
    const std::string& settings = g_worldrender;
    for (int pass = 0; pass < 30 && !g_in_menus; ++pass) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      int found = 0;
      for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {  // heaps, image, physical
        if (!IsCommitted(page)) continue;
        for (uint32_t o = page; o + 0xE0 <= page + 0x1000; o += 4) {
          if (Ld32(o) != 0x8203F9EC) continue;
          uint32_t w = Ld32(o + 0xA8), slices = Ld32(o + 0x84);
          if (w < 256 || w > 4096 || slices < 1 || slices > 8) continue;
          ++found;
          if (pass == 0 || pass == 29) {
            std::string all;
            for (const auto& f : kWorldRenderFields) all += std::string(" ") + f.name + "=" + WorldRenderValue(o, f);
            RT_INFO("worldrender %08X:%s", o, all.c_str());
          }
          for (size_t p = 0; p < settings.size();) {
            size_t comma = settings.find(',', p);
            if (comma == std::string::npos) comma = settings.size();
            std::string item = settings.substr(p, comma - p);
            p = comma + 1;
            size_t eq = item.find('=');
            const WorldRenderField* f = eq == std::string::npos ? nullptr : FindWorldRenderField(item.substr(0, eq));
            if (item.empty()) continue;
            if (f) SetWorldRenderValue(o, *f, item.substr(eq + 1));
            else if (pass == 0) RT_WARN("worldrender: unknown setting %s", item.c_str());
          }
          if (g_shadow_size) St32(o + 0xA8, g_shadow_size), St32(o + 0x78, g_shadow_size);
          if (g_shadow_distance > 0) {
            uint32_t d;
            memcpy(&d, &g_shadow_distance, 4);
            St32(o + 0xAC, d);
          }
        }
      }
      if (pass == 0 || pass == 29) RT_INFO("shadows: %d WorldRender settings copies set", found);
      ApplyOvergrowth(pass);
      ApplyEmitter(pass);
    }
  }).detach();
}
