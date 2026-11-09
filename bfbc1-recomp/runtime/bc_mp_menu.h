// Our Multiplayer menu for Bad Company (bc_mp_menu.cpp), in place of the
// game's online one (EA's servers are gone): HOST GAME, SERVER BROWSER and
// JOIN BY ADDRESS over the LAN (lan.cpp), drawn in the look of the game's own
// menus over the main menu (xgpu::SetOverlay).
#pragma once

// Needs <windows.h> and <Xinput.h> first.

namespace rt::bcmp {

// MULTIPLAYER chosen on the main menu (the game's presenceRequestOnline).
void Open();
bool IsOpen();
// Every pad poll: the menu's input (and the keyboard's, the game window in
// front). True while it is open: the game then gets none of the pad.
bool Update(const XINPUT_GAMEPAD& pad);

}  // namespace rt::bcmp
