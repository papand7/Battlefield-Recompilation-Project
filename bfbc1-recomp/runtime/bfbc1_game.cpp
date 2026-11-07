// Battlefield: Bad Company: the game-specific half of the shared runtime
// (bf1943-recomp/runtime/game_patches.cpp and bots.cpp there). No guest patches
// yet; the settings keep their defaults and bots are not supported.
#include "kernel.h"

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

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

