// Linux game (BF1943_CLIENT, game/linux): what the Windows-only parts give the
// Windows game, until each is ported. The GPU, the window and the presenter
// are Xenia's (xenia_shims: bridge.cc, window_sdl.cc). Empty elsewhere (the
// Linux server has linux_server.cpp).
#if !defined(_WIN32) && defined(BF1943_CLIENT)

#include <cstdint>

#include "kernel.h"
#include "pc_settings.h"
#include "posix_input.h"

// (The PC menus, key icons, wording and FPS counter, pc_settings.cpp,
// pc_menu.cpp, pc_ui.cpp and fps_overlay.cpp, are built as on Windows: their
// GDI / GDI+ is posix_gdiplus.cpp.)

#endif  // !_WIN32 && BF1943_CLIENT

// ---- the Linux server: the FPS counter (fps_overlay.cpp, not built there)
// only counts frames ------------------------------------------------------------
#if !defined(_WIN32) && !defined(BF1943_CLIENT)
#include <atomic>
#include <cstdint>
namespace rt {
int g_fps_overlay = 0;
std::atomic<uint64_t> g_frame_count{0};
void FrameSwapped() { ++g_frame_count; }
}  // namespace rt
#endif  // !_WIN32 && !BF1943_CLIENT
