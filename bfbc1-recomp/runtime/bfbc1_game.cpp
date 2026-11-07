// Battlefield: Bad Company: the game-specific half of the shared runtime
// (bf1943-recomp/runtime/game_patches.cpp and bots.cpp there). No guest patches
// yet; the settings keep their defaults and bots are not supported.
#include "kernel.h"

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char* g_window_title;  // xenia_shims/bridge.cc
static const bool g_title_set = (g_window_title = "Battlefield: Bad Company", true);

namespace rt {
uint32_t g_max_fps = 120;    // --max-fps (0 = no limit), applied in VdSwap
bool g_lan_host = false;     // --host
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

void RequestHost() { RT_WARN("bfbc1: hosting is not supported yet"); }
void ApplyShadowSettings() {}

std::string StartBotDirector(int) { return {}; }
void SetBotSlot(const std::string&, int) {}
bool BotLookAxis(int, float&) { return false; }
bool AddBotInput(XINPUT_GAMEPAD&) { return false; }
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
  { if (input < 8 && Ld32(pad + 8) == 1 && ctx.f1.f64 != 0.0) { static int nz = 0; if (nz++ < 40) RT_INFO("TEMP player axis in=%u f1=%f", input, ctx.f1.f64); } }
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

// Frame timer update (bf1943's 822C7388): r3 = timer, +40 mode (0 = timed,
// sleeps until 1/Game.MaxFps has passed), +44 tick rate. Game.MaxFps is read
// straight from its setting (pointer cached at 82C24C68 on the first call), 30 by
// default. The frame time it hands on is the real elapsed time, so the cap can
// go up without the game running fast: MaxFps = --max-fps (0 = no limit).
GAME_PATCH(8229B708) {
  static uint32_t logged_modes = 0;  // bit per mode seen (two timers: 0 and 3)
  if (uint32_t mode = Ld32(ctx.r3.u32 + 40); mode < 32 && !(logged_modes & (1u << mode))) {
    logged_modes |= 1u << mode;
    RT_INFO("frame timer: mode %u, tick rate %u", mode, Ld32(ctx.r3.u32 + 44));
  }
  if (uint32_t max_fps = Ld32(0x82C24C68)) {
    float f = float(g_max_fps);
    uint32_t bits;
    memcpy(&bits, &f, 4);
    St32(max_fps, bits);
  }
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
        if (!b) continue;
        press(b, 20);
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
