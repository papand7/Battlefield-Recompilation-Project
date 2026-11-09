// Our Multiplayer menu for Bad Company: see bc_mp_menu.h. The logic runs on the
// pad poll (Update); a render thread draws each screen with GDI+ into a
// 1920x1080 image in the look of the game's own menus: the screen darkened, a
// big distressed title standing on its side by a white rule, white condensed
// capitals with the selected row on a white bar, a white-framed panel on the
// right and the pad's A/B prompts at the bottom. The type is Bahnschrift (a DIN
// face that comes with Windows, close to the game's), else Arial Narrow.
#include "kernel.h"
#include "lan.h"
#include "xenia_bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "bc_mp_menu.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace rt::bcmp {
namespace {

using namespace Gdiplus;

constexpr int kW = 1920, kH = 1080;

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring Upper(std::wstring s) {
  for (wchar_t& c : s) c = towupper(c);
  return s;
}

// ---- the maps ------------------------------------------------------------------------

struct Map {
  const char* level;
  const wchar_t* name;
  const wchar_t* desc;
};
// The level folders' maps (names and descriptions are the game's own strings).
const Map kMaps[] = {
    {"mp_harbour_day1", L"VALLEY RUN",
     L"At the head of a long valley, Russians are expanding and fortifying a dam facility when the US gets wind "
     L"of large amounts of gold hidden in the area."},
    {"mp_harbour_day2", L"DECONSTRUCTION",
     L"With the water facility captured, the US has a foothold for assaulting the construction area. Girders, "
     L"pipes, and scaffolding make this a tight, labyrinthine scramble and the fight to take the bridge will be "
     L"gruelling."},
    {"mp_ignition_day1", L"OASIS",
     L"A normally arid gully has been flooded after a local dam burst, threatening to overflow a local MEC "
     L"industrial gas facility."},
    {"mp_ignition_day2", L"FINAL IGNITION",
     L"The grimy gas facility is in stark contrast to the irrigated town and plantations surrounding it, and the "
     L"US will have a hard time dislodging the defenders from the fortress like facility."},
    {"mp_monastery_day1", L"END OF THE LINE",
     L"US forces have landed on the windswept plain below the imposing, secluded hilltop monastery, which houses a "
     L"secret listening station. Before they make their way to the top, they have to get rid of the Russians' "
     L"vehicle defense."},
    {"mp_monastery_day2", L"ASCENSION",
     L"The monastery's location means the area is, geographically speaking, easily defensible. The attackers are "
     L"well armed, but taking the defender's gold will be no easy task, as every window may contain an enemy "
     L"sniper."},
    {"mp_beachhead_day1", L"HARVEST DAY",
     L"Farms are torn apart by heavy caliber gunfire, as the Americans are forced to retreat to the local town "
     L"with their hard-won gold - will they be able to halt the rampaging Russian bear?"},
    {"mp_beachhead_day2", L"OVER AND OUT",
     L"Having been pushed out of the plains, the Americans have made off with large amounts of bullion, and holed "
     L"up in a captured Russian communication centre."},
};
constexpr int kMapCount = int(sizeof(kMaps) / sizeof(kMaps[0]));
std::wstring MapName(const std::string& level) {
  for (const Map& m : kMaps)
    if (level == m.level) return m.name;
  return Upper(Wide(level));
}

// ---- what is on screen (built by the logic, drawn by the render thread) -------------

struct Item {
  std::wstring label, value;
  bool arrows = false;  // < value > (left/right change it)
  bool caret = false;   // the value is being typed
};
struct View {
  std::wstring title;  // the big one on its side
  std::vector<Item> items;
  int sel = 0;
  std::wstring head, sub;            // the panel's heading
  std::vector<std::wstring> lines;   // and its text (wrapped)
  std::wstring busy_head, busy_text;  // a "please wait" box instead of the rest
  bool can_back = true;
  int page = 0;  // a new number: the items fade in again
};

std::mutex g_view_mutex;
std::condition_variable g_wake;
View g_view;
bool g_visible = false, g_changed = false;

// ---- the renderer ---------------------------------------------------------------------

// Value noise for the worn title and the grain.
float Hash(int x, int y) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
float Noise(float x, float y) {
  int ix = int(std::floor(x)), iy = int(std::floor(y));
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
  float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

struct Fonts {
  std::unique_ptr<FontFamily> body, bold, light, title;
};
Fonts g_fonts;
std::unique_ptr<FontFamily> PickFamily(std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* n : names) {
    auto f = std::make_unique<FontFamily>(n);
    if (f->IsAvailable()) return f;
  }
  return std::make_unique<FontFamily>(L"Arial");
}
void LoadFonts() {
  g_fonts.body = PickFamily({L"Bahnschrift SemiCondensed", L"Bahnschrift Condensed", L"Arial Narrow"});
  g_fonts.bold = PickFamily({L"Bahnschrift SemiBold SemiConden", L"Bahnschrift SemiBold Condensed", L"Arial Narrow"});
  g_fonts.light = PickFamily({L"Bahnschrift Light SemiCondensed", L"Bahnschrift SemiCondensed", L"Arial Narrow"});
  g_fonts.title = PickFamily({L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed", L"Impact"});
}

// Text with the game's wide letter spacing (GDI+ has none): one character at a time.
float TextWidth(Graphics& g, const std::wstring& s, const Font& font, float tracking) {
  const StringFormat* fmt = StringFormat::GenericTypographic();
  float w = 0;
  for (wchar_t c : s) {
    RectF box;
    wchar_t one[2] = {c, 0};
    g.MeasureString(one, 1, &font, PointF(0, 0), fmt, &box);
    w += (c == L' ' ? font.GetSize() * 0.28f : box.Width) + tracking;
  }
  return s.empty() ? 0 : w - tracking;
}
void Text(Graphics& g, const std::wstring& s, const Font& font, const Brush& brush, float x, float y, float tracking) {
  const StringFormat* fmt = StringFormat::GenericTypographic();
  for (wchar_t c : s) {
    RectF box;
    wchar_t one[2] = {c, 0};
    g.MeasureString(one, 1, &font, PointF(0, 0), fmt, &box);
    if (c != L' ') g.DrawString(one, 1, &font, PointF(x, y), fmt, &brush);
    x += (c == L' ' ? font.GetSize() * 0.28f : box.Width) + tracking;
  }
}
// Wrapped onto lines no wider than width.
std::vector<std::wstring> Wrap(Graphics& g, const std::wstring& s, const Font& font, float width) {
  std::vector<std::wstring> out;
  std::wstring line, word;
  auto flush_word = [&] {
    if (word.empty()) return;
    std::wstring t = line.empty() ? word : line + L" " + word;
    if (!line.empty() && TextWidth(g, t, font, 1) > width) out.push_back(line), line = word;
    else line = t;
    word.clear();
  };
  for (wchar_t c : s) {
    if (c == L' ') flush_word();
    else if (c == L'\n') flush_word(), out.push_back(line), line.clear();
    else word += c;
  }
  flush_word();
  if (!line.empty()) out.push_back(line);
  return out;
}

// The title, worn like the game's: white type eaten by noise, cached per text.
std::unique_ptr<Bitmap> MakeTitle(const std::wstring& text) {
  Font font(g_fonts.title.get(), 170, FontStyleRegular, UnitPixel);
  Bitmap probe(8, 8, PixelFormat32bppARGB);
  Graphics pg(&probe);
  int w = int(TextWidth(pg, text, font, 4)) + 40, h = 190;
  auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
  {
    Graphics g(bmp.get());
    g.Clear(Color(0, 0, 0, 0));
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    SolidBrush white(Color(255, 245, 245, 242));
    Text(g, text, font, white, 10, 2, 4);
  }
  BitmapData data;
  Gdiplus::Rect all(0, 0, w, h);
  bmp->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &data);
  for (int y = 0; y < h; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride);
    for (int x = 0; x < w; ++x) {
      uint32_t c = row[x];
      float a = float(c >> 24) / 255.0f;
      if (a <= 0) continue;
      // Blotches, scratches across, and fine specks.
      float m = Noise(x * 0.07f, y * 0.07f) * 0.45f + Noise(x * 0.3f, y * 0.3f) * 0.35f +
                Noise(x * 0.9f, y * 0.25f) * 0.2f;
      float keep = std::clamp((m - 0.24f) * 9.0f, 0.0f, 1.0f);
      if (Hash(x * 3 + 1, y * 7 + 5) < 0.05f) keep *= 0.1f;
      a *= keep;
      row[x] = (uint32_t(a * 255.0f) << 24) | (c & 0xFFFFFF);
    }
  }
  bmp->UnlockBits(&data);
  return bmp;
}

// The backdrop: the game's screen behind, darkened (more at the edges), with grain.
std::vector<uint32_t> MakeBackdrop() {
  std::vector<uint32_t> px(size_t(kW) * kH);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      float dx = (x - kW * 0.62f) / kW, dy = (y - kH * 0.45f) / kH;
      float vignette = std::min(1.0f, (dx * dx + dy * dy) * 1.6f);
      // Nearly black: the game's main menu must not show through, only a
      // hint of its moving background.
      float a = 0.91f + 0.07f * vignette;
      if (x < 900) a = std::max(a, 0.95f);
      float grain = (Hash(x, y) - 0.5f) * 10.0f;
      int v = std::clamp(int(9 + grain), 0, 255);
      px[size_t(y) * kW + x] = (uint32_t(std::min(a, 0.985f) * 255) << 24) | (v << 16) | (v << 8) | v;
    }
  return px;
}

// Layout.
constexpr float kRuleX = 292;                 // the white rule
constexpr float kRowL = 296, kRowR = 860;     // the rows (and the selection bar)
constexpr float kRowH = 38, kRowBottom = 905;  // the last row ends here
constexpr float kPanelL = 1030, kPanelT = 150, kPanelR = 1690, kPanelB = 790;
constexpr float kPromptY = 952;

float RowTop(int i, int n) { return kRowBottom - (n - i) * kRowH; }

void DrawPrompt(Graphics& g, float x, wchar_t letter, Color color, const std::wstring& label, bool enabled) {
  BYTE alpha = enabled ? 255 : 90;
  Color c(alpha, color.GetR(), color.GetG(), color.GetB());
  SolidBrush dark(Color(alpha, 22, 22, 22));
  Pen ring(c, 3.5f);
  g.FillEllipse(&dark, x, kPromptY - 21, 42.0f, 42.0f);
  g.DrawEllipse(&ring, x, kPromptY - 21, 42.0f, 42.0f);
  Font f(g_fonts.bold.get(), 28, FontStyleRegular, UnitPixel);
  SolidBrush lb(c);
  std::wstring l(1, letter);
  float lw = TextWidth(g, l, f, 0);
  Text(g, l, f, lb, x + 21 - lw / 2, kPromptY - 17, 0);
  Font lf(g_fonts.body.get(), 34, FontStyleRegular, UnitPixel);
  SolidBrush white(Color(alpha, 235, 235, 232));
  Text(g, label, lf, white, x + 58, kPromptY - 19, 5);
}

void DrawSpinner(Graphics& g, float cx, float cy, float r, double now) {
  // The game's busy icon: a ring with two black and two white quarters, turning.
  SolidBrush white(Color(255, 240, 240, 240)), black(Color(255, 10, 10, 10));
  Pen ring(Color(255, 240, 240, 240), 3.0f);
  float start = float(std::fmod(now * 240.0, 360.0));
  RectF box(cx - r, cy - r, 2 * r, 2 * r);
  g.FillEllipse(&black, box);
  g.FillPie(&white, box, start, 90);
  g.FillPie(&white, box, start + 180, 90);
  g.DrawEllipse(&ring, box);
}

struct Anim {
  bool shown = false;
  int page = -1;
  double page_at = 0;
  float bar = -1;  // the selection bar's top
};

void Draw(Graphics& g, const View& v, Anim& anim, double now, float dt,
          std::map<std::wstring, std::unique_ptr<Bitmap>>& titles) {
  if (v.page != anim.page) anim.page = v.page, anim.page_at = now, anim.bar = -1;
  float fade = float(std::clamp((now - anim.page_at) / 0.22, 0.0, 1.0));
  BYTE fa = BYTE(255 * fade);

  // The title on its side, reading upwards, its foot by the rows.
  if (!v.title.empty()) {
    auto& bmp = titles[v.title];
    if (!bmp) bmp = MakeTitle(v.title);
    GraphicsState st = g.Save();
    g.TranslateTransform(kRuleX - 196, kRowBottom + 6);
    g.RotateTransform(-90);
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, fade, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    g.DrawImage(bmp.get(), Gdiplus::Rect(0, 0, int(bmp->GetWidth()), int(bmp->GetHeight())), 0, 0,
                int(bmp->GetWidth()), int(bmp->GetHeight()), UnitPixel, &ia);
    g.Restore(st);
  }
  // The rule, top to bottom.
  SolidBrush rule(Color(235, 240, 240, 236));
  g.FillRectangle(&rule, kRuleX - 2, 0.0f, 4.0f, float(kH));

  if (!v.busy_head.empty()) {
    // "PLEASE WAIT" in the game's dialog box.
    RectF box(kW / 2.0f - 380, 300, 760, 300);
    SolidBrush black(Color(250, 0, 0, 0));
    Pen border(Color(255, 240, 240, 240), 3.5f);
    g.FillRectangle(&black, box);
    g.DrawRectangle(&border, box);
    Font hf(g_fonts.bold.get(), 40, FontStyleRegular, UnitPixel), bf(g_fonts.body.get(), 36, FontStyleRegular, UnitPixel);
    SolidBrush white(Color(255, 240, 240, 240));
    Text(g, v.busy_head, hf, white, box.X + 26, box.Y + 26, 3);
    Text(g, v.busy_text, bf, white, box.X + 26, box.Y + 76, 3);
    DrawSpinner(g, box.GetRight() - 70, box.GetBottom() - 70, 34, now);
    return;
  }

  // The rows, the selected one on a white bar that slides to it.
  int n = int(v.items.size());
  if (n) {
    float target = RowTop(v.sel, n);
    if (anim.bar < 0) anim.bar = target;
    anim.bar += (target - anim.bar) * std::min(1.0f, dt * 20.0f);
    SolidBrush bar(Color(fa, 244, 244, 240));
    g.FillRectangle(&bar, kRowL, anim.bar + 2, kRowR - kRowL, kRowH - 5);
  }
  Font rf(g_fonts.body.get(), 32, FontStyleRegular, UnitPixel);
  for (int i = 0; i < n; ++i) {
    const Item& it = v.items[i];
    float top = RowTop(i, n);
    bool on = std::abs(anim.bar - top) < kRowH * 0.5f;
    SolidBrush ink(on ? Color(fa, 12, 12, 12) : Color(BYTE(fa * 0.92f), 236, 236, 232));
    Text(g, it.label, rf, ink, kRowL + 20, top + 4, 3);
    if (!it.value.empty() || it.caret) {
      std::wstring val = it.value;
      if (it.caret && std::fmod(now, 1.0) < 0.55) val += L"_";
      else if (it.caret) val += L" ";
      if (it.arrows) val = L"<  " + val + L"  >";
      float w = TextWidth(g, val, rf, 3);
      Text(g, val, rf, ink, kRowR - 22 - w, top + 4, 3);
    }
  }

  // The panel: a white frame, a heading and some text.
  if (!v.head.empty() || !v.lines.empty()) {
    SolidBrush inside(Color(BYTE(fa * 0.45f), 0, 0, 0));
    Pen frame(Color(fa, 240, 240, 236), 3.5f);
    RectF box(kPanelL, kPanelT, kPanelR - kPanelL, kPanelB - kPanelT);
    g.FillRectangle(&inside, box);
    g.DrawRectangle(&frame, box);
    Font hf(g_fonts.bold.get(), 56, FontStyleRegular, UnitPixel);
    Font sf(g_fonts.body.get(), 30, FontStyleRegular, UnitPixel);
    Font lf(g_fonts.light.get(), 30, FontStyleRegular, UnitPixel);
    SolidBrush white(Color(fa, 242, 242, 238)), grey(Color(fa, 170, 170, 165));
    float y = kPanelT + 34;
    Text(g, v.head, hf, white, kPanelL + 38, y, 2);
    y += 70;
    if (!v.sub.empty()) Text(g, v.sub, sf, grey, kPanelL + 40, y, 4), y += 46;
    Pen line(Color(BYTE(fa * 0.6f), 240, 240, 236), 2.0f);
    g.DrawLine(&line, kPanelL + 38, y + 4, kPanelR - 38, y + 4);
    y += 28;
    for (const std::wstring& para : v.lines) {
      if (para.empty()) {
        y += 18;
        continue;
      }
      for (const std::wstring& l : Wrap(g, para, lf, kPanelR - kPanelL - 80)) {
        if (y > kPanelB - 44) break;
        Text(g, l, lf, white, kPanelL + 40, y, 1);
        y += 40;
      }
    }
  }

  DrawPrompt(g, kRowL + 12, L'A', Color(255, 108, 190, 52), L"SELECT", true);
  DrawPrompt(g, kRowL + 262, L'B', Color(255, 215, 50, 40), L"BACK", v.can_back);
}

void RenderThread() {
  GdiplusStartupInput input;
  ULONG_PTR token;
  GdiplusStartup(&token, &input, nullptr);
  LoadFonts();
  std::vector<uint32_t> backdrop = MakeBackdrop();
  Bitmap canvas(kW, kH, PixelFormat32bppARGB);
  std::vector<uint8_t> rgba(size_t(kW) * kH * 4);
  std::map<std::wstring, std::unique_ptr<Bitmap>> titles;
  Anim anim;
  double last = Now();
  for (;;) {
    View v;
    {
      std::unique_lock<std::mutex> l(g_view_mutex);
      // About 60 frames a second while open (the bar slides, the caret and
      // spinner move); waiting while hidden.
      g_wake.wait_for(l, std::chrono::milliseconds(g_visible ? 16 : 200));
      if (!g_visible) {
        if (anim.shown) {
          anim = Anim{};
          l.unlock();
          xgpu::SetOverlay(nullptr, 0, 0);
        }
        continue;
      }
      g_changed = false;
      v = g_view;
    }
    double now = Now();
    float dt = float(std::min(0.1, now - last));
    last = now;
    anim.shown = true;

    BitmapData data;
    Gdiplus::Rect all(0, 0, kW, kH);
    canvas.LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data);
    for (int y = 0; y < kH; ++y)
      std::memcpy(static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride, &backdrop[size_t(y) * kW], kW * 4);
    canvas.UnlockBits(&data);
    {
      Graphics g(&canvas);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
      g.SetCompositingQuality(CompositingQualityHighQuality);
      g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
      Draw(g, v, anim, now, dt, titles);
    }
    // To RGBA, through the HDR curve when the output is HDR10 (as pc_menu.cpp).
    float white = xgpu::HdrWhite();
    static float lut_white = -1;
    static uint8_t lut[256];
    if (white != lut_white) {
      lut_white = white;
      for (int i = 0; i < 256; ++i) {
        if (white <= 0.0f) {
          lut[i] = uint8_t(i);
        } else {
          double y = std::pow(i / 255.0, 2.2) * white / 10000.0;
          double m = std::pow(y, 0.1593017578125);
          double pq = std::pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375);
          lut[i] = uint8_t(std::clamp(pq * 255.0 + 0.5, 0.0, 255.0));
        }
      }
    }
    canvas.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data);
    for (int y = 0; y < kH; ++y) {
      const uint32_t* src =
          reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride);
      uint8_t* o = &rgba[size_t(y) * kW * 4];
      for (int x = 0; x < kW; ++x, o += 4) {
        uint32_t c = src[x];
        o[0] = lut[(c >> 16) & 0xFF], o[1] = lut[(c >> 8) & 0xFF], o[2] = lut[c & 0xFF], o[3] = uint8_t(c >> 24);
      }
    }
    canvas.UnlockBits(&data);
    {
      std::lock_guard<std::mutex> l(g_view_mutex);
      if (!g_visible) continue;
    }
    xgpu::SetOverlay(rgba.data(), kW, kH);
  }
}

void Show(const View& v) {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(RenderThread).detach(); });
  std::lock_guard<std::mutex> l(g_view_mutex);
  g_view = v;
  g_visible = true;
  g_changed = true;
  g_wake.notify_one();
}
void Hide() {
  {
    std::lock_guard<std::mutex> l(g_view_mutex);
    g_visible = false;
    g_wake.notify_one();
  }
  xgpu::SetOverlay(nullptr, 0, 0);
}

// ---- the logic -------------------------------------------------------------------------

enum Page { kMain, kHost, kBrowser, kAddress, kBusy };

std::atomic<bool> g_open_request{false}, g_open{false};
std::mutex g_logic_mutex;
Page g_page = kMain, g_busy_back = kMain;
int g_sel = 0, g_page_id = 0, g_map = 0;
std::string g_address;
std::wstring g_note;  // a line for the panel (an error)
std::vector<lan::Server> g_servers;
bool g_searching = false;
double g_busy_at = 0;
std::wstring g_busy_text;
bool g_wait_release = false;  // after closing: the pad stays ours until let go

const wchar_t* const kMainItems[] = {L"HOST GAME", L"SERVER BROWSER", L"JOIN BY ADDRESS", L"BACK"};

void Go(Page p, int sel = 0) {
  g_page = p;
  g_sel = sel;
  g_note.clear();
  ++g_page_id;
  if (p == kBrowser) lan::Refresh(), g_searching = true;
}

View Build() {
  View v;
  v.page = g_page_id;
  std::wstring player = L"PLAYING AS " + Upper(Wide(g_gamertag));
  switch (g_page) {
    case kMain: {
      v.title = L"MULTIPLAYER";
      for (const wchar_t* s : kMainItems) v.items.push_back({s});
      static const wchar_t* heads[] = {L"HOST GAME", L"SERVER BROWSER", L"JOIN BY ADDRESS", L"MAIN MENU"};
      static const wchar_t* texts[] = {
          L"Start a Gold Rush game on one of the multiplayer maps. Players on your network find it in the "
          L"SERVER BROWSER.",
          L"Find the games hosted on your network and join one.",
          L"Join a host by its address, for a game the browser can't see (another network, a VPN).",
          L"Back to the main menu."};
      v.head = heads[g_sel], v.sub = L"LOCAL NETWORK";
      v.lines = {texts[g_sel], L"", player};
      if (g_sel == 3) v.sub.clear(), v.lines = {texts[3]};
      break;
    }
    case kHost: {
      v.title = L"HOST GAME";
      v.items = {{L"MAP", kMaps[g_map].name, true}, {L"START GAME"}, {L"BACK"}};
      v.head = kMaps[g_map].name, v.sub = L"GOLD RUSH";
      if (*kMaps[g_map].desc) v.lines = {kMaps[g_map].desc, L""};
      v.lines.push_back(L"Players on your network will see this game in their SERVER BROWSER.");
      v.lines.push_back(L"");
      v.lines.push_back(player);
      break;
    }
    case kBrowser: {
      v.title = L"SERVERS";
      for (const lan::Server& s : g_servers) v.items.push_back({Upper(Wide(s.name)), MapName(s.map)});
      v.items.push_back({L"REFRESH"});
      v.items.push_back({L"BACK"});
      int ns = int(g_servers.size());
      if (g_searching) {
        v.head = L"SEARCHING", v.lines = {L"Looking for games on your network..."};
      } else if (g_sel < ns) {
        const lan::Server& s = g_servers[g_sel];
        v.head = Upper(Wide(s.name)), v.sub = MapName(s.map) + L"  -  GOLD RUSH";
        v.lines = {L"ADDRESS  " + Wide(s.address) + L":" + std::to_wstring(s.port),
                   L"PING  " + std::to_wstring(s.ping_ms) + L" MS", L"", L"Press A to join."};
      } else if (ns == 0) {
        v.head = L"NO GAMES FOUND";
        v.lines = {L"Nobody on your network is hosting a game right now. Host one, or REFRESH to look again."};
      } else {
        v.head = std::to_wstring(ns) + (ns == 1 ? L" GAME FOUND" : L" GAMES FOUND");
        v.lines = {L"Pick a game to see its details and join it."};
      }
      break;
    }
    case kAddress: {
      v.title = L"JOIN";
      v.items = {{L"ADDRESS", Wide(g_address), false, g_sel == 0}, {L"JOIN GAME"}, {L"BACK"}};
      v.head = L"JOIN BY ADDRESS", v.sub = L"IPV4 ADDRESS";
      v.lines = {L"Type the host's address with the keyboard, for example 192.168.1.20. Add :port when the "
                 L"host's server isn't on the usual one (" + std::to_wstring(1003 + g_base_port_offset) + L").",
                 L""};
      if (!g_note.empty()) v.lines.push_back(g_note);
      break;
    }
    case kBusy:
      v.title = L"MULTIPLAYER";
      v.busy_head = L"PLEASE WAIT", v.busy_text = g_busy_text;
      v.can_back = false;
      break;
  }
  v.sel = std::clamp(g_sel, 0, std::max(0, int(v.items.size()) - 1));
  return v;
}

void Close() {
  g_open = false;
  g_wait_release = true;
  Hide();
}

void StartBusy(const std::wstring& text, Page back) {
  g_busy_text = text;
  g_busy_back = back;
  g_busy_at = Now();
  Go(kBusy);
}

void HostGame() {
  const Map& m = kMaps[g_map];
  RT_INFO("mp menu: hosting %s", m.level);
  lan::Host(m.level, 0);
  StartBusy(L"STARTING GAME", kHost);
}

void JoinGame(const std::string& address_port, const std::string& map, const std::wstring& name) {
  RT_INFO("mp menu: joining %s (%s)", address_port.c_str(), map.c_str());
  if (!lan::PrepareJoin(address_port, map.empty() ? kMaps[0].level : map)) {
    g_note = L"That isn't an address: type four numbers with dots, e.g. 192.168.1.20.";
    return;
  }
  RequestHost();
  StartBusy(L"JOINING " + name, g_page);
}

void Accept() {
  switch (g_page) {
    case kMain:
      if (g_sel == 0) Go(kHost);
      else if (g_sel == 1) Go(kBrowser);
      else if (g_sel == 2) Go(kAddress);
      else Close();
      break;
    case kHost:
      if (g_sel == 0) g_map = (g_map + 1) % kMapCount;
      else if (g_sel == 1) HostGame();
      else Go(kMain, 0);
      break;
    case kBrowser: {
      int ns = int(g_servers.size());
      if (g_sel < ns) {
        const lan::Server& s = g_servers[g_sel];
        JoinGame(s.address + ":" + std::to_string(s.port), s.map, Upper(Wide(s.name)));
      } else if (g_sel == ns) {
        Go(kBrowser, ns);
      } else {
        Go(kMain, 1);
      }
      break;
    }
    case kAddress:
      if (g_sel <= 1) {
        if (g_address.empty()) g_note = L"Type the host's address first.", g_sel = 0;
        else JoinGame(g_address, "", Upper(Wide(g_address)));
      } else {
        Go(kMain, 2);
      }
      break;
    case kBusy:
      break;
  }
}

void Back() {
  switch (g_page) {
    case kMain: Close(); break;
    case kHost: Go(kMain, 0); break;
    case kBrowser: Go(kMain, 1); break;
    case kAddress: Go(kMain, 2); break;
    case kBusy: break;
  }
}

int ItemCount() {
  switch (g_page) {
    case kMain: return 4;
    case kHost: return 3;
    case kBrowser: return int(g_servers.size()) + 2;
    case kAddress: return 3;
    default: return 0;
  }
}

void DoOpen() {
  // Back from a game: no longer hosting or joining anyone.
  g_join_address = 0;
  g_lan_host = false;
  g_host_map.clear();
  g_open = true;
  g_wait_release = true;  // the A that chose MULTIPLAYER
  Go(kMain, 0);
  RT_INFO("mp menu: open");
}

bool OurWindowInFront() {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace

// The main menu's MULTIPLAYER only goes on (to presenceRequestOnline, where
// bfbc1_game.cpp opens this menu) for a profile signed in to LIVE.
static const bool g_live = (g_report_live = true);
// Bad Company's LAN games: their own tag and beacon port, so BF1943 games don't
// show, set before main() (--host starts the beacon there).
static const bool g_lan_game = (lan::SetGame("BFBC", 19431), true);

void Open() { g_open_request = true; }
bool IsOpen() { return g_open; }

bool Update(const XINPUT_GAMEPAD& pad) {
  std::unique_lock<std::mutex> lock(g_logic_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return g_open || g_wait_release;
  if (g_open_request.exchange(false)) DoOpen();

  // The keyboard (the game window in front): edges since the last poll.
  static bool down[256] = {}, pressed[256] = {};
  bool front = OurWindowInFront(), any_key = false;
  for (int vk = 8; vk < 256; ++vk) {
    bool d = front && (GetAsyncKeyState(vk) & 0x8000) != 0;
    pressed[vk] = d && !down[vk];
    down[vk] = d;
    any_key |= d;
  }
  // The pad: edges, the d-pad and left stick repeating while held.
  static WORD prev = 0;
  WORD b = pad.wButtons;
  if (pad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
  if (pad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
  if (pad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
  if (pad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
  WORD edge = b & ~prev;
  prev = b;
  const WORD kDirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
  static WORD repeat_dir = 0;
  static double repeat_at = 0;
  double now = Now();
  if (edge & kDirs) repeat_dir = edge & kDirs, repeat_at = now + 0.35;
  else if ((b & repeat_dir) && now >= repeat_at) edge |= repeat_dir, repeat_at = now + 0.09;
  else if (!(b & repeat_dir)) repeat_dir = 0;

  if (!g_open) {
    if (g_wait_release) {
      if (b || any_key) return true;
      g_wait_release = false;
    }
    return false;
  }
  if (g_wait_release) {
    if (b || any_key) {
      Show(Build());
      return true;
    }
    g_wait_release = false;
    edge = 0;
    std::memset(pressed, 0, sizeof(pressed));
  }

  // Typing the address: the keys are text, not the pad's buttons too.
  bool typing = g_page == kAddress && g_sel == 0;
  if (typing && any_key) edge = 0;

  if (g_page == kBusy) {
    // Until the level loads (the game then takes over); else back after a while.
    if (!g_in_menus) {
      Close();
      return true;
    }
    if (now - g_busy_at > 90) {
      Go(g_busy_back);
      g_note = L"The game didn't start. Try again.";
    }
    Show(Build());
    return true;
  }
  if (g_page == kBrowser && g_searching && !lan::Browsing()) {
    g_searching = false;
    g_servers.clear();
    for (const lan::Server& s : lan::Found())
      if (!s.map.empty()) g_servers.push_back(s);  // a beacon that isn't hosting answers with no map
    g_sel = std::min(g_sel, ItemCount() - 1);
  }

  int dv = ((edge & XINPUT_GAMEPAD_DPAD_DOWN) || pressed[VK_DOWN]) - ((edge & XINPUT_GAMEPAD_DPAD_UP) || pressed[VK_UP]);
  int dh = ((edge & XINPUT_GAMEPAD_DPAD_RIGHT) || pressed[VK_RIGHT]) - ((edge & XINPUT_GAMEPAD_DPAD_LEFT) || pressed[VK_LEFT]);
  bool accept = (edge & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START)) || pressed[VK_RETURN] || (!typing && pressed[VK_SPACE]);
  bool back = (edge & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) || pressed[VK_ESCAPE] || (!typing && pressed[VK_BACK]);

  if (typing) {
    bool shift = down[VK_SHIFT];
    for (int vk = 8; vk < 256; ++vk) {
      if (!pressed[vk] || g_address.size() >= 21) continue;
      if (vk >= '0' && vk <= '9' && !shift) g_address += char(vk);
      else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) g_address += char('0' + vk - VK_NUMPAD0);
      else if (vk == VK_OEM_PERIOD || vk == VK_DECIMAL) g_address += '.';
      else if (vk == VK_OEM_1 && shift) g_address += ':';
    }
    if (pressed[VK_BACK] && !g_address.empty()) g_address.pop_back();
    if (pressed[VK_RETURN]) accept = true, g_sel = 1;
  }

  int count = ItemCount();
  if (dv) g_sel = (g_sel + dv + count) % count;
  if (dh && g_page == kHost && g_sel == 0) g_map = (g_map + dh + kMapCount) % kMapCount;
  if (accept) Accept();
  else if (back) Back();
  if (g_open) Show(Build());
  return true;
}

}  // namespace rt::bcmp
