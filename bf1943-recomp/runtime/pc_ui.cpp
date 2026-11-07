// PC look for the console UI: the Xbox button icons in the game's UI texture
// atlases are redrawn as key caps showing the PC binding (see xam.cpp) when the
// GPU first loads the atlas. Textures are recognised by size and a hash of the
// game's own data, and patched in guest memory, so no game assets are shipped.
#include "kernel.h"
#include "xenia_bridge.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
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

void OnTextureLoad(uint32_t physical, uint32_t size, uint32_t width, uint32_t height,
                   uint32_t format, bool tiled) {
  constexpr uint32_t k_8_8_8_8 = 6;
  if (format != k_8_8_8_8 || !tiled) return;
  for (const Atlas& a : Atlases()) {
    if (a.width != width || a.height != height || size < width * height * 4) continue;
    uint8_t* tex = G<uint8_t>(0xA0000000u + (physical & 0x1FFFFFFF));
    if (Fnv1a(tex, size_t(width) * height * 4) != a.hash) continue;
    for (const Icon& icon : a.icons) {
      DrawKeyCap(tex, width, icon);
      // The Back key: its prompts can be clicked (xam.cpp).
      if (std::wstring_view(icon.label) == L"⌫") {
        xgpu::AddBackIcon(physical, width, height, icon.x0, icon.y0, icon.x1 + 1, icon.y1 + 1);
      }
    }
    RT_INFO("ui: PC key icons drawn into %s atlas (%08X)", a.name, physical);
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
