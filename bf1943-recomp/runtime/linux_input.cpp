// The Linux game's input (BF1943_CLIENT): pads, keys and the mouse through
// SDL3, behind the few Windows calls xam.cpp's PC controls make
// (posix_input.h). Fed from the game window's events on the UI thread
// (xenia_shims/window_sdl.cc); read from the game's threads.
#if !defined(_WIN32) && defined(BF1943_CLIENT)

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "kernel.h"
#include "posix_input.h"
#include "window_sdl.h"

namespace {

std::atomic<bool> g_keys[256];
std::atomic<bool> g_focus{false};
std::atomic<SDL_Window*> g_window{nullptr};
std::mutex g_mouse_mutex;
long g_dx = 0, g_dy = 0, g_wheel = 0;
std::atomic<long> g_cursor_x{0}, g_cursor_y{0}, g_width{1280}, g_height{720};
std::atomic<int> g_capture_want{0};
bool g_captured = false;  // (UI thread)
uint32_t g_wake = 0;      // an event that only wakes the UI loop (to apply the capture)
std::mutex g_pad_mutex;
SDL_Gamepad* g_pads[4] = {};

int VkFromScancode(SDL_Scancode sc) {
  if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
  if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
  if (sc == SDL_SCANCODE_0) return '0';
  if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
  if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
  switch (sc) {
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
    case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LALT: return VK_LMENU;
    case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
    case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;
    case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;
    case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
    case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
    case SDL_SCANCODE_BACKSLASH: return VK_OEM_5;
    case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    default: return 0;
  }
}

int VkFromButton(uint8_t button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return VK_LBUTTON;
    case SDL_BUTTON_RIGHT: return VK_RBUTTON;
    case SDL_BUTTON_MIDDLE: return VK_MBUTTON;
    case SDL_BUTTON_X1: return VK_XBUTTON1;
    case SDL_BUTTON_X2: return VK_XBUTTON2;
    default: return 0;
  }
}

void SetKey(int vk, bool down) {
  if (vk <= 0 || vk > 255) return;
  g_keys[vk] = down;
  // The either-side keys, as Windows reports them.
  if (vk == VK_LSHIFT || vk == VK_RSHIFT) g_keys[VK_SHIFT] = g_keys[VK_LSHIFT] || g_keys[VK_RSHIFT];
  if (vk == VK_LCONTROL || vk == VK_RCONTROL) g_keys[VK_CONTROL] = g_keys[VK_LCONTROL] || g_keys[VK_RCONTROL];
  if (vk == VK_LMENU || vk == VK_RMENU) g_keys[VK_MENU] = g_keys[VK_LMENU] || g_keys[VK_RMENU];
}

void ApplyCapture() {
  SDL_Window* w = g_window;
  bool want = g_capture_want.load() && g_focus;
  if (!w || want == g_captured) return;
  SDL_SetWindowRelativeMouseMode(w, want);
  g_captured = want;
}

// The game window's events (UI thread).
void OnEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      bool down = e.type == SDL_EVENT_KEY_DOWN;
      // Alt+Enter: fullscreen and back, as on Windows.
      if (down && !e.key.repeat && e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
        if (SDL_Window* w = SDL_GetWindowFromID(e.key.windowID))
          SDL_SetWindowFullscreen(w, !(SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN));
        break;
      }
      SetKey(VkFromScancode(e.key.scancode), down);
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      SetKey(VkFromButton(e.button.button), e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      break;
    case SDL_EVENT_MOUSE_MOTION: {
      std::lock_guard l(g_mouse_mutex);
      g_dx += long(e.motion.xrel), g_dy += long(e.motion.yrel);
      g_cursor_x = long(e.motion.x), g_cursor_y = long(e.motion.y);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      std::lock_guard l(g_mouse_mutex);
      g_wheel += long(e.wheel.y * WHEEL_DELTA);
      break;
    }
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
      SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
      if (!w) break;
      g_window = w;
      int ww = 0, wh = 0;
      SDL_GetWindowSize(w, &ww, &wh);
      g_width = ww, g_height = wh;
      if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) g_focus = true;
      if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_focus = false;
        for (auto& k : g_keys) k = false;  // (released while away)
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
      std::lock_guard l(g_pad_mutex);
      for (auto& pad : g_pads)
        if (!pad) {
          pad = SDL_OpenGamepad(e.gdevice.which);
          if (pad) RT_INFO("input: controller %s", SDL_GetGamepadName(pad));
          break;
        }
      break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      std::lock_guard l(g_pad_mutex);
      for (auto& pad : g_pads)
        if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) SDL_CloseGamepad(pad), pad = nullptr;
      break;
    }
    default:
      break;
  }
  ApplyCapture();
}

const bool g_installed = [] {
  xe::ui::SdlWindowedAppContext::SetEventHook(OnEvent);
  return true;
}();

}  // namespace

short GetAsyncKeyState(int vk) { return vk > 0 && vk < 256 && g_keys[vk] ? short(0x8000) : 0; }

bool GetCursorPos(POINT* point) {
  point->x = g_cursor_x, point->y = g_cursor_y;
  return g_window.load() != nullptr;
}

bool GetClientRect(HWND, RECT* rect) {
  *rect = {0, 0, g_width.load(), g_height.load()};
  return g_window.load() != nullptr;
}

namespace rt::linput {
HWND FocusedWindow() { return g_focus ? reinterpret_cast<HWND>(g_window.load()) : nullptr; }
void CaptureMouse(bool capture) {
  if (g_capture_want.exchange(capture) == int(capture)) return;
  if (!g_wake) g_wake = SDL_RegisterEvents(1);
  SDL_Event e{};
  e.type = g_wake;  // (the UI loop applies it with its next event)
  SDL_PushEvent(&e);
}
void TakeMouse(long& dx, long& dy, long& wheel) {
  std::lock_guard l(g_mouse_mutex);
  dx = g_dx, dy = g_dy, wheel = g_wheel;
  g_dx = g_dy = g_wheel = 0;
}
}  // namespace rt::linput

// ---- pads (XInput's view) ------------------------------------------------------
DWORD XInputGetState(DWORD user, XINPUT_STATE* state) {
  std::lock_guard l(g_pad_mutex);
  SDL_Gamepad* pad = user < 4 ? g_pads[user] : nullptr;
  if (!pad) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
  static DWORD packet = 0;
  XINPUT_GAMEPAD& g = state->Gamepad;
  g = {};
  struct Map { SDL_GamepadButton b; WORD w; };
  static const Map kButtons[] = {
      {SDL_GAMEPAD_BUTTON_SOUTH, XINPUT_GAMEPAD_A},         {SDL_GAMEPAD_BUTTON_EAST, XINPUT_GAMEPAD_B},
      {SDL_GAMEPAD_BUTTON_WEST, XINPUT_GAMEPAD_X},          {SDL_GAMEPAD_BUTTON_NORTH, XINPUT_GAMEPAD_Y},
      {SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK},       {SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER},
      {SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT}};
  for (const Map& m : kButtons)
    if (SDL_GetGamepadButton(pad, m.b)) g.wButtons |= m.w;
  auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a); };
  auto flip = [](int16_t v) { return SHORT(std::clamp(-int(v), -32768, 32767)); };  // (SDL: down is +)
  g.sThumbLX = axis(SDL_GAMEPAD_AXIS_LEFTX), g.sThumbLY = flip(axis(SDL_GAMEPAD_AXIS_LEFTY));
  g.sThumbRX = axis(SDL_GAMEPAD_AXIS_RIGHTX), g.sThumbRY = flip(axis(SDL_GAMEPAD_AXIS_RIGHTY));
  g.bLeftTrigger = BYTE(std::max(0, int(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) * 255 / 32767);
  g.bRightTrigger = BYTE(std::max(0, int(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) * 255 / 32767);
  state->dwPacketNumber = ++packet;
  return 0;
}

DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* v) {
  std::lock_guard l(g_pad_mutex);
  SDL_Gamepad* pad = user < 4 ? g_pads[user] : nullptr;
  if (!pad) return 1167;
  SDL_RumbleGamepad(pad, v->wLeftMotorSpeed, v->wRightMotorSpeed, 1000);  // (until the game changes it)
  return 0;
}

DWORD XInputGetCapabilities(DWORD user, DWORD, XINPUT_CAPABILITIES* caps) {
  std::lock_guard l(g_pad_mutex);
  if (user >= 4 || !g_pads[user]) return 1167;
  *caps = {};
  caps->Type = 1, caps->SubType = 1;  // a gamepad
  return 0;
}

#endif  // !_WIN32 && BF1943_CLIENT
