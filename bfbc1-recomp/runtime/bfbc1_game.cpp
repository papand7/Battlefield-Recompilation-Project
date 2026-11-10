// Battlefield: Bad Company: the game-specific half of the shared runtime
// (bf1943-recomp/runtime/game_patches.cpp and bots.cpp there). No guest patches
// yet; the settings keep their defaults and bots are not supported.
#include "kernel.h"

#include <windows.h>
#include <Xinput.h>
#include "bc_mp_menu.h"

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
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
void ApplyShadowSettings() {}

std::string StartBotDirector(int) { return {}; }
void SetBotSlot(const std::string&, int) {}
bool BotLookAxis(int, float&) { return false; }
// Sticks held by the test automation (BF_AUTO_MP "walk", "turn" and "look" steps).
static std::atomic<int16_t> g_auto_ly{0}, g_auto_rx{0}, g_auto_ry{0};
// The pad, every poll: our Multiplayer menu (bc_mp_menu.cpp) takes it while open.
bool AddBotInput(XINPUT_GAMEPAD& pad) {
  if (int16_t v = g_auto_ly.load()) pad.sThumbLY = v;
  if (int16_t v = g_auto_rx.load()) pad.sThumbRX = v;
  if (int16_t v = g_auto_ry.load()) pad.sThumbRY = v;
  if (bcmp::Update(pad)) pad = {};
  return false;
}
int LocalVehicle() { return kOnFoot; }
bool CustomMenus() { return false; }
bool g_dedicated = false;
std::vector<std::string> g_map_rotation;
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
