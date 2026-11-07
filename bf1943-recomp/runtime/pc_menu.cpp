// The PC menus' renderer: see pc_menu.h. Paper and olive textures are made
// once (value noise, stains, fibres); each frame copies them into the canvas
// and GDI+ draws the type (Stardos Stencil and Special Elite, both free fonts
// shipped in fonts/ next to the exe; Impact and Courier New if missing).
#include "kernel.h"
#include "pc_menu.h"
#include "xenia_bridge.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#pragma comment(lib, "gdiplus.lib")

namespace rt::pcmenu {

// ---- layout ------------------------------------------------------------------

constexpr int kBand = 700;         // the olive band's width (button screens)
constexpr int kRowTop = 330, kRowH = 52;
constexpr int kListL = 110, kListR = 1810;

Rect ButtonRect(int i) { return {60, 470 + i * 92, 660, 470 + i * 92 + 80}; }
Rect TabRect(int i) { return {kListL + i * 286, 236, kListL + i * 286 + 274, 300}; }
Rect RowRect(int i) { return {kListL, kRowTop + i * kRowH, kListR, kRowTop + i * kRowH + kRowH - 4}; }
Rect LeftArrowRect(int i) {
  Rect r = RowRect(i);
  return {r.r - 560, r.t, r.r - 490, r.b};
}
Rect RightArrowRect(int i) {
  Rect r = RowRect(i);
  return {r.r - 70, r.t, r.r, r.b};
}
Rect PromptRect(int i) { return {kListL + i * 330, 985, kListL + i * 330 + 310, 1041}; }

namespace {

using namespace Gdiplus;

// ---- colours -------------------------------------------------------------------

const Color kInk(255, 38, 34, 28);
const Color kPaperInk(255, 236, 226, 198);
const Color kOlive(255, 78, 86, 36);
const Color kOliveDark(255, 50, 56, 22);
const Color kRed(255, 152, 38, 28);
const Color kFaded(255, 112, 102, 84);

// ---- textures --------------------------------------------------------------------

uint32_t Hash(int x, int y, int s) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(s) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float Rand(int x, int y, int s) { return (Hash(x, y, s) & 0xFFFF) / 65535.0f; }
float Noise(float x, float y, int s) {
  int xi = int(std::floor(x)), yi = int(std::floor(y));
  float fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
  float a = Rand(xi, yi, s), b = Rand(xi + 1, yi, s), c = Rand(xi, yi + 1, s), d = Rand(xi + 1, yi + 1, s);
  return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
float Fbm(float x, float y, int s) {
  float v = 0, amp = 0.5f;
  for (int o = 0; o < 5; ++o, x *= 2.03f, y *= 2.03f, amp *= 0.5f) v += amp * Noise(x, y, s + o);
  return v;
}

// BGRA pixels, kW x kH.
std::vector<uint32_t> MakePaper() {
  std::vector<uint32_t> px(size_t(kW) * kH);
  // A few coffee rings and creases.
  struct Ring { float x, y, r; };
  const Ring rings[] = {{1520, 260, 120}, {1660, 820, 70}, {980, 930, 160}};
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      float n = Fbm(x / 260.0f, y / 260.0f, 1);
      float fine = Rand(x, y, 7) - 0.5f;
      float fibre = Noise(x / 2.0f, y / 40.0f, 3) - 0.5f;
      float r = 222, g = 210, b = 178;
      float shade = (n - 0.5f) * 34 + fine * 9 + fibre * 6;
      // Vignette: older, darker edges.
      float dx = (x - kW * 0.5f) / (kW * 0.5f), dy = (y - kH * 0.5f) / (kH * 0.5f);
      float edge = std::max(0.0f, std::sqrt(dx * dx * 0.8f + dy * dy) - 0.55f);
      shade -= edge * 70;
      for (const Ring& ring : rings) {
        float wob = (Fbm(std::atan2(y - ring.y, x - ring.x) * 3.0f + 10, ring.r, 31) - 0.5f) * ring.r * 0.25f;
        float d = std::hypot(x - ring.x, y - ring.y) + wob;
        float band = std::exp(-std::pow((d - ring.r) / 5.0f, 2.0f)) * 0.6f +
                     (d < ring.r ? 0.12f : 0.0f) * Fbm(x / 40.0f, y / 40.0f, 9);
        shade -= band * 22 * Fbm(x / 25.0f, y / 25.0f, 33) * 1.6f;
      }
      // Creases: a fold down the middle and across.
      float crease = std::exp(-std::pow((x - 1310) / 2.5f, 2.0f)) + std::exp(-std::pow((y - 540) / 2.5f, 2.0f)) * 0.7f;
      shade -= crease * 16;
      shade += std::exp(-std::pow((x - 1314) / 4.0f, 2.0f)) * 8;
      int R = std::clamp(int(r + shade), 0, 255), G = std::clamp(int(g + shade * 0.97f), 0, 255),
          B = std::clamp(int(b + shade * 0.85f), 0, 255);
      px[size_t(y) * kW + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
    }
  }
  return px;
}

// The olive band: painted canvas, worn lighter in patches, a scuffed edge.
void PaintOlive(std::vector<uint32_t>& px, int x0, int x1) {
  for (int y = 0; y < kH; ++y) {
    for (int x = x0; x < x1; ++x) {
      float n = Fbm(x / 180.0f, y / 180.0f, 21);
      float weave = (Noise(x / 1.5f, y / 1.5f, 22) - 0.5f) * 10 + ((x + y) % 3 == 0 ? 2.0f : 0.0f);
      float wear = std::max(0.0f, Fbm(x / 60.0f, y / 60.0f, 23) - 0.62f) * 120;
      float shade = (n - 0.5f) * 30 + weave + wear;
      // Scuffed right edge.
      float edge = x1 - x + (Fbm(0.0f, y / 30.0f, 24) - 0.5f) * 30;
      if (edge < 0) continue;
      float lip = edge < 10 ? (10 - edge) * 3.0f : 0.0f;
      int R = std::clamp(int(78 + shade + lip), 0, 255), G = std::clamp(int(86 + shade + lip), 0, 255),
          B = std::clamp(int(36 + shade * 0.6f + lip * 0.5f), 0, 255);
      px[size_t(y) * kW + x] = 0xFF000000u | (R << 16) | (G << 8) | B;
    }
  }
}

// ---- fonts -------------------------------------------------------------------------

struct Fonts {
  PrivateFontCollection collection;
  std::unique_ptr<FontFamily> stencil, type;
};
Fonts* g_fonts = nullptr;

std::filesystem::path ExeDir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

void LoadFonts() {
  g_fonts = new Fonts;
  auto dir = ExeDir() / "fonts";
  g_fonts->collection.AddFontFile((dir / "StardosStencil-Bold.ttf").c_str());
  g_fonts->collection.AddFontFile((dir / "SpecialElite-Regular.ttf").c_str());
  int n = g_fonts->collection.GetFamilyCount();
  std::vector<FontFamily> fams(std::max(n, 1));
  int found = 0;
  if (n > 0) g_fonts->collection.GetFamilies(n, fams.data(), &found);
  for (int i = 0; i < found; ++i) {
    wchar_t name[LF_FACESIZE] = {};
    fams[i].GetFamilyName(name);
    if (wcsstr(name, L"Stencil")) g_fonts->stencil.reset(fams[i].Clone());
    if (wcsstr(name, L"Special Elite")) g_fonts->type.reset(fams[i].Clone());
  }
  if (!g_fonts->stencil) g_fonts->stencil = std::make_unique<FontFamily>(L"Impact");
  if (!g_fonts->type) g_fonts->type = std::make_unique<FontFamily>(L"Courier New");
  RT_INFO("menu: fonts %s", found >= 2 ? "loaded" : "missing (fonts/ next to the exe): using Impact, Courier New");
}

Font Stencil(float px) { return Font(g_fonts->stencil.get(), px, FontStyleBold, UnitPixel); }
Font Type(float px) { return Font(g_fonts->type.get(), px, FontStyleRegular, UnitPixel); }

// ---- drawing helpers ---------------------------------------------------------------

Color Fade(const Color& c, float a) { return Color(BYTE(std::clamp(c.GetA() * a, 0.0f, 255.0f)), c.GetR(), c.GetG(), c.GetB()); }

void Text(Graphics& g, const std::wstring& s, const Font& f, const Color& c, float x, float y, float w, float h,
          StringAlignment h_align = StringAlignmentNear, StringAlignment v_align = StringAlignmentCenter) {
  StringFormat fmt;
  fmt.SetAlignment(h_align);
  fmt.SetLineAlignment(v_align);
  fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  fmt.SetTrimming(StringTrimmingEllipsisCharacter);
  SolidBrush b(c);
  g.DrawString(s.c_str(), int(s.size()), &f, RectF(x, y, w, h), &fmt, &b);
}

float TextWidth(Graphics& g, const std::wstring& s, const Font& f) {
  RectF box;
  g.MeasureString(s.c_str(), int(s.size()), &f, PointF(0, 0), &box);
  return box.Width;
}

// A rubber stamp: a rotated double frame and stencil word, worn in places.
void Stamp(Graphics& g, const std::wstring& word, float cx, float cy, float angle, float alpha) {
  GraphicsState s = g.Save();
  g.TranslateTransform(cx, cy);
  g.RotateTransform(angle);
  Font f = Stencil(64);
  float w = TextWidth(g, word, f) + 50, h = 100;
  Pen p(Fade(kRed, alpha), 6), p2(Fade(kRed, alpha), 2);
  g.DrawRectangle(&p, -w / 2, -h / 2, w, h);
  g.DrawRectangle(&p2, -w / 2 + 10, -h / 2 + 10, w - 20, h - 20);
  Text(g, word, f, Fade(kRed, alpha), -w / 2, -h / 2, w, h, StringAlignmentCenter);
  // Worn: paper-coloured specks over the ink.
  SolidBrush speck(Color(BYTE(170 * alpha), 222, 210, 178));
  for (int i = 0; i < 140; ++i) {
    float x = (Rand(i, 1, 77) - 0.5f) * w, y = (Rand(i, 2, 77) - 0.5f) * h, r = 1 + Rand(i, 3, 77) * 4;
    g.FillEllipse(&speck, x, y, r, r);
  }
  g.Restore(s);
}

// A key cap prompt: a paper tag with the key, the label in typewriter.
void Prompt(Graphics& g, const Rect& r, const std::wstring& key, const std::wstring& label, bool on_dark) {
  Font kf = Stencil(26), lf = Type(28);
  float kw = std::max(52.0f, TextWidth(g, key, kf) + 20);
  SolidBrush tag(on_dark ? kPaperInk : kInk);
  g.FillRectangle(&tag, float(r.l), float(r.t + 8), kw, 40.0f);
  Text(g, key, kf, on_dark ? kInk : kPaperInk, float(r.l), float(r.t + 8), kw, 40, StringAlignmentCenter);
  Text(g, label, lf, on_dark ? kPaperInk : kInk, r.l + kw + 14, float(r.t), float(r.r - r.l) - kw - 14, 56);
}

// ---- state ----------------------------------------------------------------------------

std::mutex g_mutex;
std::condition_variable g_wake;
View g_view;
bool g_visible = false, g_changed = false;

struct Anim {
  bool shown = false;
  double open_at = 0, page_at = 0;
  int page = -1;
  float sel = -1, tab = -1;
};

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
float Ease(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return 1 - (1 - t) * (1 - t) * (1 - t);
}

// ---- the screens ----------------------------------------------------------------------

void DrawButtons(Graphics& g, const View& v, Anim& a, double now, float dt) {
  float open = Ease(float((now - a.open_at) / 0.35));
  float slide = (1 - open) * -kBand;
  // Band.
  g.TranslateTransform(slide, 0);
  Text(g, L"BATTLEFIELD", Stencil(84), kPaperInk, 66, 78, 640, 110);
  Text(g, L"1943", Stencil(200), Color(255, 196, 160, 92), 62, 160, 620, 220);
  Text(g, v.subtitle, Type(30), kPaperInk, 76, 382, 600, 50);
  Pen rule(Fade(kPaperInk, 0.7f), 3);
  g.DrawLine(&rule, 76.0f, 434.0f, 640.0f, 434.0f);
  // The selection: a paper strip that slides to the selected entry.
  float target = float(ButtonRect(v.button).t);
  if (a.sel < 0) a.sel = target;
  a.sel += (target - a.sel) * std::min(1.0f, dt * 16);
  SolidBrush strip(kPaperInk);
  g.FillRectangle(&strip, 44.0f, a.sel, 620.0f, 80.0f);
  SolidBrush marker(kRed);
  PointF tri[3] = {{52, a.sel + 24}, {52, a.sel + 56}, {74, a.sel + 40}};
  g.FillPolygon(&marker, tri, 3);
  for (size_t i = 0; i < v.buttons.size(); ++i) {
    Rect r = ButtonRect(int(i));
    float closeness = 1 - std::min(1.0f, std::abs(a.sel - r.t) / 80.0f);
    Color c(255, BYTE(236 + (38 - 236) * closeness), BYTE(226 + (34 - 226) * closeness), BYTE(198 + (28 - 198) * closeness));
    Text(g, v.buttons[i], Stencil(58), c, r.l + 40.0f, float(r.t), float(r.r - r.l - 40), float(r.b - r.t));
  }
  // Prompts on the band.
  for (size_t i = 0; i < v.prompts.size() && i < 3; ++i) {
    Rect r{76 + int(i) * 280, 985, 76 + int(i) * 280 + 270, 1041};
    Prompt(g, r, v.prompts[i].first, v.prompts[i].second, true);
  }
  g.ResetTransform();
  if (!v.opaque) return;
  // The paper side: orders typed out, a stamp.
  float fade = Ease(float((now - a.open_at - 0.15) / 0.4));
  Text(g, L"OPERATION ORDER No. 43", Stencil(54), Fade(kInk, fade), 820, 120, 900, 70);
  Pen thin(Fade(kInk, fade), 2);
  g.DrawLine(&thin, 820.0f, 196.0f, 1760.0f, 196.0f);
  const wchar_t* lines[] = {L"FROM:  PACIFIC COMMAND", L"TO:    ALL UNITS", L"RE:    " , L""};
  for (int i = 0; i < 3; ++i) {
    std::wstring s = lines[i];
    if (i == 2) s += v.buttons.empty() ? L"" : v.buttons[v.button];
    Text(g, s, Type(34), Fade(kInk, fade), 820, 220.0f + i * 50, 940, 50);
  }
  // The selected entry's note, typed.
  StringFormat fmt;
  SolidBrush ink(Fade(kInk, fade));
  Font body = Type(34);
  g.DrawString(v.desc.c_str(), int(v.desc.size()), &body, RectF(820, 400, 900, 400), &fmt, &ink);
  Stamp(g, L"TOP SECRET", 1500, 820, -12, 0.75f * fade);
}

void DrawList(Graphics& g, const View& v, Anim& a, double now, float dt) {
  float in = Ease(float((now - a.page_at) / 0.28));
  float off = (1 - in) * 80;
  Text(g, v.title, Stencil(96), kInk, kListL - 4.0f, 60, 1200, 110);
  Text(g, v.subtitle, Type(34), kRed, kListL + 4.0f, 168, 1200, 50);
  Stamp(g, L"CONFIDENTIAL", 1600, 140, 8, 0.55f);
  // Tabs: folder tabs, the open one olive.
  if (!v.tabs.empty()) {
    float target = float(TabRect(v.tab).l);
    if (a.tab < 0) a.tab = target;
    a.tab += (target - a.tab) * std::min(1.0f, dt * 16);
    SolidBrush back(Color(255, 200, 188, 154)), olive(kOlive);
    for (size_t i = 0; i < v.tabs.size(); ++i) {
      Rect r = TabRect(int(i));
      g.FillRectangle(&back, float(r.l), float(r.t), float(r.r - r.l), float(r.b - r.t));
    }
    g.FillRectangle(&olive, a.tab, float(TabRect(0).t) - 6, float(TabRect(0).r - TabRect(0).l), 70.0f);
    for (size_t i = 0; i < v.tabs.size(); ++i) {
      Rect r = TabRect(int(i));
      bool on = int(i) == v.tab;
      Text(g, v.tabs[i], Stencil(34), on ? kPaperInk : kInk, float(r.l), float(r.t - 4), float(r.r - r.l),
           float(r.b - r.t), StringAlignmentCenter);
    }
    if (v.tab_focus) {
      Rect r = TabRect(v.tab);
      Pen p(kRed, 4);
      g.DrawRectangle(&p, a.tab - 4, float(r.t) - 10, float(r.r - r.l) + 8, 78.0f);
    }
    Pen line(kOlive, 6);
    g.DrawLine(&line, float(kListL), 306.0f, float(kListR), 306.0f);
  }
  // Rows: slide in with the page; the selection slides between rows.
  g.TranslateTransform(off, 0);
  if (v.row >= 0) {
    float target = float(RowRect(v.row).t);
    if (a.sel < 0) a.sel = target;
    a.sel += (target - a.sel) * std::min(1.0f, dt * 18);
    SolidBrush olive(kOlive);
    g.FillRectangle(&olive, float(kListL), a.sel, float(kListR - kListL), float(kRowH - 4));
  }
  Pen dots(Color(90, 38, 34, 28), 1);
  dots.SetDashStyle(DashStyleDot);
  for (size_t i = 0; i < v.rows.size(); ++i) {
    Rect r = RowRect(int(i));
    float closeness = v.row >= 0 ? 1 - std::min(1.0f, std::abs(a.sel - r.t) / float(kRowH)) : 0;
    auto mix = [&](const Color& x, const Color& y) {
      return Color(BYTE(255 * in), BYTE(x.GetR() + (y.GetR() - x.GetR()) * closeness),
                   BYTE(x.GetG() + (y.GetG() - x.GetG()) * closeness), BYTE(x.GetB() + (y.GetB() - x.GetB()) * closeness));
    };
    Color ink = mix(kInk, kPaperInk);
    g.DrawLine(&dots, float(r.l), float(r.b), float(r.r), float(r.b));
    const Row& row = v.rows[i];
    Text(g, row.label, Type(32), ink, r.l + 20.0f, float(r.t), 900, float(r.b - r.t));
    if (!row.tag.empty())
      Text(g, row.tag, Type(22), mix(kRed, kPaperInk), r.r - 760.0f, float(r.t), 190, float(r.b - r.t),
           StringAlignmentFar);
    Rect la = LeftArrowRect(int(i)), ra = RightArrowRect(int(i));
    if (row.choice) {
      Text(g, L"<", Stencil(36), ink, float(la.l), float(la.t), float(la.r - la.l), float(la.b - la.t),
           StringAlignmentCenter);
      Text(g, L">", Stencil(36), ink, float(ra.l), float(ra.t), float(ra.r - ra.l), float(ra.b - ra.t),
           StringAlignmentCenter);
    }
    Text(g, row.value, Stencil(34), ink, float(la.r), float(r.t), float(ra.l - la.r), float(r.b - r.t),
         StringAlignmentCenter);
  }
  if (v.more_above) Text(g, L"/\\", Stencil(30), kInk, kListR - 60.0f, kRowTop - 40.0f, 60, 36, StringAlignmentCenter);
  if (v.more_below)
    Text(g, L"\\/", Stencil(30), kInk, kListR - 60.0f, float(kRowTop + kVisibleRows * kRowH), 60, 36,
         StringAlignmentCenter);
  g.ResetTransform();
  // The note under the rows: typed on a strip.
  Pen rule(kInk, 3);
  g.DrawLine(&rule, float(kListL), 912.0f, float(kListR), 912.0f);
  Text(g, v.desc, Type(30), kInk, kListL + 4.0f, 918, float(kListR - kListL), 56);
  for (size_t i = 0; i < v.prompts.size(); ++i)
    Prompt(g, PromptRect(int(i)), v.prompts[i].first, v.prompts[i].second, false);
}

void DrawBusy(Graphics& g, const View& v, double now) {
  Text(g, L"STAND BY", Stencil(120), kInk, 0, 380, kW, 160, StringAlignmentCenter);
  int dots = int(now * 3) % 4;
  Text(g, v.desc + std::wstring(dots, L'.'), Type(40), kInk, 0, 540, kW, 70, StringAlignmentCenter);
  Stamp(g, L"IN TRANSIT", kW / 2.0f, 760, -6, 0.7f);
}

// ---- the thread ---------------------------------------------------------------------------

void RenderThread() {
  GdiplusStartupInput input;
  ULONG_PTR token;
  GdiplusStartup(&token, &input, nullptr);
  LoadFonts();
  std::vector<uint32_t> paper = MakePaper();
  std::vector<uint32_t> banded = paper;
  PaintOlive(banded, 0, kBand);
  Bitmap canvas(kW, kH, PixelFormat32bppARGB);
  std::vector<uint8_t> rgba(size_t(kW) * kH * 4);
  Anim anim;
  double last = Now();
  for (;;) {
    View v;
    bool visible;
    {
      std::unique_lock<std::mutex> l(g_mutex);
      // Animating: about 60 frames a second; else wait for a change.
      bool animating = anim.shown && (Now() - std::max(anim.open_at, anim.page_at) < 0.6 ||
                                      g_view.screen == Screen::kBusy);
      if (!g_changed) {
        if (animating) g_wake.wait_for(l, std::chrono::milliseconds(16));
        else g_wake.wait_for(l, std::chrono::milliseconds(50));
      }
      bool changed = g_changed;
      g_changed = false;
      v = g_view;
      visible = g_visible;
      if (!visible) {
        if (anim.shown) {
          anim = Anim{};
          l.unlock();
          xgpu::SetOverlay(nullptr, 0, 0);
        }
        continue;
      }
      // Selection still sliding?
      if (!changed && !animating) {
        float target = v.screen == Screen::kButtons ? float(ButtonRect(v.button).t)
                       : v.row >= 0                 ? float(RowRect(v.row).t)
                                                    : anim.sel;
        bool tab_moving = !v.tabs.empty() && std::abs(anim.tab - TabRect(v.tab).l) > 0.5f;
        if (std::abs(anim.sel - target) < 0.5f && !tab_moving) continue;
      }
    }
    double now = Now();
    float dt = float(std::min(0.1, now - last));
    last = now;
    if (!anim.shown) anim.shown = true, anim.open_at = now, anim.page_at = now, anim.page = v.page;
    if (v.page != anim.page) anim.page = v.page, anim.page_at = now, anim.sel = -1, anim.tab = -1;

    // Background.
    const std::vector<uint32_t>& bg = v.screen == Screen::kButtons ? banded : paper;
    BitmapData data;
    Gdiplus::Rect all(0, 0, kW, kH);
    canvas.LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data);
    for (int y = 0; y < kH; ++y) {
      uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride);
      const uint32_t* src = &bg[size_t(y) * kW];
      if (v.opaque || v.screen != Screen::kButtons) {
        std::memcpy(dst, src, kW * 4);
      } else {
        // Over the game: the band, then the game darkened behind it.
        std::memcpy(dst, src, kBand * 4);
        for (int x = kBand; x < kW; ++x) dst[x] = 0x9A141808u;
      }
    }
    canvas.UnlockBits(&data);
    {
      Graphics g(&canvas);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
      g.SetCompositingQuality(CompositingQualityHighQuality);
      if (v.screen == Screen::kButtons) DrawButtons(g, v, anim, now, dt);
      else if (v.screen == Screen::kList) DrawList(g, v, anim, now, dt);
      else DrawBusy(g, v, now);
    }
    // To RGBA, through the HDR curve when the output is HDR10.
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
      std::lock_guard<std::mutex> l(g_mutex);
      if (!g_visible) continue;  // hidden meanwhile
    }
    xgpu::SetOverlay(rgba.data(), kW, kH);
  }
}

void Start() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(RenderThread).detach(); });
}

}  // namespace

void Show(const View& view) {
  Start();
  std::lock_guard<std::mutex> l(g_mutex);
  g_view = view;
  g_visible = true;
  g_changed = true;
  g_wake.notify_one();
}

void Hide() {
  Start();
  {
    std::lock_guard<std::mutex> l(g_mutex);
    if (!g_visible) return;
    g_visible = false;
    g_changed = true;
    g_wake.notify_one();
  }
  xgpu::SetOverlay(nullptr, 0, 0);
}

}  // namespace rt::pcmenu
