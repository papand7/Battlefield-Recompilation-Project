// PC settings page (pc_settings.cpp): every graphics option, the scene
// effects, mouse and key bindings and the multiplayer name, in the look of
// the game's own menus. Opened from Help & Options (its last entry reads
// PC SETTINGS) or with F10, drawn over the game (xgpu::SetOverlay).
#pragma once

// No <windows.h> here: kernel.h users include it after their own macros.
struct HWND__;

namespace rt::pcset {

// Actions the keyboard and mouse can be bound to.
enum Action {
  kForward, kBack, kLeft, kRight, kFire, kZoom, kJump, kCrouch, kSprint, kReload,
  kUse, kKnife, kGrenade, kNextWeapon, kScore, kMenu, kActionCount
};

// After the command line: reads <user>/pc_settings.ini and applies it.
void Load();
// The keys bound to an action (slot 0 the one set on the page, 1 a default
// second key that no other action uses; 0 if none).
int Key(Action action, int slot);
bool IsOpen();
void Open();
// The main menu's HOST GAME and JOIN GAME pages (lan.cpp).
void OpenHost();
void OpenJoin();
// Every pad poll (the game window in front, or null): F10, and the page's own
// input while it is open. True while it is open (the game gets no input).
bool Update(HWND__* window);

}  // namespace rt::pcset
