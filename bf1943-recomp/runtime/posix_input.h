// Linux (the console server for now): the XInput types xam.cpp uses, and no
// pads. Keyboard, mouse and window input are Windows-only (xam.cpp fences them).
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
inline DWORD XInputGetState(DWORD, XINPUT_STATE*) { return 1167; }  // ERROR_DEVICE_NOT_CONNECTED
inline DWORD XInputSetState(DWORD, XINPUT_VIBRATION*) { return 1167; }
inline DWORD XInputGetCapabilities(DWORD, DWORD, XINPUT_CAPABILITIES*) { return 1167; }

#endif  // !_WIN32
