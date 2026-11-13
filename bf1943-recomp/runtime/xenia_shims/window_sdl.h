// Linux: Xenia's window and UI-thread loop on SDL3 (the vendored Xenia has
// only the Win32 ones). The window is an X11 one (SDL's x11 driver, also under
// XWayland), so the Vulkan presenter gets the XCB surface it supports.
#pragma once
#ifndef _WIN32

#include <functional>
#include <memory>
#include <vector>

#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

union SDL_Event;
struct SDL_Window;

namespace xe::ui {

class SdlWindow;

class SdlWindowedAppContext final : public WindowedAppContext {
 public:
  SdlWindowedAppContext();
  ~SdlWindowedAppContext();
  bool Initialize();
  int RunMainMessageLoop();
  // Every SDL event, before the windows get theirs (the game's input).
  static void SetEventHook(std::function<void(const SDL_Event&)> hook);

 protected:
  void NotifyUILoopOfPendingFunctions() override;
  void PlatformQuitFromUIThread() override;

 private:
  friend class SdlWindow;
  std::vector<SdlWindow*> windows_;
  uint32_t wake_event_ = 0;
  bool quit_ = false;
};

class SdlWindow final : public Window {
 public:
  SdlWindow(WindowedAppContext& app_context, const std::string_view title, uint32_t desired_logical_width,
            uint32_t desired_logical_height);
  ~SdlWindow();
  SDL_Window* sdl_window() const { return window_; }
  void HandleEvent(const SDL_Event& e);

 protected:
  bool OpenImpl() override;
  void RequestCloseImpl() override;
  void ApplyNewFullscreen() override;
  void ApplyNewTitle() override;
  void ApplyNewCursorVisibility(CursorVisibility old_cursor_visibility) override;
  void FocusImpl() override;
  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override;
  void RequestPaintImpl() override;

 private:
  void Close();
  SDL_Window* window_ = nullptr;
  uint32_t window_id_ = 0;
};

}  // namespace xe::ui

#endif  // !_WIN32
