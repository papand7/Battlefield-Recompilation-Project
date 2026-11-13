// FPS COUNTER (the GRAPHICS page; F2 cycles it): a small panel in the window's
// top-left corner, redrawn twice a second from the guest's swaps (VdSwap).
//   FPS:  the frames of the last second.
//   FULL: that, the average and worst frame time and the 1 % low (the frame
//         rate of the slowest hundredth of frames) over the last two seconds.
#include "kernel.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "posix_gdiplus.h"  // (Linux: GDI's text calls)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {
int g_fps_overlay = 0;  // 0 off, 1 FPS, 2 FULL
std::atomic<uint64_t> g_frame_count{0};
}

using namespace rt;

namespace {
using Clock = std::chrono::steady_clock;
std::mutex g_mutex;
std::deque<Clock::time_point> g_swaps;  // the last two seconds' swaps

// The lines drawn white on a dark, see-through panel: RGBA (straight alpha).
std::vector<uint8_t> Render(const std::vector<std::wstring>& lines, int& w, int& h) {
  const int line_h = 26, pad = 8;
  HDC dc = CreateCompatibleDC(nullptr);
  HFONT font = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Bahnschrift SemiBold Condensed");
  HGDIOBJ old_font = SelectObject(dc, font);
  int text_w = 0;
  for (const std::wstring& l : lines) {
    SIZE ext{};
    GetTextExtentPoint32W(dc, l.c_str(), int(l.size()), &ext);
    text_w = std::max(text_w, int(ext.cx));
  }
  w = text_w + 2 * pad, h = int(lines.size()) * line_h + 2 * pad - 4;
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bmp = SelectObject(dc, bmp);
  memset(bits, 0, size_t(w) * h * 4);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  for (size_t i = 0; i < lines.size(); ++i)
    TextOutW(dc, pad, pad - 2 + int(i) * line_h, lines[i].c_str(), int(lines[i].size()));
  GdiFlush();
  // The text's coverage (white on black) over the panel.
  float white = xgpu::HdrWhite();
  auto out = [&](double v) {  // 0..1 sRGB, through the HDR curve when the output is HDR10
    if (white <= 0.0f) return uint8_t(std::lround(v * 255));
    double y = std::pow(v, 2.2) * white / 10000.0, m = std::pow(y, 0.1593017578125);
    return uint8_t(std::clamp(std::pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375) * 255 + 0.5, 0.0,
                              255.0));
  };
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  const auto* px = static_cast<const uint8_t*>(bits);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    double t = std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]}) / 255.0;
    double v = 0.08 + 0.92 * t;  // dark panel, white text
    rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = out(v);
    rgba[i * 4 + 3] = uint8_t(std::lround((0.55 + 0.45 * t) * 255));
  }
  SelectObject(dc, old_bmp);
  DeleteObject(bmp);
  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteDC(dc);
  return rgba;
}

void OverlayThread() {
  int shown = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const int mode = rt::g_fps_overlay;
    if (mode == 0) {
      if (shown) xgpu::SetCornerOverlay(nullptr, 0, 0), shown = 0;
      continue;
    }
    std::vector<Clock::time_point> swaps;
    {
      std::lock_guard lock(g_mutex);
      swaps.assign(g_swaps.begin(), g_swaps.end());
    }
    const auto now = Clock::now();
    int last_second = 0;
    std::vector<double> frames;  // frame times (ms)
    for (size_t i = 0; i < swaps.size(); ++i) {
      if (now - swaps[i] <= std::chrono::seconds(1)) ++last_second;
      if (i) frames.push_back(std::chrono::duration<double, std::milli>(swaps[i] - swaps[i - 1]).count());
    }
    std::vector<std::wstring> lines;
    wchar_t buf[96];
    swprintf(buf, 96, L"%d FPS", last_second);
    lines.push_back(buf);
    if (mode == 2 && !frames.empty()) {
      double sum = 0, worst = 0;
      for (double f : frames) sum += f, worst = std::max(worst, f);
      std::vector<double> sorted = frames;
      std::sort(sorted.begin(), sorted.end());
      // The 1 % low: the average of the slowest hundredth (at least one frame).
      size_t n = std::max<size_t>(1, sorted.size() / 100);
      double slow = 0;
      for (size_t i = sorted.size() - n; i < sorted.size(); ++i) slow += sorted[i];
      slow /= double(n);
      swprintf(buf, 96, L"%.1f ms   worst %.1f ms", sum / frames.size(), worst);
      lines.push_back(buf);
      swprintf(buf, 96, L"1%% low %.0f FPS", slow > 0 ? 1000.0 / slow : 0.0);
      lines.push_back(buf);
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgba = Render(lines, w, h);
    xgpu::SetCornerOverlay(rgba.data(), uint32_t(w), uint32_t(h));
    shown = mode;
  }
}
}  // namespace

namespace rt {
// VdSwap (gpu.cpp): one frame shown.
void FrameSwapped() {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(OverlayThread).detach(); });
  ++g_frame_count;
  if (!g_fps_overlay) return;
  const auto now = Clock::now();
  std::lock_guard lock(g_mutex);
  g_swaps.push_back(now);
  while (!g_swaps.empty() && now - g_swaps.front() > std::chrono::seconds(2)) g_swaps.pop_front();
}
}  // namespace rt
