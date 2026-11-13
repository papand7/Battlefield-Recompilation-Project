// Linux game (BF1943_CLIENT): the part of Windows GDI and GDI+ that the PC
// menus (pc_menu.cpp), key icons (pc_ui.cpp), settings page (pc_settings.cpp)
// and FPS counter (fps_overlay.cpp) draw with, and the few other Win32 calls
// they make. Same names and meaning as Windows', so those files build
// unchanged: GDI+ draws into 32-bit ARGB bitmaps (antialiased polygons, pens,
// paths, transforms, text with GDI+'s em sizes, padding and StringFormat
// layout), GDI draws text into DIB sections. Text is stb_truetype's outlines;
// the fonts are the TTFs shipped next to the exe and the system's (DejaVu,
// Liberation, Noto...) for the Windows faces asked for by name
// (posix_gdiplus.cpp). Empty elsewhere.
#pragma once
#if !defined(_WIN32) && defined(BF1943_CLIENT)

#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "posix_compat.h"
#include "posix_input.h"

// ---- Win32 -----------------------------------------------------------------

using UINT = unsigned;
using LONG = int32_t;
using INT = int;
using ULONG_PTR = uintptr_t;
using COLORREF = uint32_t;
constexpr int LF_FACESIZE = 32;
enum : int {
  VK_F2 = 0x71, VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76, VK_F8 = 0x77,
  VK_F9 = 0x78, VK_F10 = 0x79, VK_F11 = 0x7A,
};

// Key names as Windows gives them for a US keyboard (scan codes, set 1).
constexpr UINT MAPVK_VK_TO_VSC = 0;
UINT MapVirtualKeyW(UINT code, UINT map_type);
int GetKeyNameTextW(LONG lparam, wchar_t* name, int size);
// The exe's path (GetModuleFileNameW(nullptr, ...)).
DWORD GetModuleFileNameW(void* module, wchar_t* path, DWORD size);
// Environment variables; APPDATA is the XDG config folder (~/.config), where
// Electron keeps the launcher's appData on Linux.
DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size);
inline HANDLE GetCurrentProcess() { return nullptr; }
inline BOOL TerminateProcess(HANDLE, unsigned code) {
  fflush(nullptr);
  _exit(int(code));
}
inline BOOL CreateDirectoryA(const char* path, void*) { return mkdir(path, 0777) == 0; }

// ---- GDI: text into DIB sections --------------------------------------------

namespace posix_gdi {
struct Face;      // a loaded TrueType / OpenType font (posix_gdiplus.cpp)
struct Resolved;  // a family's faces
}  // namespace posix_gdi

struct GdiObject {
  virtual ~GdiObject() = default;
};
struct GdiFont : GdiObject {
  std::shared_ptr<posix_gdi::Face> face;
  float x_scale = 1, px = 16;
};
struct GdiBitmap : GdiObject {
  int w = 1, h = 1;
  std::vector<uint32_t> px = std::vector<uint32_t>(1, 0);
};
struct GdiDc;
using HGDIOBJ = GdiObject*;
using HDC = GdiDc*;
using HFONT = GdiFont*;
using HBITMAP = GdiBitmap*;

struct SIZE {
  LONG cx, cy;
};
struct BITMAPINFOHEADER {
  DWORD biSize;
  LONG biWidth, biHeight;
  WORD biPlanes, biBitCount;
  DWORD biCompression, biSizeImage;
  LONG biXPelsPerMeter, biYPelsPerMeter;
  DWORD biClrUsed, biClrImportant;
};
struct RGBQUAD {
  BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
};
struct BITMAPINFO {
  BITMAPINFOHEADER bmiHeader;
  RGBQUAD bmiColors[1];
};
constexpr DWORD BI_RGB = 0;
constexpr UINT DIB_RGB_COLORS = 0;
constexpr int FW_NORMAL = 400, FW_SEMIBOLD = 600, FW_BOLD = 700;
constexpr DWORD DEFAULT_CHARSET = 1, OUT_DEFAULT_PRECIS = 0, CLIP_DEFAULT_PRECIS = 0, ANTIALIASED_QUALITY = 4,
                DEFAULT_PITCH = 0;
constexpr int TRANSPARENT = 1, OPAQUE = 2;
constexpr UINT DT_LEFT = 0, DT_TOP = 0, DT_CENTER = 0x1, DT_RIGHT = 0x2, DT_VCENTER = 0x4, DT_BOTTOM = 0x8,
               DT_SINGLELINE = 0x20, DT_NOPREFIX = 0x800;
constexpr COLORREF RGB(int r, int g, int b) { return COLORREF((r & 255) | (g & 255) << 8 | (b & 255) << 16); }

HDC CreateCompatibleDC(HDC dc);
BOOL DeleteDC(HDC dc);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic, DWORD underline,
                  DWORD strike_out, DWORD charset, DWORD out_precision, DWORD clip_precision, DWORD quality,
                  DWORD pitch_and_family, const wchar_t* face);
// Top-down 32-bit only (a negative biHeight, as the callers ask).
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ object);
BOOL DeleteObject(HGDIOBJ object);
int SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF color);
BOOL GetTextExtentPoint32W(HDC dc, const wchar_t* text, int length, SIZE* size);
BOOL TextOutW(HDC dc, int x, int y, const wchar_t* text, int length);
int DrawTextW(HDC dc, const wchar_t* text, int length, RECT* rect, UINT format);
inline BOOL GdiFlush() { return TRUE; }

// ---- GDI+ ---------------------------------------------------------------------

namespace Gdiplus {

using REAL = float;
using ARGB = uint32_t;
using PixelFormat = int;
using GraphicsState = unsigned;
constexpr PixelFormat PixelFormat32bppARGB = 0x26200A;

enum FontStyle { FontStyleRegular = 0, FontStyleBold = 1, FontStyleItalic = 2, FontStyleBoldItalic = 3 };
enum Unit { UnitWorld = 0, UnitDisplay = 1, UnitPixel = 2, UnitPoint = 3 };
enum StringAlignment { StringAlignmentNear = 0, StringAlignmentCenter = 1, StringAlignmentFar = 2 };
enum StringFormatFlags {
  StringFormatFlagsNoFitBlackBox = 0x4, StringFormatFlagsMeasureTrailingSpaces = 0x800,
  StringFormatFlagsNoWrap = 0x1000, StringFormatFlagsLineLimit = 0x2000, StringFormatFlagsNoClip = 0x4000,
};
enum StringTrimming {
  StringTrimmingNone = 0, StringTrimmingCharacter = 1, StringTrimmingWord = 2, StringTrimmingEllipsisCharacter = 3,
  StringTrimmingEllipsisWord = 4, StringTrimmingEllipsisPath = 5,
};
enum SmoothingMode {
  SmoothingModeDefault = 0, SmoothingModeHighSpeed = 1, SmoothingModeHighQuality = 2, SmoothingModeNone = 3,
  SmoothingModeAntiAlias = 4,
};
enum TextRenderingHint {
  TextRenderingHintSystemDefault = 0, TextRenderingHintSingleBitPerPixelGridFit = 1,
  TextRenderingHintSingleBitPerPixel = 2, TextRenderingHintAntiAliasGridFit = 3, TextRenderingHintAntiAlias = 4,
  TextRenderingHintClearTypeGridFit = 5,
};
enum CompositingQuality {
  CompositingQualityDefault = 0, CompositingQualityHighSpeed = 1, CompositingQualityHighQuality = 2,
  CompositingQualityGammaCorrected = 3, CompositingQualityAssumeLinear = 4,
};
enum DashStyle { DashStyleSolid = 0, DashStyleDash = 1, DashStyleDot = 2, DashStyleDashDot = 3, DashStyleDashDotDot = 4 };
enum ImageLockMode { ImageLockModeRead = 1, ImageLockModeWrite = 2, ImageLockModeUserInputBuf = 4 };

struct GdiplusStartupInput {
  uint32_t GdiplusVersion = 1;
  void* DebugEventCallback = nullptr;
  BOOL SuppressBackgroundThread = FALSE, SuppressExternalCodecs = FALSE;
};
inline int GdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput*, void*) {
  if (token) *token = 1;
  return 0;
}
inline void GdiplusShutdown(ULONG_PTR) {}

class Color {
 public:
  Color() : argb_(0xFF000000u) {}
  Color(BYTE r, BYTE g, BYTE b) : argb_(0xFF000000u | uint32_t(r) << 16 | uint32_t(g) << 8 | b) {}
  Color(BYTE a, BYTE r, BYTE g, BYTE b) : argb_(uint32_t(a) << 24 | uint32_t(r) << 16 | uint32_t(g) << 8 | b) {}
  BYTE GetA() const { return BYTE(argb_ >> 24); }
  BYTE GetR() const { return BYTE(argb_ >> 16); }
  BYTE GetG() const { return BYTE(argb_ >> 8); }
  BYTE GetB() const { return BYTE(argb_); }
  ARGB GetValue() const { return argb_; }

 private:
  ARGB argb_;
};

struct PointF {
  REAL X = 0, Y = 0;
  PointF() = default;
  PointF(REAL x, REAL y) : X(x), Y(y) {}
};
struct RectF {
  REAL X = 0, Y = 0, Width = 0, Height = 0;
  RectF() = default;
  RectF(REAL x, REAL y, REAL w, REAL h) : X(x), Y(y), Width(w), Height(h) {}
};
struct Rect {
  INT X = 0, Y = 0, Width = 0, Height = 0;
  Rect() = default;
  Rect(INT x, INT y, INT w, INT h) : X(x), Y(y), Width(w), Height(h) {}
};

class FontFamily {
 public:
  FontFamily() = default;
  explicit FontFamily(const wchar_t* name);  // a system font (the nearest Linux has)
  bool IsAvailable() const;
  FontFamily* Clone() const { return new FontFamily(*this); }
  int GetFamilyName(wchar_t name[LF_FACESIZE], int language = 0) const;

  std::wstring name_;
  std::shared_ptr<posix_gdi::Resolved> faces_;
};

class PrivateFontCollection {
 public:
  void AddFontFile(const wchar_t* path);
  void AddFontFile(const char* path);
  INT GetFamilyCount() const { return INT(families_.size()); }
  void GetFamilies(INT count, FontFamily* out, INT* found) const;

 private:
  std::vector<FontFamily> families_;
};

class Font {
 public:
  Font(const FontFamily* family, REAL em_size, INT style = FontStyleRegular, Unit unit = UnitPoint);
  REAL GetSize() const { return size_; }  // (in the font's unit: pixels here)
  INT GetStyle() const { return style_; }

  std::shared_ptr<posix_gdi::Resolved> faces_;
  REAL size_ = 0, px_ = 0;  // px_: the em in pixels
  INT style_ = 0;
};

class StringFormat {
 public:
  StringFormat() = default;
  void SetAlignment(StringAlignment a) { align_ = a; }
  void SetLineAlignment(StringAlignment a) { line_align_ = a; }
  void SetFormatFlags(INT flags) { flags_ = flags; }
  INT GetFormatFlags() const { return flags_; }
  void SetTrimming(StringTrimming t) { trimming_ = t; }
  StringAlignment GetAlignment() const { return align_; }
  StringAlignment GetLineAlignment() const { return line_align_; }
  StringTrimming GetTrimming() const { return trimming_; }
  // No padding, no clipping, no trimming (as GDI+'s).
  static const StringFormat* GenericTypographic();
  static const StringFormat* GenericDefault();

  StringAlignment align_ = StringAlignmentNear, line_align_ = StringAlignmentNear;
  INT flags_ = 0;
  StringTrimming trimming_ = StringTrimmingCharacter;
  bool typographic_ = false;
};

class Brush {
 public:
  explicit Brush(const Color& c) : color_(c) {}
  virtual ~Brush() = default;
  Color color_;
};
class SolidBrush : public Brush {
 public:
  explicit SolidBrush(const Color& c) : Brush(c) {}
  void SetColor(const Color& c) { color_ = c; }
  void GetColor(Color* c) const { *c = color_; }
};

class Pen {
 public:
  Pen(const Color& color, REAL width = 1.0f) : color_(color), width_(width) {}
  void SetDashStyle(DashStyle s) { dash_ = s; }
  void SetColor(const Color& c) { color_ = c; }
  void SetWidth(REAL w) { width_ = w; }
  Color color_;
  REAL width_;
  DashStyle dash_ = DashStyleSolid;
};

class GraphicsPath {
 public:
  void AddArc(REAL x, REAL y, REAL width, REAL height, REAL start_angle, REAL sweep_angle);
  void AddLine(REAL x1, REAL y1, REAL x2, REAL y2);
  void AddRectangle(const RectF& r);
  void CloseFigure();
  void StartFigure();

  struct Figure {
    std::vector<PointF> points;
    bool closed = false;
  };
  std::vector<Figure> figures_;

 private:
  Figure& Current();
};

struct ColorMatrix {
  REAL m[5][5];
};
class ImageAttributes {
 public:
  void SetColorMatrix(const ColorMatrix* m, INT = 0, INT = 0) {
    matrix_ = *m;
    has_matrix_ = true;
  }
  ColorMatrix matrix_{};
  bool has_matrix_ = false;
};

struct BitmapData {
  UINT Width = 0, Height = 0;
  INT Stride = 0;
  int PixelFormat = 0;
  void* Scan0 = nullptr;
  ULONG_PTR Reserved = 0;
};

// 32 bpp ARGB, not premultiplied (as PixelFormat32bppARGB).
class Bitmap {
 public:
  Bitmap(INT width, INT height, PixelFormat format = PixelFormat32bppARGB);
  UINT GetWidth() const { return UINT(w_); }
  UINT GetHeight() const { return UINT(h_); }
  void LockBits(const Rect* rect, UINT flags, PixelFormat format, BitmapData* data);
  void UnlockBits(BitmapData*) {}

  int w_, h_;
  std::vector<uint32_t> px_;
};

class Graphics {
 public:
  explicit Graphics(Bitmap* target);
  void SetSmoothingMode(SmoothingMode m) { smoothing_ = m; }
  void SetTextRenderingHint(TextRenderingHint h) { hint_ = h; }
  void SetCompositingQuality(CompositingQuality) {}
  void Clear(const Color& c);

  void TranslateTransform(REAL dx, REAL dy);
  void RotateTransform(REAL degrees);
  void ScaleTransform(REAL sx, REAL sy);
  void ResetTransform();
  GraphicsState Save();
  void Restore(GraphicsState state);

  void FillRectangle(const Brush* brush, REAL x, REAL y, REAL w, REAL h);
  void FillRectangle(const Brush* brush, const RectF& r) { FillRectangle(brush, r.X, r.Y, r.Width, r.Height); }
  void DrawRectangle(const Pen* pen, REAL x, REAL y, REAL w, REAL h);
  void DrawRectangle(const Pen* pen, const RectF& r) { DrawRectangle(pen, r.X, r.Y, r.Width, r.Height); }
  void FillEllipse(const Brush* brush, REAL x, REAL y, REAL w, REAL h);
  void FillPolygon(const Brush* brush, const PointF* points, INT count);
  void DrawLine(const Pen* pen, REAL x1, REAL y1, REAL x2, REAL y2);
  void FillPath(const Brush* brush, const GraphicsPath* path);
  void DrawPath(const Pen* pen, const GraphicsPath* path);
  void DrawImage(Bitmap* image, const Rect& dest, INT src_x, INT src_y, INT src_w, INT src_h, Unit unit,
                 const ImageAttributes* attributes = nullptr);

  void DrawString(const wchar_t* text, INT length, const Font* font, const RectF& layout, const StringFormat* format,
                  const Brush* brush);
  void DrawString(const wchar_t* text, INT length, const Font* font, const PointF& origin, const StringFormat* format,
                  const Brush* brush);
  void DrawString(const wchar_t* text, INT length, const Font* font, const PointF& origin, const Brush* brush) {
    DrawString(text, length, font, origin, nullptr, brush);
  }
  void MeasureString(const wchar_t* text, INT length, const Font* font, const PointF& origin, RectF* box) const {
    MeasureString(text, length, font, origin, nullptr, box);
  }
  void MeasureString(const wchar_t* text, INT length, const Font* font, const PointF& origin,
                     const StringFormat* format, RectF* box) const;
  void MeasureString(const wchar_t* text, INT length, const Font* font, const RectF& layout,
                     const StringFormat* format, RectF* box) const;

  struct Matrix {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e, y' = b x + d y + f
  };

  Bitmap* target_;
  Matrix m_;
  std::vector<std::pair<GraphicsState, Matrix>> saved_;
  GraphicsState next_state_ = 1;
  SmoothingMode smoothing_ = SmoothingModeDefault;
  TextRenderingHint hint_ = TextRenderingHintSystemDefault;
};

}  // namespace Gdiplus

#endif  // !_WIN32 && BF1943_CLIENT
