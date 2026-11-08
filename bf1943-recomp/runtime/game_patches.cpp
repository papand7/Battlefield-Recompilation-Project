// Game patches: strong definitions of chosen weak sub_ symbols from the
// recompiled code.
#include "kernel.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
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
  if (multiplayer && g_lan_host && !g_port_offset) LaunchBots();  // --bots: join this server
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
  // Not in the windowless bot copies; BF_STOCK_MENUS keeps the game's.
  static const bool stock = std::getenv("BF_STOCK_MENUS") != nullptr;
  return !stock && g_present;
}
int rt::GameDialogs() { return g_dialogs; }
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
    }
  }).detach();
}
