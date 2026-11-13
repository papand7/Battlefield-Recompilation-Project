// The PC menus' renderer: see pc_menu.h. Paper and olive textures are made
// once (value noise, stains, fibres); each frame copies them into the canvas
// and GDI+ draws the type (Stardos Stencil and Special Elite, both free fonts
// shipped in fonts/ next to the exe; Impact and Courier New if missing).
#include "kernel.h"
#include "pc_menu.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#else
#include "posix_gdiplus.h"  // (Linux: GDI+)
#endif

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#ifdef _WIN32
#include <gdiplus.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#pragma comment(lib, "gdiplus.lib")
#endif

namespace rt::pcmenu {

// ---- layout ------------------------------------------------------------------

constexpr int kBand = 700;         // the olive band's width (button screens)
// Bad Company's look (SetBadCompanyLook) moves the list right of its title.
bool g_bc = false;
int kRowTop = 330, kRowH = 52;
int kListL = 110, kListR = 1810;
int kTabT = 236, kTabB = 300, kTabStep = 286, kPromptT = 985;

void SetBadCompanyLook() {
  g_bc = true;
  kRowTop = 262, kRowH = 50, kListL = 340, kListR = 1800;
  kTabT = 168, kTabB = 216, kTabStep = (kListR - kListL + 12) / 6, kPromptT = 930;
}

Rect ButtonRect(int i) {
  if (g_bc) return {kListL, 420 + i * 80, kListL + 720, 420 + i * 80 + 66};
  return {60, 470 + i * 92, 660, 470 + i * 92 + 80};
}
Rect TabRect(int i) { return {kListL + i * kTabStep, kTabT, kListL + i * kTabStep + kTabStep - 12, kTabB}; }
Rect RowRect(int i) { return {kListL, kRowTop + i * kRowH, kListR, kRowTop + i * kRowH + kRowH - 4}; }
Rect LeftArrowRect(int i) {
  Rect r = RowRect(i);
  return {r.r - 560, r.t, r.r - 490, r.b};
}
Rect RightArrowRect(int i) {
  Rect r = RowRect(i);
  return {r.r - 70, r.t, r.r, r.b};
}
Rect PromptRect(int i) { return {kListL + i * 330, kPromptT, kListL + i * 330 + 310, kPromptT + 56}; }

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

// ---- Bad Company's look ---------------------------------------------------------------------
// As the game's own menus (and bc_mp_menu.cpp): the game darkened behind,
// the page title worn white on its side left of a white rule, white
// condensed capitals with the game's wide spacing, the selected row on a
// white bar, key caps for the prompts. The type is Bahnschrift (a DIN face
// that comes with Windows, close to the game's), else Arial Narrow.

struct BcFonts {
  std::unique_ptr<FontFamily> body, bold, title;
};
BcFonts* g_bc_fonts = nullptr;
std::unique_ptr<FontFamily> BcFamily(std::initializer_list<const wchar_t*> names) {
  for (const wchar_t* n : names) {
    auto f = std::make_unique<FontFamily>(n);
    if (f->IsAvailable()) return f;
  }
  return std::make_unique<FontFamily>(L"Arial");
}
void LoadBcFonts() {
  g_bc_fonts = new BcFonts;
  g_bc_fonts->body = BcFamily({L"Bahnschrift SemiCondensed", L"Bahnschrift Condensed", L"Arial Narrow"});
  g_bc_fonts->bold = BcFamily({L"Bahnschrift SemiBold SemiConden", L"Bahnschrift SemiBold Condensed", L"Arial Narrow"});
  g_bc_fonts->title = BcFamily({L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed", L"Impact"});
}
Font BcBody(float px) { return Font(g_bc_fonts->body.get(), px, FontStyleRegular, UnitPixel); }
Font BcBold(float px) { return Font(g_bc_fonts->bold.get(), px, FontStyleRegular, UnitPixel); }

const Color kBcWhite(255, 240, 240, 236), kBcBlack(255, 12, 12, 12), kBcGrey(255, 160, 160, 155);

// Text with the game's wide letter spacing (GDI+ has none): one character at a time.
float BcWidth(Graphics& g, const std::wstring& s, const Font& font, float tracking) {
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
void BcText(Graphics& g, const std::wstring& s, const Font& font, const Color& color, float x, float y,
            float tracking, float max_w = 1e9f) {
  const StringFormat* fmt = StringFormat::GenericTypographic();
  SolidBrush brush(color);
  float x0 = x;
  for (wchar_t c : s) {
    RectF box;
    wchar_t one[2] = {c, 0};
    g.MeasureString(one, 1, &font, PointF(0, 0), fmt, &box);
    float step = (c == L' ' ? font.GetSize() * 0.28f : box.Width) + tracking;
    if (x + step - x0 > max_w) break;
    if (c != L' ') g.DrawString(one, 1, &font, PointF(x, y), fmt, &brush);
    x += step;
  }
}
// Centred vertically in a row from top to bottom.
void BcRowText(Graphics& g, const std::wstring& s, const Font& f, const Color& c, float x, float top, float bottom,
               float tracking, float max_w = 1e9f) {
  BcText(g, s, f, c, x, top + (bottom - top - f.GetSize() * 1.18f) / 2, tracking, max_w);
}

// Value noise for the worn title.
float BcHash(int x, int y) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
float BcNoise(float x, float y) {
  int ix = int(std::floor(x)), iy = int(std::floor(y));
  float fx = x - ix, fy = y - iy;
  fx = fx * fx * (3 - 2 * fx), fy = fy * fy * (3 - 2 * fy);
  float a = BcHash(ix, iy), b = BcHash(ix + 1, iy), c = BcHash(ix, iy + 1), d = BcHash(ix + 1, iy + 1);
  return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}
// The title, worn like the game's: white type eaten by noise, made once per text.
Bitmap* BcTitle(const std::wstring& text) {
  // (Never freed: the render thread can still be drawing at exit.)
  static std::wstring& made_for = *new std::wstring;
  static std::unique_ptr<Bitmap>& bmp = *new std::unique_ptr<Bitmap>;
  if (bmp && made_for == text) return bmp.get();
  made_for = text;
  Font font(g_bc_fonts->title.get(), 170, FontStyleRegular, UnitPixel);
  Bitmap probe(8, 8, PixelFormat32bppARGB);
  Graphics pg(&probe);
  int w = int(BcWidth(pg, text, font, 4)) + 40, h = 190;
  bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
  {
    Graphics g(bmp.get());
    g.Clear(Color(0, 0, 0, 0));
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    BcText(g, text, font, Color(255, 245, 245, 242), 10, 2, 4);
  }
  BitmapData data;
  Gdiplus::Rect all(0, 0, w, h);
  bmp->LockBits(&all, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppARGB, &data);
  for (int y = 0; y < h; ++y) {
    uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(data.Scan0) + size_t(y) * data.Stride);
    for (int x = 0; x < w; ++x) {
      float a = float(row[x] >> 24) / 255.0f;
      if (a <= 0) continue;
      // Blotches, scratches across, and fine specks.
      float m = BcNoise(x * 0.07f, y * 0.07f) * 0.45f + BcNoise(x * 0.3f, y * 0.3f) * 0.35f +
                BcNoise(x * 0.9f, y * 0.25f) * 0.2f;
      float keep = std::clamp((m - 0.24f) * 9.0f, 0.0f, 1.0f);
      if (BcHash(x * 3 + 1, y * 7 + 5) < 0.05f) keep *= 0.1f;
      row[x] = (uint32_t(a * keep * 255.0f) << 24) | (row[x] & 0xFFFFFF);
    }
  }
  bmp->UnlockBits(&data);
  return bmp.get();
}
// The game darkened behind (more at the edges and on the left), with grain.
std::vector<uint32_t> MakeBcBackdrop() {
  std::vector<uint32_t> px(size_t(kW) * kH);
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      float dx = (x - kW * 0.62f) / kW, dy = (y - kH * 0.45f) / kH;
      // Nearly black: the game's menu under it must not show through, only
      // a hint of the picture behind.
      // Solid where the game draws its own menus (left, and the prompts along
      // the bottom), easing to 0.93 towards the top right.
      float a = 0.93f + 0.05f * std::min(1.0f, (dx * dx + dy * dy) * 1.6f);
      float solid = std::max(std::clamp((1000.0f - x) / 150.0f, 0.0f, 1.0f), std::clamp((y - 880.0f) / 80.0f, 0.0f, 1.0f));
      a += (1.0f - a) * solid;
      int v = std::clamp(int(9 + (BcHash(x, y) - 0.5f) * 10.0f), 0, 255);
      px[size_t(y) * kW + x] = (uint32_t(std::min(a, 1.0f) * 255) << 24) | (v << 16) | (v << 8) | v;
    }
  return px;
}

constexpr float kBcRuleX = 292;

void BcTitleAndRule(Graphics& g, const std::wstring& title, float fade) {
  if (!title.empty()) {
    Bitmap* bmp = BcTitle(title);
    GraphicsState st = g.Save();
    // On its side, reading upwards, its foot near the bottom.
    g.TranslateTransform(kBcRuleX - 196, 905 + 6);
    g.RotateTransform(-90);
    ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, fade, 0}, {0, 0, 0, 0, 1}}};
    ImageAttributes ia;
    ia.SetColorMatrix(&cm);
    g.DrawImage(bmp, Gdiplus::Rect(0, 0, int(bmp->GetWidth()), int(bmp->GetHeight())), 0, 0,
                int(bmp->GetWidth()), int(bmp->GetHeight()), UnitPixel, &ia);
    g.Restore(st);
  }
  SolidBrush rule(Color(235, 240, 240, 236));
  g.FillRectangle(&rule, kBcRuleX - 2, 0.0f, 4.0f, float(kH));
}

// A prompt: the key in a white-ringed rounded cap, its label in capitals.
void BcPrompt(Graphics& g, const Rect& r, const std::wstring& key, const std::wstring& label) {
  Font kf = BcBold(22), lf = BcBody(32);
  float kw = std::max(42.0f, BcWidth(g, key, kf, 1) + 22), top = float(r.t) + 7, h = 42;
  GraphicsPath cap;
  float rr = 10;
  cap.AddArc(float(r.l), top, rr * 2, rr * 2, 180, 90);
  cap.AddArc(r.l + kw - rr * 2, top, rr * 2, rr * 2, 270, 90);
  cap.AddArc(r.l + kw - rr * 2, top + h - rr * 2, rr * 2, rr * 2, 0, 90);
  cap.AddArc(float(r.l), top + h - rr * 2, rr * 2, rr * 2, 90, 90);
  cap.CloseFigure();
  SolidBrush dark(Color(255, 22, 22, 22));
  Pen ring(kBcWhite, 3);
  g.FillPath(&dark, &cap);
  g.DrawPath(&ring, &cap);
  BcText(g, key, kf, kBcWhite, r.l + (kw - BcWidth(g, key, kf, 1)) / 2, top + 8, 1);
  BcText(g, label, lf, kBcWhite, r.l + kw + 16, top + 2, 4, float(r.r - r.l) - kw - 16);
}

void DrawBcList(Graphics& g, const View& v, Anim& a, double now, float dt) {
  float in = Ease(float((now - a.page_at) / 0.22));
  BYTE fa = BYTE(255 * in);
  auto faded = [&](const Color& c, float k = 1.0f) { return Color(BYTE(fa * k), c.GetR(), c.GetG(), c.GetB()); };
  BcTitleAndRule(g, v.title, std::min(1.0f, float((now - a.open_at) / 0.22)));
  // Tabs: capitals in a row, the open one on a white bar (sliding), a line under.
  if (!v.tabs.empty()) {
    float target = float(TabRect(v.tab).l);
    if (a.tab < 0) a.tab = target;
    a.tab += (target - a.tab) * std::min(1.0f, dt * 18);
    Rect t0 = TabRect(0);
    SolidBrush bar(kBcWhite);
    g.FillRectangle(&bar, a.tab, float(t0.t), float(t0.r - t0.l), float(t0.b - t0.t));
    Font tf = BcBold(30);
    for (size_t i = 0; i < v.tabs.size(); ++i) {
      Rect r = TabRect(int(i));
      bool on = std::abs(a.tab - r.l) < (r.r - r.l) * 0.5f;
      float w = BcWidth(g, v.tabs[i], tf, 3);
      BcRowText(g, v.tabs[i], tf, on ? kBcBlack : kBcWhite, r.l + (r.r - r.l - w) / 2, float(r.t), float(r.b), 3);
    }
    if (v.tab_focus) {
      Rect r = TabRect(v.tab);
      Pen p(kBcWhite, 3);
      g.DrawRectangle(&p, a.tab - 5, float(r.t) - 5, float(r.r - r.l) + 10, float(r.b - r.t) + 10);
    }
    SolidBrush line(Color(200, 240, 240, 236));
    g.FillRectangle(&line, float(kListL), float(t0.b) + 18, float(kListR - kListL), 2.0f);
  }
  // Rows: the selected one on a white bar that slides to it.
  if (v.row >= 0) {
    float target = float(RowRect(v.row).t);
    if (a.sel < 0) a.sel = target;
    a.sel += (target - a.sel) * std::min(1.0f, dt * 20);
    SolidBrush bar(faded(kBcWhite));
    g.FillRectangle(&bar, float(kListL), a.sel + 2, float(kListR - kListL), float(kRowH - 6));
  }
  Font rf = BcBody(32), vf = BcBold(32), tagf = BcBody(22);
  for (size_t i = 0; i < v.rows.size(); ++i) {
    Rect r = RowRect(int(i));
    bool on = v.row >= 0 && std::abs(a.sel - r.t) < kRowH * 0.5f;
    Color ink = on ? faded(kBcBlack) : faded(kBcWhite, 0.92f);
    const Row& row = v.rows[i];
    float top = float(r.t) + 2, bottom = float(r.b) - 4;
    BcRowText(g, row.label, rf, ink, r.l + 20.0f, top, bottom, 3, 760);
    if (!row.tag.empty()) {
      float w = BcWidth(g, row.tag, tagf, 2);
      BcRowText(g, row.tag, tagf, on ? faded(Color(255, 150, 20, 16)) : faded(Color(255, 225, 90, 70)),
                r.r - 580.0f - w, top, bottom, 2);
    }
    Rect la = LeftArrowRect(int(i)), ra = RightArrowRect(int(i));
    if (row.choice) {
      BcRowText(g, L"<", vf, ink, la.l + 24.0f, top, bottom, 0);
      BcRowText(g, L">", vf, ink, ra.l + 30.0f, top, bottom, 0);
    }
    float vw = BcWidth(g, row.value, vf, 3);
    float span = float(ra.l - la.r);
    BcRowText(g, row.value, vf, ink, la.r + std::max(0.0f, (span - vw) / 2), top, bottom, 3, span);
  }
  if (v.more_above) BcText(g, L"▲", BcBody(24), faded(kBcWhite), kListR - 40.0f, kRowTop - 34.0f, 0);
  if (v.more_below) BcText(g, L"▼", BcBody(24), faded(kBcWhite), kListR - 40.0f, float(kRowTop + kVisibleRows * kRowH) + 2, 0);
  // The note under the rows, on up to two lines.
  float note_y = float(kRowTop + kVisibleRows * kRowH) + 30;
  Font nf = BcBody(28);
  std::wstring line, word;
  int lines = 0;
  auto put = [&](bool last) {
    if (lines < 2 && !line.empty()) BcText(g, line, nf, faded(kBcGrey), float(kListL) + 4, note_y + lines * 36.0f, 2, float(kListR - kListL));
    ++lines, line.clear();
    (void)last;
  };
  for (size_t i = 0; i <= v.desc.size(); ++i) {
    wchar_t c = i < v.desc.size() ? v.desc[i] : L' ';
    if (c != L' ') { word += c; continue; }
    if (word.empty()) continue;
    std::wstring t = line.empty() ? word : line + L" " + word;
    if (!line.empty() && BcWidth(g, t, nf, 2) > kListR - kListL - 8) put(false), t = word;
    line = t, word.clear();
  }
  put(true);
  for (size_t i = 0; i < v.prompts.size(); ++i) BcPrompt(g, PromptRect(int(i)), v.prompts[i].first, v.prompts[i].second);
}

void DrawBcButtons(Graphics& g, const View& v, Anim& a, double now, float dt) {
  float in = Ease(float((now - a.open_at) / 0.22));
  BcTitleAndRule(g, v.title.empty() ? L"PC" : v.title, in);
  if (!v.subtitle.empty()) BcText(g, v.subtitle, BcBody(32), kBcGrey, float(kListL), 330, 4);
  float target = float(ButtonRect(v.button).t);
  if (a.sel < 0) a.sel = target;
  a.sel += (target - a.sel) * std::min(1.0f, dt * 20);
  Rect b0 = ButtonRect(0);
  SolidBrush bar(kBcWhite);
  g.FillRectangle(&bar, float(b0.l), a.sel, float(b0.r - b0.l), float(b0.b - b0.t));
  Font bf = BcBold(46);
  for (size_t i = 0; i < v.buttons.size(); ++i) {
    Rect r = ButtonRect(int(i));
    bool on = std::abs(a.sel - r.t) < (r.b - r.t) * 0.5f;
    BcRowText(g, v.buttons[i], bf, on ? kBcBlack : kBcWhite, r.l + 24.0f, float(r.t), float(r.b), 4);
  }
  if (!v.desc.empty()) BcText(g, v.desc, BcBody(30), kBcGrey, 1100, 420, 2, 760);
  for (size_t i = 0; i < v.prompts.size(); ++i) BcPrompt(g, PromptRect(int(i)), v.prompts[i].first, v.prompts[i].second);
}

void DrawBcBusy(Graphics& g, const View& v, double now) {
  RectF box(kW / 2.0f - 380, 380, 760, 260);
  SolidBrush black(Color(250, 0, 0, 0));
  Pen border(kBcWhite, 3.5f);
  g.FillRectangle(&black, box);
  g.DrawRectangle(&border, box);
  BcText(g, L"PLEASE WAIT", BcBold(40), kBcWhite, box.X + 26, box.Y + 26, 3);
  int dots = int(now * 3) % 4;
  BcText(g, v.desc + std::wstring(dots, L'.'), BcBody(34), kBcWhite, box.X + 26, box.Y + 86, 2, box.Width - 52);
}

// ---- the thread ---------------------------------------------------------------------------

void RenderThread() {
  GdiplusStartupInput input;
  ULONG_PTR token;
  GdiplusStartup(&token, &input, nullptr);
  LoadFonts();
  if (g_bc) LoadBcFonts();
  std::vector<uint32_t> paper = g_bc ? MakeBcBackdrop() : MakePaper();
  std::vector<uint32_t> banded = paper;
  if (!g_bc) PaintOlive(banded, 0, kBand);
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
      if (g_bc || v.opaque || v.screen != Screen::kButtons) {
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
      if (g_bc) {
        if (v.screen == Screen::kButtons) DrawBcButtons(g, v, anim, now, dt);
        else if (v.screen == Screen::kList) DrawBcList(g, v, anim, now, dt);
        else DrawBcBusy(g, v, now);
      } else if (v.screen == Screen::kButtons) {
        DrawButtons(g, v, anim, now, dt);
      } else if (v.screen == Screen::kList) {
        DrawList(g, v, anim, now, dt);
      } else {
        DrawBusy(g, v, now);
      }
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
