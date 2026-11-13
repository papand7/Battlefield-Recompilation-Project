// Linux: the XInput types xam.cpp uses. The server has no pads; the game
// (BF1943_CLIENT) gets pads, keys and the mouse through SDL3 (linux_input.cpp).
#pragma once
#ifndef _WIN32

#include <cstdint>

using WORD = uint16_t;
using BYTE = uint8_t;
using SHORT = int16_t;

struct XINPUT_GAMEPAD {
  WORD wButtons;
  BYTE bLeftTrigger, bRightTrigger;
  SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XINPUT_STATE {
  DWORD dwPacketNumber;
  XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_VIBRATION {
  WORD wLeftMotorSpeed, wRightMotorSpeed;
};
struct XINPUT_CAPABILITIES {
  BYTE Type, SubType;
  WORD Flags;
  XINPUT_GAMEPAD Gamepad;
  XINPUT_VIBRATION Vibration;
};
enum : WORD {
  XINPUT_GAMEPAD_DPAD_UP = 0x0001, XINPUT_GAMEPAD_DPAD_DOWN = 0x0002, XINPUT_GAMEPAD_DPAD_LEFT = 0x0004,
  XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008, XINPUT_GAMEPAD_START = 0x0010, XINPUT_GAMEPAD_BACK = 0x0020,
  XINPUT_GAMEPAD_LEFT_THUMB = 0x0040, XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
  XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100, XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
  XINPUT_GAMEPAD_A = 0x1000, XINPUT_GAMEPAD_B = 0x2000, XINPUT_GAMEPAD_X = 0x4000, XINPUT_GAMEPAD_Y = 0x8000,
};
#ifndef BF1943_CLIENT
inline DWORD XInputGetState(DWORD, XINPUT_STATE*) { return 1167; }  // ERROR_DEVICE_NOT_CONNECTED
inline DWORD XInputSetState(DWORD, XINPUT_VIBRATION*) { return 1167; }
inline DWORD XInputGetCapabilities(DWORD, DWORD, XINPUT_CAPABILITIES*) { return 1167; }
#else
// The Linux game (BF1943_CLIENT): pads, keys and the mouse through SDL3
// (linux_input.cpp), behind the few Windows calls xam.cpp's PC controls make.
DWORD XInputGetState(DWORD user, XINPUT_STATE* state);
DWORD XInputSetState(DWORD user, XINPUT_VIBRATION* vibration);
DWORD XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps);

struct HWND__;
using HWND = HWND__*;
struct POINT { long x, y; };
struct RECT { long left, top, right, bottom; };
constexpr int WHEEL_DELTA = 120;
// Windows' virtual key numbers (the PC settings keep keys as these).
enum : int {
  VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
  VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
  VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
  VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
  VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_NUMPAD0 = 0x60, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B,
  VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F12 = 0x7B,
  VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5,
  VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE,
  VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};
short GetAsyncKeyState(int vk);              // bit 15: held (in the game's window)
bool GetCursorPos(POINT* point);             // in the window's own pixels
inline bool ScreenToClient(HWND, POINT*) { return true; }  // (already the window's)
bool GetClientRect(HWND window, RECT* rect);  // its size in pixels
namespace rt::linput {
HWND FocusedWindow();                  // the game's window when it has the keyboard, else null
void CaptureMouse(bool capture);       // hidden, relative movement (mouse look)
void TakeMouse(long& dx, long& dy, long& wheel);  // movement since the last call (counts; wheel: 120 a notch)
}  // namespace rt::linput
#endif

#endif  // !_WIN32
