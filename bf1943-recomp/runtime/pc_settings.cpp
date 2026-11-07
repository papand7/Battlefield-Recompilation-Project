// PC settings page: see pc_settings.h. The page is drawn with GDI into a
// 1920x1080 image in the style of the game's menus (white rows with a black
// rim, the selected one green, condensed capitals, key-cap prompts at the
// bottom) and shown over the game by the presenter. Settings that work live
// are applied at once; the others (marked RESTART) are written to the
// launcher's settings, which it passes on the next start. Everything is
// also kept in <user>/pc_settings.ini.
#include "kernel.h"
#include "pc_settings.h"
#include "xenia_bridge.h"
#include "pbr_shaders.h"
#include "lan.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace rt;

namespace rt::pcset {
namespace {

// ---- the settings ------------------------------------------------------------

enum class Kind { kChoice, kKey, kText, kButton };
enum Category { kGraphics, kEffects, kControls, kKeys, kProfile, kCategoryCount };
const wchar_t* const kCategoryNames[kCategoryCount] = {L"GRAPHICS", L"EFFECTS", L"MOUSE",
                                                       L"KEYS", L"PROFILE"};

struct Setting {
  Category category;
  const char* key;              // in pc_settings.ini (and the launcher's settings)
  const wchar_t* label;
  const wchar_t* desc;
  Kind kind = Kind::kChoice;
  std::vector<std::wstring> names;   // shown
  std::vector<std::string> values;   // stored
  int index = 0;                     // current choice
  bool restart = false;              // applies on the next start
  const char* launcher = nullptr;    // the launcher's settings.json key
  int action = -1;                   // kKey: the action
  std::string text;                  // kText
  int button = 0;                    // kButton: what it does (Button)
  std::string address, map;          // kButton on the join page: the server
};
enum Button { kHostStart = 1, kJoinServer, kJoinAddress, kRefresh };

std::vector<Setting> g_settings;
// HOST GAME and JOIN GAME use the page too (lan.cpp does the networking).
enum Mode { kModeSettings, kModeHost, kModeJoin };
Mode g_mode = kModeSettings;
std::vector<Setting> g_host_rows, g_join_rows;
std::vector<Setting>& Page() {
  return g_mode == kModeHost ? g_host_rows : g_mode == kModeJoin ? g_join_rows : g_settings;
}
const std::vector<std::wstring> kMapNames = {L"WAKE ISLAND", L"IWO JIMA", L"GUADALCANAL",
                                             L"CORAL SEA (AIR)"};
const std::vector<std::string> kMaps = {"Wake_island_s", "Iwo_Jima_s", "Guadal_Canal", "Coral_sea"};
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring MapName(const std::string& map) {
  for (size_t i = 0; i < kMaps.size(); ++i)
    if (kMaps[i] == map) return kMapNames[i];
  return Wide(map);
}
int g_keys[kActionCount][2];

struct ActionInfo {
  const wchar_t* name;
  int key, second;
};
const ActionInfo kActions[kActionCount] = {
    {L"MOVE FORWARD", 'W', 0},       {L"MOVE BACK", 'S', 0},
    {L"MOVE LEFT", 'A', 0},          {L"MOVE RIGHT", 'D', 0},
    {L"FIRE", VK_LBUTTON, 0},        {L"ZOOM / AIM", VK_RBUTTON, 0},
    {L"JUMP", VK_SPACE, 0},          {L"CROUCH", VK_CONTROL, 'C'},
    {L"SPRINT", VK_SHIFT, 0},        {L"RELOAD", 'R', 0},
    {L"ENTER / USE", 'E', 0},        {L"KNIFE", 'F', 'V'},
    {L"GRENADE", 'G', VK_MBUTTON},   {L"SWITCH WEAPON", '1', '2'},
    {L"SCOREBOARD", VK_TAB, 0},      {L"PAUSE MENU", VK_ESCAPE, 0}};

Setting Choice(Category c, const char* key, const wchar_t* label, const wchar_t* desc,
               std::vector<std::wstring> names, std::vector<std::string> values, int index,
               bool restart = false, const char* launcher = nullptr) {
  Setting s;
  s.category = c, s.key = key, s.label = label, s.desc = desc, s.names = std::move(names);
  s.values = std::move(values), s.index = index, s.restart = restart, s.launcher = launcher;
  return s;
}
const std::vector<std::wstring> kOffOn = {L"OFF", L"ON"};

void Build() {
  auto& v = g_settings;
  // Graphics.
  v.push_back(Choice(kGraphics, "preset", L"LOOK",
                     L"Enhanced: every effect. Toon, Noir and WW2 are enhanced with a film look. "
                     L"Stock: the game as it was (from stock or to it on the next start).",
                     {L"STOCK", L"ENHANCED", L"TOON", L"NOIR", L"WW2"},
                     {"stock", "enhanced", "toon", "noir", "ww2"}, 1, false, "preset"));
  v.push_back(Choice(kGraphics, "resolution", L"RESOLUTION SCALE",
                     L"The game drawn at 1, 2 or 3 times its own 1280x720.",
                     {L"1X (720P)", L"2X (1440P)", L"3X (2160P)"}, {"1", "2", "3"}, 1, true,
                     "resolution"));
  v.push_back(Choice(kGraphics, "upscaling", L"UPSCALING", L"How the frame is fitted to the window.",
                     {L"FSR", L"CAS SHARPEN", L"BILINEAR"}, {"fsr", "cas", "none"}, 0, true,
                     "upscaling"));
  v.push_back(Choice(kGraphics, "window", L"WINDOW SIZE", L"The game window's size.",
                     {L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160"},
                     {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, 2, true,
                     "window"));
  v.push_back(Choice(kGraphics, "fps", L"FRAME RATE LIMIT", L"Most frames a second.",
                     {L"30", L"60", L"120", L"144", L"165", L"240", L"UNLIMITED"},
                     {"30", "60", "120", "144", "165", "240", "0"}, 2, false, "frameRate"));
  v.push_back(Choice(kGraphics, "hdr", L"HDR",
                     L"HDR10 output. Needs HDR on in the Windows display settings.", kOffOn,
                     {"0", "1"}, 0, true, "hdr"));
  v.push_back(Choice(kGraphics, "hdr_white", L"HDR BRIGHTNESS",
                     L"How bright the game's white is in HDR (nits).",
                     {L"80", L"100", L"120", L"140", L"160", L"180", L"200", L"240", L"280", L"320"},
                     {"80", "100", "120", "140", "160", "180", "200", "240", "280", "320"}, 6));
  v.push_back(Choice(kGraphics, "textures", L"HD TEXTURES",
                     L"The texture pack: upscaled textures with PBR materials.", kOffOn, {"0", "1"}, 1,
                     true, "textures"));
  v.push_back(Choice(kGraphics, "parallax", L"PARALLAX",
                     L"Depth in flat textures (planks, stone) from their height maps.", kOffOn,
                     {"0", "1"}, 1, true));
  v.push_back(Choice(kGraphics, "blood", L"BLOOD", L"Blood on hits, bodies torn apart by explosions.",
                     kOffOn, {"0", "1"}, 0, true, "blood"));
  // Effects (the scene effects, live).
  v.push_back(Choice(kEffects, "rt", L"RAY-TRACED SHADOWS",
                     L"Sun shadows from every object, on screen or not.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "shadows", L"SCREEN SHADOWS", L"Contact shadows from the depth.",
                     {L"OFF", L"LOW", L"FULL"}, {"0", "0.5", "1"}, 2));
  v.push_back(Choice(kEffects, "gi", L"BOUNCE LIGHT", L"Light bouncing off nearby surfaces.",
                     {L"OFF", L"LOW", L"NORMAL", L"HIGH"}, {"0", "0.5", "1", "1.5"}, 2));
  v.push_back(Choice(kEffects, "ao", L"AMBIENT OCCLUSION",
                     L"Contact shading in corners and under objects.",
                     {L"OFF", L"LOW", L"NORMAL", L"HIGH"}, {"0", "0.5", "1", "1.5"}, 2));
  v.push_back(Choice(kEffects, "clouds", L"CLOUDS", L"Volumetric clouds.",
                     {L"OFF", L"FEW", L"NORMAL", L"MANY"}, {"0", "0.3", "0.45", "0.65"}, 2));
  v.push_back(Choice(kEffects, "cloud_shadows", L"CLOUD SHADOWS",
                     L"Clouds shadow the ground.", kOffOn, {"0", "0.85"}, 1));
  v.push_back(Choice(kEffects, "density", L"FOG", L"Height fog over the sea.",
                     {L"LIGHT", L"NORMAL", L"THICK"}, {"0.0009", "0.0018", "0.0036"}, 1));
  v.push_back(Choice(kEffects, "shafts", L"LIGHT SHAFTS", L"Shadowed air in the fog.",
                     {L"OFF", L"NORMAL", L"STRONG"}, {"0", "0.6", "0.9"}, 1));
  v.push_back(Choice(kEffects, "reflect", L"SEA REFLECTIONS", L"Ships and islands mirrored in the sea.",
                     kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "gloss", L"SURFACE REFLECTIONS",
                     L"Smooth surfaces reflect what is around them.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "water_foam", L"WATER FOAM", L"Foam along hulls and beaches.", kOffOn,
                     {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "water_refract", L"WATER REFRACTION",
                     L"What is under the water bent by the waves.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "water_shallow", L"SHALLOW WATER",
                     L"Clear turquoise water over sand.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "detail", L"SURFACE DETAIL",
                     L"Pebbles, cracks and grime on the ground.", {L"OFF", L"ON", L"STRONG"},
                     {"0", "1", "1.5"}, 1));
  v.push_back(Choice(kEffects, "flash", L"MUZZLE FLASH LIGHT",
                     L"On: a short, shadowed light at the gun. Off: the game's own.", kOffOn,
                     {"0", "1"}, 1));
  // Mouse.
  {
    std::vector<std::wstring> names;
    std::vector<std::string> values;
    for (int i = 1; i <= 16; ++i) {
      wchar_t n[16];
      swprintf(n, 16, L"%.2f", i * 0.25);
      char s[16];
      snprintf(s, 16, "%.2f", i * 0.25);
      names.push_back(n), values.push_back(s);
    }
    v.push_back(Choice(kControls, "mouse_sens", L"MOUSE SENSITIVITY", L"How far the view turns.",
                       names, values, 3));
  }
  v.push_back(Choice(kControls, "invert_mouse", L"INVERT MOUSE", L"Mouse forward looks down.", kOffOn,
                     {"0", "1"}, 0));
  {
    std::vector<std::wstring> names;
    std::vector<std::string> values;
    for (int i = 1; i <= 16; ++i) {
      wchar_t n[16];
      swprintf(n, 16, L"%.2f", i * 0.25);
      char s[16];
      snprintf(s, 16, "%.2f", i * 0.25);
      names.push_back(n), values.push_back(s);
    }
    v.push_back(Choice(kControls, "plane_sens", L"PLANE MOUSE SENSITIVITY",
                       L"How fast the mouse pitches and rolls a plane.", names, values, 3));
  }
  v.push_back(Choice(kControls, "plane_invert", L"INVERT PLANE MOUSE",
                     L"On: mouse forward pulls the nose up. Off: it pushes it down, like a stick.", kOffOn,
                     {"0", "1"}, 0));
  // Keys.
  for (int a = 0; a < kActionCount; ++a) {
    Setting s;
    s.category = kKeys, s.kind = Kind::kKey, s.action = a, s.label = kActions[a].name;
    static std::string keys[kActionCount];
    keys[a] = "key_" + std::to_string(a);
    s.key = keys[a].c_str();
    s.desc = L"Enter or click, then press a key or mouse button (Esc cancels).";
    v.push_back(s);
  }
  // Profile.
  {
    Setting s;
    s.category = kProfile, s.kind = Kind::kText, s.key = "name", s.label = L"MULTIPLAYER NAME";
    s.desc = L"The name other players see (up to 15 letters). Enter or click to type.";
    s.restart = true, s.launcher = "name";
    s.text = g_gamertag;
    v.push_back(s);
  }
}

Setting* Find(const char* key) {
  for (Setting& s : g_settings)
    if (!std::strcmp(s.key, key)) return &s;
  return nullptr;
}
const std::string& Value(const char* key) {
  static const std::string none;
  Setting* s = Find(key);
  return s && s->kind == Kind::kChoice ? s->values[s->index] : none;
}
void Select(Setting& s, const std::string& value) {
  for (size_t i = 0; i < s.values.size(); ++i)
    if (s.values[i] == value) s.index = int(i);
}

// ---- files -----------------------------------------------------------------

std::filesystem::path IniPath() { return g_user_root / "pc_settings.ini"; }
std::filesystem::path LauncherPath() {
  wchar_t buffer[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
  if (!n || n >= MAX_PATH) return {};
  return std::filesystem::path(buffer) / L"BF1943Launcher" / L"settings.json";
}

// The launcher's settings: a flat JSON object of strings and booleans.
std::map<std::string, std::string> ReadJson(const std::filesystem::path& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  size_t at = 0;
  while ((at = s.find('"', at)) != std::string::npos) {
    size_t end = s.find('"', at + 1);
    if (end == std::string::npos) break;
    std::string key = s.substr(at + 1, end - at - 1);
    size_t colon = s.find(':', end);
    if (colon == std::string::npos) break;
    size_t v = s.find_first_not_of(" \t\r\n", colon + 1);
    if (v == std::string::npos) break;
    if (s[v] == '"') {
      size_t ve = v + 1;
      std::string value;
      while (ve < s.size() && s[ve] != '"') {
        if (s[ve] == '\\' && ve + 1 < s.size()) ++ve;
        value += s[ve++];
      }
      out[key] = "\"" + value + "\"";
      at = ve + 1;
    } else {
      size_t ve = s.find_first_of(",}\r\n", v);
      out[key] = s.substr(v, ve - v);
      at = ve;
    }
  }
  return out;
}

void WriteJson(const std::filesystem::path& path, const std::map<std::string, std::string>& map) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  out << "{\n";
  size_t i = 0;
  for (const auto& [k, v] : map) out << "  \"" << k << "\": " << v << (++i < map.size() ? ",\n" : "\n");
  out << "}";
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

void Save() {
  std::ofstream ini(IniPath(), std::ios::trunc);
  ini << "# PC settings (the game's PC SETTINGS page)\n";
  for (const Setting& s : g_settings) {
    if (s.kind == Kind::kChoice) ini << s.key << "=" << s.values[s.index] << "\n";
    else if (s.kind == Kind::kKey) ini << s.key << "=" << g_keys[s.action][0] << "\n";
    else ini << s.key << "=" << s.text << "\n";
  }
  std::filesystem::path lp = LauncherPath();
  if (lp.empty()) return;
  auto json = ReadJson(lp);
  for (const Setting& s : g_settings) {
    if (!s.launcher) continue;
    if (s.kind == Kind::kText) {
      json[s.launcher] = JsonString(s.text);
    } else {
      const std::string& v = s.values[s.index];
      bool flag = !std::strcmp(s.launcher, "hdr") || !std::strcmp(s.launcher, "blood") ||
                  !std::strcmp(s.launcher, "textures");
      json[s.launcher] = flag ? (v == "1" ? "true" : "false") : JsonString(v);
    }
  }
  WriteJson(lp, json);
}

// ---- applying ----------------------------------------------------------------

int g_running_look = -1;  // the look this run started with (stock needs a restart)

void ApplyLive() {
  std::string fx;
  for (const Setting& s : g_settings) {
    if (s.category != kEffects) continue;
    fx += std::string(fx.empty() ? "" : ",") + s.key + "=" + s.values[s.index];
  }
  xgpu::ApplySceneFx(fx.c_str());
  const std::string& look = Value("preset");
  if (g_running_look != 0 && look != "stock") {
    xgpu::SetLook(look == "toon" ? 1 : look == "noir" ? 2 : look == "ww2" ? 3 : 0);
  }
  g_max_fps = uint32_t(std::atoi(Value("fps").c_str()));
  xgpu::SetHdrWhite(float(std::atof(Value("hdr_white").c_str())));
  g_mouse_sensitivity = float(std::atof(Value("mouse_sens").c_str()));
  g_mouse_invert = Value("invert_mouse") == "1";
  g_plane_mouse_sensitivity = float(std::atof(Value("plane_sens").c_str()));
  g_plane_mouse_invert = Value("plane_invert") == "1";
}

// ---- drawing -----------------------------------------------------------------

constexpr int kW = 1920, kH = 1080;
constexpr COLORREF kKey = RGB(255, 0, 255);  // see-through (the game dimmed)
constexpr COLORREF kGreen = RGB(112, 143, 82), kRow = RGB(236, 236, 236), kBlack = RGB(0, 0, 0),
                   kWhite = RGB(255, 255, 255), kGrey = RGB(120, 120, 120);

struct Layout {
  RECT categories[kCategoryCount];
  std::vector<std::pair<int, RECT>> rows;  // setting index, its row
  RECT left_arrow[64], right_arrow[64];
  RECT back_prompt{}, select_prompt{};
  int first = 0;  // first visible row of the category
};

int g_category = 0, g_row = 0, g_focus = 0;  // focus 0 categories, 1 rows
int g_scroll = 0;
bool g_capture = false, g_editing = false;
Layout g_layout;
std::wstring g_status;

std::vector<int> RowsOf(int category) {
  std::vector<int> out;
  if (g_mode != kModeSettings) {
    for (size_t i = 0; i < Page().size(); ++i) out.push_back(int(i));
    return out;
  }
  for (size_t i = 0; i < g_settings.size(); ++i)
    if (g_settings[i].category == category) out.push_back(int(i));
  return out;
}

std::wstring KeyName(int vk) {
  switch (vk) {
    case 0: return L"-";
    case VK_LBUTTON: return L"LEFT MOUSE";
    case VK_RBUTTON: return L"RIGHT MOUSE";
    case VK_MBUTTON: return L"MIDDLE MOUSE";
    case VK_XBUTTON1: return L"MOUSE 4";
    case VK_XBUTTON2: return L"MOUSE 5";
    case VK_SHIFT: return L"SHIFT";
    case VK_CONTROL: return L"CTRL";
    case VK_SPACE: return L"SPACE";
    case VK_UP: return L"UP";
    case VK_DOWN: return L"DOWN";
    case VK_LEFT: return L"LEFT";
    case VK_RIGHT: return L"RIGHT";
  }
  UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
  wchar_t name[64] = {};
  if (scan && GetKeyNameTextW(LONG(scan << 16), name, 64) > 0) {
    std::wstring s = name;
    for (wchar_t& c : s) c = towupper(c);
    return s;
  }
  wchar_t buf[16];
  swprintf(buf, 16, L"KEY %d", vk);
  return buf;
}

std::wstring ValueText(const Setting& s, bool selected) {
  if (s.kind == Kind::kButton) return s.names.empty() ? L"" : s.names[0];
  if (s.kind == Kind::kKey) {
    if (g_capture && selected) return L"PRESS A KEY...";
    std::wstring t = KeyName(g_keys[s.action][0]);
    if (int second = Key(Action(s.action), 1)) t += L"  /  " + KeyName(second);
    return t;
  }
  if (s.kind == Kind::kText) {
    std::wstring t(s.text.begin(), s.text.end());
    if (g_editing && selected) t += L"_";
    if (t.empty()) return std::strcmp(s.key, "address") ? L"PLAYER" : L"TYPE AN ADDRESS";
    return t;
  }
  return s.names[s.index];
}

HFONT Font(int height, int weight = FW_SEMIBOLD, bool italic = false) {
  return CreateFontW(-height, 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                     L"Bahnschrift SemiBold Condensed");
}

void Fill(HDC dc, RECT r, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, &r, b);
  DeleteObject(b);
}
void Frame(HDC dc, RECT r, COLORREF c, int width) {
  for (int i = 0; i < width; ++i) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
    InflateRect(&r, -1, -1);
  }
}
void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT format) {
  SetTextColor(dc, c);
  DrawTextW(dc, s.c_str(), int(s.size()), &r, format | DT_SINGLELINE | DT_NOPREFIX);
}

// A key cap like the game's prompts: a white rounded box with a black label.
void KeyCap(HDC dc, int x, int y, const std::wstring& label, HFONT font) {
  SelectObject(dc, font);
  SIZE size{};
  GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &size);
  int w = std::max(44, int(size.cx) + 18);
  HBRUSH b = CreateSolidBrush(kWhite);
  HPEN p = CreatePen(PS_SOLID, 3, kBlack);
  SelectObject(dc, b);
  SelectObject(dc, p);
  RoundRect(dc, x, y, x + w, y + 44, 12, 12);
  DeleteObject(b);
  DeleteObject(p);
  RECT r{x, y, x + w, y + 44};
  Text(dc, label, r, kBlack, DT_CENTER | DT_VCENTER);
}

// The prompt: a key cap and its label on a black plate, as clickable area.
RECT Prompt(HDC dc, int x, int y, const std::wstring& key, const std::wstring& label, HFONT key_font,
            HFONT font) {
  SelectObject(dc, font);
  SIZE size{};
  GetTextExtentPoint32W(dc, label.c_str(), int(label.size()), &size);
  SelectObject(dc, key_font);
  SIZE ksize{};
  GetTextExtentPoint32W(dc, key.c_str(), int(key.size()), &ksize);
  int kw = std::max(44, int(ksize.cx) + 18);
  RECT plate{x - 8, y - 6, x + kw + 16 + size.cx + 14, y + 50};
  Fill(dc, plate, kBlack);
  KeyCap(dc, x, y, key, key_font);
  SelectObject(dc, font);
  RECT t{x + kw + 12, y, plate.right, y + 44};
  Text(dc, label, t, kWhite, DT_LEFT | DT_VCENTER);
  return plate;
}

std::vector<uint8_t> Render() {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = kW;
  bi.bmiHeader.biHeight = -kH;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void* bits = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  HGDIOBJ old = SelectObject(dc, bmp);
  SetBkMode(dc, TRANSPARENT);
  RECT all{0, 0, kW, kH};
  Fill(dc, all, kKey);

  HFONT title = Font(120, FW_BOLD, true), sub = Font(52, FW_BOLD, true), row = Font(34),
        key_font = Font(26), prompt = Font(34), desc = Font(30, FW_NORMAL);
  // Heading: like the game's brush-stroke titles, on black.
  RECT banner{120, 70, 860, 205};
  Fill(dc, banner, kBlack);
  SelectObject(dc, title);
  Text(dc, g_mode == kModeHost ? L"HOST GAME!" : g_mode == kModeJoin ? L"JOIN GAME!" : L"PC SETTINGS!",
       RECT{150, 70, 860, 205}, kWhite, DT_LEFT | DT_VCENTER);
  RECT subbanner{520, 190, 1060, 262};
  Fill(dc, subbanner, kBlack);
  Frame(dc, subbanner, kWhite, 3);
  SelectObject(dc, sub);
  Text(dc, g_mode == kModeHost ? L"YOUR SERVER!" : g_mode == kModeJoin ? L"FIND A FIGHT!" : L"TUNE YOUR RIG!",
       subbanner, kWhite, DT_CENTER | DT_VCENTER);

  Layout& L = g_layout;
  constexpr int kRowH = 48;
  // Categories.
  for (int c = 0; c < kCategoryCount; ++c) L.categories[c] = RECT{0, 0, 0, 0};
  for (int c = 0; c < kCategoryCount && g_mode == kModeSettings; ++c) {
    RECT r{170, 330 + c * kRowH, 640, 330 + (c + 1) * kRowH};
    L.categories[c] = r;
    bool sel = c == g_category;
    Fill(dc, r, sel ? kGreen : kRow);
    Frame(dc, r, kBlack, 2);
    SelectObject(dc, row);
    RECT t = r;
    t.left += 18;
    Text(dc, kCategoryNames[c], t, sel ? kWhite : kBlack, DT_LEFT | DT_VCENTER);
  }
  // Rows of the category.
  std::vector<int> rows = RowsOf(g_category);
  constexpr int kVisible = 11;
  g_scroll = std::clamp(g_scroll, std::max(0, g_row - kVisible + 1), std::max(0, g_row));
  g_scroll = std::clamp(g_scroll, 0, std::max(0, int(rows.size()) - kVisible));
  L.rows.clear();
  L.first = g_scroll;
  for (int i = g_scroll; i < int(rows.size()) && i < g_scroll + kVisible; ++i) {
    const Setting& s = Page()[rows[i]];
    int y = 330 + (i - g_scroll) * kRowH;
    RECT r{g_mode == kModeSettings ? 700 : 170, y, 1750, y + kRowH};
    L.rows.push_back({rows[i], r});
    bool sel = g_focus == 1 && i == g_row;
    bool hi = i == g_row;
    Fill(dc, r, sel ? kGreen : kRow);
    Frame(dc, r, kBlack, 2);
    SelectObject(dc, row);
    COLORREF ink = sel ? kWhite : kBlack;
    RECT t = r;
    t.left += 18;
    Text(dc, s.label, t, ink, DT_LEFT | DT_VCENTER);
    // The value: arrows for a choice, between them the value.
    RECT v{r.right - 480, y, r.right - 50, y + kRowH};
    int k = int(L.rows.size()) - 1;
    L.left_arrow[k] = RECT{r.right - 510, y, r.right - 450, y + kRowH};
    L.right_arrow[k] = RECT{r.right - 60, y, r.right, y + kRowH};
    if (s.kind == Kind::kChoice) {
      Text(dc, L"◄", L.left_arrow[k], ink, DT_CENTER | DT_VCENTER);
      Text(dc, L"►", L.right_arrow[k], ink, DT_CENTER | DT_VCENTER);
    }
    Text(dc, ValueText(s, hi), v, ink, DT_CENTER | DT_VCENTER);
    if (s.restart) {
      SelectObject(dc, key_font);
      RECT tag{r.right - 650, y, r.right - 510, y + kRowH};
      Text(dc, L"RESTART", tag, sel ? kWhite : kGrey, DT_RIGHT | DT_VCENTER);
    }
  }
  // Scroll marks.
  SelectObject(dc, row);
  if (g_scroll > 0) Text(dc, L"▲", RECT{1700, 280, 1750, 328}, kWhite, DT_CENTER | DT_VCENTER);
  if (g_scroll + kVisible < int(rows.size()))
    Text(dc, L"▼", RECT{1700, 330 + kVisible * kRowH, 1750, 378 + kVisible * kRowH}, kWhite,
         DT_CENTER | DT_VCENTER);
  // Description of the selected row (or the status).
  std::wstring d;
  if (!g_status.empty()) d = g_status;
  else if (g_focus == 1 && g_row < int(rows.size())) {
    const Setting& s = Page()[rows[g_row]];
    d = s.desc;
    if (s.restart) d += L"  Applies when you start the game again.";
  } else {
    d = L"Choose a page. Changes are saved as you make them.";
  }
  RECT db{170, 880, 1750, 940};
  Fill(dc, db, kBlack);
  SelectObject(dc, desc);
  RECT dt = db;
  dt.left += 18;
  Text(dc, d, dt, kWhite, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
  // Prompts, bottom left, like the game's.
  L.select_prompt = Prompt(dc, 110, 975, L"↵", L"SELECT", key_font, prompt);
  L.back_prompt = Prompt(dc, L.select_prompt.right + 30, 975, L"⌫", L"BACK", key_font, prompt);
  Prompt(dc, L.back_prompt.right + 30, 975, L"← →", L"CHANGE", key_font, prompt);

  for (HFONT f : {title, sub, row, key_font, prompt, desc}) DeleteObject(f);
  GdiFlush();
  // To RGBA: the key colour is the dimmed game behind.
  std::vector<uint8_t> out(size_t(kW) * kH * 4);
  const uint32_t* px = static_cast<const uint32_t*>(bits);
  float white = xgpu::HdrWhite();
  uint8_t lut[256];
  for (int i = 0; i < 256; ++i) {
    if (white <= 0.0f) {
      lut[i] = uint8_t(i);
    } else {
      // HDR10: gamma 2.2 at the game's white, then PQ.
      double y = std::pow(i / 255.0, 2.2) * white / 10000.0;
      double m = std::pow(y, 0.1593017578125);
      double pq = std::pow((0.8359375 + 18.8515625 * m) / (1.0 + 18.6875 * m), 78.84375);
      lut[i] = uint8_t(std::clamp(pq * 255.0 + 0.5, 0.0, 255.0));
    }
  }
  for (size_t i = 0; i < size_t(kW) * kH; ++i) {
    uint32_t c = px[i] & 0xFFFFFF;
    uint8_t* o = &out[i * 4];
    if (c == 0xFF00FF) {
      o[0] = o[1] = o[2] = 0;
      o[3] = 215;
    } else {
      o[0] = lut[(c >> 16) & 0xFF], o[1] = lut[(c >> 8) & 0xFF], o[2] = lut[c & 0xFF];
      o[3] = 255;
    }
  }
  SelectObject(dc, old);
  DeleteObject(bmp);
  DeleteDC(dc);
  return out;
}

// ---- input -------------------------------------------------------------------

bool g_open = false;
bool g_dirty = true;
bool g_prev[256] = {};
bool g_wait_release = false;
POINT g_last_mouse{-1, -1};

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Change(Setting& s, int dir) {
  if (s.kind != Kind::kChoice) return;
  int n = int(s.values.size());
  s.index = (s.index + dir + n) % n;
  if (g_mode == kModeSettings) {
    ApplyLive();
    Save();
  }
  g_status.clear();
}

void Close();
void BuildJoinRows();
const std::string& PageValue(const char* key) {
  static const std::string none;
  for (const Setting& s : Page())
    if (!std::strcmp(s.key, key) && s.kind == Kind::kChoice) return s.values[s.index];
  return none;
}
const std::string& PageText(const char* key) {
  static const std::string none;
  for (const Setting& s : Page())
    if (!std::strcmp(s.key, key)) return s.text;
  return none;
}

void Activate(Setting& s) {
  if (s.kind == Kind::kButton) {
    if (s.button == kHostStart) {
      lan::Host(PageValue("host_map"), std::atoi(PageValue("bots").c_str()));
      Close();
    } else if (s.button == kJoinServer) {
      lan::Join(s.address, s.map);
      Close();
    } else if (s.button == kJoinAddress) {
      if (!PageText("address").empty()) {
        lan::Join(PageText("address"), PageValue("join_map"));
        Close();
      }
    } else if (s.button == kRefresh) {
      lan::Refresh();
      g_status = L"Looking for games on your network...";
    }
    return;
  }
  if (s.kind == Kind::kChoice) Change(s, 1);
  else if (s.kind == Kind::kKey) g_capture = true, g_wait_release = true;
  else g_editing = true, g_wait_release = true;
}

void Close() {
  g_open = false;
  g_capture = g_editing = false;
  if (g_mode == kModeSettings) Save();
  xgpu::SetOverlay(nullptr, 0, 0);
  // The keys that closed the page must not reach the game.
  g_wait_release = true;
}

}  // namespace

int Key(Action action, int slot) {
  if (slot == 0) return g_keys[action][0];
  int second = kActions[action].second;
  if (!second) return 0;
  for (int a = 0; a < kActionCount; ++a)
    if (g_keys[a][0] == second) return 0;
  return second;
}

bool IsOpen() { return g_open; }

namespace {
// The join page: the games found, then refresh and an address to type.
void BuildJoinRows() {
  std::string address, map = "Wake_island_s";
  for (const Setting& s : g_join_rows) {
    if (!std::strcmp(s.key, "address")) address = s.text;
    if (!std::strcmp(s.key, "join_map")) map = s.values[s.index];
  }
  g_join_rows.clear();
  static std::string keys[64];
  int k = 0;
  for (const lan::Server& sv : lan::Found()) {
    if (k >= 48) break;
    Setting s;
    s.category = kGraphics, s.kind = Kind::kButton, s.button = kJoinServer;
    keys[k] = "server" + std::to_string(k);
    s.key = keys[k].c_str();
    static std::wstring labels[64], descs[64];
    labels[k] = Wide(sv.name);
    for (wchar_t& c : labels[k]) c = towupper(c);
    descs[k] = L"Join " + Wide(sv.name) + L"'s game on " + MapName(sv.map) + L" at " + Wide(sv.address) +
               L" (" + std::to_wstring(sv.ping_ms) + L" ms).";
    s.label = labels[k].c_str();
    s.desc = descs[k].c_str();
    s.names = {MapName(sv.map)};
    s.address = sv.address, s.map = sv.map;
    g_join_rows.push_back(s);
    ++k;
  }
  Setting refresh;
  refresh.category = kGraphics, refresh.kind = Kind::kButton, refresh.key = "refresh", refresh.button = kRefresh;
  refresh.label = L"REFRESH";
  refresh.desc = L"Look again for games on your network.";
  refresh.names = {L"SEARCH"};
  g_join_rows.push_back(refresh);
  Setting addr;
  addr.category = kGraphics, addr.kind = Kind::kText, addr.key = "address", addr.label = L"ADDRESS";
  addr.desc = L"A game not on your network: type its IP address (Enter or click to type).";
  addr.text = address;
  g_join_rows.push_back(addr);
  Setting jm = Choice(kGraphics, "join_map", L"ITS MAP", L"The map that game is on.", kMapNames, kMaps, 0);
  for (size_t i = 0; i < kMaps.size(); ++i)
    if (kMaps[i] == map) jm.index = int(i);
  g_join_rows.push_back(jm);
  Setting go;
  go.category = kGraphics, go.kind = Kind::kButton, go.key = "join_address", go.button = kJoinAddress;
  go.label = L"JOIN ADDRESS";
  go.desc = L"Join the game at the address above.";
  go.names = {L"GO!"};
  g_join_rows.push_back(go);
}
}  // namespace

void Open() {
  g_mode = kModeSettings;
  g_open = true;
  g_focus = 0;
  g_dirty = true;
  g_wait_release = true;
  g_status.clear();
}

void OpenHost() {
  g_mode = kModeHost;
  g_host_rows.clear();
  g_host_rows.push_back(Choice(kGraphics, "host_map", L"MAP", L"The battlefield.", kMapNames, kMaps, 0));
  g_host_rows.push_back(Choice(kGraphics, "bots", L"BOTS",
                               L"Computer players that join your game (each runs as its own copy).",
                               {L"NONE", L"2", L"4", L"6", L"8", L"12", L"15"},
                               {"0", "2", "4", "6", "8", "12", "15"}, 0));
  Setting start;
  start.category = kGraphics, start.kind = Kind::kButton, start.key = "start", start.button = kHostStart;
  start.label = L"START SERVER";
  start.desc = L"Hosts the game on your network: others join it with JOIN GAME.";
  start.names = {L"GO!"};
  g_host_rows.push_back(start);
  g_open = true, g_focus = 1, g_row = 0, g_scroll = 0, g_dirty = true, g_wait_release = true;
  g_status.clear();
}

void OpenJoin() {
  g_mode = kModeJoin;
  lan::Refresh();
  BuildJoinRows();
  g_open = true, g_focus = 1, g_row = 0, g_scroll = 0, g_dirty = true, g_wait_release = true;
  g_status = L"Looking for games on your network...";
}

void Load() {
  if (g_settings.empty()) Build();
  for (int a = 0; a < kActionCount; ++a) g_keys[a][0] = kActions[a].key;
  // Current values: the launcher's settings, then the page's own file.
  std::filesystem::path lp = LauncherPath();
  if (!lp.empty()) {
    auto json = ReadJson(lp);
    for (Setting& s : g_settings) {
      if (!s.launcher || !json.count(s.launcher)) continue;
      std::string v = json[s.launcher];
      if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
      if (v == "true") v = "1";
      if (v == "false") v = "0";
      if (s.kind == Kind::kText) s.text = v;
      else Select(s, v);
    }
  }
  std::ifstream ini(IniPath());
  std::string line;
  while (std::getline(ini, line)) {
    size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    Setting* s = Find(key.c_str());
    if (!s || s->launcher) continue;  // the launcher's own are read from it
    if (s->kind == Kind::kKey) {
      if (int vk = std::atoi(value.c_str())) g_keys[s->action][0] = vk;
    } else if (s->kind == Kind::kChoice) {
      Select(*s, value);
    }
  }
  // This run: the name and frame limit as started; stock stays stock.
  if (Setting* s = Find("name")) s->text = g_gamertag;
  if (Setting* s = Find("fps")) Select(*s, std::to_string(g_max_fps));
  g_running_look = Value("preset") == "stock" ? 0 : 1;
  xe::gpu::pbr::SetPomScale(Value("parallax") == "0" ? 0.0f : 1.0f);
  ApplyLive();
}

bool Update(HWND__* window) {
  bool pressed[256] = {};
  for (int vk = 1; vk < 256; ++vk) {
    bool d = window && Down(vk);
    pressed[vk] = d && !g_prev[vk];
    g_prev[vk] = d;
  }
  if (!g_open) {
    if (pressed[VK_F10]) Open();
    if (!g_open) {
      // After closing: nothing until the keys are let go.
      if (g_wait_release) {
        bool any = false;
        for (int vk = 1; vk < 256; ++vk) any |= g_prev[vk];
        if (any) return true;
        g_wait_release = false;
      }
      return false;
    }
  }
  // Wait for the keys that opened the page (or a row) to be let go.
  if (g_wait_release) {
    bool any = false;
    for (int vk = 1; vk < 256; ++vk) any |= g_prev[vk];
    if (any) {
      if (g_dirty) xgpu::SetOverlay(Render().data(), kW, kH), g_dirty = false;
      return true;
    }
    g_wait_release = false;
    for (int vk = 1; vk < 256; ++vk) pressed[vk] = false;
  }
  static bool was_browsing = false;
  if (g_mode == kModeJoin && was_browsing && !lan::Browsing()) {
    BuildJoinRows();
    size_t n = lan::Found().size();
    g_status = n ? L"" : L"No games found on your network. Refresh, or type an address.";
    g_row = std::min(g_row, int(g_join_rows.size()) - 1);
    g_dirty = true;
  }
  was_browsing = lan::Browsing();
  std::vector<int> rows = RowsOf(g_category);
  auto sel_setting = [&]() -> Setting* {
    return g_row < int(rows.size()) ? &Page()[rows[g_row]] : nullptr;
  };
  bool changed = false;
  if (g_capture) {
    // The next key or button: bound (Esc cancels).
    for (int vk = 1; vk < 256; ++vk) {
      if (!pressed[vk] || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) continue;
      int bind = vk;
      if (vk == VK_LSHIFT || vk == VK_RSHIFT) bind = VK_SHIFT;
      if (vk == VK_LCONTROL || vk == VK_RCONTROL) bind = VK_CONTROL;
      if (vk == VK_ESCAPE) {
        g_capture = false;
      } else if (Setting* s = sel_setting()) {
        // A key bound elsewhere moves here.
        for (int a = 0; a < kActionCount; ++a)
          if (g_keys[a][0] == bind && a != s->action) g_keys[a][0] = 0;
        g_keys[s->action][0] = bind;
        g_capture = false;
        Save();
      }
      g_wait_release = true;
      changed = true;
      break;
    }
  } else if (g_editing) {
    Setting* s = sel_setting();
    bool shift = Down(VK_SHIFT);
    for (int vk = 1; vk < 256 && s; ++vk) {
      if (!pressed[vk]) continue;
      if (vk == VK_RETURN || vk == VK_ESCAPE || vk == VK_LBUTTON) {
        g_editing = false;
        if (g_mode == kModeSettings) Save();
        g_wait_release = true;
      } else if (vk == VK_BACK) {
        if (!s->text.empty()) s->text.pop_back();
      } else if (s->text.size() < 15) {
        char c = 0;
        if (vk >= 'A' && vk <= 'Z') c = char(shift ? vk : vk + 32);
        else if (vk >= '0' && vk <= '9') c = char(vk);
        else if (vk == VK_SPACE) c = ' ';
        else if (vk == VK_OEM_MINUS) c = shift ? '_' : '-';
        else if (vk == VK_OEM_PERIOD) c = '.';
        if (c) s->text += c;
      }
      changed = true;
    }
  } else {
    // Mouse: hover, click, wheel.
    POINT cursor;
    RECT client;
    int mouse_over_cat = -1, mouse_over_row = -1, mouse_arrow = 0;
    bool over_back = false;
    if (window && GetCursorPos(&cursor) && ScreenToClient(window, &cursor) &&
        GetClientRect(window, &client) && client.right > 0 && client.bottom > 0) {
      POINT p{LONG(cursor.x * kW / client.right), LONG(cursor.y * kH / client.bottom)};
      bool moved = cursor.x != g_last_mouse.x || cursor.y != g_last_mouse.y;
      g_last_mouse = cursor;
      for (int c = 0; c < kCategoryCount; ++c)
        if (PtInRect(&g_layout.categories[c], p)) mouse_over_cat = c;
      for (size_t i = 0; i < g_layout.rows.size(); ++i) {
        if (!PtInRect(&g_layout.rows[i].second, p)) continue;
        mouse_over_row = g_layout.first + int(i);
        if (PtInRect(&g_layout.left_arrow[i], p)) mouse_arrow = -1;
        if (PtInRect(&g_layout.right_arrow[i], p)) mouse_arrow = 1;
      }
      over_back = PtInRect(&g_layout.back_prompt, p);
      if (moved && mouse_over_cat >= 0 && (g_focus != 0 || g_category != mouse_over_cat)) {
        g_focus = 0, g_category = mouse_over_cat, g_row = 0, g_scroll = 0, changed = true;
      }
      if (moved && mouse_over_row >= 0 && (g_focus != 1 || g_row != mouse_over_row)) {
        g_focus = 1, g_row = mouse_over_row, changed = true;
      }
    }
    if (pressed[VK_LBUTTON]) {
      if (over_back) {
        if (g_focus == 1 && g_mode == kModeSettings) g_focus = 0;
        else Close();
      } else if (mouse_over_cat >= 0) {
        g_category = mouse_over_cat, g_focus = 1, g_row = 0, g_scroll = 0;
      } else if (mouse_over_row >= 0) {
        g_focus = 1, g_row = mouse_over_row;
        if (Setting* s = sel_setting()) {
          if (mouse_arrow) Change(*s, mouse_arrow);
          else Activate(*s);
        }
      }
      changed = true;
    }
    if (pressed[VK_RBUTTON]) {
      if (g_focus == 1 && g_mode == kModeSettings) g_focus = 0;
      else Close();
      changed = true;
    }
    if (pressed[VK_RETURN] || pressed[VK_SPACE]) {
      if (g_focus == 0) g_focus = 1, g_row = 0, g_scroll = 0;
      else if (Setting* s = sel_setting()) Activate(*s);
      changed = true;
    }
    if (pressed[VK_ESCAPE] || pressed[VK_BACK] || pressed[VK_F10]) {
      if (g_focus == 1 && !pressed[VK_F10] && g_mode == kModeSettings) g_focus = 0;
      else Close();
      changed = true;
    }
    int dv = (pressed[VK_DOWN] || pressed['S']) - (pressed[VK_UP] || pressed['W']);
    int dh = (pressed[VK_RIGHT] || pressed['D']) - (pressed[VK_LEFT] || pressed['A']);
    if (dv) {
      if (g_focus == 0) g_category = (g_category + dv + kCategoryCount) % kCategoryCount, g_row = 0, g_scroll = 0;
      else if (!rows.empty()) g_row = (g_row + dv + int(rows.size())) % int(rows.size());
      changed = true;
    }
    if (dh) {
      if (g_focus == 0 && dh > 0) g_focus = 1, g_row = 0, g_scroll = 0;
      else if (g_focus == 1 && dh < 0 && sel_setting() && sel_setting()->kind != Kind::kChoice &&
               g_mode == kModeSettings)
        g_focus = 0;
      else if (g_focus == 1 && sel_setting()) Change(*sel_setting(), dh);
      changed = true;
    }
  }
  if (!g_open) return true;
  if (changed || g_dirty) {
    xgpu::SetOverlay(Render().data(), kW, kH);
    g_dirty = false;
  }
  return true;
}

}  // namespace rt::pcset
