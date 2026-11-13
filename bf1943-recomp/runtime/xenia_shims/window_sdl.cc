// Linux: Xenia's window and UI-thread loop on SDL3 (see window_sdl.h).
#ifndef _WIN32

#include "window_sdl.h"

#include <SDL3/SDL.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>

#include <cstdio>

#include "xenia/ui/surface_gnulinux.h"

namespace xe::ui {

namespace {
std::function<void(const SDL_Event&)> g_event_hook;
}

// ---- the XCB surface's size (upstream's surface_gnulinux.cc) ----------------
bool XcbWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  xcb_get_geometry_cookie_t cookie = xcb_get_geometry(connection_, window_);
  xcb_get_geometry_reply_t* reply = xcb_get_geometry_reply(connection_, cookie, nullptr);
  if (!reply) return false;
  width_out = reply->width, height_out = reply->height;
  free(reply);
  return true;
}

// ---- the UI thread's loop ----------------------------------------------------
SdlWindowedAppContext::SdlWindowedAppContext() = default;
SdlWindowedAppContext::~SdlWindowedAppContext() { SDL_Quit(); }

void SdlWindowedAppContext::SetEventHook(std::function<void(const SDL_Event&)> hook) { g_event_hook = std::move(hook); }

bool SdlWindowedAppContext::Initialize() {
  // The presenter draws to an X11 window (XCB): SDL's x11 driver, which
  // Wayland desktops run through XWayland.
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
    fprintf(stderr, "[xgpu] SDL: %s\n", SDL_GetError());
    return false;
  }
  wake_event_ = SDL_RegisterEvents(1);
  return wake_event_ != 0;
}

void SdlWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  SDL_Event e{};
  e.type = wake_event_;
  SDL_PushEvent(&e);  // (thread-safe)
}

void SdlWindowedAppContext::PlatformQuitFromUIThread() { quit_ = true; }

int SdlWindowedAppContext::RunMainMessageLoop() {
  while (!quit_) {
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, 100)) {
      ExecutePendingFunctionsFromUIThread();
      continue;
    }
    do {
      if (e.type == wake_event_) {
        ExecutePendingFunctionsFromUIThread();
        continue;
      }
      if (g_event_hook) g_event_hook(e);
      if (e.type == SDL_EVENT_QUIT) {
        for (SdlWindow* w : std::vector<SdlWindow*>(windows_)) w->RequestClose();
        continue;
      }
      for (SdlWindow* w : std::vector<SdlWindow*>(windows_)) w->HandleEvent(e);
    } while (!quit_ && SDL_PollEvent(&e));
    ExecutePendingFunctionsFromUIThread();
  }
  return 0;
}

// ---- the window --------------------------------------------------------------
std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context, const std::string_view title,
                                       uint32_t desired_logical_width, uint32_t desired_logical_height) {
  return std::make_unique<SdlWindow>(app_context, title, desired_logical_width, desired_logical_height);
}

SdlWindow::SdlWindow(WindowedAppContext& app_context, const std::string_view title, uint32_t desired_logical_width,
                     uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

SdlWindow::~SdlWindow() {
  EnterDestructor();
  if (window_) {
    auto& ctx = static_cast<SdlWindowedAppContext&>(app_context());
    std::erase(ctx.windows_, this);
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool SdlWindow::OpenImpl() {
  std::string title(GetTitle());
  window_ = SDL_CreateWindow(title.c_str(), int(GetDesiredLogicalWidth()), int(GetDesiredLogicalHeight()),
                             SDL_WINDOW_RESIZABLE | (IsFullscreen() ? SDL_WINDOW_FULLSCREEN : 0));
  if (!window_) {
    fprintf(stderr, "[xgpu] SDL window: %s\n", SDL_GetError());
    return false;
  }
  window_id_ = SDL_GetWindowID(window_);
  static_cast<SdlWindowedAppContext&>(app_context()).windows_.push_back(this);
  WindowDestructionReceiver destruction_receiver(this);
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window_, &w, &h);
  OnActualSizeUpdate(uint32_t(w), uint32_t(h), WindowResizeAction::kManual, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyedOrClosed()) return true;
  OnFocusUpdate((SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0, destruction_receiver);
  return true;
}

void SdlWindow::Close() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) return;
  auto& ctx = static_cast<SdlWindowedAppContext&>(app_context());
  std::erase(ctx.windows_, this);
  if (window_) SDL_DestroyWindow(window_), window_ = nullptr;
  OnAfterClose();
}

void SdlWindow::RequestCloseImpl() { Close(); }

void SdlWindow::ApplyNewFullscreen() {
  if (window_) SDL_SetWindowFullscreen(window_, IsFullscreen());
}

void SdlWindow::ApplyNewTitle() {
  if (window_) SDL_SetWindowTitle(window_, std::string(GetTitle()).c_str());
}

void SdlWindow::ApplyNewCursorVisibility(CursorVisibility) {
  if (GetCursorVisibility() == CursorVisibility::kVisible) SDL_ShowCursor();
  else SDL_HideCursor();
}

void SdlWindow::FocusImpl() {
  if (window_) SDL_RaiseWindow(window_);
}

std::unique_ptr<Surface> SdlWindow::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!window_ || !(allowed_types & (Surface::TypeFlags(1) << Surface::kTypeIndex_XcbWindow))) return nullptr;
  SDL_PropertiesID props = SDL_GetWindowProperties(window_);
  auto* display = static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  auto xid = xcb_window_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!display || !xid) {
    fprintf(stderr, "[xgpu] the window is not an X11 one: no Vulkan surface\n");
    return nullptr;
  }
  return std::make_unique<XcbWindowSurface>(XGetXCBConnection(display), xid);
}

void SdlWindow::RequestPaintImpl() {
  // Painted at once from the UI thread (the presenter asks from there).
  app_context().CallInUIThreadDeferred([this] { OnPaint(); });
}

void SdlWindow::HandleEvent(const SDL_Event& e) {
  if (!window_) return;
  // (every window event carries its window's id)
  if (e.type >= SDL_EVENT_WINDOW_FIRST && e.type <= SDL_EVENT_WINDOW_LAST && e.window.windowID != window_id_) return;
  WindowDestructionReceiver destruction_receiver(this);
  switch (e.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      OnActualSizeUpdate(uint32_t(e.window.data1), uint32_t(e.window.data2), WindowResizeAction::kManual,
                         destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      OnFocusUpdate(true, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      OnFocusUpdate(false, destruction_receiver);
      break;
    case SDL_EVENT_WINDOW_EXPOSED:
      OnPaint(true);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      Close();
      break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
      OnDesiredFullscreenUpdate(e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN);
      break;
    default:
      break;
  }
}

}  // namespace xe::ui

#endif  // !_WIN32
