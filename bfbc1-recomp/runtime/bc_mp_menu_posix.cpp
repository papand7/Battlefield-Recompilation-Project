// The Linux server: no Multiplayer menu (bc_mp_menu.cpp, the games' own on
// Windows and Linux). Empty elsewhere.
#if !defined(_WIN32) && !defined(BF1943_CLIENT)

#include "kernel.h"  // (Win32 names on Linux: guest.h, posix_compat.h)
#include "posix_input.h"
#include "bc_mp_menu.h"

namespace rt::bcmp {
void Open() {}
bool IsOpen() { return false; }
bool Update(const XINPUT_GAMEPAD&) { return false; }
}  // namespace rt::bcmp

#endif  // !_WIN32 && !BF1943_CLIENT
