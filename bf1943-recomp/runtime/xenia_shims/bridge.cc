// bf1943: runtime side of Xenia's GPU (see runtime/xenia_bridge.h).
#include "../xenia_bridge.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/vulkan/bf_gore.h"
#include "xenia/gpu/vulkan/bf_look.h"
#include "xenia/gpu/vulkan/bf_menu.h"
#include "xenia/gpu/vulkan/scene_fx.h"
#include "xenia/ui/immediate_drawer.h"
#include "xenia/ui/ui_drawer.h"
#include "xenia/gpu/vulkan/scene_fx.h"
#include "xenia/ui/vulkan/bf_hdr.h"

#include <cstdio>
#include <filesystem>

DECLARE_path(log_file);
DECLARE_bool(log_to_stdout);
DECLARE_int32(anisotropic_override);
DECLARE_uint32(texture_cache_memory_limit_soft);
DECLARE_uint32(texture_cache_memory_limit_hard);
DECLARE_path(dump_shaders);
DECLARE_int32(draw_resolution_scale_x);
DECLARE_uint32(draw_resolution_scale_native_pitch);
DECLARE_int32(draw_resolution_scale_y);
#include "xenia/cpu/processor.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#ifdef _WIN32
#include "xenia/ui/windowed_app_context_win.h"
#else
#include <pthread.h>

#include "window_sdl.h"  // (Linux: an SDL3 window, an X11 one for the presenter)
#endif

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

extern "C" std::atomic<uint64_t> g_title_swaps{0};  // guest frames presented (VdSwap), for the fps in the title
extern "C" const char* g_window_title = "Battlefield 1943";  // window title; a game build can change it (bfbc1)

namespace rt::xgpu {
namespace {
xe::ui::Presenter* g_graphics_presenter();
xe::ui::GraphicsProvider* g_graphics_provider();
// Closing the window ends the game, like the console's power button.
class CloseListener : public xe::ui::WindowListener {
 public:
  void OnClosing(xe::ui::UIEvent&) override {
    fprintf(stderr, "[xgpu] window closed, exiting\n");
    fflush(nullptr);
#ifdef _WIN32
    ExitProcess(0);
#else
    std::_Exit(0);
#endif
  }
};

// The PC settings page (pc_settings.cpp): an RGBA image drawn over the whole
// window while it is open.
std::mutex g_overlay_mutex;
std::vector<uint8_t> g_overlay_pixels;
uint32_t g_overlay_width = 0, g_overlay_height = 0;
uint64_t g_overlay_version = 0;

class OverlayDrawer : public xe::ui::UIDrawer {
 public:
  std::unique_ptr<xe::ui::ImmediateDrawer> drawer;
  void Draw(xe::ui::UIDrawContext& context) override {
    if (!drawer) return;
    {
      std::lock_guard lock(g_overlay_mutex);
      if (g_overlay_pixels.empty()) {
        texture_.reset();
        version_ = g_overlay_version;
        return;
      }
      if (version_ != g_overlay_version || !texture_) {
        texture_ = drawer->CreateTexture(g_overlay_width, g_overlay_height,
                                         xe::ui::ImmediateTextureFilter::kLinear, false,
                                         g_overlay_pixels.data());
        version_ = g_overlay_version;
      }
    }
    if (!texture_) return;
    float w = float(context.render_target_width()), h = float(context.render_target_height());
    drawer->Begin(context, w, h);
    const xe::ui::ImmediateVertex vertices[4] = {{0, 0, 0, 0, 0xFFFFFFFF},
                                                 {w, 0, 1, 0, 0xFFFFFFFF},
                                                 {w, h, 1, 1, 0xFFFFFFFF},
                                                 {0, h, 0, 1, 0xFFFFFFFF}};
    const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    xe::ui::ImmediateDrawBatch batch;
    batch.vertices = vertices;
    batch.vertex_count = 4;
    batch.indices = indices;
    batch.index_count = 6;
    drawer->BeginDrawBatch(batch);
    xe::ui::ImmediateDraw draw;
    draw.count = 6;
    draw.texture = texture_.get();
    drawer->Draw(draw);
    drawer->EndDrawBatch();
    drawer->End();
  }

 private:
  std::unique_ptr<xe::ui::ImmediateTexture> texture_;
  uint64_t version_ = 0;
};
OverlayDrawer g_overlay;

int g_present_effect = 2;
int g_window_width = 1280, g_window_height = 720;

// UI thread: the app context (Win32, or SDL3 on Linux), the game window and
// its message loop.
void UiThread() {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"ui");
  xe::ui::Win32WindowedAppContext app_context(GetModuleHandleW(nullptr), SW_SHOWNORMAL);
#else
  pthread_setname_np(pthread_self(), "ui");
  xe::ui::SdlWindowedAppContext app_context;
#endif
  if (!app_context.Initialize()) {
    fprintf(stderr, "[xgpu] UI context initialisation failed\n");
    return;
  }
  auto window = xe::ui::Window::Create(app_context, g_window_title, g_window_width, g_window_height);
  static CloseListener close_listener;
  window->AddListener(&close_listener);
  if (!window->Open()) {
    fprintf(stderr, "[xgpu] could not open the window\n");
    return;
  }
  fprintf(stderr, "[xgpu] window open\n");
  window->SetPresenter(g_graphics_presenter());
  if (auto* provider = g_graphics_provider()) {
    g_overlay.drawer = provider->CreateImmediateDrawer();
    if (g_overlay.drawer) {
      g_overlay.drawer->SetPresenter(g_graphics_presenter());
      g_graphics_presenter()->AddUIDrawerFromUIThread(&g_overlay, 1);
    }
  }
  {
    using Config = xe::ui::Presenter::GuestOutputPaintConfig;
    Config config = g_graphics_presenter()->GetGuestOutputPaintConfigFromUIThread();
    config.SetEffect(g_present_effect == 2 ? Config::Effect::kFsr
                     : g_present_effect == 1 ? Config::Effect::kCas
                                             : Config::Effect::kBilinear);
    g_graphics_presenter()->SetGuestOutputPaintConfigFromUIThread(config);
  }
  // The title shows the guest frame rate, refreshed twice a second.
  std::atomic<bool> stop_title{false};
  std::thread title_thread([&] {
    const std::string base = g_window_title;
    uint64_t last = g_title_swaps;
    auto t0 = std::chrono::steady_clock::now();
    while (!stop_title) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto t1 = std::chrono::steady_clock::now();
      uint64_t now = g_title_swaps;
      double fps = double(now - last) / std::chrono::duration<double>(t1 - t0).count();
      last = now, t0 = t1;
      char buf[32];
      snprintf(buf, sizeof buf, "  |  %.0f fps", fps);
      app_context.CallInUIThread([&window, title = base + buf] { window->SetTitle(title); });
    }
  });
  fprintf(stderr, "[xgpu] presenter attached, entering message loop\n");
  int code = app_context.RunMainMessageLoop();
  stop_title = true;
  title_thread.join();  // at most one 500 ms sleep
  fprintf(stderr, "[xgpu] message loop ended (%d)\n", code);
}

std::unique_ptr<xe::Memory> g_own_memory;
xe::Memory* g_memory = nullptr;  // shared with apu_bridge.cc
std::unique_ptr<xe::cpu::Processor> g_processor;
std::unique_ptr<xe::kernel::KernelState> g_kernel;
std::unique_ptr<xe::gpu::vulkan::VulkanGraphicsSystem> g_graphics;
xe::ui::Presenter* g_graphics_presenter() { return g_graphics->presenter(); }
xe::ui::GraphicsProvider* g_graphics_provider() { return g_graphics->provider(); }
}  // namespace

void SetLook(int look) { xe::gpu::bflook::look = look; }
int MenuRows(float (*rows)[4], int max, int* selected) {
  *selected = -1;
  std::vector<xe::gpu::bfmenu::Box> boxes = xe::gpu::bfmenu::Boxes();
  // The highlight: a filled green box the size of a row.
  const xe::gpu::bfmenu::Box* hi = nullptr;
  for (const auto& b : boxes) {
    float w = b.x1 - b.x0, h = b.y1 - b.y0;
    if (w > 0.05f && h > 0.012f && h < 0.1f && b.a > 0.5f && b.g > b.r + 0.05f &&
        b.g > b.b + 0.1f) {
      hi = &b;
    }
  }
  if (!hi) return 0;
  // Its list: filled boxes with the same left and right edges and height.
  float h = hi->y1 - hi->y0;
  std::vector<const xe::gpu::bfmenu::Box*> list;
  for (const auto& b : boxes) {
    if (std::abs(b.x0 - hi->x0) < 0.004f && std::abs(b.x1 - hi->x1) < 0.004f &&
        std::abs((b.y1 - b.y0) - h) < 0.3f * h && b.a > 0.2f) {
      bool dup = false;
      for (auto* o : list) dup |= std::abs(o->y0 - b.y0) < 0.25f * h;
      if (!dup) list.push_back(&b);
    }
  }
  std::sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->y0 < b->y0; });
  int n = 0;
  for (auto* b : list) {
    if (n >= max) break;
    if (std::abs(b->y0 - hi->y0) < 0.25f * h) *selected = n;
    rows[n][0] = b->x0, rows[n][1] = b->y0, rows[n][2] = b->x1, rows[n][3] = b->y1;
    ++n;
  }
  return n;
}

void AddBackIcon(uint32_t physical, uint32_t width, uint32_t height, float u0, float v0,
                 float u1, float v1) {
  xe::gpu::bfmenu::AddBackIcon({physical, width, height, u0, v0, u1, v1});
}
bool OnBackPrompt(float x, float y) {
  for (const auto& b : xe::gpu::bfmenu::BackBoxes()) {
    if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) return true;
  }
  return false;
}

void SetOverlay(const uint8_t* rgba, uint32_t width, uint32_t height) {
  std::lock_guard lock(g_overlay_mutex);
  if (!rgba) {
    g_overlay_pixels.clear();
  } else {
    g_overlay_pixels.assign(rgba, rgba + size_t(width) * height * 4);
    g_overlay_width = width;
    g_overlay_height = height;
  }
  ++g_overlay_version;
}
float HdrWhite() {
  if (!xe::ui::bfhdr::active) return 0.0f;
  return xe::ui::bfhdr::paper_white * (xe::ui::bfhdr::no_stretch ? 0.65f : 1.0f);
}
void SetHdrWhite(float nits) { xe::ui::bfhdr::paper_white = std::clamp(nits, 80.0f, 500.0f); }
void ApplySceneFx(const char* settings) {
  if (xe::gpu::scenefx::Enabled()) xe::gpu::scenefx::Configure(settings);
}

bool CameraEye(float out[3]) { return xe::gpu::scenefx::Eye(out); }

void SetHdrStretch(bool on) { xe::ui::bfhdr::no_stretch = !on; }

void SetHdr(bool on, float paper_white, float peak) {
  xe::ui::bfhdr::requested = on;
  xe::ui::bfhdr::paper_white = std::clamp(paper_white, 80.0f, 500.0f);
  xe::ui::bfhdr::peak = std::clamp(peak, xe::ui::bfhdr::paper_white.load(), 10000.0f);
}
void SetGore(bool on, uint32_t scratch_physical, uint32_t scratch_size) {
  xe::gpu::bflook::gore = on;
  xe::gpu::bflook::scratch_physical = scratch_physical;
  xe::gpu::bflook::scratch_size = scratch_size;
  // The blood is drawn by the scene effects pass (with the world depth): on
  // without its effects when they are off.
  if (on && !xe::gpu::scenefx::Enabled()) xe::gpu::scenefx::Configure("steps=0,shadows=0,rt=0,shafts=0");
}
void GoreHit(const float feet[3], float damage) { xe::gpu::bflook::AddHit(feet, damage); }
void GoreDeath(const float feet[3], float damage, bool gib, const float push[3]) {
  xe::gpu::bflook::AddDeath(feet, damage, gib, push);
}

void SetDisplayOptions(int resolution_scale, int effect, int window_width, int window_height, int shadowmap) {
  cvars::draw_resolution_scale_x = cvars::draw_resolution_scale_y = std::clamp(resolution_scale, 1, 3);
  // The sun shadowmap (pitch = its size aligned to 80-pixel EDRAM tiles) stays
  // native: scaling it costs a third of the frame rate.
  cvars::draw_resolution_scale_native_pitch =
      resolution_scale > 1 ? uint32_t((shadowmap + 79) / 80 * 80) : 0;
  g_present_effect = effect;
  if (window_width > 0 && window_height > 0) g_window_width = window_width, g_window_height = window_height;
}

void SetOptions(int anisotropic, uint32_t texture_memory_mb, const char* shader_dump_dir) {
  cvars::anisotropic_override = anisotropic;
  if (shader_dump_dir && *shader_dump_dir) {  // every shader's ucode + disassembly, once
    cvars::dump_shaders = std::filesystem::absolute(shader_dump_dir);
    std::filesystem::create_directories(cvars::dump_shaders);
  }
  if (texture_memory_mb) {
    cvars::texture_cache_memory_limit_soft = texture_memory_mb;
    cvars::texture_cache_memory_limit_hard = texture_memory_mb * 2;
  }
}

bool Start(uint8_t* guest_base, bool present, InterruptFn interrupt, uint32_t title_id) {
  cvars::log_file = std::filesystem::absolute("xenia-gpu.log");
  cvars::log_to_stdout = false;
  xe::InitializeLogging("xenia-gpu");
  fprintf(stderr, "[xgpu] logging up, creating Vulkan graphics system\n");
  g_memory = xe::GetMemory();
  if (!g_memory) {
    g_own_memory = std::make_unique<xe::Memory>(guest_base);
    g_memory = g_own_memory.get();
  }
  g_processor = std::make_unique<xe::cpu::Processor>(g_memory);
  g_kernel = std::make_unique<xe::kernel::KernelState>(g_memory);
  g_kernel->set_title_id(title_id);
  g_graphics = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
  g_graphics->on_interrupt = interrupt;
#ifndef _WIN32
  // Mesa offers only FIFO on XWayland, and its X11 FIFO waits for present-done
  // events that can stop coming: the window froze on its first frames. Mailbox
  // (no tearing either; the guest paces itself) unless the player set it.
  setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 0);
#endif
  fprintf(stderr, "[xgpu] Setup(present=%d)\n", int(present));
  if (g_graphics->Setup(g_processor.get(), g_kernel.get(), nullptr, present) != 0) {
    g_graphics.reset();
    return false;
  }
  fprintf(stderr, "[xgpu] graphics system ready\n");
  // Persistent shader and pipeline storage (shader_cache/): shaders seen in earlier
  // runs are translated and their pipelines created in the background at startup
  // instead of stalling the first frames that use them.
  {
    auto* cp = g_graphics->command_processor();
    auto cache_root = std::filesystem::absolute("shader_cache");
    cp->CallInThread([cp, cache_root] {
      cp->InitializeShaderStorage(cache_root, 0xBF194300u, false, nullptr);
    });
  }
  if (present) std::thread(UiThread).detach();
  return true;
}

bool Running() { return g_graphics != nullptr; }

void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  g_graphics->InitializeRingBuffer(ptr, size_log2);
}
void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  g_graphics->EnableReadPointerWriteBack(ptr, block_size_log2);
}
void SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  g_graphics->SetInterruptCallback(callback, user_data);
}
void WriteRegister(uint32_t index, uint32_t value) { g_graphics->WriteRegister(index, value); }
void Vblank() { g_graphics->command_processor()->increment_counter(); }
void TriggerWatches(uint32_t physical_address, uint32_t length) {
  if (g_memory) g_memory->TriggerWatches(physical_address, length);
}

bool CaptureFrame(const char* png_path, FrameStats* stats) {
  if (!g_graphics || !g_graphics->presenter()) return false;
  xe::ui::RawImage image;
  if (!g_graphics->presenter()->CaptureGuestOutput(image) || !image.width) return false;
  std::vector<bool> seen(1 << 12);
  uint64_t non_black = 0;
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* row = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      const uint8_t* p = row + x * 4;
      if (p[0] > 16 || p[1] > 16 || p[2] > 16) ++non_black;
      seen[(p[0] >> 4) << 8 | (p[1] >> 4) << 4 | (p[2] >> 4)] = true;
    }
  }
  stats->width = image.width;
  stats->height = image.height;
  stats->non_black = double(non_black) / (double(image.width) * image.height);
  stats->distinct_colors = uint32_t(std::count(seen.begin(), seen.end(), true));
  // RGBX -> RGBA with opaque alpha for the PNG.
  for (uint32_t y = 0; y < image.height; ++y)
    for (uint32_t x = 0; x < image.width; ++x) image.data[y * image.stride + x * 4 + 3] = 255;
  return stbi_write_png(png_path, int(image.width), int(image.height), 4, image.data.data(),
                        int(image.stride)) != 0;
}

}  // namespace rt::xgpu
