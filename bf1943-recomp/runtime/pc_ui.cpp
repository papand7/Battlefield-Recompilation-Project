// PC look for the console UI: the Xbox button icons in the game's UI texture
// atlases are redrawn as key caps showing the PC binding (see xam.cpp) when the
// GPU first loads the atlas. Textures are recognised by size and a hash of the
// game's own data, and patched in guest memory, so no game assets are shipped.
#include "kernel.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#else
#include "posix_gdiplus.h"  // (Linux: GDI's text calls, XInput)
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xe::gpu {
extern void (*g_texture_load_hook)(uint32_t physical_address, uint32_t size, uint32_t width,
                                   uint32_t height, uint32_t format, bool tiled);
}

using namespace rt;

namespace {

struct Icon {
  uint16_t x0, y0, x1, y1;  // inclusive pixel box in the atlas
  const wchar_t* label;
};
struct Atlas {
  const char* name;
  uint32_t width, height;
  uint64_t hash;  // FNV-1a 64 of the texture data as loaded from the game files
  std::vector<Icon> icons;
};

// Labels: key names, arrows, or "@x" for a mouse picture (see RenderMouse).
// Icons that only the PS3 build shows (black rings, L1/R2...) are left alone.
const std::vector<Atlas>& Atlases() {
  static const std::vector<Atlas> kAtlases = {
      {"menu_common", 256, 128, 0x1099bef8dcbbf9fbull,
       {{31, 1, 58, 28, L"@M"},  {91, 1, 114, 24, L"R"},     {87, 29, 110, 52, L"\u232B"},
        {1, 31, 27, 58, L"@R"},    {29, 31, 54, 58, L"@L"},   {56, 31, 84, 52, L"@W"},
        {113, 47, 134, 66, L"ESC"}, {87, 55, 110, 78, L"\u21B5"}, {31, 61, 59, 82, L"G"},
        {87, 81, 108, 100, L"TAB"}, {1, 89, 24, 112, L"F"}}},
      {"popups", 128, 128, 0x25ace164cca0161bull,
       {{1, 65, 24, 88, L"\u232B"}, {27, 65, 50, 88, L"F"}, {1, 91, 24, 114, L"\u21B5"},
        {27, 91, 50, 114, L"R"}}},
      {"hud_root_cq", 512, 512, 0x2e9696cc53bf9110ull,
       {{314, 1, 341, 28, L"@M"},    {375, 1, 398, 24, L"SPACE"},   {344, 29, 369, 56, L"@L"},
        {372, 29, 395, 52, L"F"},       {302, 73, 329, 100, L"@M"}, {325, 103, 351, 130, L"@R"},
        {263, 151, 290, 178, L"CTRL"},  {243, 181, 270, 208, L"W/S"},  {265, 119, 292, 146, L"\u2193"},
        {213, 183, 240, 210, L"\u2191"}, {204, 215, 231, 242, L"\u2192"}, {65, 295, 92, 322, L"\u2190"},
        {319, 181, 342, 204, L"R"},     {345, 179, 366, 198, L"ESC"},  {264, 239, 287, 262, L"E"},
        {293, 262, 314, 281, L"TAB"},   {142, 265, 169, 292, L"SHIFT"}, {201, 276, 229, 297, L"@W"},
        {95, 295, 122, 322, L"@M"},  {1, 313, 28, 340, L"A/D"},     {31, 313, 58, 340, L"WASD"},
        {91, 341, 119, 362, L"G"},      {121, 341, 144, 364, L"E"}}},
      {"igm_mp_cq", 512, 256, 0x733675e0481eb2c5ull,
       {{77, 1, 104, 28, L"@M"}, {107, 1, 130, 24, L"SPACE"}, {107, 27, 130, 50, L"E"},
        {89, 61, 112, 84, L"F"},    {1, 89, 28, 116, L"WASD"}}},
      // Bad Company (found with BF_UI_DUMP): its button atlas (Xbox and PS3
      // icons; the Xbox ones redrawn) and the HUD atlas's Y, A and B.
      {"bc_buttons", 256, 256, 0x29c10b3a334250d9ull,
       {{123, 3, 156, 28, L"ESC"}, {184, 2, 213, 31, L"F"},     {81, 37, 118, 68, L"@M"},
        {124, 35, 144, 72, L"@R"}, {150, 34, 179, 63, L"E"},    {166, 68, 195, 97, L"R"},
        {82, 74, 115, 99, L"TAB"}, {122, 80, 160, 99, L"G"},    {3, 110, 38, 138, L"CTRL"},
        {82, 106, 102, 143, L"@L"}, {3, 145, 41, 164, L"1"},    {45, 172, 74, 201, L"SPACE"}}},
      {"bc_hud", 1024, 512, 0x91558f3fb6ca83f2ull,
       {{2, 418, 31, 447, L"F"}, {70, 418, 99, 447, L"SPACE"}, {322, 217, 351, 246, L"E"}}},
  };
  return kAtlases;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// Xenos 2D tiling (as Xenia's texture_util::GetTiledOffset2D), in bytes.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2_bpp) {
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_bpp + 7);
  uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpp;
  uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Text coverage (0..255) for a w*h box, largest bold font that fits.
std::vector<uint8_t> RenderLabel(const wchar_t* text, int w, int h) {
  std::vector<uint8_t> cover(size_t(w) * h, 0);
  HDC dc = CreateCompatibleDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old_bmp = SelectObject(dc, bmp);
  int len = int(wcslen(text));
  int max_w = w - 4;
  for (int size = int(h * (len > 1 ? 0.62 : 1.05)); size >= 6; --size) {
    // Condensed so words like SHIFT stay readable in a 24-pixel icon.
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             len > 1 ? L"Bahnschrift SemiBold Condensed" : L"Segoe UI Symbol");
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE ext{};
    GetTextExtentPoint32W(dc, text, len, &ext);
    if (ext.cx <= max_w || size == 6) {
      memset(bits, 0, size_t(w) * h * 4);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      RECT r{0, 0, w, h};
      DrawTextW(dc, text, len, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      GdiFlush();
      auto* px = static_cast<const uint8_t*>(bits);
      for (size_t i = 0; i < cover.size(); ++i)
        cover[i] = std::max({px[i * 4], px[i * 4 + 1], px[i * 4 + 2]});
      SelectObject(dc, old_font);
      DeleteObject(font);
      break;
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
  }
  SelectObject(dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(dc);
  return cover;
}

// Mouse pictogram coverage (0..255): "@M" plain (look), "@L"/"@R" with that
// button filled, "@W" with the wheel filled.
std::vector<uint8_t> RenderMouse(wchar_t part, int w, int h) {
  std::vector<uint8_t> cover(size_t(w) * h, 0);
  float s = float(std::min(w, h)), cx = w * 0.5f, cy = h * 0.5f;
  float hw = 0.25f * s, hh = 0.38f * s, line = 0.075f * s, split = cy - 0.06f * s;
  auto inside = [&](float x, float y, float inset) {  // capsule body
    float r = hw - inset, dx = std::abs(x - cx), dy = std::max(std::abs(y - cy) - (hh - hw), 0.f);
    return dx * dx + dy * dy <= r * r;
  };
  constexpr int kSamples = 4;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int hit = 0;
      for (int sy = 0; sy < kSamples; ++sy) {
        for (int sx = 0; sx < kSamples; ++sx) {
          float fx = x + (sx + 0.5f) / kSamples, fy = y + (sy + 0.5f) / kSamples;
          if (!inside(fx, fy, 0)) continue;
          bool on = !inside(fx, fy, line);                      // outline
          on |= std::abs(fy - split) < line * 0.5f;             // button edge
          on |= fy < split && std::abs(fx - cx) < line * 0.5f;  // button split
          on |= part == L'L' && fy < split && fx < cx;
          on |= part == L'R' && fy < split && fx > cx;
          on |= part == L'W' && std::abs(fx - cx) < line * 1.1f && fy > cy - hh * 0.75f &&
                fy < split - line;
          hit += on;
        }
      }
      cover[size_t(y) * w + x] = uint8_t(hit * 255 / (kSamples * kSamples));
    }
  }
  return cover;
}

// Dark key cap with a light rim and the label, written over the icon's box in
// a tiled big-endian ARGB8 texture.
void DrawKeyCap(uint8_t* tex, uint32_t pitch, const Icon& icon) {
  int w = icon.x1 - icon.x0 + 1, h = icon.y1 - icon.y0 + 1;
  std::vector<uint8_t> text = icon.label[0] == L'@' ? RenderMouse(icon.label[1], w, h)
                                                    : RenderLabel(icon.label, w, h);
  float r = std::min(w, h) * 0.28f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Signed distance to the rounded rectangle (negative inside).
      float px = x + 0.5f - w * 0.5f, py = y + 0.5f - h * 0.5f;
      float qx = std::abs(px) - (w * 0.5f - 0.5f - r), qy = std::abs(py) - (h * 0.5f - 0.5f - r);
      float d = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
      float shape = std::clamp(0.5f - d, 0.f, 1.f);
      float rim = std::clamp(1.0f - std::abs(d + 1.0f), 0.f, 1.f);
      float t = text[size_t(y) * w + x] / 255.f;
      float gray = 28 + (200 - 28) * rim;
      gray = gray + (255 - gray) * t;
      uint8_t* p = tex + TiledOffset(icon.x0 + x, icon.y0 + y, pitch, 2);
      p[0] = uint8_t(std::lround(shape * 235));
      p[1] = p[2] = p[3] = uint8_t(std::lround(gray));
    }
  }
}

// ---- controller prompts (BUTTON PROMPTS) ----------------------------------
// The same icons in a controller's look, drawn here: which button an icon is
// follows from its PC key label (the key bound to that button).
enum Style { kAuto, kKeys, kXbox360, kXboxSeries, kPs3, kPs5, kSteamDeck };

const char* PadButton(std::wstring_view label) {
  static const std::pair<std::wstring_view, const char*> kButtons[] = {
      {L"↵", "a"},       {L"SPACE", "a"},        {L"⌫", "b"},       {L"E", "b"},
      {L"R", "x"},            {L"F", "y"},            {L"G", "lb"},           {L"@W", "rb"},
      {L"@R", "lt"},          {L"@L", "rt"},          {L"ESC", "start"},      {L"TAB", "back"},
      {L"@M", "rs"},          {L"SHIFT", "ls_click"}, {L"CTRL", "rs_click"},  {L"WASD", "ls"},
      {L"W/S", "ls_ud"},      {L"A/D", "ls_lr"},      {L"↑", "dpad_up"}, {L"↓", "dpad_down"},
      {L"←", "dpad_left"}, {L"→", "dpad_right"}};
  for (const auto& [key, button] : kButtons)
    if (key == label) return button;
  return nullptr;
}

struct Rgb {
  float r, g, b;
};
// A picture built from coverage layers, each with its colour, then written
// over the icon's box (anything not covered is transparent).
struct Canvas {
  int w, h;
  std::vector<float> a, r, g, b;
  Canvas(int w_, int h_) : w(w_), h(h_), a(size_t(w_) * h_), r(a.size()), g(a.size()), b(a.size()) {}
  // Paints the colour where inside(x, y) holds (4x4 samples a pixel).
  template <typename F>
  void Fill(Rgb c, F inside) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        int hit = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) hit += inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4);
        Blend(x, y, c, hit / 16.0f);
      }
  }
  void Blend(int x, int y, Rgb c, float cover) {
    if (cover <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
    size_t i = size_t(y) * w + x;
    r[i] += (c.r - r[i]) * cover, g[i] += (c.g - g[i]) * cover, b[i] += (c.b - b[i]) * cover;
    a[i] += (1 - a[i]) * cover;
  }
  void Text(Rgb c, const wchar_t* text, float shrink) {
    int tw = std::max(4, int(w * shrink)), th = std::max(4, int(h * shrink));
    std::vector<uint8_t> t = RenderLabel(text, tw, th);
    int ox = (w - tw) / 2, oy = (h - th) / 2;
    for (int y = 0; y < th; ++y)
      for (int x = 0; x < tw; ++x) Blend(ox + x, oy + y, c, t[size_t(y) * tw + x] / 255.0f);
  }
  // Tiled big-endian ARGB8, as the atlases are.
  void Write(uint8_t* tex, uint32_t pitch, int x0, int y0) const {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        size_t i = size_t(y) * w + x;
        uint8_t* p = tex + TiledOffset(x0 + x, y0 + y, pitch, 2);
        p[0] = uint8_t(std::lround(a[i] * 255));
        p[1] = uint8_t(std::lround(std::clamp(r[i], 0.f, 1.f) * 255));
        p[2] = uint8_t(std::lround(std::clamp(g[i], 0.f, 1.f) * 255));
        p[3] = uint8_t(std::lround(std::clamp(b[i], 0.f, 1.f) * 255));
      }
  }
};

float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay;
  float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
  return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

void DrawPadIcon(uint8_t* tex, uint32_t pitch, const Icon& icon, Style style, std::string_view button) {
  const int w = icon.x1 - icon.x0 + 1, h = icon.y1 - icon.y0 + 1;
  Canvas c(w, h);
  const float cx = w * 0.5f, cy = h * 0.5f, s = float(std::min(w, h));
  const bool ps = style == kPs3 || style == kPs5;
  const Rgb dark = style == kPs3 ? Rgb{0.05f, 0.05f, 0.06f} : Rgb{0.13f, 0.13f, 0.14f};
  const Rgb rim{0.55f, 0.55f, 0.58f}, white{0.95f, 0.95f, 0.95f}, light{0.82f, 0.82f, 0.84f};
  const float R = s * 0.47f, edge = std::max(1.0f, s * 0.06f);
  auto disc = [&](float radius) { return [=](float x, float y) { return std::hypot(x - cx, y - cy) <= radius; }; };
  auto plate = [&](float hw, float hh, float round) {  // a rounded rectangle about the centre
    return [=](float x, float y) {
      float qx = std::abs(x - cx) - (hw - round), qy = std::abs(y - cy) - (hh - round);
      return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) <= round;
    };
  };
  if (button == "a" || button == "b" || button == "x" || button == "y") {
    c.Fill(rim, disc(R));
    c.Fill(dark, disc(R - edge));
    const int k = button == "a" ? 0 : button == "b" ? 1 : button == "x" ? 2 : 3;
    if (ps) {
      // Cross, circle, square, triangle (coloured on the PS3, grey on the PS5).
      static const Rgb kPs3Colours[4] = {{0.49f, 0.66f, 0.92f}, {0.93f, 0.33f, 0.36f}, {0.86f, 0.53f, 0.79f},
                                         {0.25f, 0.78f, 0.64f}};
      const Rgb col = style == kPs3 ? kPs3Colours[k] : light;
      const float line = s * 0.06f, size = s * 0.2f;
      if (k == 0) {
        c.Fill(col, [&](float x, float y) {
          return SegmentDistance(x, y, cx - size, cy - size, cx + size, cy + size) <= line ||
                 SegmentDistance(x, y, cx - size, cy + size, cx + size, cy - size) <= line;
        });
      } else if (k == 1) {
        c.Fill(col, [&](float x, float y) { return std::abs(std::hypot(x - cx, y - cy) - size) <= line; });
      } else if (k == 2) {
        c.Fill(col, [&](float x, float y) {
          return std::abs(std::max(std::abs(x - cx), std::abs(y - cy)) - size * 0.9f) <= line;
        });
      } else {
        const float top = cy - size * 1.05f, bottom = cy + size * 0.75f, half = size * 1.1f;
        c.Fill(col, [&](float x, float y) {
          return SegmentDistance(x, y, cx, top, cx - half, bottom) <= line ||
                 SegmentDistance(x, y, cx, top, cx + half, bottom) <= line ||
                 SegmentDistance(x, y, cx - half, bottom, cx + half, bottom) <= line;
        });
      }
    } else {
      static const Rgb kXboxColours[4] = {{0.38f, 0.74f, 0.25f}, {0.91f, 0.29f, 0.24f}, {0.24f, 0.55f, 0.91f},
                                          {0.95f, 0.76f, 0.19f}};
      static const wchar_t* const kLetters[4] = {L"A", L"B", L"X", L"Y"};
      c.Text(style == kSteamDeck ? white : kXboxColours[k], kLetters[k], 0.8f);
    }
  } else if (button == "lb" || button == "rb" || button == "lt" || button == "rt") {
    const bool trigger = button[1] == 't', left = button[0] == 'l';
    const float hh = h * (trigger ? 0.46f : 0.34f);
    c.Fill(rim, plate(w * 0.48f, hh, s * 0.18f));
    c.Fill(dark, plate(w * 0.48f - 1.5f, hh - 1.5f, s * 0.16f));
    const wchar_t* text = ps || style == kSteamDeck ? (left ? (trigger ? L"L2" : L"L1") : (trigger ? L"R2" : L"R1"))
                                                     : (left ? (trigger ? L"LT" : L"LB") : (trigger ? L"RT" : L"RB"));
    c.Text(white, text, 0.85f);
  } else if (button == "start" || button == "back") {
    const bool start = button == "start";
    c.Fill(rim, disc(R));
    c.Fill(dark, disc(R - edge));
    if (style == kPs3) {
      c.Text(white, start ? L"START" : L"SELECT", 0.85f);
    } else if (style == kPs5) {
      c.Text(white, start ? L"OPTIONS" : L"CREATE", 0.85f);
    } else if (start) {  // the menu button: three lines
      const float half = s * 0.2f, line = s * 0.045f;
      c.Fill(white, [&](float x, float y) {
        if (std::abs(x - cx) > half) return false;
        for (int i = -1; i <= 1; ++i)
          if (std::abs(y - (cy + i * s * 0.13f)) <= line) return true;
        return false;
      });
    } else {  // the view button: two overlapping squares
      const float size = s * 0.13f, line = s * 0.04f, shift = s * 0.06f;
      auto square = [&](float ox, float oy, float x, float y) {
        return std::abs(std::max(std::abs(x - ox), std::abs(y - oy)) - size) <= line;
      };
      c.Fill(white, [&](float x, float y) {
        return square(cx - shift, cy - shift, x, y) || square(cx + shift, cy + shift, x, y);
      });
    }
  } else if (button.substr(0, 5) == "dpad_") {
    const float arm = s * 0.16f, reach = s * 0.44f;
    auto cross = [&](float inset) {
      return [=](float x, float y) {
        return (std::abs(x - cx) <= arm - inset && std::abs(y - cy) <= reach - inset) ||
               (std::abs(y - cy) <= arm - inset && std::abs(x - cx) <= reach - inset);
      };
    };
    c.Fill(rim, cross(0.0f));
    c.Fill(dark, cross(1.2f));
    const char d = button[5];  // u, d, l, r
    c.Fill(white, [&](float x, float y) {
      float px = x - cx, py = y - cy;
      float along = d == 'u' ? -py : d == 'd' ? py : d == 'l' ? -px : px;
      float side = d == 'u' || d == 'd' ? px : py;
      return along >= reach * 0.35f && along <= reach - 2.0f && std::abs(side) <= arm - 2.0f;
    });
  } else {  // the sticks: a ring with L or R, clicks marked, moves with arrows
    const bool left = button[0] == 'l';
    c.Fill(rim, disc(R));
    c.Fill(dark, disc(R - edge));
    const bool click = button.find("click") != std::string_view::npos;
    const wchar_t* text = click ? (ps || style == kSteamDeck ? (left ? L"L3" : L"R3") : (left ? L"LS" : L"RS"))
                                : (left ? L"L" : L"R");
    c.Text(white, text, click ? 0.7f : 0.6f);
    if (button == "ls_ud" || button == "ls_lr" || button == "rs_ud" || button == "rs_lr") {
      const bool vertical = button.substr(3) == "ud";
      const float tip = R - 1.5f, base = R * 0.62f, half = s * 0.09f;
      c.Fill(light, [&](float x, float y) {
        float along = vertical ? std::abs(y - cy) : std::abs(x - cx), side = vertical ? x - cx : y - cy;
        return along >= base && along <= tip && std::abs(side) <= half * (tip - along) / (tip - base);
      });
    }
  }
  c.Write(tex, pitch, icon.x0, icon.y0);
}

// AUTO: the keys, or the Xbox 360's own buttons with a controller plugged in.
Style ResolvedStyle() {
  Style style = Style(std::clamp(g_prompt_style, 0, int(kSteamDeck)));
  if (style == kAuto) {
    XINPUT_STATE pad{};
    bool connected = false;
    for (DWORD i = 0; i < 4 && !connected; ++i) connected = XInputGetState(i, &pad) == ERROR_SUCCESS;
    style = connected ? kXbox360 : kKeys;
  }
  return style;
}

// BF_UI_DUMP=1 (finding a game's icon atlases): each texture up to 1024 x
// 1024 as it loads, once per content hash, logged; the tiled 8888 ones also
// saved untiled (ARGB bytes) to ui_dump/<width>x<height>_<hash>.argb.
void DumpTexture(uint32_t physical, uint32_t size, uint32_t width, uint32_t height, uint32_t format, bool tiled) {
  static std::mutex m;
  static std::unordered_map<uint64_t, bool> seen;
  if (width > 1024 || height > 1024 || width < 16 || height < 16) return;
  const uint8_t* tex = G<uint8_t>(0xA0000000u + (physical & 0x1FFFFFFF));
  uint64_t h = Fnv1a(tex, std::min<size_t>(size, size_t(width) * height * 4));
  std::lock_guard l(m);
  if (seen.size() > 3000 || !seen.emplace(h, true).second) return;
  RT_INFO("ui dump: %ux%u format %u%s hash %016llx at %08X", width, height, format, tiled ? " tiled" : "",
          (unsigned long long)h, physical);
  if (format != 6 || !tiled || size < width * height * 4) return;
  CreateDirectoryA("ui_dump", nullptr);
  char path[96];
  snprintf(path, sizeof(path), "ui_dump/%ux%u_%016llx.argb", width, height, (unsigned long long)h);
  std::vector<uint8_t> out(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) memcpy(&out[(size_t(y) * width + x) * 4], tex + TiledOffset(x, y, width, 2), 4);
  if (FILE* f = fopen(path, "wb")) fwrite(out.data(), 1, out.size(), f), fclose(f);
}

void OnTextureLoad(uint32_t physical, uint32_t size, uint32_t width, uint32_t height,
                   uint32_t format, bool tiled) {
  constexpr uint32_t k_8_8_8_8 = 6;
  static const bool dump = getenv("BF_UI_DUMP") != nullptr;
  if (dump) DumpTexture(physical, size, width, height, format, tiled);
  if (format != k_8_8_8_8 || !tiled) return;
  for (const Atlas& a : Atlases()) {
    if (a.width != width || a.height != height || size < width * height * 4) continue;
    uint8_t* tex = G<uint8_t>(0xA0000000u + (physical & 0x1FFFFFFF));
    if (Fnv1a(tex, size_t(width) * height * 4) != a.hash) continue;
    const Style style = ResolvedStyle();
    if (style == kXbox360) {  // the game's own pictures
      RT_INFO("ui: %s atlas left with the Xbox 360's buttons", a.name);
      return;
    }
    for (const Icon& icon : a.icons) {
      const char* button = style == kKeys ? nullptr : PadButton(icon.label);
      if (button) DrawPadIcon(tex, width, icon, style, button);
      else DrawKeyCap(tex, width, icon);
      // The Back key: its prompts can be clicked (xam.cpp).
      if (std::wstring_view(icon.label) == L"⌫") {
        xgpu::AddBackIcon(physical, width, height, icon.x0, icon.y0, icon.x1 + 1, icon.y1 + 1);
      }
    }
    RT_INFO("ui: %s button icons drawn into %s atlas (%08X)",
            style == kKeys ? "PC key" : style == kXboxSeries ? "Xbox Series" : style == kPs3 ? "PlayStation 3"
            : style == kPs5 ? "PlayStation 5" : "Steam Deck", a.name, physical);
    return;
  }
}

// PC wording for the English console strings. Each replacement is no longer
// than the original, so it is written in place and the rest zero-filled.
const std::unordered_map<std::string_view, std::string_view>& PcText() {
  static const std::unordered_map<std::string_view, std::string_view> kText = {
      {"PRESS START TO BEGIN", "PRESS ANY KEY"},
      // Help & Options' last entry opens the PC settings page (pc_settings.h).
      {"GAME CREDITS", "PC SETTINGS"},
      // The main menu's multiplayer entries open LAN pages (lan.cpp).
      {"QUICK MATCH", "HOST GAME"},
      {"JOIN MATCH WITH FRIENDS", "JOIN GAME"},
      {"INVITE XBOX LIVE PARTY", "INVITE TO PARTY"},
      {"JOIN XBOX LIVE PARTY MEMBER", "JOIN PARTY MEMBER"},
      {"SEE GAMER CARD", "SEE PROFILE"},
      {"VIEW GAMER CARD", "VIEW PROFILE"},
      {"WHEN YOU SEE THIS ICON, THE GAME IS SAVING. DO NOT TURN OFF THE XBOX 360 CONSOLE OR "
       "REMOVE THE XBOX 360 MEMORY UNIT WHILE THE GAME IS SAVING.",
       "WHEN YOU SEE THIS ICON, THE GAME IS SAVING. DO NOT CLOSE THE GAME OR TURN OFF YOUR "
       "COMPUTER WHILE THE GAME IS SAVING."},
      {"YOU NEED TO BE SIGNED INTO XBOX LIVE TO ACCESS XBOX LIVE GAME PLAY FEATURES.",
       "YOU NEED TO BE SIGNED IN TO ACCESS ONLINE GAME PLAY FEATURES."},
      {"THE XBOX LIVE SERVICE IS BUSY.", "THE ONLINE SERVICE IS BUSY."},
      {"YOU DID NOT CHOOSE A GAMER PROFILE. PLAYING ON XBOX LIVE WILL NOT BE POSSIBLE. TO PLAY "
       "ONLINE, PLEASE CHOOSE A VALID PROFILE.",
       "YOU DID NOT CHOOSE A PROFILE. PLAYING ONLINE WILL NOT BE POSSIBLE. TO PLAY ONLINE, "
       "PLEASE CHOOSE A VALID PROFILE."},
      {"YOU DID NOT CHOOSE A GAMER PROFILE. PLAYING ON XBOX LIVE WILL NOT BE POSSIBLE. TO PLAY "
       "ONLINE, PLEASE CHOOSE A VALID GAMER PROFILE.",
       "YOU DID NOT CHOOSE A PROFILE. PLAYING ONLINE WILL NOT BE POSSIBLE. TO PLAY ONLINE, "
       "PLEASE CHOOSE A VALID PROFILE."},
      {"YOU DID NOT CHOOSE A GAMER PROFILE. ONLINE GAMEPLAY WILL NOT BE POSSIBLE. TO PLAY "
       "ONLINE, PLEASE CHOOSE A VALID GAMER PROFILE.",
       "YOU DID NOT CHOOSE A PROFILE. ONLINE GAMEPLAY WILL NOT BE POSSIBLE. TO PLAY ONLINE, "
       "PLEASE CHOOSE A VALID PROFILE."},
      {"COULD NOT FIND ACCOUNT INFORMATION FOR INVITED GAMERTAG",
       "COULD NOT FIND ACCOUNT INFORMATION FOR INVITED PLAYER"},
      {"FAILED TO CONNECT TO XBOX LIVE", "FAILED TO CONNECT TO SERVER"},
      {"YOUR GAMER PROFILE DOES NOT HAVE THE CORRECT PERMISSIONS TO ACCESS XBOX LIVE "
       "FUNCTIONALITY.",
       "YOUR PROFILE DOES NOT HAVE THE CORRECT PERMISSIONS TO ACCESS ONLINE FUNCTIONALITY."},
      {"YOU MUST INSTALL AN XBOX LIVE UPDATE BEFORE PLAYING ONLINE.",
       "YOU MUST INSTALL A GAME UPDATE BEFORE PLAYING ONLINE."},
      {"CONNECTION TO XBOX LIVE HAS BEEN LOST", "CONNECTION TO SERVER HAS BEEN LOST"},
      {"COULD NOT REACH XBOX LIVE TO COMPLETE ACCOUNT CREATION",
       "COULD NOT REACH SERVER TO COMPLETE ACCOUNT CREATION"},
      {"ARE YOU SURE YOU WANT TO JOIN THIS XBOX LIVE GAME? YOU WILL EXIT ANY CURRENT GAME.",
       "ARE YOU SURE YOU WANT TO JOIN THIS ONLINE GAME? YOU WILL EXIT ANY CURRENT GAME."},
      {"YOU CANNOT JOIN SOMEONE WHO IS PLAYING ON THE SAME CONSOLE AS YOU.",
       "YOU CANNOT JOIN SOMEONE WHO IS PLAYING ON THE SAME PC AS YOU."},
      {"READING FROM YOUR XBOX 360 MEMORY UNIT, PLEASE DO NOT REMOVE IT FROM YOUR  XBOX 360 "
       "CONSOLE",
       "READING SAVE DATA, PLEASE DO NOT CLOSE THE GAME"},
      {"PLEASE RECONNECT THE XBOX 360 CONTROLLER", "PLEASE RECONNECT THE CONTROLLER"},
      {"NO GAMER PROFILE CHOSEN", "NO PROFILE CHOSEN"},
      {"READING XBOX 360 MEMORY UNIT ", "READING SAVE DATA"},
      {"RECONNECT THE XBOX 360 CONTROLLER", "RECONNECT THE CONTROLLER"},
      {"XBOX LIVE", "ONLINE"},
      {"YOU WERE DISCONNECTED FROM XBOX LIVE", "DISCONNECTED FROM THE SERVER"},
      {"YOU ARE ALSO REWARDED ONE GAMER PIC AND A UNIQUE PREORDER XBOX THEME.",
       "YOU ARE ALSO REWARDED A UNIQUE PREORDER BONUS."},
      {"IF YOU ARE 13 AND OVER, YOU WILL NEED AN EA ACCOUNT TO PLAY ONLINE. BY PRESSING SUBMIT, "
       "YOU ACKNOWLEDGE THAT MICROSOFT WILL SEND YOUR XBOX ACCOUNT INFO OVER TO ELECTRONIC ARTS "
       "IN THE U.S. TO SET UP YOUR EA ACCOUNT.",
       "IF YOU ARE 13 AND OVER, YOU WILL NEED AN EA ACCOUNT TO PLAY ONLINE. BY PRESSING SUBMIT, "
       "YOU ACKNOWLEDGE THAT YOUR ACCOUNT INFO WILL BE SENT TO THE ONLINE SERVER TO SET UP YOUR "
       "EA ACCOUNT."},
      {"CONTROLLER SENSITIVITY", "LOOK SENSITIVITY"},
      {"GAMERTAG", "PLAYER"},
      {"Gamertag", "Player"},
      {"DELETING PLAYER PROFILE DATA. PLEASE DO NOT TURN OFF YOUR XBOX 360 CONSOLE.",
       "DELETING PLAYER PROFILE DATA. PLEASE DO NOT CLOSE THE GAME."},
      {"LOADING PLAYER PROFILE DATA. PLEASE DO NOT TURN OFF YOUR XBOX 360 CONSOLE.",
       "LOADING PLAYER PROFILE DATA. PLEASE DO NOT CLOSE THE GAME."},
      {"SAVING PLAYER PROFILE DATA. PLEASE DO NOT TURN OFF YOUR XBOX 360 CONSOLE.",
       "SAVING PLAYER PROFILE DATA. PLEASE DO NOT CLOSE THE GAME."},
      {"YOU MUST SIGN IN A GAMER PROFILE TO SAVE YOUR SETTINGS. CONTINUE WITHOUT SAVING.",
       "YOU MUST SIGN IN A PROFILE TO SAVE YOUR SETTINGS. CONTINUE WITHOUT SAVING."},
      {"YOU ARE NOT SIGNED IN WITH AN ACTIVE GAMER PROFILE. YOU WILL NOT BE ABLE TO SAVE OR LOAD "
       "FILES TO YOUR XBOX 360 CONSOLE WITHOUT AN ACTIVE GAMER PROFILE.",
       "YOU ARE NOT SIGNED IN WITH AN ACTIVE PROFILE. YOU WILL NOT BE ABLE TO SAVE OR LOAD "
       "FILES WITHOUT AN ACTIVE PROFILE."},
      {"YOU HAVE SIGNED OUT YOUR GAMER PROFILE DURING THE PROCESS. SAVE FAILED.",
       "YOU HAVE SIGNED OUT YOUR PROFILE DURING THE PROCESS. SAVE FAILED."},
  };
  return kText;
}

}  // namespace

namespace rt {
void InstallPcUi() { xe::gpu::g_texture_load_hook = OnTextureLoad; }

// Rewrites the console strings in a block read from build_overlay-00.fbrb (the
// localization tables are stored uncompressed there). Only strings that are
// zero-terminated on both sides inside the block are touched.
void PatchPcText(uint8_t* data, size_t size) {
  const auto& text = PcText();
  int count = 0;
  size_t start = 0;
  bool bounded = false;  // string start follows a zero inside the block
  for (size_t i = 0; i < size; ++i) {
    if (data[i] != 0) continue;
    if (bounded && i > start) {
      auto it = text.find(std::string_view(reinterpret_cast<const char*>(data + start), i - start));
      if (it != text.end()) {
        memset(data + start, 0, i - start);
        memcpy(data + start, it->second.data(), it->second.size());
        ++count;
      }
    }
    start = i + 1;
    bounded = true;
  }
  if (count) RT_INFO("ui: %d console text strings rewritten for PC", count);
}
}  // namespace rt
