// xam.xex imports: loader, user/sign-in, input (host XInput), notifications,
// network (offline). Anything not here is a
// logged stub from the generated import_stubs.cpp.
#include "kernel.h"
#include "xenia_bridge.h"
#include "pc_settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace rt;

namespace {
uint32_t XexHeaderField(uint32_t key) {
  uint32_t hdr = g_module.xex_header;
  uint32_t count = Ld32(hdr + 0x14);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t e = hdr + 0x18 + i * 8;
    if (Ld32(e) == key) return (key & 0xFF) <= 1 ? Ld32(e + 4) : hdr + Ld32(e + 4);
  }
  return 0;
}

uint32_t TitleId() {
  uint32_t info = XexHeaderField(0x00040006);  // XEX_HEADER_EXECUTION_INFO
  return info ? Ld32(info + 0x0C) : 0;
}

// Local profile. The default name keeps the original XUID; other names (--gamertag,
// e.g. a second copy on one PC) get their own, derived from the name.
static uint64_t Xuid() {
  static const uint64_t xuid = [] {
    if (g_gamertag == "Player") return 0xE000000019430001ull;
    uint32_t h = 2166136261u;
    for (char c : g_gamertag) h = (h ^ uint8_t(c)) * 16777619u;
    return 0xE000000000000000ull | h;
  }();
  return xuid;
}
}  // namespace

// ---- loader -------------------------------------------------------------------
GUEST_IMPORT(XamLoaderGetLaunchData) { RETURN(ERROR_NOT_FOUND); }
GUEST_IMPORT(XamLoaderSetLaunchData) { RETURN(ERROR_SUCCESS); }
GUEST_IMPORT(XamLoaderTerminateTitle) {
  RT_INFO("XamLoaderTerminateTitle: exiting");
  fflush(nullptr);
  ExitProcess(0);
}
GUEST_IMPORT(XamLoaderLaunchTitle) {
  RT_INFO("XamLoaderLaunchTitle(%s): exiting", ReadCString(ARG32(3)).c_str());
  fflush(nullptr);
  ExitProcess(0);
}
GUEST_IMPORT(XamGetExecutionId) {
  static uint32_t info = [] {
    uint32_t b = g_system_heap->Alloc(0x18);
    memset(G(b), 0, 0x18);
    if (uint32_t src = XexHeaderField(0x00040006)) memcpy(G(b), G(src), 0x18);
    return b;
  }();
  St32(ARG32(3), info);
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(XNetLogonGetTitleID) { RETURN(TitleId()); }
GUEST_IMPORT(XamGetSystemVersion) { RETURN(0x20449700); }  // 2.0.17559.0 (major<<28 | minor<<24 | build<<8 | qfe)
GUEST_IMPORT(XGetGameRegion) { RETURN(0xFF); }
GUEST_IMPORT(XGetLanguage) { RETURN(1); }  // English

// ---- users ------------------------------------------------------------------------
// 1 = signed in locally, 2 = signed in to Live. Online mode (--online-server) needs 2,
// or the game's network setup shows the sign-in UI and gives up.
GUEST_IMPORT(XamUserGetSigninState) { RETURN(ARG32(3) != 0 ? 0 : g_online_server.empty() ? 1 : 2); }
GUEST_IMPORT(XamUserGetXUID) {
  if (ARG32(3) != 0) RETURN(ERROR_NOT_FOUND);
  St64(ARG32(5) ? ARG32(5) : ARG32(4), Xuid());
  RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamUserGetName) {
  if (ARG32(3) != 0) RETURN(ERROR_NOT_FOUND);
  uint32_t n = std::min<uint32_t>(ARG32(5), uint32_t(g_gamertag.size() + 1));
  memcpy(G(ARG32(4)), g_gamertag.c_str(), n);
  RETURN(ERROR_SUCCESS);
}
// XamUserGetSigninInfo(user, flags, info*)
GUEST_IMPORT(XamUserGetSigninInfo) {
  uint32_t info = ARG32(5);
  memset(G(info), 0, 0x28);
  if (ARG32(3) != 0) RETURN(ERROR_NOT_FOUND);
  St64(info, Xuid());
  St32(info + 0x0C, g_online_server.empty() ? 1 : 2);  // signin state
  memcpy(G(info + 0x18), g_gamertag.c_str(), std::min<size_t>(g_gamertag.size(), 15));
  RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamUserCheckPrivilege) {
  Out32(ARG32(5), 1);
  RETURN(ERROR_SUCCESS);
}
GUEST_IMPORT(XamUserAreUsersFriends) {
  Out32(ARG32(6), 0);
  RETURN(ERROR_SUCCESS);
}

// ---- input --------------------------------------------------------------------------
// X_INPUT_GAMEPAD: be16 buttons, u8 lt, u8 rt, be16 lx, ly, rx, ry (same bit layout as XInput).
static void WriteGamepad(uint32_t p, const XINPUT_GAMEPAD& g) {
  St16(p + 0, g.wButtons);
  St8(p + 2, g.bLeftTrigger);
  St8(p + 3, g.bRightTrigger);
  St16(p + 4, uint16_t(g.sThumbLX));
  St16(p + 6, uint16_t(g.sThumbLY));
  St16(p + 8, uint16_t(g.sThumbRX));
  St16(p + 10, uint16_t(g.sThumbRY));
}

// XamInputGetCapabilities(user, flags, caps*)
GUEST_IMPORT(XamInputGetCapabilities) {
  uint32_t user = ARG32(3) & 0xFF, caps = ARG32(5);
  XINPUT_CAPABILITIES c{};
  // User 0 always reports a pad so the game can start without one plugged in.
  if (user != 0 && user != 0xFF && XInputGetCapabilities(user, 0, &c) != ERROR_SUCCESS)
    RETURN(ERROR_DEVICE_NOT_CONNECTED);
  memset(G(caps), 0, 0x14);
  St8(caps + 0, 1);  // XINPUT_DEVTYPE_GAMEPAD
  St8(caps + 1, 1);  // XINPUT_DEVSUBTYPE_GAMEPAD
  XINPUT_GAMEPAD all{0xFFFF, 0xFF, 0xFF, -1, -1, -1, -1};
  WriteGamepad(caps + 4, all);
  St16(caps + 0x10, 0xFFFF);
  St16(caps + 0x12, 0xFFFF);
  RETURN(ERROR_SUCCESS);
}

// ---- virtual pad on user 0: keyboard/mouse plus scripted presses -------------
// PC controls (only while a window of this process has focus), mapped onto the
// game's pad actions (Help & Options > Controls):
//   Mouse = look (raw input)  LMB = fire (RT)  RMB = zoom (LT)
// Battlefield PC (Bad Company 2) layout:
//   Wheel / 1 / 2 = toggle weapon (RB)  G / MMB = grenade (LB)  F / V / 3 = knife (Y)
//   WASD = move  Shift = sprint (L3)  Ctrl / C = crouch (R3)  Space = jump (A)
//   R = reload (X)  E = enter, use, pick up (B)  Tab (hold) = score (Back)
//   Esc = menu (Start)  Enter = A  Backspace = B  arrows = D-pad (menus)
//   In menus (front end, Esc, spawn screen): wheel = up/down, LMB = select (A),
//   RMB = back (B), WASD = D-pad, free cursor; in the front end Esc = back (B).
//   Title screen ("press start"): any key or mouse button = Start.
// Hold Alt to free the mouse cursor.
namespace rt {
std::vector<ScriptedPress> g_scripted_presses;
std::atomic<uint16_t> g_auto_buttons{0};  // held by the game-side automation (bfbc1_game.cpp), OR-ed into pad 0
float g_mouse_sensitivity = 1.0f;  // --mouse-sens
bool g_mouse_invert = false;       // --invert-mouse
float g_plane_mouse_sensitivity = 1.0f;
bool g_plane_mouse_invert = false;
double g_test_mouse_at = 0;        // --test-mouse <s>: synthetic look input for tests
}

namespace {
HWND OurForegroundWindow() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  return pid == GetCurrentProcessId() ? w : nullptr;
}
// Tests: BF_TEST_KEY="<seconds>:<virtual key>[,...]" holds those keys for
// 0.2 s at those times, whatever window has focus.
bool TestKeyDown(int vk) {
  static const auto t0 = std::chrono::steady_clock::now();
  static const std::vector<std::pair<double, int>> keys = [] {
    std::vector<std::pair<double, int>> out;
    if (const char* e = std::getenv("BF_TEST_KEY")) {
      for (const char* p = e; *p;) {
        double at = std::atof(p);
        const char* colon = std::strchr(p, ':');
        if (!colon) break;
        out.emplace_back(at, std::atoi(colon + 1));
        const char* comma = std::strchr(colon, ',');
        if (!comma) break;
        p = comma + 1;
      }
    }
    return out;
  }();
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  // BF_TEST_KEY_FILE: a file a test appends virtual keys to (one per line),
  // each held 0.2 s from when it is read (checked every 50 ms).
  static const char* file = std::getenv("BF_TEST_KEY_FILE");
  if (file) {
    static std::mutex m;
    static std::vector<std::pair<double, int>> live;
    static size_t offset = 0;
    static double next_read = 0;
    std::lock_guard l(m);
    if (now >= next_read) {
      next_read = now + 0.05;
      if (FILE* f = std::fopen(file, "rb")) {
        std::fseek(f, long(offset), SEEK_SET);
        char line[64];
        while (std::fgets(line, sizeof line, f)) {
          if (!std::strchr(line, 10)) break;  // not written out yet (no newline)
          offset += std::strlen(line);
          if (int key = std::atoi(line)) live.emplace_back(now, key);
        }
        std::fclose(f);
      }
    }
    for (const auto& [at, key] : live)
      if (key == vk && now >= at && now < at + 0.2) return true;
  }
  if (keys.empty()) return false;
  for (const auto& [at, key] : keys) {
    if (key == vk && now >= at && now < at + 0.2) return true;
  }
  return false;
}
bool TestKeysActive() { return std::getenv("BF_TEST_KEY") != nullptr || std::getenv("BF_TEST_KEY_FILE") != nullptr; }
bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 || TestKeyDown(vk); }

// Raw mouse movement, collected on a hidden message-only window.
std::mutex g_mouse_mutex;
long g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_wheel = 0;

void MouseThread() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
    if (m == WM_INPUT) {
      RAWINPUT ri;
      UINT size = sizeof(ri);
      if (GetRawInputData(HRAWINPUT(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
          ri.header.dwType == RIM_TYPEMOUSE && OurForegroundWindow()) {
        std::lock_guard lock(g_mouse_mutex);
        if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
          g_mouse_dx += ri.data.mouse.lLastX;
          g_mouse_dy += ri.data.mouse.lLastY;
        }
        if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) g_mouse_wheel += SHORT(ri.data.mouse.usButtonData);
      }
    }
    return DefWindowProcW(h, m, w, l);
  };
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"bf1943-raw-mouse";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  RAWINPUTDEVICE dev{0x01, 0x02, RIDEV_INPUTSINK, hwnd};  // generic desktop / mouse
  if (!hwnd || !RegisterRawInputDevices(&dev, 1, sizeof(dev))) {
    RT_WARN("input: raw mouse input unavailable (%lu)", GetLastError());
    return;
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

// Keeps the cursor hidden and pinned to the window centre while playing.
void CaptureCursor(HWND w, bool capture) {
  static HWND captured = nullptr;
  if (capture) {
    RECT r;
    GetClientRect(w, &r);
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    ClientToScreen(w, &c);
    RECT clip{c.x, c.y, c.x + 1, c.y + 1};
    ClipCursor(&clip);
    if (captured != w) {
      SetClassLongPtrW(w, GCLP_HCURSOR, 0);
      AttachThreadInput(GetCurrentThreadId(), GetWindowThreadProcessId(w, nullptr), TRUE);
      SetCursor(nullptr);
      captured = w;
    }
  } else if (captured) {
    ClipCursor(nullptr);
    SetClassLongPtrW(captured, GCLP_HCURSOR, LONG_PTR(LoadCursorW(nullptr, IDC_ARROW)));
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    captured = nullptr;
  }
}

// Mouse speed -> right-stick units, unclamped: the pad's axis read
// (sub_821C7FD8, patched in game_patches.cpp) takes these instead of the
// deadzoned, +-1 limited stick, so fast flicks are not capped at full-stick speed.
float StickFromSpeed(double counts_per_second) {
  constexpr double kFullDeflection = 2500.0;  // counts/s for full stick at sensitivity 1
  return float(counts_per_second * g_mouse_sensitivity / kFullDeflection);
}

struct MouseLook {
  float rx = 0, ry = 0;
  int wheel_frames = 0;  // RB held while > 0 (one notch = one weapon toggle)
  int wheel_dir = 0;     // +1 = last notch was away from the user (up)
};
MouseLook SampleMouse() {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(MouseThread).detach(); });
  static std::mutex m;
  static MouseLook look;
  static auto last = std::chrono::steady_clock::now();
  static int wheel_pending = 0;
  std::lock_guard lock(m);
  auto now = std::chrono::steady_clock::now();
  double dt = std::chrono::duration<double>(now - last).count();
  if (dt < 0.004) return look;  // several game threads poll the pad per frame
  last = now;
  long dx, dy, wheel;
  {
    std::lock_guard l2(g_mouse_mutex);
    dx = g_mouse_dx, dy = g_mouse_dy, wheel = g_mouse_wheel;
    g_mouse_dx = g_mouse_dy = g_mouse_wheel = 0;
  }
  dt = std::min(dt, 0.05);
  static const auto t0 = now;
  if (g_test_mouse_at > 0) {
    // Test: 1 s right (1500 counts/s), then 0.3 s up, then one wheel notch.
    double t = std::chrono::duration<double>(now - t0).count() - g_test_mouse_at;
    if (t >= 0 && t < 1) dx += long(1500 * dt);
    if (t >= 1 && t < 1.3) dy -= long(1500 * dt);
    static bool notch = false;
    if (t >= 2.5 && !notch) wheel += WHEEL_DELTA, notch = true;
  }
  look.rx = StickFromSpeed(dx / dt);
  look.ry = StickFromSpeed((g_mouse_invert ? dy : -dy) / dt);
  if (wheel) look.wheel_dir = wheel > 0 ? 1 : -1;
  if (wheel) wheel_pending = std::min(4, wheel_pending + std::max(1, int(std::abs(wheel) / WHEEL_DELTA)));
  // Press for ~80 ms, release for ~80 ms, per notch.
  if (look.wheel_frames > 0) {
    look.wheel_frames -= int(dt * 1000);
  } else if (look.wheel_frames > -80) {
    look.wheel_frames -= int(dt * 1000);
  } else if (wheel_pending) {
    --wheel_pending;
    look.wheel_frames = 80;
  }
  return look;
}
}  // namespace

namespace rt { bool BotLookAxis(int axis, float& value); }  // bots.cpp
static std::atomic<bool> g_local_flying{false};
bool rt::SimulatedKeyDown(int vk) { return TestKeyDown(vk); }
float rt::MouseLookAxis(int axis) {
  if (float v; BotLookAxis(axis, v)) return v;
  if (Down(VK_MENU) && g_test_mouse_at <= 0) return 0;
  MouseLook look = SampleMouse();
  float v = axis == 0 ? look.rx : look.ry;
  // Planes (the right stick is pitch and roll): their own sensitivity, and
  // optionally mouse forward pulls the nose up.
  if (g_local_flying.load(std::memory_order_relaxed)) {
    if (g_mouse_sensitivity > 0) v *= g_plane_mouse_sensitivity / g_mouse_sensitivity;
    if (axis == 1 && g_plane_mouse_invert) v = -v;
  }
  return v;
}

// Set when the game reads the profile: Start was taken at the title screen.
// (g_local_flying: in a plane, from the keyboard poll; read by mouse look.)
namespace rt { std::atomic<bool> g_title_passed{false}; }  // declared in kernel.h
using rt::g_title_passed;

static void AddKeyboard(XINPUT_GAMEPAD& g) {
  HWND w = OurForegroundWindow();
  bool menus = g_in_menus || g_level_menu;
  // A click that closes a menu (spawn, resume) must not fire or zoom: mouse
  // buttons count again once released.
  static bool menus_before = menus, buttons_held = false;
  if (menus_before && !menus) buttons_held = Down(VK_LBUTTON) || Down(VK_RBUTTON);
  if (buttons_held && !Down(VK_LBUTTON) && !Down(VK_RBUTTON)) buttons_held = false;
  menus_before = menus;
  // The PC settings page takes all input while it is open (F10 opens it).
  bool page = pcset::Update(w, g_title_passed);
  bool free_cursor = Down(VK_MENU) || menus || page;
  CaptureCursor(w, w && !free_cursor && g_present);
  MouseLook look = SampleMouse();
  if (page) return;
  if (!w && g_test_mouse_at <= 0 && !TestKeysActive()) return;
  // The title screen asks for Start: any key or mouse button gives it, until
  // the game has taken it (it then reads the profile; a click to focus the
  // window too early must not end this).
  if (g_in_menus && !g_title_passed) {
    for (int vk = 1; vk < 0xFF; ++vk) {
      if (vk != VK_MENU && vk != VK_LMENU && vk != VK_RMENU && Down(vk)) {
        g.wButtons |= XINPUT_GAMEPAD_START;
        break;
      }
    }
    return;
  }
  // Esc: back in the front end; the pause menu (Start, rebindable) in a level.
  if (g_in_menus && Down(VK_ESCAPE)) g.wButtons |= XINPUT_GAMEPAD_B;
  // The game's actions on the keys bound on the PC settings page (two each:
  // the bound one and a default second), and the fixed menu keys.
  using pcset::Key;
  struct Bind { pcset::Action action; WORD button; };
  static const Bind kBinds[] = {
      {pcset::kScore, XINPUT_GAMEPAD_BACK},          {pcset::kJump, XINPUT_GAMEPAD_A},
      {pcset::kUse, XINPUT_GAMEPAD_B},               {pcset::kKnife, XINPUT_GAMEPAD_Y},
      {pcset::kReload, XINPUT_GAMEPAD_X},            {pcset::kGrenade, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {pcset::kNextWeapon, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {pcset::kSprint, XINPUT_GAMEPAD_LEFT_THUMB},   {pcset::kCrouch, XINPUT_GAMEPAD_RIGHT_THUMB}};
  for (const Bind& b : kBinds) {
    for (int slot = 0; slot < 2; ++slot) {
      int vk = Key(b.action, slot);
      // Mouse buttons in menus are select and back (below).
      if (vk && Down(vk) && !(menus && (vk == VK_LBUTTON || vk == VK_RBUTTON))) g.wButtons |= b.button;
    }
  }
  if (!g_in_menus) {
    for (int slot = 0; slot < 2; ++slot)
      if (int vk = Key(pcset::kMenu, slot); vk && Down(vk)) g.wButtons |= XINPUT_GAMEPAD_START;
  }
  struct Map { int vk; WORD button; };
  static const Map kMenuKeys[] = {
      {VK_RETURN, XINPUT_GAMEPAD_A},       {VK_BACK, XINPUT_GAMEPAD_B},
      {VK_UP, XINPUT_GAMEPAD_DPAD_UP},     {VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT}, {VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT}};
  // The vehicle the player is in (none in menus): the keys drive it.
  int vehicle = menus ? kOnFoot : LocalVehicle();
  g_local_flying = vehicle == kFlying;
  for (const Map& m : kMenuKeys) {
    bool arrow = m.vk == VK_UP || m.vk == VK_DOWN || m.vk == VK_LEFT || m.vk == VK_RIGHT;
    if (Down(m.vk) && !(arrow && vehicle == kFlying)) g.wButtons |= m.button;
  }
  if (menus) {
    // Menus (front end and over a level): wheel = up/down, left click =
    // select (A), right = back (B).
    if (look.wheel_frames > 0) g.wButtons |= look.wheel_dir > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
    // Hover: the menu only takes the pad, so once the cursor moves onto a row
    // of the highlighted list (as drawn, xgpu::MenuRows) the highlight is
    // stepped there with D-pad presses; a click waits until it is there.
    bool hover_pending = false;
    {
      static POINT last{-1, -1};
      static bool armed = false;
      static auto pulse_at = std::chrono::steady_clock::now();
      POINT cursor;
      RECT client;
      if (w && GetCursorPos(&cursor) && ScreenToClient(w, &cursor) && GetClientRect(w, &client) &&
          client.right > 0 && client.bottom > 0) {
        if (cursor.x != last.x || cursor.y != last.y) armed = true;
        last = cursor;
        float mx = float(cursor.x) / float(client.right), my = float(cursor.y) / float(client.bottom);
        float rows[32][4];
        int selected = -1;
        int count = xgpu::MenuRows(rows, 32, &selected);
        // Only a fresh Enter, Space or click opens the PC pages below (one
        // still held from the screen before must not).
        static bool accept_before = true;
        bool accept_now = Down(VK_RETURN) || Down(VK_SPACE) || Down(VK_LBUTTON);
        bool accept = accept_now && !accept_before;
        accept_before = accept_now;
        // The main menu (four rows in the middle): HOST GAME and JOIN GAME
        // open the LAN pages instead of the game's online matchmaking.
        if (g_in_menus && count == 4 && rows[0][0] > 0.3f && (selected == 0 || selected == 1) && accept &&
            (Down(VK_RETURN) || Down(VK_SPACE) ||
             (Down(VK_LBUTTON) && mx >= rows[selected][0] && mx <= rows[selected][2] &&
              my >= rows[selected][1] && my <= rows[selected][3]))) {
          g.wButtons = 0;
          if (selected == 0) pcset::OpenHost();
          else pcset::OpenJoin();
          return;
        }
        // Help & Options (five rows on the left): its last, PC SETTINGS,
        // opens the PC settings page instead of the credits.
        if (g_in_menus && count == 5 && rows[0][0] < 0.2f && selected == 4 && accept &&
            (Down(VK_RETURN) || Down(VK_SPACE) ||
             (Down(VK_LBUTTON) && mx >= rows[4][0] && mx <= rows[4][2] && my >= rows[4][1] &&
              my <= rows[4][3]))) {
          // (A was set above for Enter: the game must not see it.)
          g.wButtons = 0;
          pcset::Open();
          return;
        }
        int target = -1;
        for (int i = 0; i < count; ++i) {
          if (mx >= rows[i][0] && mx <= rows[i][2] && my >= rows[i][1] && my <= rows[i][3]) target = i;
        }
        if (armed && target >= 0 && selected >= 0 && target != selected) {
          hover_pending = true;
          // One step per press: 50 ms down, 50 ms up, then look again.
          auto now = std::chrono::steady_clock::now();
          double t = std::chrono::duration<double>(now - pulse_at).count();
          if (t > 0.1) pulse_at = now, t = 0.0;
          if (t < 0.05) g.wButtons |= target > selected ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP;
        } else if (target == selected) {
          armed = false;
        }
      }
    }
    // A click on a Back prompt (the key icon and its label) goes back.
    bool on_back = false;
    {
      POINT cursor;
      RECT client;
      if (w && GetCursorPos(&cursor) && ScreenToClient(w, &cursor) && GetClientRect(w, &client) &&
          client.right > 0 && client.bottom > 0) {
        on_back = xgpu::OnBackPrompt(float(cursor.x) / float(client.right),
                                     float(cursor.y) / float(client.bottom));
      }
    }
    if (Down(VK_LBUTTON) && on_back) g.wButtons |= XINPUT_GAMEPAD_B;
    else if (Down(VK_LBUTTON) && !hover_pending) g.wButtons |= XINPUT_GAMEPAD_A;
    if (Down(VK_RBUTTON)) g.wButtons |= XINPUT_GAMEPAD_B;
    auto any = [&](pcset::Action a) {
      return (Key(a, 0) && Down(Key(a, 0))) || (Key(a, 1) && Down(Key(a, 1)));
    };
    if (any(pcset::kForward)) g.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (any(pcset::kBack)) g.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (any(pcset::kLeft)) g.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (any(pcset::kRight)) g.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
  } else if (look.wheel_frames > 0) {
    g.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
  }
  auto held = [&](pcset::Action a) {
    return (Key(a, 0) && Down(Key(a, 0))) || (Key(a, 1) && Down(Key(a, 1)));
  };
  auto axis = [&](pcset::Action neg, pcset::Action pos) -> SHORT {
    return SHORT((held(pos) ? 32767 : 0) - (held(neg) ? 32767 : 0));
  };
  if (SHORT x = axis(pcset::kLeft, pcset::kRight)) g.sThumbLX = x;
  if (SHORT y = axis(pcset::kBack, pcset::kForward)) g.sThumbLY = y;
  // Vehicles, as the game's first (default) vehicle and aircraft layouts
  // (objects/vehicles/common/input/*.dbx): LT is the throttle, LB the brake,
  // left stick X steers (cars, boats) or is the rudder (planes), the right
  // stick aims or flies (pitch, roll). Forward/back also stay on the left
  // stick, for the layouts that drive with it.
  bool forward = held(pcset::kForward), back = held(pcset::kBack);
  if (vehicle == kDriving || vehicle == kFlying) {
    if (forward) g.bLeftTrigger = 255;
    if (back && vehicle == kDriving) g.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
  }
  if (vehicle == kFlying) {
    // Arrows: pitch (up = stick forward, nose down) and roll.
    SHORT rx = SHORT((Down(VK_RIGHT) ? 32767 : 0) - (Down(VK_LEFT) ? 32767 : 0));
    SHORT ry = SHORT((Down(VK_UP) ? 32767 : 0) - (Down(VK_DOWN) ? 32767 : 0));
    if (rx) g.sThumbRX = rx;
    if (ry) g.sThumbRY = ry;
  }
  if (!free_cursor) {
    // Mouse movement must count as pad activity (the packet number changes).
    if (look.rx || look.ry) g.sThumbRX ^= 1;
    if (held(pcset::kFire) && !buttons_held) g.bRightTrigger = 255;
    if (held(pcset::kZoom) && !buttons_held && vehicle != kFlying) g.bLeftTrigger = 255;
  }
}

// Scripted presses (--press "12:start,15:a,30:back*3"): held 150 ms or as given.
static void AddScripted(XINPUT_GAMEPAD& g) {
  static const auto t0 = std::chrono::steady_clock::now();
  double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  for (const auto& p : g_scripted_presses) {
    if (now < p.at_seconds || now >= p.at_seconds + p.hold_seconds) continue;
    g.wButtons |= p.buttons;
    SHORT v = SHORT(p.direction * 32767);
    switch (p.axis) {
      case 0: g.sThumbLX = v; break;
      case 1: g.sThumbLY = v; break;
      case 2: g.sThumbRX = v; break;
      case 3: g.sThumbRY = v; break;
      case 4: g.bLeftTrigger = 255; break;
      case 5: g.bRightTrigger = 255; break;
    }
  }
}

namespace rt { bool AddBotInput(XINPUT_GAMEPAD& g); }  // bots.cpp

// XamInputGetState(user, flags, state*)
GUEST_IMPORT(XamInputGetState) {
  uint32_t user = ARG32(3) & 0xFF, state = ARG32(5);
  if (user == 0xFF) user = 0;
  if (user > 3) RETURN(ERROR_DEVICE_NOT_CONNECTED);
  XINPUT_STATE s{};
  if (XInputGetState(user, &s) != ERROR_SUCCESS) {
    if (user != 0) RETURN(ERROR_DEVICE_NOT_CONNECTED);
    s = {};  // virtual pad on user 0
  }
  if (user == 0) {
    XINPUT_GAMEPAD before = s.Gamepad;
    AddKeyboard(s.Gamepad);
    AddScripted(s.Gamepad);
    s.Gamepad.wButtons |= g_auto_buttons.load();
    AddBotInput(s.Gamepad);
    // The PC menus are up (or working the game's pause menu): the game gets
    // none of the pad, only the menus' own presses.
    if (pcset::ControlsPad()) {
      s.Gamepad = {};
      s.Gamepad.wButtons = pcset::PuppetButtons();
    }
    // Like XInput, a new packet number only when the state changes: a held key
    // must not look like repeated presses (the scoreboard toggles on each one).
    static std::mutex m;
    static XINPUT_GAMEPAD last{};
    static DWORD packet = 0;
    std::lock_guard l(m);
    if (memcmp(&before, &s.Gamepad, sizeof(before)) != 0) {
      if (memcmp(&last, &s.Gamepad, sizeof(last)) != 0) ++packet;
      last = s.Gamepad;
      s.dwPacketNumber += packet;
    }
  }
  St32(state, s.dwPacketNumber);
  WriteGamepad(state + 4, s.Gamepad);
  RETURN(ERROR_SUCCESS);
}

GUEST_IMPORT(XamInputSetState) {
  uint32_t user = ARG32(3) & 0xFF, vib = ARG32(5);
  if (user <= 3 && vib) {
    XINPUT_VIBRATION v{Ld16(vib), Ld16(vib + 2)};
    XInputSetState(user, &v);
  }
  RETURN(ERROR_SUCCESS);
}

// ---- notifications --------------------------------------------------------------------
// Each listener is a notification event, signalled while its queue is not empty.
// Like Xenia, the first system listener gets XN_SYS_UI(0) and
// XN_SYS_SIGNINCHANGED(1) at startup; games wait for them before profile/save init.
namespace {
struct Listener {
  uint32_t handle, event;
  std::vector<std::pair<uint32_t, uint32_t>> queue;  // id, param
};
std::mutex g_notify_mutex;
std::vector<Listener> g_listeners;
bool g_notified_startup = false;
}  // namespace

// XamNotifyCreateListener(mask, max_version)
GUEST_IMPORT(XamNotifyCreateListener) {
  uint64_t mask = ARG64(3);
  uint32_t o = g_system_heap->Alloc(0x20);
  InitDispatcherHeader(o, kNotificationEvent, 0);
  uint32_t h = CreateHandle(o);
  std::lock_guard lock(g_notify_mutex);
  Listener& l = g_listeners.emplace_back(Listener{h, o, {}});
  if (!g_notified_startup && (mask & 1)) {  // XNOTIFY_SYSTEM
    g_notified_startup = true;
    l.queue = {{0x00000009, 0}, {0x0000000A, 1}};
    SetEvent(o);
  }
  RT_INFO("XamNotifyCreateListener: mask %llX -> %08X", (unsigned long long)mask, h);
  RETURN(h);
}
// XNotifyGetNext(handle, match_id, id*, param*)
GUEST_IMPORT(XNotifyGetNext) {
  uint32_t handle = ARG32(3), match = ARG32(4), id_ptr = ARG32(5), param_ptr = ARG32(6);
  if (param_ptr) St32(param_ptr, 0);
  if (!id_ptr) RETURN(0);
  St32(id_ptr, 0);
  std::lock_guard lock(g_notify_mutex);
  for (Listener& l : g_listeners) {
    if (l.handle != handle) continue;
    auto it = std::find_if(l.queue.begin(), l.queue.end(),
                           [&](auto& n) { return !match || n.first == match; });
    if (it == l.queue.end()) RETURN(0);
    St32(id_ptr, it->first);
    if (param_ptr) St32(param_ptr, it->second);
    RT_INFO("XNotifyGetNext: %08X param %u", it->first, it->second);
    l.queue.erase(it);
    if (l.queue.empty()) ResetEvent(l.event);
    RETURN(1);
  }
  RETURN(0);
}
GUEST_IMPORT(XNotifyPositionUI) { RETURN(0); }

// ---- tasks ------------------------------------------------------------------------------
// XamTaskSchedule(callback, message, optional_args*, handle*): runs callback(message)
// on a new guest thread, as Xenia does.
GUEST_IMPORT(XamTaskSchedule) {
  uint32_t id = 0;
  uint32_t h = CreateGuestThread(0x10000, 0, ARG32(3), ARG32(4), false, &id, "XamTask");
  Out32(ARG32(6), h);
  RT_INFO("XamTaskSchedule(%08X, %08X) -> thread %u", ARG32(3), ARG32(4), id);
  RETURN(h ? 0 : 0x8007000E);
}
GUEST_IMPORT(XamTaskShouldExit) { RETURN(0); }
GUEST_IMPORT(XamTaskCloseHandle) { RETURN(CloseHandle(ARG32(3)) ? 0 : 1); }

// ---- content (saves) and async XAM calls ----------------------------------------
// XOVERLAPPED: +0 result, +4 length, +8 context, +0xC event handle, +0x10 completion routine.
static uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0) {
  if (!overlapped) return result;
  St32(overlapped + 0, result);
  St32(overlapped + 4, length);
  St32(overlapped + 8, 0);
  if (uint32_t ev = Ld32(overlapped + 0xC))
    if (uint32_t o = HandleToObject(ev)) SetEvent(o);
  if (Ld32(overlapped + 0x10)) RT_WARN("XOVERLAPPED completion routine %08X not called", Ld32(overlapped + 0x10));
  return ERROR_IO_PENDING;
}

// An enumerator with nothing in it (no saves, no achievements, no friends yet).
static uint32_t NewEmptyEnumerator() {
  uint32_t o = g_system_heap->Alloc(0x20);
  memset(G(o), 0, 0x20);
  return CreateHandle(o);
}

// XamContentCreateEnumerator(user, device, type, flags, items, buffer_size*, handle*)
GUEST_IMPORT(XamContentCreateEnumerator) {
  Out32(ARG32(8), ARG32(7) * 0x134);  // XCONTENT_DATA per item
  Out32(ARG32(9), NewEmptyEnumerator());
  RETURN(ERROR_SUCCESS);
}
// XamUserCreateAchievementEnumerator(title, user, xuid, flags, offset, count, buffer_size*, handle*)
GUEST_IMPORT(XamUserCreateAchievementEnumerator) {
  Out32(ARG32(9), 0x80);
  Out32(ARG32(10), NewEmptyEnumerator());
  RETURN(ERROR_SUCCESS);
}
// XamCreateEnumeratorHandle(...) and XamGetPrivateEnumStructureFromHandle(handle, ...):
// Bad Company's online lists use these; an empty enumerator, with its structure
// returned as a zeroed block (count 0), keeps the list code from faulting.
GUEST_IMPORT(XamCreateEnumeratorHandle) { RETURN(NewEmptyEnumerator()); }
GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle) {
  uint32_t s = g_system_heap->Alloc(0x80);
  memset(G(s), 0, 0x80);
  RETURN(s);
}

// XamEnumerate(handle, flags, buffer, size, items_returned*, overlapped*)
GUEST_IMPORT(XamEnumerate) {
  Out32(ARG32(7), 0);
  RETURN(CompleteOverlapped(ARG32(8), ERROR_NO_MORE_FILES));
}
// XamContentGetDeviceState(device, overlapped*)
GUEST_IMPORT(XamContentGetDeviceState) { RETURN(CompleteOverlapped(ARG32(4), ERROR_SUCCESS)); }
// XamContentGetDeviceData(device, data*)
GUEST_IMPORT(XamContentGetDeviceData) {
  uint32_t d = ARG32(4);
  memset(G(d), 0, 0x50);
  St32(d + 0, ARG32(3));                 // device id
  St32(d + 4, 1);                        // type: hard drive
  St64(d + 8, 0x100000000ull);           // total bytes
  St64(d + 16, 0x80000000ull);           // free bytes
  const char16_t name[] = u"Hard Drive";
  for (size_t i = 0; i < sizeof(name) / 2; ++i) St16(d + 24 + uint32_t(i) * 2, uint16_t(name[i]));
  RETURN(ERROR_SUCCESS);
}
// XamContentCreateEx(user, root_name, XCONTENT_DATA*, flags, disposition*, license_mask*,
//                    cache_size, content_size, overlapped): mounts "<root>:" on
// <user root>/content/<type>/<file_name>. XCONTENT_DATA: +0 device, +4 type,
// +8 display name (wchar[128]), +0x108 file name (char[42]).
GUEST_IMPORT(XamContentCreateEx) {
  std::string root = ReadCString(ARG32(4));
  uint32_t data = ARG32(5), flags = ARG32(6), overlapped = Ld32(ctx.r1.u32 + 0x54);
  std::string file(G<char>(data + 0x108), strnlen(G<char>(data + 0x108), 42));
  if (root.empty() || file.empty()) RETURN(0x7B);  // ERROR_INVALID_NAME
  while (!root.empty() && root.back() == ':') root.pop_back();
  char type[16];
  snprintf(type, sizeof(type), "%08X", Ld32(data + 4));
  auto host = g_user_root / "content" / type / file;
  std::error_code ec;
  bool exists = std::filesystem::is_directory(host, ec);
  uint32_t result = 0, disposition = 0;
  switch (flags & 0xF) {
    case 1: result = exists ? 183 : 0; disposition = 1; break;  // CREATE_NEW (ERROR_ALREADY_EXISTS)
    case 2: if (exists) std::filesystem::remove_all(host, ec); disposition = 1; break;  // CREATE_ALWAYS
    case 3: result = exists ? 0 : 3; disposition = 2; break;  // OPEN_EXISTING (ERROR_PATH_NOT_FOUND)
    case 4: disposition = exists ? 2 : 1; break;              // OPEN_ALWAYS
    case 5:                                                   // TRUNCATE_EXISTING
      if (!exists) result = 3; else std::filesystem::remove_all(host, ec);
      disposition = 1;
      break;
    default: result = 87;
  }
  if (result == 0) {
    std::filesystem::create_directories(host, ec);
    MountContentRoot(root, host);
    Out32(ARG32(7), disposition);
    Out32(ARG32(8), 0xFFFFFFFF);
  }
  RT_INFO("XamContentCreateEx(%s:, type %s, '%s', flags %X) -> %u", root.c_str(), type, file.c_str(), flags, result);
  RETURN(CompleteOverlapped(overlapped, result));
}
// XamContentClose(root_name, overlapped)
GUEST_IMPORT(XamContentClose) {
  std::string root = ReadCString(ARG32(3));
  while (!root.empty() && root.back() == ':') root.pop_back();
  uint32_t result = UnmountContentRoot(root) ? 0 : 3;
  RETURN(CompleteOverlapped(ARG32(4), result));
}
// XamShowKeyboardUI(user, flags, default_text, title, description, buffer, length, overlapped):
// no keyboard UI, answers with the default text.
GUEST_IMPORT(XamShowKeyboardUI) {
  uint32_t def = ARG32(5), buffer = ARG32(8), length = ARG32(9);
  if (!buffer) RETURN(87);
  for (uint32_t i = 0; i < length; ++i) {
    uint16_t c = def ? Ld16(def + i * 2) : 0;
    St16(buffer + i * 2, c);
    if (!c) def = 0;
  }
  if (length) St16(buffer + (length - 1) * 2, 0);
  RETURN(CompleteOverlapped(ARG32(10), ERROR_SUCCESS));
}
// XamShowDeviceSelectorUI(user, type, flags, bytes_requested, device_id*, overlapped*)
GUEST_IMPORT(XamShowDeviceSelectorUI) {
  Out32(ARG32(7), 1);  // the hard drive
  RETURN(CompleteOverlapped(ARG32(8), ERROR_SUCCESS));
}
GUEST_IMPORT(XamContentGetLicenseMask) {
  Out32(ARG32(3), 0xFFFFFFFF);  // full game
  RETURN(CompleteOverlapped(ARG32(4), ERROR_SUCCESS));
}
// XamUserReadProfileSettings(title, user, xuid_count, xuids, setting_count, ids, size*, results, overlapped)
// No stored settings: every requested setting comes back with source 0 ("not
// set"), so the game uses its own defaults. Layout as in Xenia's xam_user.cc:
// header {count, settings*}, then 40-byte records {source, pad, xuid/user,
// id, data{type, pad, value}}, then room for binary/string payloads.
GUEST_IMPORT(XamUserReadProfileSettings) {
  g_title_passed = true;
  uint32_t user = ARG32(4), count = ARG32(7), ids = ARG32(8), size_ptr = ARG32(9),
           buffer = ARG32(10);
  uint32_t overlapped = Ld32(ctx.r1.u32 + 0x54);
  if (count < 1 || count > 32 || !size_ptr) RETURN(87);  // ERROR_INVALID_PARAMETER
  uint32_t needed = 8 + count * 40;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = Ld32(ids + i * 4), type = id >> 28;
    if (type == 4 || type == 6) needed += (id >> 16) & 0xFFF;  // WSTRING, BINARY payload
  }
  if (!buffer || Ld32(size_ptr) < needed) {
    if (!Ld32(size_ptr)) St32(size_ptr, needed);
    RETURN(122);  // ERROR_INSUFFICIENT_BUFFER
  }
  memset(G(buffer), 0, needed);
  St32(buffer, count);
  St32(buffer + 4, buffer + 8);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t rec = buffer + 8 + i * 40, id = Ld32(ids + i * 4);
    St32(rec + 8, 0xFFFFFFFF);  // xuid -1 / user index
    St32(rec + 12, user);
    St32(rec + 16, id);
    G<uint8_t>(rec + 24)[0] = uint8_t(id >> 28);
    RT_INFO("XamUserReadProfileSettings: %08X not set (defaults)", id);
  }
  RETURN(CompleteOverlapped(overlapped, ERROR_SUCCESS));
}
GUEST_IMPORT(XamShowSigninUI) { RETURN(ERROR_SUCCESS); }
// XamShowMessageBoxUIEx(user, title, text, count, buttons, active, flags, unused, result*, overlapped)
GUEST_IMPORT(XamShowMessageBoxUIEx) {
  RT_WARN("XamShowMessageBoxUIEx: answered with the first button");
  Out32(Ld32(ctx.r1.u32 + 0x54), 0);
  RETURN(CompleteOverlapped(Ld32(ctx.r1.u32 + 0x5C), ERROR_SUCCESS));
}

// ---- XMsg: messages to system apps (0xFA XMP music player, 0xFB XGI, 0xFC XLiveBase) ----
static void LogMessageOnce(const char* how, uint32_t app, uint32_t msg) {
  static std::mutex m;
  static std::vector<uint64_t> seen;
  std::lock_guard lock(m);
  uint64_t key = uint64_t(app) << 32 | msg;
  if (std::find(seen.begin(), seen.end(), key) != seen.end()) return;
  seen.push_back(key);
  RT_INFO("%s(app %02X, message %08X)", how, app, msg);
}

// Answers the XMP (music player) queries: no user soundtrack is playing, so the
// game keeps its own music (it mutes its music bus while XMP plays).
static bool HandleXmp(uint32_t msg, uint32_t buffer) {
  switch (msg) {
    case 0x00070009:  // XMPGetStatus {client, state*}
      if (uint32_t p = Ld32(buffer + 4)) St32(p, 0);  // XMP_STATE_IDLE
      return true;
    case 0x0007000B:  // XMPGetVolume {client, float*}
      if (uint32_t p = Ld32(buffer + 4)) St32(p, 0x3F800000);
      return true;
    case 0x0007001B:  // XMPGetPlaybackController {client, controller*, locked*}
      // 0 = the title controls playback, not locked. Titles set their music
      // volume only in that case and use 0 otherwise.
      if (uint32_t p = Ld32(buffer + 4)) St32(p, 0);
      if (uint32_t p = Ld32(buffer + 8)) St32(p, 0);
      return true;
  }
  return false;
}

// XMsgInProcessCall(app, message, buffer, length)
GUEST_IMPORT(XMsgInProcessCall) {
  LogMessageOnce("XMsgInProcessCall", ARG32(3), ARG32(4));
  if (ARG32(3) == 0xFA) HandleXmp(ARG32(4), ARG32(5));
  // XOnlineGetServiceInfo(service id, XONLINE_SERVICE_INFO*): the XLSP address the
  // game reaches FESL/Theater through. Info: be32 id, in_addr ip, be16 port, be16 0.
  if (ARG32(3) == 0xFC && ARG32(4) == 0x00058007) {
    uint32_t id = ARG32(5), info = ARG32(6);
    if (g_online_server.empty()) RETURN(0x80151100);  // not logged on
    unsigned a = 0, b = 0, c = 0, d = 0;
    sscanf(g_online_server.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
    memset(G(info), 0, 12);
    St32(info, id);
    St32(info + 4, a << 24 | b << 16 | c << 8 | d);
    RT_INFO("XOnlineGetServiceInfo(%08X) -> %s", id, g_online_server.c_str());
    RETURN(0);
  }
  RETURN(ERROR_SUCCESS);
}
// XMsgStartIORequest(app, message, overlapped, buffer, length)
GUEST_IMPORT(XMsgStartIORequest) {
  LogMessageOnce("XMsgStartIORequest", ARG32(3), ARG32(4));
  if (ARG32(3) == 0xFA) HandleXmp(ARG32(4), ARG32(6));
  RETURN(CompleteOverlapped(ARG32(5), ERROR_SUCCESS));
}
// XMsgStartIORequestEx(app, message, overlapped, buffer, length, task_args)
GUEST_IMPORT(XMsgStartIORequestEx) {
  LogMessageOnce("XMsgStartIORequestEx", ARG32(3), ARG32(4));
  if (ARG32(3) == 0xFA) HandleXmp(ARG32(4), ARG32(6));
  RETURN(CompleteOverlapped(ARG32(5), ERROR_SUCCESS));
}
