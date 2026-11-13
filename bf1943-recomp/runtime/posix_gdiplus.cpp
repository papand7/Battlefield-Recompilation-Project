// Linux game (BF1943_CLIENT): GDI and GDI+ as the PC menus use them (see
// posix_gdiplus.h). Everything is drawn as polygons: rectangles, ellipses,
// arcs, pen strokes and the glyphs' outlines (stb_truetype) are flattened,
// transformed and filled with exact-area antialiasing (a signed-area
// accumulation rasterizer), then blended into the bitmap. Empty elsewhere.
#if !defined(_WIN32) && defined(BF1943_CLIENT)

#include "posix_gdiplus.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include "kernel.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

namespace fs = std::filesystem;

namespace posix_gdi {

// ---- fonts ---------------------------------------------------------------------

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Face {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  float upem = 1000;                                  // design units per em
  float ascent = 0, descent = 0, line_spacing = 0;    // design units (GDI's cell ascent, descent, line spacing)
  bool bold = false, condensed = false;
  std::wstring family;
  std::mutex mutex;
  std::unordered_map<int, std::vector<stbtt_vertex>> shapes;

  int Glyph(uint32_t cp) { return stbtt_FindGlyphIndex(&info, int(cp)); }
  float Advance(int glyph) {
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyph, &advance, &lsb);
    return float(advance);
  }
  const std::vector<stbtt_vertex>& Shape(int glyph) {
    std::lock_guard l(mutex);
    auto it = shapes.find(glyph);
    if (it != shapes.end()) return it->second;
    stbtt_vertex* v = nullptr;
    int n = stbtt_GetGlyphShape(&info, glyph, &v);
    std::vector<stbtt_vertex>& out = shapes[glyph];
    if (v && n > 0) out.assign(v, v + n);
    stbtt_FreeShape(&info, v);
    return out;
  }
};

std::wstring NameString(const stbtt_fontinfo& info, int name_id) {
  int len = 0;
  // Windows' (UTF-16 BE), then the Mac's (Roman, taken as Latin-1).
  if (const char* s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                              STBTT_MS_LANG_ENGLISH, name_id)) {
    std::wstring out;
    for (int i = 0; i + 1 < len; i += 2) out += wchar_t(Be16(reinterpret_cast<const uint8_t*>(s) + i));
    return out;
  }
  if (const char* s = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MAC, 0, 0, name_id))
    return std::wstring(s, s + len);
  return {};
}

std::shared_ptr<Face> LoadFace(const fs::path& path) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<Face>> loaded;
  std::lock_guard l(m);
  if (auto it = loaded.find(path.string()); it != loaded.end()) return it->second;
  std::shared_ptr<Face>& slot = loaded[path.string()];
  std::ifstream in(path, std::ios::binary);
  if (!in) return nullptr;
  auto face = std::make_shared<Face>();
  face->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  int offset = stbtt_GetFontOffsetForIndex(face->data.data(), 0);
  if (face->data.size() < 12 || offset < 0 || !stbtt_InitFont(&face->info, face->data.data(), offset)) return nullptr;
  face->upem = 1.0f / stbtt_ScaleForMappingEmToPixels(&face->info, 1.0f);
  // GDI's metrics: the cell from OS/2's Windows ascent and descent, the line
  // spacing that plus hhea's external leading.
  int hasc = 0, hdesc = 0, gap = 0;
  stbtt_GetFontVMetrics(&face->info, &hasc, &hdesc, &gap);
  const uint8_t* d = face->data.data();
  uint32_t os2 = stbtt__find_table(face->data.data(), face->info.fontstart, "OS/2");
  float win_a = 0, win_d = 0;
  int weight = 400, width_class = 5;
  if (os2 && os2 + 78 <= face->data.size()) {
    weight = Be16(d + os2 + 4), width_class = Be16(d + os2 + 6);
    win_a = Be16(d + os2 + 74), win_d = Be16(d + os2 + 76);
  }
  if (win_a + win_d <= 0) win_a = float(hasc), win_d = float(-hdesc);
  float external = std::max(0.0f, float(gap) - ((win_a + win_d) - float(hasc - hdesc)));
  face->ascent = win_a, face->descent = win_d, face->line_spacing = win_a + win_d + external;
  uint32_t head = stbtt__find_table(face->data.data(), face->info.fontstart, "head");
  bool mac_bold = head && head + 46 <= face->data.size() && (Be16(d + head + 44) & 1);
  face->bold = mac_bold || weight >= 600;
  face->condensed = width_class < 5;
  face->family = NameString(face->info, 1);
  slot = face;
  return face;
}

struct Pick {
  std::shared_ptr<Face> face;
  float x_scale = 1;  // a condensed face asked for, a normal one found: narrowed
};
struct Resolved {
  Pick regular, bold;
  const Pick& Get(int style) const {
    bool want_bold = style & Gdiplus::FontStyleBold;
    if (want_bold) return bold.face ? bold : regular;
    return regular.face ? regular : bold;
  }
};

fs::path ExeDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  return n > 0 ? fs::path(std::string(buf, size_t(n))).parent_path() : fs::path(".");
}

// The system's font files by lower-case file name (fontconfig's folders).
const std::map<std::string, fs::path>& SystemFonts() {
  static const std::map<std::string, fs::path> fonts = [] {
    std::map<std::string, fs::path> out;
    std::vector<fs::path> dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* x = getenv("XDG_DATA_HOME"); x && *x) dirs.push_back(fs::path(x) / "fonts");
    if (const char* h = getenv("HOME"); h && *h) {
      dirs.push_back(fs::path(h) / ".local/share/fonts");
      dirs.push_back(fs::path(h) / ".fonts");
    }
    for (const fs::path& dir : dirs) {
      std::error_code ec;
      for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
           !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (name.size() > 4 && (name.ends_with(".ttf") || name.ends_with(".otf") || name.ends_with(".ttc")))
          out.emplace(name, it->path());
      }
    }
    return out;
  }();
  return fonts;
}

std::shared_ptr<Face> FirstFont(std::initializer_list<const char*> names) {
  const auto& fonts = SystemFonts();
  for (const char* n : names)
    if (auto it = fonts.find(n); it != fonts.end())
      if (auto face = LoadFace(it->second)) return face;
  return nullptr;
}
std::shared_ptr<Face> Shipped(const char* name) { return LoadFace(ExeDir() / "fonts" / name); }

// The Windows faces asked for by name, as the nearest a Linux system has:
// Bahnschrift / Arial Narrow / Impact a condensed sans, Courier a monospace,
// Segoe UI and Arial a plain sans (DejaVu, Liberation, Noto, FreeFont...).
std::shared_ptr<Resolved> ResolveSystem(const std::wstring& wide_name) {
  static std::mutex m;
  static std::map<std::wstring, std::shared_ptr<Resolved>> cache;
  std::lock_guard l(m);
  if (auto it = cache.find(wide_name); it != cache.end()) return it->second;
  std::string name;
  for (wchar_t c : wide_name) name += char(std::tolower(c < 128 ? int(c) : '?'));
  auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
  const bool mono = has("courier") || has("mono") || has("consol");
  const bool condensed = has("condensed") || has("narrow") || has("semiconden") || has("impact") ||
                         has("bahnschrift");
  const bool heavy = has("bold") || has("impact") || has("black") || has("heavy");
  std::shared_ptr<Face> regular, bold;
  if (mono) {
    regular = FirstFont({"dejavusansmono.ttf", "liberationmono-regular.ttf", "notosansmono-regular.ttf",
                         "ubuntumono-r.ttf", "freemono.ttf"});
    bold = FirstFont({"dejavusansmono-bold.ttf", "liberationmono-bold.ttf", "notosansmono-bold.ttf",
                      "ubuntumono-b.ttf", "freemonobold.ttf"});
  } else {
    if (condensed) {
      regular = FirstFont({"dejavusanscondensed.ttf", "liberationsansnarrow-regular.ttf",
                           "notosans-condensed.ttf", "notosans-condensedmedium.ttf"});
      bold = FirstFont({"dejavusanscondensed-bold.ttf", "liberationsansnarrow-bold.ttf",
                        "notosans-condensedsemibold.ttf", "notosans-condensedbold.ttf"});
    }
    if (!regular)
      regular = FirstFont({"dejavusans.ttf", "liberationsans-regular.ttf", "notosans-regular.ttf",
                           "ubuntu-r.ttf", "freesans.ttf", "opensans-regular.ttf"});
    if (!bold)
      bold = FirstFont({"dejavusans-bold.ttf", "liberationsans-bold.ttf", "notosans-bold.ttf", "ubuntu-b.ttf",
                        "freesansbold.ttf", "opensans-bold.ttf"});
  }
  // No system fonts at all: the menus' own.
  if (!regular && !bold) regular = Shipped(mono ? "SpecialElite-Regular.ttf" : "StardosStencil-Bold.ttf");
  // A bold family ("Bahnschrift SemiBold Condensed"): bold in its regular style too.
  if (heavy && bold) regular = bold;
  auto r = std::make_shared<Resolved>();
  auto pick = [&](const std::shared_ptr<Face>& f) {
    Pick p;
    p.face = f;
    if (f && condensed && !f->condensed) p.x_scale = 0.84f;
    return p;
  };
  r->regular = pick(regular), r->bold = pick(bold);
  if (r->regular.face || r->bold.face) {
    const Face* f = r->regular.face ? r->regular.face.get() : r->bold.face.get();
    RT_INFO("fonts: \"%ls\" drawn with %ls%s", wide_name.c_str(), f->family.c_str(),
            r->regular.x_scale < 1 ? " (narrowed)" : "");
  }
  cache[wide_name] = r;
  return r;
}

// Glyphs a face lacks (arrows, the backspace sign, the ellipsis...): from these.
const std::vector<std::shared_ptr<Face>>& FallbackFaces() {
  static const std::vector<std::shared_ptr<Face>> faces = [] {
    std::vector<std::shared_ptr<Face>> out;
    for (const char* n : {"dejavusans.ttf", "dejavusans-bold.ttf", "liberationsans-regular.ttf", "notosans-regular.ttf",
                          "notosanssymbols-regular.ttf", "notosanssymbols2-regular.ttf", "symbola.ttf",
                          "freeserif.ttf", "freesans.ttf", "dejavusansmono.ttf"})
      if (auto f = FirstFont({n})) out.push_back(f);
    for (const char* n : {"SpecialElite-Regular.ttf", "StardosStencil-Bold.ttf"})
      if (auto f = Shipped(n)) out.push_back(f);
    return out;
  }();
  return faces;
}

struct GlyphRef {
  Face* face = nullptr;
  int glyph = 0;
};
GlyphRef FindGlyph(Face* face, uint32_t cp) {
  if (!face) return {};
  if (int g = face->Glyph(cp)) return {face, g};
  if (cp == 0x2026) return {face, 0};  // (the caller spells it "..." then)
  for (const auto& f : FallbackFaces())
    if (int g = f->Glyph(cp)) return {f.get(), g};
  return {face, 0};
}

// ---- geometry ---------------------------------------------------------------------

struct Pt {
  float x, y;
};
using Contour = std::vector<Pt>;
using Shape = std::vector<Contour>;

// A glyph's outline, its origin at (ox, oy) (the baseline), y down; curves
// flattened to about a tenth of a pixel.
void AppendGlyph(const GlyphRef& g, float px, float x_scale, float ox, float oy, Shape& out) {
  if (!g.face || !g.glyph) return;
  const float s = px / g.face->upem, sx = s * x_scale;
  auto at = [&](float x, float y) { return Pt{ox + x * sx, oy - y * s}; };
  Pt last{ox, oy};
  for (const stbtt_vertex& v : g.face->Shape(g.glyph)) {
    Pt p = at(v.x, v.y);
    if (v.type == STBTT_vmove) {
      out.emplace_back();
      out.back().push_back(p);
    } else if (out.empty()) {
      continue;
    } else if (v.type == STBTT_vline) {
      out.back().push_back(p);
    } else if (v.type == STBTT_vcurve) {
      Pt c = at(v.cx, v.cy);
      float dd = std::hypot(last.x - 2 * c.x + p.x, last.y - 2 * c.y + p.y);
      int n = std::clamp(int(std::ceil(std::sqrt(dd * 1.25f))), 1, 32);
      for (int i = 1; i <= n; ++i) {
        float t = float(i) / n, u = 1 - t;
        out.back().push_back({u * u * last.x + 2 * u * t * c.x + t * t * p.x, u * u * last.y + 2 * u * t * c.y + t * t * p.y});
      }
    } else if (v.type == STBTT_vcubic) {
      Pt c1 = at(v.cx, v.cy), c2 = at(v.cx1, v.cy1);
      float dd = std::max(std::hypot(last.x - 2 * c1.x + c2.x, last.y - 2 * c1.y + c2.y),
                          std::hypot(c1.x - 2 * c2.x + p.x, c1.y - 2 * c2.y + p.y));
      int n = std::clamp(int(std::ceil(std::sqrt(dd * 7.5f))), 1, 48);
      for (int i = 1; i <= n; ++i) {
        float t = float(i) / n, u = 1 - t;
        float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
        out.back().push_back({a * last.x + b * c1.x + c * c2.x + d * p.x, a * last.y + b * c1.y + c * c2.y + d * p.y});
      }
    }
    last = p;
  }
}

// Sutherland-Hodgman against an axis-aligned box (keeps the winding inside).
void ClipToBox(Shape& shape, float x0, float y0, float x1, float y1) {
  auto clip = [](const Contour& in, auto inside, auto cross) {
    Contour out;
    for (size_t i = 0; i < in.size(); ++i) {
      const Pt& a = in[i];
      const Pt& b = in[(i + 1) % in.size()];
      bool ia = inside(a), ib = inside(b);
      if (ia) out.push_back(a);
      if (ia != ib) out.push_back(cross(a, b));
    }
    return out;
  };
  for (Contour& c : shape) {
    if (c.empty()) continue;
    auto at_x = [](float x) {
      return [x](const Pt& a, const Pt& b) { return Pt{x, a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x)}; };
    };
    auto at_y = [](float y) {
      return [y](const Pt& a, const Pt& b) { return Pt{a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y), y}; };
    };
    c = clip(c, [&](const Pt& p) { return p.x >= x0; }, at_x(x0));
    if (!c.empty()) c = clip(c, [&](const Pt& p) { return p.x <= x1; }, at_x(x1));
    if (!c.empty()) c = clip(c, [&](const Pt& p) { return p.y >= y0; }, at_y(y0));
    if (!c.empty()) c = clip(c, [&](const Pt& p) { return p.y <= y1; }, at_y(y1));
  }
}

// One edge into the accumulation buffer (the signed area it adds to each
// pixel's right; a row's running sum is then the coverage).
void AccumulateLine(Pt a, Pt b, int w, int h, int stride, float* acc) {
  if (a.y == b.y) return;
  float dir = 1;
  if (a.y > b.y) std::swap(a, b), dir = -1;
  const float dxdy = (b.x - a.x) / (b.y - a.y);
  const int y_start = std::max(0, int(std::floor(a.y))), y_end = std::min(h, int(std::ceil(b.y)));
  for (int y = y_start; y < y_end; ++y) {
    const float ya = std::max(float(y), a.y), yb = std::min(float(y + 1), b.y), dy = yb - ya;
    if (dy <= 0) continue;
    const float xa = std::clamp(a.x + (ya - a.y) * dxdy, 0.0f, float(w)),
                xb = std::clamp(a.x + (yb - a.y) * dxdy, 0.0f, float(w));
    const float d = dy * dir;
    float* row = acc + size_t(y) * stride;
    const float lo = std::min(xa, xb), hi = std::max(xa, xb);
    const int ilo = int(std::floor(lo)), ihi = int(std::ceil(hi));
    if (ihi <= ilo + 1) {
      const float xm = 0.5f * (xa + xb) - float(ilo);
      row[ilo] += d * (1 - xm);
      row[ilo + 1] += d * xm;
    } else {
      const float s = 1.0f / (hi - lo), x0f = lo - float(ilo), x1f = hi - float(ihi) + 1;
      const float a0 = 0.5f * s * (1 - x0f) * (1 - x0f), am = 0.5f * s * x1f * x1f;
      row[ilo] += d * a0;
      if (ihi == ilo + 2) {
        row[ilo + 1] += d * (1 - a0 - am);
      } else {
        const float a1 = s * (1.5f - x0f);
        row[ilo + 1] += d * (a1 - a0);
        for (int xi = ilo + 2; xi < ihi - 1; ++xi) row[xi] += d * s;
        const float a2 = a1 + float(ihi - ilo - 3) * s;
        row[ihi - 1] += d * (1 - a2 - am);
      }
      row[ihi] += d * am;
    }
  }
}

// Fills a shape (pixel x covering [x, x+1)), nonzero-like: |winding| clamped
// to 1 (the pieces of a stroke are all wound the same way). blend(x, y, coverage).
template <typename Blend>
void Rasterize(const Shape& shape, int clip_w, int clip_h, Blend&& blend) {
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
  for (const Contour& c : shape)
    for (const Pt& p : c) minx = std::min(minx, p.x), miny = std::min(miny, p.y), maxx = std::max(maxx, p.x), maxy = std::max(maxy, p.y);
  if (!(minx <= maxx)) return;
  const int x0 = std::max(0, int(std::floor(minx))), y0 = std::max(0, int(std::floor(miny)));
  const int x1 = std::min(clip_w, int(std::ceil(maxx))), y1 = std::min(clip_h, int(std::ceil(maxy)));
  if (x0 >= x1 || y0 >= y1) return;
  const int w = x1 - x0, h = y1 - y0, stride = w + 2;
  thread_local std::vector<float> acc;
  acc.assign(size_t(stride) * h, 0.0f);
  for (const Contour& c : shape) {
    const size_t n = c.size();
    if (n < 2) continue;
    for (size_t i = 0; i < n; ++i) {
      const Pt& a = c[i];
      const Pt& b = c[(i + 1) % n];
      AccumulateLine({a.x - x0, a.y - y0}, {b.x - x0, b.y - y0}, w, h, stride, acc.data());
    }
  }
  for (int y = 0; y < h; ++y) {
    const float* row = &acc[size_t(y) * stride];
    float sum = 0;
    for (int x = 0; x < w; ++x) {
      sum += row[x];
      const float cover = std::min(1.0f, std::fabs(sum));
      if (cover > 1.0f / 1024) blend(x0 + x, y0 + y, cover);
    }
  }
}

// Source over a not-premultiplied ARGB pixel.
inline void BlendArgb(uint32_t& dst, uint32_t argb, float cover) {
  const float sa = float(argb >> 24) / 255.0f * cover;
  if (sa <= 0) return;
  const float sr = float((argb >> 16) & 255), sg = float((argb >> 8) & 255), sb = float(argb & 255);
  const uint32_t d = dst;
  const float dr = float((d >> 16) & 255), dg = float((d >> 8) & 255), db = float(d & 255);
  const uint32_t da8 = d >> 24;
  float r, g, b, a;
  if (da8 == 255) {
    r = dr + (sr - dr) * sa, g = dg + (sg - dg) * sa, b = db + (sb - db) * sa, a = 255;
  } else {
    const float da = float(da8) / 255.0f, oa = sa + da * (1 - sa);
    if (oa <= 0) return;
    const float k = da * (1 - sa);
    r = (sr * sa + dr * k) / oa, g = (sg * sa + dg * k) / oa, b = (sb * sa + db * k) / oa, a = oa * 255;
  }
  dst = uint32_t(std::lround(a)) << 24 | uint32_t(std::lround(r)) << 16 | uint32_t(std::lround(g)) << 8 |
        uint32_t(std::lround(b));
}

// ---- text layout (GDI+'s) -------------------------------------------------------

struct TextFont {
  Face* face = nullptr;
  float px = 0, x_scale = 1;
  bool round = false;  // grid fit: whole-pixel advances
  float Scale() const { return face ? px / face->upem : 0; }
  float Ascent() const { return face ? face->ascent * Scale() : 0; }
  float LineHeight() const { return face ? face->line_spacing * Scale() : px * 1.2f; }
  float Advance(const GlyphRef& g) const {
    if (!g.face) return 0;
    float a = g.face->Advance(g.glyph) * px / g.face->upem * x_scale;
    return round ? std::round(a) : a;
  }
  float Advance(wchar_t c) const {
    if (c == 0x2026 && face && !face->Glyph(c)) return 3 * Advance(L'.');
    return Advance(FindGlyph(face, uint32_t(c)));
  }
  float Width(const std::wstring& s, bool trailing) const {
    size_t n = s.size();
    if (!trailing)
      while (n && s[n - 1] == L' ') --n;
    float w = 0;
    for (size_t i = 0; i < n; ++i) w += Advance(s[i]);
    return w;
  }
};

TextFont MakeTextFont(const Gdiplus::Font* font, Gdiplus::TextRenderingHint hint) {
  TextFont t;
  if (!font || !font->faces_) return t;
  const Pick& p = font->faces_->Get(font->style_);
  t.face = p.face.get(), t.x_scale = p.x_scale, t.px = font->px_;
  t.round = hint != Gdiplus::TextRenderingHintAntiAlias && hint != Gdiplus::TextRenderingHintSingleBitPerPixel;
  return t;
}

struct TextLayout {
  std::vector<std::wstring> lines;
  std::vector<float> widths;
  float pad = 0, line_h = 0;
};

TextLayout LayoutText(const TextFont& tf, const wchar_t* text, int length, float width, const Gdiplus::StringFormat& f) {
  using namespace Gdiplus;
  TextLayout out;
  std::wstring s = length < 0 ? std::wstring(text) : std::wstring(text, size_t(length));
  out.pad = f.typographic_ ? 0.0f : tf.px / 6.0f;  // GDI+'s 1/6 em either side
  out.line_h = tf.LineHeight();
  const bool trailing = f.flags_ & StringFormatFlagsMeasureTrailingSpaces;
  const bool wrap = !(f.flags_ & StringFormatFlagsNoWrap) && width > 0;
  const float avail = width > 0 ? std::max(0.0f, width - 2 * out.pad) : 1e30f;
  size_t start = 0;
  while (start <= s.size()) {
    size_t nl = s.find(L'\n', start);
    if (nl == std::wstring::npos) nl = s.size();
    std::wstring para = s.substr(start, nl - start);
    para.erase(std::remove(para.begin(), para.end(), L'\r'), para.end());
    if (!wrap) {
      out.lines.push_back(para);
    } else {
      std::wstring line;
      size_t i = 0;
      while (i < para.size()) {
        size_t sp_end = para.find_first_not_of(L' ', i);
        if (sp_end == std::wstring::npos) {
          line += para.substr(i);
          break;
        }
        size_t word_end = para.find(L' ', sp_end);
        if (word_end == std::wstring::npos) word_end = para.size();
        std::wstring spaces = para.substr(i, sp_end - i), word = para.substr(sp_end, word_end - sp_end);
        std::wstring candidate = line + spaces + word;
        if (!line.empty() && tf.Width(candidate, false) > avail) {
          out.lines.push_back(line);
          line = word;
        } else {
          line = candidate;
        }
        // A word wider than the box: broken between letters.
        while (line.size() > 1 && tf.Width(line, false) > avail) {
          size_t fit = 1;
          while (fit < line.size() && tf.Width(line.substr(0, fit + 1), false) <= avail) ++fit;
          out.lines.push_back(line.substr(0, fit));
          line = line.substr(fit);
        }
        i = word_end;
      }
      out.lines.push_back(line);
    }
    start = nl + 1;
  }
  // Ellipsis trimming of lines too long for the box.
  if (!wrap && width > 0 &&
      (f.trimming_ == StringTrimmingEllipsisCharacter || f.trimming_ == StringTrimmingEllipsisWord ||
       f.trimming_ == StringTrimmingEllipsisPath)) {
    for (std::wstring& line : out.lines) {
      if (tf.Width(line, trailing) <= avail) continue;
      std::wstring base = line;
      while (!base.empty()) {
        base.pop_back();
        while (!base.empty() && base.back() == L' ') base.pop_back();
        if (tf.Width(base + L"…", false) <= avail) break;
      }
      line = base + L"…";
    }
  }
  for (const std::wstring& line : out.lines) out.widths.push_back(tf.Width(line, trailing));
  return out;
}

}  // namespace posix_gdi

using namespace posix_gdi;

// ---- Win32 ---------------------------------------------------------------------------

namespace {
std::wstring FromUtf8(const std::string& s) {
  std::wstring out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    uint32_t cp = c;
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n) cp = c & (0x3F >> n);
    ++i;
    for (int k = 0; k < n && i < s.size(); ++k, ++i) cp = cp << 6 | (s[i] & 0x3F);
    out += wchar_t(cp);
  }
  return out;
}
std::string ToUtf8(const wchar_t* s) {
  std::string out;
  for (; *s; ++s) {
    uint32_t c = uint32_t(*s);
    if (c < 0x80) out += char(c);
    else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 0x3F));
    else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    else out += char(0xF0 | c >> 18), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
         out += char(0x80 | (c & 0x3F));
  }
  return out;
}
DWORD CopyOut(const std::wstring& s, wchar_t* buffer, DWORD size) {
  if (!buffer || size <= s.size()) return DWORD(s.size() + 1);  // (too small: the size needed)
  std::wcsncpy(buffer, s.c_str(), size);
  buffer[s.size()] = 0;
  return DWORD(s.size());
}
}  // namespace

DWORD GetModuleFileNameW(void*, wchar_t* path, DWORD size) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  if (n <= 0 || !size) return 0;
  std::wstring w = FromUtf8(std::string(buf, size_t(n)));
  if (w.size() >= size) w.resize(size - 1);
  std::wcsncpy(path, w.c_str(), size);
  path[w.size()] = 0;
  return DWORD(w.size());
}

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) {
  std::string n = ToUtf8(name);
  if (n == "APPDATA") {
    if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x) return CopyOut(FromUtf8(x), buffer, size);
    if (const char* h = getenv("HOME"); h && *h) return CopyOut(FromUtf8(std::string(h) + "/.config"), buffer, size);
    return 0;
  }
  const char* v = getenv(n.c_str());
  return v ? CopyOut(FromUtf8(v), buffer, size) : 0;
}

UINT MapVirtualKeyW(UINT vk, UINT map_type) {
  if (map_type != MAPVK_VK_TO_VSC) return 0;
  static const uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                       0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
  static const uint8_t kNumpad[10] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
  if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
  if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
  if (vk == '0') return 0x0B;
  if (vk >= VK_F1 && vk <= VK_F10) return 0x3B + (vk - VK_F1);
  if (vk >= unsigned(VK_NUMPAD0) && vk <= unsigned(VK_NUMPAD0) + 9) return kNumpad[vk - VK_NUMPAD0];
  switch (int(vk)) {
    case VK_F11: return 0x57;
    case VK_F12: return 0x58;
    case VK_ESCAPE: return 0x01;
    case VK_BACK: return 0x0E;
    case VK_TAB: return 0x0F;
    case VK_RETURN: return 0x1C;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return 0x1D;
    case VK_SHIFT: case VK_LSHIFT: return 0x2A;
    case VK_RSHIFT: return 0x36;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return 0x38;
    case VK_SPACE: return 0x39;
    case VK_CAPITAL: return 0x3A;
    case VK_PAUSE: return 0x45;
    case 0x90: return 0x45;  // VK_NUMLOCK
    case 0x91: return 0x46;  // VK_SCROLL
    case VK_HOME: return 0x47;
    case VK_UP: return 0x48;
    case VK_PRIOR: return 0x49;
    case VK_LEFT: return 0x4B;
    case VK_RIGHT: return 0x4D;
    case VK_END: return 0x4F;
    case VK_DOWN: return 0x50;
    case VK_NEXT: return 0x51;
    case VK_INSERT: return 0x52;
    case VK_DELETE: return 0x53;
    case VK_MULTIPLY: return 0x37;
    case VK_SUBTRACT: return 0x4A;
    case VK_ADD: return 0x4E;
    case VK_DECIMAL: return 0x53;
    case VK_DIVIDE: return 0x35;
    case VK_OEM_1: return 0x27;
    case VK_OEM_PLUS: return 0x0D;
    case VK_OEM_COMMA: return 0x33;
    case VK_OEM_MINUS: return 0x0C;
    case VK_OEM_PERIOD: return 0x34;
    case VK_OEM_2: return 0x35;
    case VK_OEM_3: return 0x29;
    case VK_OEM_4: return 0x1A;
    case VK_OEM_5: return 0x2B;
    case VK_OEM_6: return 0x1B;
    case VK_OEM_7: return 0x28;
    case 0xE2: return 0x56;  // VK_OEM_102
    default: return 0;
  }
}

int GetKeyNameTextW(LONG lparam, wchar_t* name, int size) {
  const unsigned scan = (unsigned(lparam) >> 16) & 0xFF;
  const bool ext = (unsigned(lparam) >> 24) & 1;
  static const char* const kNames[0x59] = {
      nullptr, "Esc", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "Backspace", "Tab",
      "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "Enter", "Ctrl", "A", "S",
      "D", "F", "G", "H", "J", "K", "L", ";", "'", "`", "Shift", "\\", "Z", "X", "C", "V",
      "B", "N", "M", ",", ".", "/", "Right Shift", "Num *", "Alt", "Space", "Caps Lock", "F1", "F2", "F3",
      "F4", "F5", "F6", "F7", "F8", "F9", "F10", "Pause", "Scroll Lock", "Num 7", "Num 8", "Num 9", "Num -",
      "Num 4", "Num 5", "Num 6", "Num +", "Num 1", "Num 2", "Num 3", "Num 0", "Num Del", "Sys Req", nullptr,
      "\\", "F11", "F12"};
  const char* n = nullptr;
  if (ext) {
    switch (scan) {
      case 0x1C: n = "Num Enter"; break;
      case 0x1D: n = "Right Ctrl"; break;
      case 0x35: n = "Num /"; break;
      case 0x38: n = "Right Alt"; break;
      case 0x45: n = "Num Lock"; break;
      case 0x47: n = "Home"; break;
      case 0x48: n = "Up"; break;
      case 0x49: n = "Page Up"; break;
      case 0x4B: n = "Left"; break;
      case 0x4D: n = "Right"; break;
      case 0x4F: n = "End"; break;
      case 0x50: n = "Down"; break;
      case 0x51: n = "Page Down"; break;
      case 0x52: n = "Insert"; break;
      case 0x53: n = "Delete"; break;
    }
  }
  if (!n && scan < 0x59) n = kNames[scan];
  if (!n || size <= 0) return 0;
  int len = 0;
  for (; n[len] && len < size - 1; ++len) name[len] = wchar_t(n[len]);
  name[len] = 0;
  return len;
}

// ---- GDI -----------------------------------------------------------------------------

struct GdiDc {
  GdiFont* font;
  GdiBitmap* bitmap;
  COLORREF color = 0;
  int bk_mode = OPAQUE;
};

namespace {
GdiFont* StockFont() {
  static GdiFont* f = [] {
    auto* font = new GdiFont;
    const Pick& p = ResolveSystem(L"System")->Get(0);
    font->face = p.face, font->x_scale = p.x_scale;
    return font;
  }();
  return f;
}
GdiBitmap* StockBitmap() {
  static GdiBitmap* b = new GdiBitmap;
  return b;
}

struct GdiMetrics {
  int ascent = 0, height = 0;
  float scale = 0;
};
GdiMetrics Metrics(const GdiFont* f) {
  GdiMetrics m;
  if (!f || !f->face) return m;
  const Face& face = *f->face;
  m.scale = f->px / face.upem;
  m.ascent = int(std::lround(face.ascent * m.scale));
  m.height = m.ascent + int(std::lround(face.descent * m.scale));
  return m;
}
int GdiAdvance(const GdiFont* f, wchar_t c) {
  GlyphRef g = FindGlyph(f->face.get(), uint32_t(c));
  if (!g.face) return 0;
  return int(std::lround(g.face->Advance(g.glyph) * f->px / g.face->upem * f->x_scale));
}
}  // namespace

HDC CreateCompatibleDC(HDC) {
  auto* dc = new GdiDc;
  dc->font = StockFont(), dc->bitmap = StockBitmap();
  return dc;
}
BOOL DeleteDC(HDC dc) {
  delete dc;
  return TRUE;
}

HFONT CreateFontW(int height, int, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD,
                  const wchar_t* face) {
  auto* f = new GdiFont;
  const Pick& p = ResolveSystem(face ? face : L"")->Get(weight >= FW_SEMIBOLD ? Gdiplus::FontStyleBold : 0);
  f->face = p.face, f->x_scale = p.x_scale;
  if (height < 0) {
    f->px = float(-height);
  } else if (height > 0 && f->face) {
    const Face& fc = *f->face;  // the cell height: the em is smaller
    f->px = float(height) * fc.upem / std::max(1.0f, fc.ascent + fc.descent);
  }
  return f;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* info, UINT, void** bits, HANDLE, DWORD) {
  auto* b = new GdiBitmap;
  b->w = std::max(1, int(info->bmiHeader.biWidth));
  b->h = std::max(1, std::abs(int(info->bmiHeader.biHeight)));
  b->px.assign(size_t(b->w) * b->h, 0);
  if (bits) *bits = b->px.data();
  return b;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ object) {
  if (!dc || !object) return nullptr;
  if (auto* f = dynamic_cast<GdiFont*>(object)) {
    GdiFont* old = dc->font;
    dc->font = f;
    return old;
  }
  if (auto* b = dynamic_cast<GdiBitmap*>(object)) {
    GdiBitmap* old = dc->bitmap;
    dc->bitmap = b;
    return old;
  }
  return nullptr;
}
BOOL DeleteObject(HGDIOBJ object) {
  if (!object || object == StockFont() || object == StockBitmap()) return FALSE;
  delete object;
  return TRUE;
}
int SetBkMode(HDC dc, int mode) {
  int old = dc->bk_mode;
  dc->bk_mode = mode;
  return old;
}
COLORREF SetTextColor(HDC dc, COLORREF color) {
  COLORREF old = dc->color;
  dc->color = color;
  return old;
}

BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* text, int length, SIZE* size) {
  GdiMetrics m = Metrics(dc->font);
  int w = 0;
  for (int i = 0; i < length; ++i) w += GdiAdvance(dc->font, text[i]);
  size->cx = w, size->cy = m.height;
  return TRUE;
}

BOOL TextOutW(HDC dc, int x, int y, const wchar_t* text, int length) {
  const GdiFont* f = dc->font;
  GdiBitmap* b = dc->bitmap;
  GdiMetrics m = Metrics(f);
  if (!m.scale) return FALSE;
  const float r = float(dc->color & 255), g = float((dc->color >> 8) & 255), bl = float((dc->color >> 16) & 255);
  int pen = x;
  const int baseline = y + m.ascent;
  for (int i = 0; i < length; ++i) {
    GlyphRef gl = FindGlyph(f->face.get(), uint32_t(text[i]));
    Shape shape;
    AppendGlyph(gl, f->px, f->x_scale, float(pen), float(baseline), shape);
    Rasterize(shape, b->w, b->h, [&](int px, int py, float cover) {
      uint32_t& d = b->px[size_t(py) * b->w + px];
      float dr = float((d >> 16) & 255), dg = float((d >> 8) & 255), db = float(d & 255);
      dr += (r - dr) * cover, dg += (g - dg) * cover, db += (bl - db) * cover;
      d = (d & 0xFF000000u) | uint32_t(std::lround(dr)) << 16 | uint32_t(std::lround(dg)) << 8 | uint32_t(std::lround(db));
    });
    pen += GdiAdvance(f, text[i]);
  }
  return TRUE;
}

int DrawTextW(HDC dc, const wchar_t* text, int length, RECT* rect, UINT format) {
  if (length < 0) length = int(std::wcslen(text));
  SIZE ext{};
  GetTextExtentPoint32W(dc, text, length, &ext);
  int x = int(rect->left), y = int(rect->top);
  if (format & DT_CENTER) x = int(rect->left + (rect->right - rect->left - ext.cx) / 2);
  else if (format & DT_RIGHT) x = int(rect->right - ext.cx);
  if (format & DT_SINGLELINE) {
    if (format & DT_VCENTER) y = int(rect->top + (rect->bottom - rect->top - ext.cy) / 2);
    else if (format & DT_BOTTOM) y = int(rect->bottom - ext.cy);
  }
  TextOutW(dc, x, y, text, length);
  return int(ext.cy);
}

// ---- GDI+ ----------------------------------------------------------------------------

namespace Gdiplus {

FontFamily::FontFamily(const wchar_t* name) : name_(name ? name : L""), faces_(ResolveSystem(name_)) {}
bool FontFamily::IsAvailable() const { return faces_ && (faces_->regular.face || faces_->bold.face); }
int FontFamily::GetFamilyName(wchar_t name[LF_FACESIZE], int) const {
  size_t n = std::min<size_t>(name_.size(), LF_FACESIZE - 1);
  std::wmemcpy(name, name_.c_str(), n);
  name[n] = 0;
  return 0;
}

void PrivateFontCollection::AddFontFile(const char* path) {
  std::shared_ptr<Face> face = LoadFace(path);
  if (!face) return;
  std::wstring family = face->family.empty() ? fs::path(path).stem().wstring() : face->family;
  FontFamily* fam = nullptr;
  for (FontFamily& f : families_)
    if (f.name_ == family) fam = &f;
  if (!fam) {
    families_.emplace_back();
    fam = &families_.back();
    fam->name_ = family;
    fam->faces_ = std::make_shared<Resolved>();
  }
  (face->bold ? fam->faces_->bold : fam->faces_->regular).face = face;
}
void PrivateFontCollection::AddFontFile(const wchar_t* path) { AddFontFile(ToUtf8(path).c_str()); }
void PrivateFontCollection::GetFamilies(INT count, FontFamily* out, INT* found) const {
  INT n = std::min<INT>(count, INT(families_.size()));
  for (INT i = 0; i < n; ++i) out[i] = families_[size_t(i)];
  if (found) *found = n;
}

Font::Font(const FontFamily* family, REAL em_size, INT style, Unit unit)
    : faces_(family && family->IsAvailable() ? family->faces_ : ResolveSystem(L"Arial")),
      size_(em_size),
      px_(unit == UnitPoint ? em_size * 96.0f / 72.0f : em_size),
      style_(style) {}

const StringFormat* StringFormat::GenericTypographic() {
  static const StringFormat f = [] {
    StringFormat s;
    s.flags_ = StringFormatFlagsLineLimit | StringFormatFlagsNoClip | StringFormatFlagsNoFitBlackBox;
    s.trimming_ = StringTrimmingNone;
    s.typographic_ = true;
    return s;
  }();
  return &f;
}
const StringFormat* StringFormat::GenericDefault() {
  static const StringFormat f;
  return &f;
}

GraphicsPath::Figure& GraphicsPath::Current() {
  if (figures_.empty() || figures_.back().closed) figures_.emplace_back();
  return figures_.back();
}
void GraphicsPath::AddArc(REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep) {
  Figure& f = Current();
  const float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
  int n = std::clamp(int(std::ceil(std::fabs(sweep) / 360.0f * 3.14159265f * (rx + ry))), 4, 128);
  for (int i = 0; i <= n; ++i) {
    float a = (start + sweep * float(i) / float(n)) * 3.14159265f / 180.0f;
    f.points.emplace_back(cx + rx * std::cos(a), cy + ry * std::sin(a));
  }
}
void GraphicsPath::AddLine(REAL x1, REAL y1, REAL x2, REAL y2) {
  Figure& f = Current();
  f.points.emplace_back(x1, y1);
  f.points.emplace_back(x2, y2);
}
void GraphicsPath::AddRectangle(const RectF& r) {
  StartFigure();
  Figure& f = Current();
  f.points = {{r.X, r.Y}, {r.X + r.Width, r.Y}, {r.X + r.Width, r.Y + r.Height}, {r.X, r.Y + r.Height}};
  f.closed = true;
}
void GraphicsPath::CloseFigure() {
  if (!figures_.empty()) figures_.back().closed = true;
}
void GraphicsPath::StartFigure() {
  if (!figures_.empty() && !figures_.back().closed && !figures_.back().points.empty()) figures_.emplace_back();
}

Bitmap::Bitmap(INT width, INT height, PixelFormat) : w_(std::max(1, width)), h_(std::max(1, height)) {
  px_.assign(size_t(w_) * h_, 0);
}
void Bitmap::LockBits(const Rect* rect, UINT, PixelFormat format, BitmapData* data) {
  Rect r = rect ? *rect : Rect(0, 0, w_, h_);
  data->Width = UINT(r.Width), data->Height = UINT(r.Height), data->Stride = w_ * 4;
  data->PixelFormat = format;
  data->Scan0 = px_.data() + size_t(r.Y) * w_ + r.X;
}

Graphics::Graphics(Bitmap* target) : target_(target) {}

void Graphics::Clear(const Color& c) { std::fill(target_->px_.begin(), target_->px_.end(), c.GetValue()); }

void Graphics::TranslateTransform(REAL dx, REAL dy) {
  m_.e += m_.a * dx + m_.c * dy;
  m_.f += m_.b * dx + m_.d * dy;
}
void Graphics::RotateTransform(REAL degrees) {
  const float r = degrees * 3.14159265358979f / 180.0f, cs = std::cos(r), sn = std::sin(r);
  Matrix m = m_;
  m_.a = m.a * cs + m.c * sn, m_.b = m.b * cs + m.d * sn;
  m_.c = -m.a * sn + m.c * cs, m_.d = -m.b * sn + m.d * cs;
}
void Graphics::ScaleTransform(REAL sx, REAL sy) { m_.a *= sx, m_.b *= sx, m_.c *= sy, m_.d *= sy; }
void Graphics::ResetTransform() { m_ = Matrix{}; }
GraphicsState Graphics::Save() {
  saved_.emplace_back(next_state_, m_);
  return next_state_++;
}
void Graphics::Restore(GraphicsState state) {
  for (size_t i = saved_.size(); i-- > 0;) {
    if (saved_[i].first != state) continue;
    m_ = saved_[i].second;
    saved_.resize(i);
    return;
  }
}

}  // namespace Gdiplus

namespace {
using namespace Gdiplus;

// Local shapes to the bitmap: through the transform, GDI+'s pixel centres on
// whole coordinates (its default PixelOffsetModeNone), filled with a colour.
void FillShape(Graphics& g, Shape shape, const Color& color) {
  if (color.GetA() == 0) return;
  const Graphics::Matrix& m = g.m_;
  for (Contour& c : shape)
    for (Pt& p : c) p = {m.a * p.x + m.c * p.y + m.e + 0.5f, m.b * p.x + m.d * p.y + m.f + 0.5f};
  Bitmap& b = *g.target_;
  const uint32_t argb = color.GetValue();
  Rasterize(shape, b.w_, b.h_, [&](int x, int y, float cover) { BlendArgb(b.px_[size_t(y) * b.w_ + x], argb, cover); });
}

Contour RectContour(float x, float y, float w, float h) { return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}; }

// A stroke from a to b, as GDI+'s flat caps: a quad (always wound the same way).
void SegmentQuad(Pt a, Pt b, float half, Shape& out) {
  float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
  if (len <= 1e-6f) return;
  float nx = -dy / len * half, ny = dx / len * half;
  out.push_back({{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
}

float PenHalf(const Pen* pen) { return std::max(1.0f, pen->width_) * 0.5f; }

void StrokePolyline(Graphics& g, const Pen* pen, std::vector<Pt> pts, bool closed) {
  // Drop repeated points.
  std::vector<Pt> p;
  for (const Pt& q : pts)
    if (p.empty() || std::hypot(q.x - p.back().x, q.y - p.back().y) > 1e-4f) p.push_back(q);
  if (closed && p.size() > 1 && std::hypot(p[0].x - p.back().x, p[0].y - p.back().y) <= 1e-4f) p.pop_back();
  const float half = PenHalf(pen);
  Shape shape;
  if (closed && p.size() >= 3) {
    // Mitred both sides: the outline and the inner line reversed.
    Contour outer, inner;
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i) {
      const Pt& prev = p[(i + n - 1) % n];
      const Pt& cur = p[i];
      const Pt& next = p[(i + 1) % n];
      float ax = cur.x - prev.x, ay = cur.y - prev.y, al = std::hypot(ax, ay);
      float bx = next.x - cur.x, by = next.y - cur.y, bl = std::hypot(bx, by);
      float n1x = -ay / al, n1y = ax / al, n2x = -by / bl, n2y = bx / bl;
      float mx = n1x + n2x, my = n1y + n2y, ml = std::hypot(mx, my);
      if (ml < 1e-3f) mx = n2x, my = n2y;
      else mx /= ml, my /= ml;
      float k = half / std::max(0.25f, mx * n2x + my * n2y);
      outer.push_back({cur.x + mx * k, cur.y + my * k});
      inner.push_back({cur.x - mx * k, cur.y - my * k});
    }
    std::reverse(inner.begin(), inner.end());
    shape.push_back(std::move(outer));
    shape.push_back(std::move(inner));
  } else {
    for (size_t i = 0; i + 1 < p.size(); ++i) SegmentQuad(p[i], p[i + 1], half, shape);
    if (closed && p.size() > 2) SegmentQuad(p.back(), p[0], half, shape);
  }
  FillShape(g, std::move(shape), pen->color_);
}

// Text: laid out as GDI+ lays it out, each glyph's outline filled.
void DrawText(Graphics& g, const wchar_t* text, int length, const Font* font, const RectF& box,
              const StringFormat* format, const Color& color) {
  const StringFormat& f = format ? *format : *StringFormat::GenericDefault();
  TextFont tf = MakeTextFont(font, g.hint_);
  if (!tf.face || color.GetA() == 0) return;
  TextLayout lay = LayoutText(tf, text, length, box.Width, f);
  const float total = float(lay.lines.size()) * lay.line_h;
  float top = box.Y;
  if (f.line_align_ == StringAlignmentCenter) top = box.Height > 0 ? box.Y + (box.Height - total) / 2 : box.Y - total / 2;
  if (f.line_align_ == StringAlignmentFar) top = box.Height > 0 ? box.Y + box.Height - total : box.Y - total;
  const bool clip = !(f.flags_ & StringFormatFlagsNoClip) && box.Width > 0 && box.Height > 0;
  for (size_t i = 0; i < lay.lines.size(); ++i) {
    const std::wstring& line = lay.lines[i];
    const float w = lay.widths[i];
    float x = box.X + lay.pad;
    if (f.align_ == StringAlignmentCenter) x = box.Width > 0 ? box.X + (box.Width - w) / 2 : box.X - w / 2;
    if (f.align_ == StringAlignmentFar) x = box.Width > 0 ? box.X + box.Width - lay.pad - w : box.X - lay.pad - w;
    const float baseline = top + float(i) * lay.line_h + tf.Ascent();
    for (wchar_t c : line) {
      if (c == 0x2026 && !tf.face->Glyph(c)) {  // no ellipsis in the font: three dots
        for (int k = 0; k < 3; ++k) {
          Shape shape;
          GlyphRef dot = FindGlyph(tf.face, L'.');
          AppendGlyph(dot, tf.px, tf.x_scale, x, baseline, shape);
          if (clip) ClipToBox(shape, box.X, box.Y, box.X + box.Width, box.Y + box.Height);
          FillShape(g, std::move(shape), color);
          x += tf.Advance(dot);
        }
        continue;
      }
      GlyphRef gl = FindGlyph(tf.face, uint32_t(c));
      if (c != L' ' && gl.glyph) {
        Shape shape;
        AppendGlyph(gl, tf.px, tf.x_scale, x, baseline, shape);
        if (clip) ClipToBox(shape, box.X, box.Y, box.X + box.Width, box.Y + box.Height);
        FillShape(g, std::move(shape), color);
      }
      x += tf.Advance(gl);
    }
  }
}

void Measure(const Graphics& g, const wchar_t* text, int length, const Font* font, const RectF& box,
             const StringFormat* format, RectF* out) {
  const StringFormat& f = format ? *format : *StringFormat::GenericDefault();
  *out = RectF(box.X, box.Y, 0, 0);
  if (length == 0 || !text || (length < 0 && !*text)) return;
  TextFont tf = MakeTextFont(font, g.hint_);
  if (!tf.face) return;
  TextLayout lay = LayoutText(tf, text, length, box.Width, f);
  float w = 0;
  for (float lw : lay.widths) w = std::max(w, lw);
  out->Width = w + 2 * lay.pad;
  if (box.Width > 0) out->Width = std::min(out->Width, box.Width);
  out->Height = float(lay.lines.size()) * lay.line_h;
}
}  // namespace

namespace Gdiplus {

void Graphics::FillRectangle(const Brush* brush, REAL x, REAL y, REAL w, REAL h) {
  if (w <= 0 || h <= 0) return;
  FillShape(*this, {RectContour(x, y, w, h)}, brush->color_);
}

void Graphics::DrawRectangle(const Pen* pen, REAL x, REAL y, REAL w, REAL h) {
  const float half = PenHalf(pen);
  Shape shape{RectContour(x - half, y - half, w + 2 * half, h + 2 * half)};
  if (w > 2 * half && h > 2 * half) {
    Contour inner = RectContour(x + half, y + half, w - 2 * half, h - 2 * half);
    std::reverse(inner.begin(), inner.end());
    shape.push_back(std::move(inner));
  }
  FillShape(*this, std::move(shape), pen->color_);
}

void Graphics::FillEllipse(const Brush* brush, REAL x, REAL y, REAL w, REAL h) {
  if (w <= 0 || h <= 0) return;
  const float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
  int n = std::clamp(int(3.14159265f * (rx + ry) / 1.5f), 12, 256);
  Contour c;
  for (int i = 0; i < n; ++i) {
    float a = 2 * 3.14159265f * float(i) / float(n);
    c.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
  }
  FillShape(*this, {std::move(c)}, brush->color_);
}

void Graphics::FillPolygon(const Brush* brush, const PointF* points, INT count) {
  Contour c;
  for (INT i = 0; i < count; ++i) c.push_back({points[i].X, points[i].Y});
  FillShape(*this, {std::move(c)}, brush->color_);
}

void Graphics::DrawLine(const Pen* pen, REAL x1, REAL y1, REAL x2, REAL y2) {
  const float half = PenHalf(pen), width = 2 * half;
  if (pen->dash_ == DashStyleSolid) {
    StrokePolyline(*this, pen, {{x1, y1}, {x2, y2}}, false);
    return;
  }
  // Dashes, in pen widths (GDI+'s patterns).
  static const std::vector<float> kDash = {3, 1}, kDot = {1, 1}, kDashDot = {3, 1, 1, 1}, kDashDotDot = {3, 1, 1, 1, 1, 1};
  const std::vector<float>& pattern = pen->dash_ == DashStyleDash      ? kDash
                                      : pen->dash_ == DashStyleDot     ? kDot
                                      : pen->dash_ == DashStyleDashDot ? kDashDot
                                                                       : kDashDotDot;
  const float len = std::hypot(x2 - x1, y2 - y1);
  if (len <= 0) return;
  const float ux = (x2 - x1) / len, uy = (y2 - y1) / len;
  Shape shape;
  float t = 0;
  for (size_t i = 0; t < len; i = (i + 1) % pattern.size()) {
    float seg = pattern[i] * width;
    if (i % 2 == 0) {
      float t1 = std::min(len, t + seg);
      SegmentQuad({x1 + ux * t, y1 + uy * t}, {x1 + ux * t1, y1 + uy * t1}, half, shape);
    }
    t += seg;
  }
  FillShape(*this, std::move(shape), pen->color_);
}

void Graphics::FillPath(const Brush* brush, const GraphicsPath* path) {
  Shape shape;
  for (const GraphicsPath::Figure& f : path->figures_) {
    Contour c;
    for (const PointF& p : f.points) c.push_back({p.X, p.Y});
    if (c.size() >= 3) shape.push_back(std::move(c));
  }
  FillShape(*this, std::move(shape), brush->color_);
}

void Graphics::DrawPath(const Pen* pen, const GraphicsPath* path) {
  for (const GraphicsPath::Figure& f : path->figures_) {
    std::vector<Pt> pts;
    for (const PointF& p : f.points) pts.push_back({p.X, p.Y});
    StrokePolyline(*this, pen, std::move(pts), f.closed);
  }
}

// The image's pixels mapped back through the transform (bilinear), each
// through the colour matrix, over the bitmap.
void Graphics::DrawImage(Bitmap* image, const Rect& dest, INT src_x, INT src_y, INT src_w, INT src_h, Unit,
                         const ImageAttributes* attributes) {
  if (!image || dest.Width <= 0 || dest.Height <= 0 || src_w <= 0 || src_h <= 0) return;
  const Matrix& m = m_;
  const float det = m.a * m.d - m.b * m.c;
  if (std::fabs(det) < 1e-12f) return;
  const float ia = m.d / det, ib = -m.b / det, ic = -m.c / det, id = m.a / det;
  float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
  for (int k = 0; k < 4; ++k) {
    float lx = float(dest.X + (k & 1 ? dest.Width : 0)), ly = float(dest.Y + (k & 2 ? dest.Height : 0));
    float dx = m.a * lx + m.c * ly + m.e, dy = m.b * lx + m.d * ly + m.f;
    minx = std::min(minx, dx), maxx = std::max(maxx, dx), miny = std::min(miny, dy), maxy = std::max(maxy, dy);
  }
  Bitmap& t = *target_;
  const int x0 = std::max(0, int(std::floor(minx))), x1 = std::min(t.w_, int(std::ceil(maxx)));
  const int y0 = std::max(0, int(std::floor(miny))), y1 = std::min(t.h_, int(std::ceil(maxy)));
  const float kx = float(src_w) / float(dest.Width), ky = float(src_h) / float(dest.Height);
  const bool cm = attributes && attributes->has_matrix_;
  auto texel = [&](int x, int y, float* premul) {
    x = std::clamp(x, src_x, src_x + src_w - 1), y = std::clamp(y, src_y, src_y + src_h - 1);
    x = std::clamp(x, 0, image->w_ - 1), y = std::clamp(y, 0, image->h_ - 1);
    uint32_t p = image->px_[size_t(y) * image->w_ + x];
    float a = float(p >> 24) / 255.0f;
    premul[0] = float((p >> 16) & 255) / 255.0f * a, premul[1] = float((p >> 8) & 255) / 255.0f * a;
    premul[2] = float(p & 255) / 255.0f * a, premul[3] = a;
  };
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const float px = x + 0.5f - m.e, py = y + 0.5f - m.f;
      const float u = ia * px + ic * py, v = ib * px + id * py;  // in the destination rectangle's space
      if (u < dest.X || v < dest.Y || u >= dest.X + dest.Width || v >= dest.Y + dest.Height) continue;
      const float sx = float(src_x) + (u - float(dest.X)) * kx - 0.5f, sy = float(src_y) + (v - float(dest.Y)) * ky - 0.5f;
      const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
      const float fx = sx - float(ix), fy = sy - float(iy);
      float c00[4], c10[4], c01[4], c11[4], c[4];
      texel(ix, iy, c00), texel(ix + 1, iy, c10), texel(ix, iy + 1, c01), texel(ix + 1, iy + 1, c11);
      for (int k = 0; k < 4; ++k)
        c[k] = (c00[k] * (1 - fx) + c10[k] * fx) * (1 - fy) + (c01[k] * (1 - fx) + c11[k] * fx) * fy;
      if (c[3] <= 0) continue;
      float v5[5] = {c[0] / c[3], c[1] / c[3], c[2] / c[3], c[3], 1};
      float o[4] = {v5[0], v5[1], v5[2], v5[3]};
      if (cm) {
        for (int j = 0; j < 4; ++j) {
          float s = 0;
          for (int i = 0; i < 5; ++i) s += v5[i] * attributes->matrix_.m[i][j];
          o[j] = std::clamp(s, 0.0f, 1.0f);
        }
      }
      uint32_t argb = uint32_t(std::lround(o[3] * 255)) << 24 | uint32_t(std::lround(o[0] * 255)) << 16 |
                      uint32_t(std::lround(o[1] * 255)) << 8 | uint32_t(std::lround(o[2] * 255));
      BlendArgb(t.px_[size_t(y) * t.w_ + x], argb, 1.0f);
    }
  }
}

void Graphics::DrawString(const wchar_t* text, INT length, const Font* font, const RectF& layout,
                          const StringFormat* format, const Brush* brush) {
  DrawText(*this, text, length, font, layout, format, brush->color_);
}
void Graphics::DrawString(const wchar_t* text, INT length, const Font* font, const PointF& origin,
                          const StringFormat* format, const Brush* brush) {
  DrawText(*this, text, length, font, RectF(origin.X, origin.Y, 0, 0), format, brush->color_);
}
void Graphics::MeasureString(const wchar_t* text, INT length, const Font* font, const PointF& origin,
                             const StringFormat* format, RectF* box) const {
  Measure(*this, text, length, font, RectF(origin.X, origin.Y, 0, 0), format, box);
}
void Graphics::MeasureString(const wchar_t* text, INT length, const Font* font, const RectF& layout,
                             const StringFormat* format, RectF* box) const {
  Measure(*this, text, length, font, layout, format, box);
}

}  // namespace Gdiplus

#endif  // !_WIN32 && BF1943_CLIENT
