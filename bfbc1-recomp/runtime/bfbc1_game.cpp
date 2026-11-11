// Battlefield: Bad Company: the game-specific half of the shared runtime
// (bf1943-recomp/runtime/game_patches.cpp and bots.cpp there). No guest patches
// yet; the settings keep their defaults and bots are not supported.
#include "kernel.h"

#include <windows.h>
#include <Xinput.h>
#include "bc_mp_menu.h"
#include "xenia_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <vector>

extern "C" const char* g_window_title;  // xenia_shims/bridge.cc
static const bool g_title_set = (g_window_title = "Battlefield: Bad Company", true);

namespace rt {
uint32_t g_max_fps = 120;    // --max-fps (0 = no limit), applied in VdSwap
bool g_lan_host = false;     // --host
// Bad Company's ports are BF1943's + 500 (1500 VDP, 1503 server), so both games
// can host on one PC; BF1943's bot copies use offsets up to 150.
static const bool g_bc_ports = (g_base_port_offset = g_port_offset = 500, true);
bool g_vsync = false;        // --vsync
std::string g_host_map;      // --host-map
bool g_log_ui = false;       // --log-ui
std::string g_ui_call;       // --ui-call
std::string g_log_settings;  // --log-settings
uint32_t g_shadow_size = 0;  // the game's own
float g_shadow_distance = 0;
std::string g_worldrender;
std::string g_overgrowth;
float g_lod = 1.0f;
bool g_auto_start = false;  // set by main.cpp for dedicated/bot copies (bf1943)

std::atomic<bool> g_host_request{false};  // HostFromMainLoop starts hosting g_host_map
void RequestHost() { g_host_request = true; }

std::string StartBotDirector(int) { return {}; }
void SetBotSlot(const std::string&, int) {}
bool BotLookAxis(int, float&) { return false; }
// Sticks held by the test automation (BF_AUTO_MP "walk", "turn" and "look" steps).
static std::atomic<int16_t> g_auto_ly{0}, g_auto_rx{0}, g_auto_ry{0}, g_auto_lx{0};
static std::atomic<uint8_t> g_auto_rt{0};
// The host's autopilot (bc_bots.cpp, BF_BOT_HOST_PILOT): sticks and buttons.
std::atomic<int16_t> g_pilot_lx{0}, g_pilot_ly{0}, g_pilot_rx{0}, g_pilot_ry{0};
std::atomic<uint16_t> g_pilot_buttons{0};
std::atomic<uint8_t> g_pilot_rt{0};
// The pad, every poll: our Multiplayer menu (bc_mp_menu.cpp) takes it while open.
bool AddBotInput(XINPUT_GAMEPAD& pad) {
  if (int16_t v = g_auto_ly.load()) pad.sThumbLY = v;
  if (int16_t v = g_auto_rx.load()) pad.sThumbRX = v;
  if (int16_t v = g_auto_ry.load()) pad.sThumbRY = v;
  if (int16_t v = g_auto_lx.load()) pad.sThumbLX = v;
  if (uint8_t v = g_auto_rt.load()) pad.bRightTrigger = v;
  if (int16_t v = g_pilot_lx.load()) pad.sThumbLX = v;
  if (int16_t v = g_pilot_ly.load()) pad.sThumbLY = v;
  if (int16_t v = g_pilot_rx.load()) pad.sThumbRX = v;
  if (int16_t v = g_pilot_ry.load()) pad.sThumbRY = v;
  pad.wButtons |= g_pilot_buttons.load();
  if (uint8_t v = g_pilot_rt.load()) pad.bRightTrigger = v;
  if (bcmp::Update(pad)) pad = {};
  return false;
}
bool CustomMenus() { return false; }
bool g_dedicated = false;
std::vector<std::string> g_map_rotation;
std::string g_emitter;  // --emitter: the particle settings (ApplyEmitter below)
float g_fov = 0.0f;  // --fov, the FIELD OF VIEW setting (the camera hooks below)
std::atomic<bool> g_aiming{false};  // the infantry camera zoomed in (sights), for AIM SENSITIVITY
int GameDialogs() { return 0; }
}  // namespace rt


using namespace rt;

#define GAME_PATCH(addr)                    extern "C" PPC_FUNC(__imp__sub_##addr);   PPC_FUNC(sub_##addr)

// Pad axis read (same function as bf1943's 821C7FD8): r3 = pad, r4 = input; returns
// 0..1 in f1. Inputs 4/5 = right stick right/left, 6/7 = up/down. Pad +8 is the
// user index + 1. Mouse look is added here, unclamped (see MouseLookAxis).
GAME_PATCH(82181390) {
  uint32_t input = ctx.r4.u32, pad = ctx.r3.u32;
  __imp__sub_82181390(ctx, base);
  if (input < 4 || input > 7 || Ld32(pad + 8) != 1) return;
  float v = MouseLookAxis(input >= 6);
  if (input & 1) v = -v;
  if (v > 0) ctx.f1.f64 = std::max(ctx.f1.f64, double(v));
}

// D3D queues a flip (bf1943's 82160160): r3 = front buffer | interval << 8 | ...
// The flip waits for last flip + interval vblanks (the game asks for 2 = 30 fps
// at 60 Hz). Interval 0 flips at once; VdSwap (gpu.cpp) caps at --max-fps.
GAME_PATCH(82160148) {
  if (!g_vsync) ctx.r3.u64 &= ~uint64_t(0xF00);
  __imp__sub_82160148(ctx, base);
}

// --host-map <level> (mp_harbour_day1...) or --ui-call <callback>[:<args>]: two seconds after the main menu is up,
// call the menu's hostMultiplayer callback (hostMultiplayerLocal without
// --online-server) on the menu thread, as the Host Game screen would.
// Set by the menu callback dispatcher hook (sub_8242F698) below.
static std::atomic<uint32_t> g_menu_manager{0};
static std::atomic<DWORD> g_menu_thread{0};
static std::atomic<int64_t> g_menu_up_ms{0};
static void CallMenu(PPCContext& ctx, uint8_t* base, const std::string& cb, const std::string& arg);
// Online (--online-server) the game logs in first (presenceRequestOnline, as
// opening the Multiplayer menu does) and hosts once the Theater login answered.
// RequestHost (our Multiplayer menu, lan.cpp) starts it again later.
static std::atomic<bool> g_internal_call{false};  // CallMenu from here, not the player
static void HostFromMainLoop(PPCContext& ctx, uint8_t* base) {
  static int state = -1;  // -1 idle, 0 waiting for the menu, 1 logging in
  static bool startup = !g_host_map.empty() || !g_ui_call.empty();  // --host-map, --ui-call
  if (!g_menu_up_ms || GetCurrentThreadId() != g_menu_thread) return;
  if (startup) startup = false, state = 0;
  if (g_host_request.exchange(false)) state = 0;
  if (state < 0 || g_host_map.empty() && g_ui_call.empty()) return;
  if (int64_t(GetTickCount64()) - g_menu_up_ms < 2000) return;
  if (state == 0 && !g_online_server.empty()) {
    state = 1;
    RT_INFO("host: logging in");
    CallMenu(ctx, base, "presenceRequestOnline", "1");
    return;
  }
  if (state == 1 && !g_online_ready) return;
  state = -1;
  std::string cb = g_online_server.empty() ? "hostMultiplayerLocal" : "hostMultiplayer", arg = g_host_map;
  if (!g_ui_call.empty()) {  // --ui-call name:args (testing)
    size_t colon = g_ui_call.find(':');
    cb = g_ui_call.substr(0, colon);
    arg = colon == std::string::npos ? "" : g_ui_call.substr(colon + 1);
  }
  RT_INFO("host: %s(%s)", cb.c_str(), arg.c_str());
  CallMenu(ctx, base, cb, arg);
}

GAME_PATCH(8229B708) {
  static uint32_t logged_modes = 0;  // bit per mode seen (two timers: 0 and 3)
  if (uint32_t mode = Ld32(ctx.r3.u32 + 40); mode < 32 && !(logged_modes & (1u << mode))) {
    logged_modes |= 1u << mode;
    RT_INFO("frame timer: mode %u, tick rate %u", mode, Ld32(ctx.r3.u32 + 44));
  }
  // Frame timer update (bf1943's 822C7388): r3 = timer, +40 mode, +44 tick rate,
  // +48 ticks. Game.MaxFps is read straight from its setting (pointer cached at
  // 82C24C68 on the first call), 30 by default.
  // With a limit, the timer counts Game.MaxFps ticks per second (+48), but every
  // tick still lasts 1/rate (+44, 30), so a limit above 30 makes the game run
  // fast. With 0 (no limit) it counts +44 ticks per second of real time. So the
  // client's timers get 0 and VdSwap (gpu.cpp) caps the frame rate at --max-fps.
  // The server's timer (mode 1, only while hosting: one tick per loop, paced by
  // sleeping 1/Game.MaxFps) keeps 30, else it spins. The setting is shared and
  // read at the start of each call, so it's set per call.
  if (uint32_t max_fps = Ld32(0x82C24C68)) {
    float f = Ld32(ctx.r3.u32 + 40) == 1 ? 30.0f : 0.0f;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    St32(max_fps, bits);
  }
  HostFromMainLoop(ctx, base);
  __imp__sub_8229B708(ctx, base);
}

// Sets the frame limiter's mode (bf1943's 822BBCF8; r4: 2 = vblank-synced with
// Game.VsyncEnable, else 0/1 = timed), called from the main loop at 82343F24.
GAME_PATCH(8228CCD8) {
  if (!g_vsync) ctx.r4.u64 = 0;
  __imp__sub_8228CCD8(ctx, base);
}


// Test automation (BF_AUTO_MP=1): presses Start at the title until the profile
// is read, then waits for the main menu file. After that it watches the file
// BF_AUTO_MP_FILE (default auto_steps.txt in the run folder): each new line is one
// button (up, down, left, right, a, b, start), pressed 1.5 s apart, so the menus
// can be driven one step at a time while looking at screenshots.
#include <thread>
#include <set>
#include <string>
#include <fstream>
#include <cstdio>
static const bool g_auto_mp_started = [] {
  if (!getenv("BF_AUTO_MP")) return false;
  const char* env_file = getenv("BF_AUTO_MP_FILE");
  std::string path = env_file ? env_file : "auto_steps.txt";
  std::thread([path] {
    auto press = [](uint16_t buttons, int ms) {
      g_auto_buttons = buttons;
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      g_auto_buttons = 0;
    };
    while (!g_title_passed) {
      press(XINPUT_GAMEPAD_START, 200);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    RT_INFO("auto mp: title passed, waiting for the main menu");
    while (!g_mainmenu_open) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    RT_INFO("auto mp: main menu file open; reading %s", path.c_str());
    size_t done = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      std::ifstream in(path);
      std::vector<std::string> lines;
      for (std::string line; std::getline(in, line);) lines.push_back(line);
      while (done < lines.size()) {
        const std::string& name = lines[done++];
        uint16_t b = name == "down" ? XINPUT_GAMEPAD_DPAD_DOWN : name == "up" ? XINPUT_GAMEPAD_DPAD_UP
                   : name == "right" ? XINPUT_GAMEPAD_DPAD_RIGHT : name == "left" ? XINPUT_GAMEPAD_DPAD_LEFT
                   : name == "a" ? XINPUT_GAMEPAD_A : name == "b" ? XINPUT_GAMEPAD_B
                   : name == "start" ? XINPUT_GAMEPAD_START : 0;
        if (name == "wait") std::this_thread::sleep_for(std::chrono::seconds(3));
        if (name.rfind("hold:", 0) == 0) {  // hold:<a|b|x|y|lb|rb|ls|rs|back>[:ms] (1.5 s)
          std::string btn = name.substr(5);
          int ms = 1500;
          if (size_t c = btn.find(':'); c != std::string::npos) ms = std::atoi(btn.c_str() + c + 1), btn.resize(c);
          uint16_t bits = btn == "a" ? XINPUT_GAMEPAD_A : btn == "b" ? XINPUT_GAMEPAD_B
                        : btn == "x" ? XINPUT_GAMEPAD_X : btn == "y" ? XINPUT_GAMEPAD_Y
                        : btn == "lb" ? XINPUT_GAMEPAD_LEFT_SHOULDER : btn == "rb" ? XINPUT_GAMEPAD_RIGHT_SHOULDER
                        : btn == "ls" ? XINPUT_GAMEPAD_LEFT_THUMB : btn == "rs" ? XINPUT_GAMEPAD_RIGHT_THUMB
                        : btn == "back" ? XINPUT_GAMEPAD_BACK : 0;
          press(bits, ms);
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (name == "strafe" || name == "fire") {  // left stick right / right trigger, 1 s
          if (name == "strafe") g_auto_lx = 32000; else g_auto_rt = 255;
          std::this_thread::sleep_for(std::chrono::seconds(1));
          g_auto_lx = 0, g_auto_rt = 0;
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (name == "walklong") {  // left stick forward 8 s
          g_auto_ly = 32000;
          std::this_thread::sleep_for(std::chrono::seconds(8));
          g_auto_ly = 0;
          RT_INFO("auto mp: walklong (step %zu)", done);
          continue;
        }
        if (name == "walk" || name == "turn" || name == "look") {
          // left stick forward 2 s / right stick right or up 0.5 s
          auto& stick = name == "walk" ? g_auto_ly : name == "turn" ? g_auto_rx : g_auto_ry;
          stick = 32000;
          std::this_thread::sleep_for(std::chrono::milliseconds(name == "walk" ? 2000 : 500));
          stick = 0;
          RT_INFO("auto mp: %s (step %zu)", name.c_str(), done);
          continue;
        }
        if (!b) continue;
        press(b, 150);  // long enough for the menu to see it at ~40 fps (two instances)
        RT_INFO("auto mp: pressed %s (step %zu)", name.c_str(), done);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
      }
    }
  }).detach();
  return true;
}();

// Replay (BF_REPLAY=<file>): plays back a recording made with BF_RECORD. "cp title" and
// "cp menu" lines wait until the game reaches that point; the press lines after a
// checkpoint are timed from it (see RecordPad in xam.cpp).
static const bool g_replay_started = [] {
  const char* path = getenv("BF_REPLAY");
  if (!path) return false;
  std::string file = path;
  std::thread([file] {
    int64_t zero = 0;
    RT_INFO("replay: thread started, reading %s", file.c_str());
    while ((zero = g_pad_t0_ns.load()) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    RT_INFO("replay: first pad read seen, starting");
    std::ifstream in(file);
    int64_t base = zero;
    for (std::string line; std::getline(in, line);) {
      if (line.rfind("cp ", 0) == 0) {
        // The menu checkpoint is set during boot in some runs, so only the title waits.
        if (line == "cp title") {
          RT_INFO("replay: waiting for the title");
          // Start is pressed again every 2 s, so a press that lands during the intro is not lost.
          while (!g_title_passed.load()) {
            g_auto_buttons = XINPUT_GAMEPAD_START;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            g_auto_buttons = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1800));
          }
        }
        base = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
        continue;
      }
      double ms;
      unsigned buttons;
      if (std::sscanf(line.c_str(), "%lf %u", &ms, &buttons) != 2) continue;
      std::this_thread::sleep_until(std::chrono::steady_clock::time_point(
          std::chrono::nanoseconds(base + static_cast<int64_t>(ms * 1e6))));
      g_auto_buttons = static_cast<uint16_t>(buttons);
    }
    g_auto_buttons = 0;
    RT_INFO("replay: done");
  }).detach();
  return true;
}();

// Server message handler (same code as bf1943's 82413BC0: r4 = message, r5 = sub id,
// r6 = server object; 39/16 creates the server, +44 backend 1 = multiplayer UDP).
// A multiplayer server is listed on Arcadia (net.cpp RegisterHostedGame).
GAME_PATCH(823D1568) {
  bool create = ctx.r4.u32 == 39 && ctx.r5.u32 == 16;
  uint32_t server = ctx.r6.u32;
  // --host / --join (lan.cpp): a multiplayer server (+44 = 1, real UDP) that
  // isn't bound to 127.0.0.1 only (+41), as bf1943's 82413BC0 patch does.
  if (create) RT_INFO("server: create, backend %u, localhost only %u", Ld32(server + 44), *G<uint8_t>(server + 41));
  if (create && g_lan_host) {
    St32(server + 44, 1);
    St8(server + 41, 0);
  }
  __imp__sub_823D1568(ctx, base);
  if (g_join_address) return;  // a joiner's own server isn't listed
  // Listed with the VDP port (1000): a joiner first sends its session handshake
  // there (to the listed port, from its own VDP socket), not to the server's 1003.
  if (create && Ld32(server + 44) == 1)
    RegisterHostedGame(uint16_t(1000 + g_port_offset), g_host_map.empty() ? std::string("bfbc1") : g_host_map);
}

// Server setup (r3 = server): copies Server.LoadingTimeout to +556 and
// Server.IngameTimeout to +560 (seconds). With --host / --join a player who
// hasn't pressed Ready on the loading screen, or idles, isn't sent back to the
// menu (which also closes a host's server), so both are made very long.
GAME_PATCH(8236DED8) {
  uint32_t server = ctx.r3.u32;
  __imp__sub_8236DED8(ctx, base);
  if (!g_lan_host) return;
  float loading, ingame, never = 1e7f;
  uint32_t v = Ld32(server + 556), w = Ld32(server + 560), n;
  memcpy(&loading, &v, 4), memcpy(&ingame, &w, 4), memcpy(&n, &never, 4);
  RT_INFO("server: loading timeout %.0f s, ingame timeout %.0f s -> off", loading, ingame);
  St32(server + 556, n);
  St32(server + 560, n);
}

// Menu callback dispatcher: r3 = menu manager, r4 = callback name, r5 = its
// arguments as one string ("Movies/UIMAIntro", "0"; several are comma separated).
// startBackgroundMovie is called once the main menu is up.

GAME_PATCH(8242F698) {
  g_menu_manager = ctx.r3.u32;
  g_menu_thread = GetCurrentThreadId();
  if (!strcmp(G<char>(ctx.r4.u32), "startBackgroundMovie")) {
    RT_INFO("menu: main menu up");
    if (!g_menu_up_ms) g_menu_up_ms = GetTickCount64();
  }
  // MULTIPLAYER on the main menu logs in to EA's servers (gone) and opens the
  // game's online menu: ours (bc_mp_menu.cpp, LAN host/join) opens instead.
  // The game also asks by itself some minutes after a match, which only opens
  // ours when the player has just pressed A (or Enter, or clicked).
  if (!g_internal_call && g_online_server.empty() && !strcmp(G<char>(ctx.r4.u32), "presenceRequestOnline")) {
    uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now - g_last_accept_ms.load() < 3000) bcmp::Open();
    else RT_INFO("mp menu: not opened (the game asked, no A press)");
    return;
  }
  __imp__sub_8242F698(ctx, base);
}

static void CallMenu(PPCContext& ctx, uint8_t* base, const std::string& cb, const std::string& arg) {
  static uint32_t strs = g_system_heap->Alloc(512, 16);
  snprintf(G<char>(strs), 64, "%s", cb.c_str());
  snprintf(G<char>(strs + 64), 448, "%s", arg.c_str());
  PPCContext c = ctx;
  c.r3.u64 = g_menu_manager;
  c.r4.u64 = strs;
  c.r5.u64 = strs + 64;
  g_internal_call = true;
  sub_8242F698(c, base);
  g_internal_call = false;
}

// The camera, as bf1943's (game_patches.cpp; the same code and offsets here).
// FIELD OF VIEW (g_fov, horizontal degrees at 16:9; the game's own 85.6): the
// camera manager's frame sub_8226DB00(manager) calls the view's SetFov
// (sub_827F2A20, f1 = vertical radians) first for the camera (0x8226DD50),
// then maybe an override (0x8226DD98); sub_8226D8B8 hands the angle to another
// view (sub_82482970). Each is widened by one ratio of half-angle tangents, so
// a sight's zoom narrows as much as before. The manager's FOV controller
// (+0xE0: +0x28 the camera's own angle, +0x30 the angle now) tells aiming.
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
thread_local uint32_t t_camera_manager = 0;
thread_local int t_camera_setfov = 0;
thread_local bool t_second_view = false;
thread_local bool t_view_effects = false;
}  // namespace
GAME_PATCH(8226DB00) {
  const uint32_t manager = ctx.r3.u32, before = t_camera_manager;
  const int calls = t_camera_setfov;
  t_camera_manager = manager, t_camera_setfov = 0;
  __imp__sub_8226DB00(ctx, base);
  t_camera_manager = before, t_camera_setfov = calls;
}
GAME_PATCH(8226D8B8) {
  const bool before = t_second_view;
  t_second_view = true;
  __imp__sub_8226D8B8(ctx, base);
  t_second_view = before;
}
GAME_PATCH(827F2A20) {
  if (t_camera_manager && t_camera_setfov++ == 0) {
    const uint32_t controller = Ld32(t_camera_manager + 0xE0);
    if (controller) {
      float now = GuestFloat(controller + 0x30), own = GuestFloat(controller + 0x28);
      rt::g_aiming = own > 1.0f && now < own - 0.5f;
    }
    ctx.f1.f64 = WiderFov(ctx.f1.f64);
  }
  __imp__sub_827F2A20(ctx, base);
}
GAME_PATCH(82482970) {
  if (t_second_view) ctx.f1.f64 = WiderFov(ctx.f1.f64);
  __imp__sub_82482970(ctx, base);
}

// CAMERA SHAKE: three curves (yaw, roll, pitch) at shaker+0x50/+0x54/+0x58,
// each with its value now at curve+8, turned into this frame's rotation by
// sub_82232DF8(shaker); scaled for that call (g_camera_shake), then set back.
GAME_PATCH(82232DF8) {
  const float scale = rt::g_camera_shake;
  const uint32_t shaker = ctx.r3.u32;
  if (scale == 1.0f || !shaker) {
    __imp__sub_82232DF8(ctx, base);
    return;
  }
  uint32_t curves[3];
  float kept[3];
  for (int i = 0; i < 3; ++i) {
    curves[i] = Ld32(shaker + 0x50 + 4 * i);
    if (curves[i]) kept[i] = GuestFloat(curves[i] + 8), SetGuestFloat(curves[i] + 8, kept[i] * scale);
  }
  __imp__sub_82232DF8(ctx, base);
  for (int i = 0; i < 3; ++i)
    if (curves[i]) SetGuestFloat(curves[i] + 8, kept[i]);
}

// MOTION BLUR off: the view's effects are gathered by sub_82269688(manager,
// camera), with the first-person camera's blur from sub_82263350 and the
// manager's own (+0x100, used above 0); sub_82812AB8 is the blur pass (the
// renderer's vtable slot 31). Off, the blur reads 0 there and the pass is skipped.
GAME_PATCH(82269688) {
  const uint32_t manager = ctx.r3.u32;
  if (rt::g_motion_blur || !manager) {
    __imp__sub_82269688(ctx, base);
    return;
  }
  const float own = GuestFloat(manager + 0x100);
  SetGuestFloat(manager + 0x100, 0.0f);
  t_view_effects = true;
  __imp__sub_82269688(ctx, base);
  t_view_effects = false;
  SetGuestFloat(manager + 0x100, own);
}
GAME_PATCH(82263350) {
  __imp__sub_82263350(ctx, base);
  if (t_view_effects) ctx.f1.f64 = 0.0;
}
GAME_PATCH(82812AB8) {
  if (rt::g_motion_blur) __imp__sub_82812AB8(ctx, base);
}

// HUD and menu animations above 30 fps (bf1943's 82954388): the UI's update
// (sub_826C54F8) steps its Flash player one movie frame a call,
// sub_8278ED20(player, 33, 1), once a game frame, and the movies are made for
// 30 frames a second: the player is stepped as many times as a 30 Hz clock has
// ticked since the last step (none on most frames, two at most after a hitch).
GAME_PATCH(8278ED20) {
  if (ctx.r4.u32 != 33 || rt::g_max_fps == 30) {
    __imp__sub_8278ED20(ctx, base);
    return;
  }
  static const auto start = std::chrono::steady_clock::now();
  static int64_t stepped = -1;  // movie frames played (on the 30 Hz clock)
  const int64_t due = int64_t(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 30.0);
  if (stepped < 0 || due - stepped > 2) stepped = due - (stepped < 0 ? 1 : 2);
  const PPCContext call = ctx;
  for (; stepped < due; ++stepped) {
    ctx = call;
    __imp__sub_8278ED20(ctx, base);
  }
}

// Physics effects (debris, falling trees; bf1943's 8253EDF8): sub_824BBB10(world,
// f1, f2) gets the tick's time in f1 and the frame's in f2, which only a second,
// smoothed physics system inside uses; with ticks and frames apart it ran at the
// wrong speed, so it gets the tick's.
GAME_PATCH(824BBB10) {
  if (ctx.f1.f64 > 0.0) ctx.f2.f64 = ctx.f1.f64;
  __imp__sub_824BBB10(ctx, base);
}

// CRT startup step (bf1943's 827E44F8) run just before the entry point checks
// the "parse the command line" flag at 82E5E760, which retail never sets: set
// when --args gave the game a command line (ExLoadedCommandLine).
GAME_PATCH(8292C5B8) {
  __imp__sub_8292C5B8(ctx, base);
  if (!g_guest_args.empty()) St32(0x82E5E760, 1);
}

// The game's own music and voice volumes (PC settings; bf1943's AudioTick):
// the sound system (the object at *82C7C034, vtable 8204BA68) keeps them at
// +740 and +744 with a changed flag at +749 (its setters 8267A1B0 / 8267A1C0,
// vtable slots 30 and 31, called by name, MusicVolume and DialogueVolume, from
// 822538E0). 100 % is the game's own level (read when the object is first
// seen); set again every few seconds (a level may reset them).
namespace {
void AudioTick(PPCContext&) {
  static uint32_t system_seen = 0;
  static float game_music = 1.0f, game_voice = 1.0f, music = -1.0f, voice = -1.0f;
  static int ticks = 0;
  uint32_t system = Ld32(0x82C7C034);
  if (!system) return;
  if (system != system_seen) {
    system_seen = system;
    float m = GuestFloat(system + 740), v = GuestFloat(system + 744);
    game_music = m >= 0.0f && m <= 4.0f ? m : 1.0f;
    game_voice = v >= 0.0f && v <= 4.0f ? v : 1.0f;
    RT_INFO("sound: the game's music %.2f, voice %.2f (sound system %08X)", game_music, game_voice, system);
    ticks = 500;
  }
  if (++ticks < 500 && music == rt::g_music_volume && voice == rt::g_voice_volume) return;  // ~2.7 s
  ticks = 0;
  music = rt::g_music_volume, voice = rt::g_voice_volume;
  SetGuestFloat(system + 740, game_music * music);
  SetGuestFloat(system + 744, game_voice * voice);
  *G<uint8_t>(system + 749) = 1;
}
struct AudioTickInstall {
  AudioTickInstall() { rt::g_audio_tick = AudioTick; }
} g_audio_tick_install;
}  // namespace

// A lost connection (bf1943's 8237CE40): sub_823259F0(connection, reason), the
// network client's vtable slot 30, closes the game's connection to its server.
// The reason is kept for the main menu's notice (pc_settings.cpp).
GAME_PATCH(823259F0) {
  const uint32_t reason = ctx.r4.u32;
  RT_INFO("net: the connection to the server closed (reason %u)", reason);
  rt::g_disconnect_reason = int(reason);
  rt::g_disconnect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  __imp__sub_823259F0(ctx, base);
}

// In-level menus (the pause menu, the spawn screen; bf1943's 823CD3E8): r3 =
// the client's menu handler, r4 = show; it pushes or pops the "Menu" input
// context (r3+60). The mouse works the menu way while it is set (xam.cpp).
GAME_PATCH(82381890) {
  g_level_menu = (ctx.r4.u32 & 0xFF) != 0;
  if (g_log_ui) RT_INFO("ui: level menu %s", g_level_menu ? "shown" : "hidden");
  __imp__sub_82381890(ctx, base);
}

// Shadow, WorldRender, vegetation and particle settings (bf1943's
// game_patches.cpp, the same fields at the same offsets here): after a level
// starts loading, every copy of the WorldRender settings (vtable 820632A0:
// shadowmap width +A8, height +78, slices +84, view distance +AC) found in
// guest memory gets --shadowmap, --shadow-distance and --worldrender, a few
// times while the level's objects are created; OvergrowthSettings --lod and
// --overgrowth; EmitterSystemSettings --emitter.
namespace {
// WorldRender fields (as bf1943's: the same names and offsets): offset and type (f = float,
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
// OvergrowthSettings (vegetation; vtable 8204DB4C, field table at 82E82538).
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
// EmitterSystemSettings (vtable 82042014; its fields from the reflection
// table, found through the type info at 82EA7BB8): the game's particle
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
      if (Ld32(o) != 0x82042014) continue;
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
  // bf1943's Wake Island, 0.25 shows the far palms and bushes at no measured cost,
  // 0.1 costs a third of the frame rate for little more, 4 removes them.
  std::string settings;
  if (rt::g_lod > 0 && rt::g_lod != 1.0f) settings = "GlobalLodScale=" + std::to_string(1.0f / rt::g_lod) + ",";
  settings += rt::g_overgrowth;
  if (settings.empty()) return;
  int found = 0;
  for (uint32_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (!IsCommitted(page)) continue;
    for (uint32_t o = page; o + 0x74 <= page + 0x1000; o += 4) {
      if (Ld32(o) != 0x8204DB4C) continue;
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
          if (Ld32(o) != 0x820632A0) continue;
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

// The local player's vehicle (bf1943's WatchVehicle, bots.cpp), for the
// vehicle keys (xam.cpp: W/S throttle and brake, A/D steer; arrows fly). Ours
// is the client soldier (vtable 82008D48, its feet at +370) nearest the
// camera's eye; its player is at +1D0, the entity it controls at player +114
// (a ClientVehicleEntity, vtable 820082E8, when in one) and its seat at player
// +118 (0 the driver's). The vehicle's data +14, its name at data +C: AH60
// flies; Howitzer and KORD are guns; a passenger's seat is a gunner's too.
// Read with ReadProcessMemory: a level can be unloaded under this thread.
namespace {
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
int VehicleClass(const std::string& kind, uint32_t seat) {
  if (kind.find("AH60") != std::string::npos) return seat == 0 ? rt::kFlying : rt::kGunner;
  if (kind.rfind("KORD", 0) == 0 || kind.rfind("Howitzer", 0) == 0 || seat != 0) return rt::kGunner;
  return rt::kDriving;
}
void WatchVehicle() {
  constexpr uint32_t kClientSoldier = 0x82008D48, kClientVehicle = 0x820082E8;
  std::vector<uint32_t> soldiers;
  auto last_scan = std::chrono::steady_clock::time_point{};
  std::string last_kind;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto now = std::chrono::steady_clock::now();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    float eye[3];
    if (now_ms - g_vehicle_asked.load() > 1000 || !xgpu::CameraEye(eye)) {
      g_local_vehicle = rt::kOnFoot;
      soldiers.clear();
      continue;
    }
    bool stale = false;
    for (uint32_t c : soldiers) {
      uint32_t vt = 0;
      if (!SafeLd32(c, vt) || vt != kClientSoldier) stale = true;
    }
    auto since = now - last_scan;
    if ((stale && since > std::chrono::seconds(1)) || since > std::chrono::seconds(3) ||
        (soldiers.empty() && since > std::chrono::seconds(1))) {
      soldiers = SafeScan(kClientSoldier);
      last_scan = now;
    }
    uint32_t mine = 0;
    float best = 60.0f;
    for (uint32_t c : soldiers) {
      float x, y, z;
      if (!SafeLdF(c + 0x370, x) || !SafeLdF(c + 0x374, y) || !SafeLdF(c + 0x378, z)) continue;
      float d = std::sqrt((x - eye[0]) * (x - eye[0]) + (y - eye[1]) * (y - eye[1]) + (z - eye[2]) * (z - eye[2]));
      if (d < best) best = d, mine = c;
    }
    int kind_class = rt::kOnFoot;
    std::string kind;
    uint32_t player = 0, v = 0, vt = 0, seat = 0, data = 0, name = 0;
    if (mine && SafeLd32(mine + 0x1D0, player) && player && SafeLd32(player + 0x114, v) && v && SafeLd32(v, vt) &&
        vt == kClientVehicle) {
      SafeLd32(player + 0x118, seat);
      if (SafeLd32(v + 0x14, data) && data && SafeLd32(data + 0xC, name) && name) kind = SafeString(name);
      kind_class = VehicleClass(kind, seat);
    }
    std::string label = kind.empty() ? "" : kind + " seat " + std::to_string(seat);
    if (label != last_kind) {
      RT_INFO("vehicle keys: %s", label.empty() ? "on foot" : label.c_str());
      last_kind = label;
    }
    // Steady for 0.45 s before it counts (a frame's camera can be elsewhere).
    static int pending = -1, seen = 0;
    if (kind_class != pending) pending = kind_class, seen = 0;
    if (++seen >= 3) g_local_vehicle = kind_class;
  }
}
}  // namespace
int rt::LocalVehicle() {
  static const bool off = getenv("BF_NO_VEHICLE_KEYS") != nullptr;
  if (off) return kOnFoot;
  static std::once_flag started;
  std::call_once(started, [] { std::thread(WatchVehicle).detach(); });
  g_vehicle_asked = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  return g_local_vehicle.load(std::memory_order_relaxed);
}
