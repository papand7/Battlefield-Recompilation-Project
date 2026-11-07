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
#include "pc_menu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>

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
  bool touched = false;              // changed on the page this run (only those go to the launcher)
};
enum Button { kHostStart = 1, kJoinServer, kJoinAddress, kRefresh };

std::vector<Setting> g_settings;
// HOST GAME and JOIN GAME use the page too (lan.cpp does the networking).
enum Mode { kModeSettings, kModeHost, kModeJoin, kModeMain, kModePause };
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
    // Only what was changed here: the rest of this run (its name, frame
    // limit... from the command line) must not overwrite the launcher's.
    if (!s.launcher || !s.touched) continue;
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

// ---- screens -----------------------------------------------------------------
//
// The look is pc_menu.cpp's; here each screen is described as a View. Main
// menu (after the title screen, the game's own front end kept underneath and
// never shown) and pause menu (Esc / Start in a level) are big entries on an
// olive band; settings, host and join are lists.

int g_category = 0, g_row = 0, g_focus = 0;  // focus 0 tabs, 1 rows
int g_scroll = 0;
bool g_capture = false, g_editing = false;
std::wstring g_status;
int g_button = 0;   // main / pause menu entry
int g_page = 0;     // a new page slides in
Mode g_back_to = kModeSettings;  // Back from a list: to the main or pause menu (kModeSettings: close)
bool g_pad_used = false;
bool g_suppress_main = false;    // a game is starting: no main menu until the next front end
// The game's own pause menu, worked by pad presses under a STAND BY screen
// (suicide, quit): its rows My Squad, Scoreboard, Help & Options,
// Leaderboards, Achievements, Exit Game, Suicide; Exit and Suicide ask
// "Are you sure?" (A = OK).
struct Puppet {
  bool on = false;
  int row = 0, stage = 0;
  double t0 = 0, press_until = 0, next_pulse = 0;
  uint16_t press = 0;
  int tries = 0;
} g_puppet;
constexpr int kExitGameRow = 5, kSuicideRow = 6;

const std::vector<std::wstring> kMainButtons = {L"HOST GAME", L"JOIN GAME", L"SETTINGS", L"QUIT"};
const std::vector<std::wstring> kMainNotes = {
    L"Start a battle on your network. Pick the island and how many bots fight with you; "
    L"others join you from JOIN GAME.",
    L"Find a battle on your network, or type the address of one further away.",
    L"Graphics, effects, mouse and key bindings, and your call sign.",
    L"Leave the Pacific and return to the desktop."};
const std::vector<std::wstring> kPauseButtons = {L"RESUME", L"SETTINGS", L"SUICIDE", L"QUIT TO MENU"};
const std::vector<std::wstring> kPauseNotes = {
    L"Back to the fight.", L"Graphics, effects, mouse and key bindings.",
    L"Respawn at once, elsewhere.", L"Leave this battle for the main menu."};

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
bool IsList() { return g_mode == kModeSettings || g_mode == kModeHost || g_mode == kModeJoin; }

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

void ClampScroll(int count) {
  constexpr int kVisible = pcmenu::kVisibleRows;
  g_scroll = std::clamp(g_scroll, std::max(0, g_row - kVisible + 1), std::max(0, g_row));
  g_scroll = std::clamp(g_scroll, 0, std::max(0, count - kVisible));
}

pcmenu::View BuildView() {
  pcmenu::View v;
  v.pad = g_pad_used;
  v.page = g_page;
  std::wstring sel = g_pad_used ? L"A" : L"ENTER", back = g_pad_used ? L"B" : L"ESC";
  if (g_puppet.on) {
    v.screen = pcmenu::Screen::kBusy;
    v.desc = g_puppet.row == kExitGameRow ? L"Returning to headquarters" : L"Requesting a new deployment";
    return v;
  }
  if (g_mode == kModeMain || g_mode == kModePause) {
    bool main = g_mode == kModeMain;
    v.screen = pcmenu::Screen::kButtons;
    v.opaque = main;
    v.subtitle = main ? L"PACIFIC THEATRE - FIELD ORDERS" : L"IN THE FIELD - ORDERS PAUSED";
    v.buttons = main ? kMainButtons : kPauseButtons;
    v.button = g_button;
    v.desc = (main ? kMainNotes : kPauseNotes)[g_button];
    v.prompts = {{sel, L"SELECT"}};
    if (!main) v.prompts.push_back({back, L"RESUME"});
    return v;
  }
  v.screen = pcmenu::Screen::kList;
  v.title = g_mode == kModeHost ? L"HOST GAME" : g_mode == kModeJoin ? L"JOIN GAME" : L"SETTINGS";
  v.subtitle = g_mode == kModeHost   ? L"Your server, your island."
               : g_mode == kModeJoin ? L"Find a fight on the network."
                                     : L"Tune your rig, soldier.";
  if (g_mode == kModeSettings) {
    for (int c = 0; c < kCategoryCount; ++c) v.tabs.push_back(kCategoryNames[c]);
    v.tab = g_category;
    v.tab_focus = g_focus == 0;
  }
  std::vector<int> rows = RowsOf(g_category);
  ClampScroll(int(rows.size()));
  for (int i = g_scroll; i < int(rows.size()) && i < g_scroll + pcmenu::kVisibleRows; ++i) {
    const Setting& s = Page()[rows[i]];
    pcmenu::Row r;
    r.label = s.label;
    r.value = ValueText(s, i == g_row);
    r.choice = s.kind == Kind::kChoice;
    if (s.restart) r.tag = L"RESTART";
    v.rows.push_back(r);
  }
  v.row = g_focus == 1 && g_row >= g_scroll && g_row < g_scroll + pcmenu::kVisibleRows ? g_row - g_scroll : -1;
  v.more_above = g_scroll > 0;
  v.more_below = g_scroll + pcmenu::kVisibleRows < int(rows.size());
  if (!g_status.empty()) {
    v.desc = g_status;
  } else if (g_focus == 1 && g_row < int(rows.size())) {
    const Setting& s = Page()[rows[g_row]];
    v.desc = s.desc;
    if (s.restart) v.desc += L"  Applies when you start the game again.";
  } else {
    v.desc = L"Choose a page. Changes are saved as you make them.";
  }
  v.prompts = {{sel, L"SELECT"}, {back, L"BACK"}, {g_pad_used ? L"LB RB" : L"< >", g_pad_used ? L"PAGE" : L"CHANGE"}};
  return v;
}

// ---- input -------------------------------------------------------------------

bool g_open = false;
bool g_dirty = true;
bool g_prev[256] = {};
bool g_wait_release = false;
POINT g_last_mouse{-1, -1};

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 || SimulatedKeyDown(vk); }

void Change(Setting& s, int dir) {
  if (s.kind != Kind::kChoice) return;
  int n = int(s.values.size());
  s.index = (s.index + dir + n) % n;
  s.touched = true;
  if (g_mode == kModeSettings) {
    ApplyLive();
    Save();
  }
  g_status.clear();
}

void Close();
void BuildJoinRows();
void OpenMode(Mode mode, Mode back_to);
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

// A game is starting: the menus give way to the game's loading screen.
void StartingGame() {
  g_suppress_main = true;
  Close();
}

void Activate(Setting& s) {
  if (s.kind == Kind::kButton) {
    if (s.button == kHostStart) {
      lan::Host(PageValue("host_map"), std::atoi(PageValue("bots").c_str()));
      StartingGame();
    } else if (s.button == kJoinServer) {
      lan::Join(s.address, s.map);
      StartingGame();
    } else if (s.button == kJoinAddress) {
      if (!PageText("address").empty()) {
        lan::Join(PageText("address"), PageValue("join_map"));
        StartingGame();
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

void StartPuppet(int row) {
  g_puppet = Puppet{};
  g_puppet.on = true;
  g_puppet.row = row;
  g_puppet.t0 = 0;
  g_dirty = true;
}

// Main and pause menu entries.
void PressButton() {
  if (g_mode == kModeMain) {
    switch (g_button) {
      case 0: OpenMode(kModeHost, kModeMain); break;
      case 1: OpenMode(kModeJoin, kModeMain); break;
      case 2: OpenMode(kModeSettings, kModeMain); break;
      case 3: TerminateProcess(GetCurrentProcess(), 0); break;
    }
  } else {
    switch (g_button) {
      case 0: Close(); break;
      case 1: OpenMode(kModeSettings, kModePause); break;
      case 2: StartPuppet(kSuicideRow); break;
      case 3: StartPuppet(kExitGameRow); break;
    }
  }
}

// Back from a page: to the menu it came from, or closed.
void GoBack() {
  if (g_mode == kModeMain) return;  // the main menu stays
  if (g_mode == kModePause) {
    Close();
    return;
  }
  if (g_mode == kModeSettings && g_focus == 1) {
    g_focus = 0;
    return;
  }
  if (g_mode == kModeSettings) Save();
  if (g_back_to == kModeMain || g_back_to == kModePause) {
    Mode to = g_back_to, from = g_mode;
    g_mode = to;
    g_button = to == kModePause ? 1 : from == kModeHost ? 0 : from == kModeJoin ? 1 : 2;
    g_back_to = kModeSettings;
    ++g_page;
    g_status.clear();
  } else {
    Close();
  }
}

void Close() {
  g_open = false;
  g_capture = g_editing = false;
  if (g_mode == kModeSettings) Save();
  pcmenu::Hide();
  // The keys that closed the page must not reach the game.
  g_wait_release = true;
}

// One poll of the game's pause menu worked under the STAND BY screen.
uint16_t PuppetStep(double now) {
  Puppet& p = g_puppet;
  if (!p.on) return 0;
  if (p.t0 == 0) p.t0 = now;
  if (now < p.press_until) return p.press;
  float rows[32][4];
  int selected = -1;
  int count = xgpu::MenuRows(rows, 32, &selected);
  auto press = [&](uint16_t b, double hold) {
    p.press = b;
    p.press_until = now + hold;
    return b;
  };
  double t = now - p.t0;
  switch (p.stage) {
    case 0:  // open the game's menu (Start), until its seven rows are up
      if (count == 7 && selected >= 0) {
        p.stage = 1, p.t0 = now;
        return 0;
      }
      if (t > 8) break;  // gave up
      if (now >= p.next_pulse) {
        // Start opens it in play; on the spawn screen Back (B) does.
        p.next_pulse = now + 1.2;
        return press(++p.tries % 2 ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_B, 0.12);
      }
      return 0;
    case 1:  // step to the row
      if (t > 5) break;
      if (count == 7 && selected == p.row) {
        p.stage = 2, p.t0 = now;
        return press(XINPUT_GAMEPAD_A, 0.12);
      }
      if (now >= p.next_pulse && selected >= 0) {
        p.next_pulse = now + 0.12;
        return press(selected < p.row ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP, 0.06);
      }
      return 0;
    case 2:  // "Are you sure?": OK
      if (t > 0.7) {
        p.stage = 3, p.t0 = now;
        return press(XINPUT_GAMEPAD_A, 0.12);
      }
      return 0;
    case 3:
      if (t > 0.6) break;
      return 0;
  }
  // Done (or given up): the game carries on from there.
  bool quit = p.row == kExitGameRow;
  p = Puppet{};
  if (quit) g_suppress_main = false;
  Close();
  return 0;
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

void OpenMode(Mode mode, Mode back_to) {
  g_mode = mode;
  g_back_to = back_to;
  g_open = true;
  g_dirty = true;
  g_wait_release = true;
  g_status.clear();
  g_row = 0, g_scroll = 0;
  ++g_page;
  if (mode == kModeSettings) {
    g_focus = 0;
  } else if (mode == kModeHost) {
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
    g_focus = 1;
  } else if (mode == kModeJoin) {
    lan::Refresh();
    BuildJoinRows();
    g_focus = 1;
    g_status = L"Looking for games on your network...";
  } else {
    g_button = 0;
  }
}
}  // namespace

void Open() { OpenMode(kModeSettings, g_open && g_mode == kModeMain ? kModeMain : kModeSettings); }
void OpenHost() { OpenMode(kModeHost, kModeSettings); }
void OpenJoin() { OpenMode(kModeJoin, kModeSettings); }

bool ControlsPad() { return g_open || g_puppet.on || g_wait_release; }
uint16_t PuppetButtons() {
  using namespace std::chrono;
  return PuppetStep(duration<double>(steady_clock::now().time_since_epoch()).count());
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

bool Update(HWND__* window, bool title_passed) {
  using namespace std::chrono;
  double now = duration<double>(steady_clock::now().time_since_epoch()).count();
  bool pressed[256] = {};
  for (int vk = 1; vk < 256; ++vk) {
    bool d = (window && (GetAsyncKeyState(vk) & 0x8000) != 0) || SimulatedKeyDown(vk);
    pressed[vk] = d && !g_prev[vk];
    g_prev[vk] = d;
  }
  // The pad, read here too: menus answer it, the game doesn't see it.
  XINPUT_STATE pad{};
  bool have_pad = XInputGetState(0, &pad) == ERROR_SUCCESS;
  static WORD pad_prev = 0;
  static double repeat_at = 0;
  static int repeat_dir = 0;
  WORD pb = have_pad ? pad.Gamepad.wButtons : 0;
  // Left stick as the D-pad.
  if (have_pad) {
    if (pad.Gamepad.sThumbLY > 16000) pb |= XINPUT_GAMEPAD_DPAD_UP;
    if (pad.Gamepad.sThumbLY < -16000) pb |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (pad.Gamepad.sThumbLX > 16000) pb |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (pad.Gamepad.sThumbLX < -16000) pb |= XINPUT_GAMEPAD_DPAD_LEFT;
  }
  WORD pad_pressed = pb & ~pad_prev;
  pad_prev = pb;
  // Held directions repeat.
  const WORD kDirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
  if (pad_pressed & kDirs) repeat_dir = pad_pressed & kDirs, repeat_at = now + 0.35;
  else if ((pb & repeat_dir) && now >= repeat_at) pad_pressed |= repeat_dir, repeat_at = now + 0.09;
  else if (!(pb & repeat_dir)) repeat_dir = 0;
  if (pad_pressed) g_pad_used = true;
  for (int vk = 1; vk < 256; ++vk)
    if (pressed[vk]) g_pad_used = false;

  // The game's pause menu being worked: nothing else.
  if (g_puppet.on) {
    if (g_dirty) pcmenu::Show(BuildView()), g_dirty = false;
    return true;
  }

  // The main menu: once the title is past and the game's front end idle
  // (no dialog up), in place of the game's.
  if (!g_in_menus) g_suppress_main = false;
  static double title_at = 0;
  if (title_passed && title_at == 0) title_at = now;
  int dialogs = GameDialogs();
  if (dialogs < 0 && title_at > 0 && now - title_at > 6) dialogs = 0;  // never asked: none
  if (CustomMenus() && !g_open && g_in_menus && title_passed && dialogs == 0 && !g_suppress_main) {
    OpenMode(kModeMain, kModeSettings);
    g_wait_release = false;
  }
  // A level loading under the main menu (or its pages): the game takes over.
  if (g_open && !g_in_menus && g_mode != kModePause && g_back_to != kModePause && g_mode != kModeSettings) Close();
  // In a level: Esc (the pause key) or Start opens the pause menu.
  bool pause_key = false;
  for (int slot = 0; slot < 2; ++slot)
    if (int vk = Key(kMenu, slot); vk && pressed[vk]) pause_key = true;
  if (CustomMenus() && !g_open && !g_in_menus && !g_wait_release &&
      (pause_key || (pad_pressed & XINPUT_GAMEPAD_START))) {
    OpenMode(kModePause, kModeSettings);
    for (int vk = 1; vk < 256; ++vk) pressed[vk] = false;
    pad_pressed = 0;
  }

  if (!g_open) {
    if (pressed[VK_F10]) OpenMode(kModeSettings, kModeSettings);
    if (!g_open) {
      // After closing: nothing until the keys and buttons are let go.
      if (g_wait_release) {
        bool any = pb != 0;
        for (int vk = 1; vk < 256; ++vk) any |= g_prev[vk];
        if (any) return true;
        g_wait_release = false;
      }
      return false;
    }
  }
  // Wait for the keys that opened the page (or a row) to be let go.
  if (g_wait_release) {
    bool any = (pb & ~kDirs) != 0;
    for (int vk = 1; vk < 256; ++vk) any |= g_prev[vk];
    if (any) {
      if (g_dirty) pcmenu::Show(BuildView()), g_dirty = false;
      return true;
    }
    g_wait_release = false;
    for (int vk = 1; vk < 256; ++vk) pressed[vk] = false;
    pad_pressed = 0;
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
    return IsList() && g_row < int(rows.size()) ? &Page()[rows[g_row]] : nullptr;
  };
  // Abstract navigation from keys and pad.
  int dv = (pressed[VK_DOWN] || pressed['S'] || (pad_pressed & XINPUT_GAMEPAD_DPAD_DOWN)) -
           (pressed[VK_UP] || pressed['W'] || (pad_pressed & XINPUT_GAMEPAD_DPAD_UP));
  int dh = (pressed[VK_RIGHT] || pressed['D'] || (pad_pressed & XINPUT_GAMEPAD_DPAD_RIGHT)) -
           (pressed[VK_LEFT] || pressed['A'] || (pad_pressed & XINPUT_GAMEPAD_DPAD_LEFT));
  bool accept = pressed[VK_RETURN] || pressed[VK_SPACE] || (pad_pressed & XINPUT_GAMEPAD_A);
  bool back = pressed[VK_ESCAPE] || pressed[VK_BACK] || (pad_pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_BACK)) ||
              (g_mode == kModePause && (pad_pressed & XINPUT_GAMEPAD_START));
  int dtab = ((pad_pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0) - ((pad_pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0);
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
    if (pad_pressed & XINPUT_GAMEPAD_B) g_capture = false, changed = true;
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
        s->touched = true;
      } else if (s->text.size() < 15) {
        char c = 0;
        if (vk >= 'A' && vk <= 'Z') c = char(shift ? vk : vk + 32);
        else if (vk >= '0' && vk <= '9') c = char(vk);
        else if (vk == VK_SPACE) c = ' ';
        else if (vk == VK_OEM_MINUS) c = shift ? '_' : '-';
        else if (vk == VK_OEM_PERIOD) c = '.';
        if (c) s->text += c, s->touched = true;
      }
      changed = true;
    }
    if (pad_pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B)) g_editing = false, changed = true;
  } else {
    // Mouse: hover, click.
    POINT cursor;
    RECT client;
    int over_button = -1, over_tab = -1, over_row = -1, mouse_arrow = 0, over_prompt = -1;
    if (window && GetCursorPos(&cursor) && ScreenToClient(window, &cursor) && GetClientRect(window, &client) &&
        client.right > 0 && client.bottom > 0) {
      int x = int(cursor.x * pcmenu::kW / client.right), y = int(cursor.y * pcmenu::kH / client.bottom);
      bool moved = cursor.x != g_last_mouse.x || cursor.y != g_last_mouse.y;
      g_last_mouse = cursor;
      if (g_mode == kModeMain || g_mode == kModePause) {
        for (int i = 0; i < 4; ++i)
          if (pcmenu::ButtonRect(i).Has(x, y)) over_button = i;
        if (moved && over_button >= 0 && over_button != g_button) g_button = over_button, changed = true;
        for (int i = 0; i < 2; ++i)
          if (pcmenu::Rect{76 + i * 280, 985, 76 + i * 280 + 270, 1041}.Has(x, y)) over_prompt = i;
      } else {
        for (int c = 0; c < kCategoryCount && g_mode == kModeSettings; ++c)
          if (pcmenu::TabRect(c).Has(x, y)) over_tab = c;
        int visible = std::min(pcmenu::kVisibleRows, int(rows.size()) - g_scroll);
        for (int i = 0; i < visible; ++i) {
          if (!pcmenu::RowRect(i).Has(x, y)) continue;
          over_row = g_scroll + i;
          if (pcmenu::LeftArrowRect(i).Has(x, y)) mouse_arrow = -1;
          if (pcmenu::RightArrowRect(i).Has(x, y)) mouse_arrow = 1;
        }
        for (int i = 0; i < 3; ++i)
          if (pcmenu::PromptRect(i).Has(x, y)) over_prompt = i;
        if (moved && over_tab >= 0 && (g_focus != 0 || g_category != over_tab))
          g_focus = 0, g_category = over_tab, g_row = 0, g_scroll = 0, changed = true;
        if (moved && over_row >= 0 && (g_focus != 1 || g_row != over_row)) g_focus = 1, g_row = over_row, changed = true;
      }
    }
    if (pressed[VK_LBUTTON]) {
      if (over_prompt == 1) back = true;
      else if (over_prompt == 0) accept = true;
      else if (over_button >= 0) g_button = over_button, accept = true;
      else if (over_tab >= 0) g_category = over_tab, g_focus = 1, g_row = 0, g_scroll = 0, changed = true;
      else if (over_row >= 0) {
        g_focus = 1, g_row = over_row;
        if (Setting* s = sel_setting()) {
          if (mouse_arrow) Change(*s, mouse_arrow);
          else Activate(*s);
        }
        changed = true;
      }
    }
    if (pressed[VK_RBUTTON]) back = true;
    if (pressed[VK_F10] && g_mode == kModeSettings && g_back_to == kModeSettings) back = true, g_focus = 0;
    if (accept) {
      if (g_mode == kModeMain || g_mode == kModePause) PressButton();
      else if (g_focus == 0) g_focus = 1, g_row = 0, g_scroll = 0;
      else if (Setting* s = sel_setting()) Activate(*s);
      changed = true;
    } else if (back) {
      GoBack();
      changed = true;
    } else {
      if (g_mode == kModeMain || g_mode == kModePause) {
        if (dv) g_button = (g_button + dv + 4) % 4, changed = true;
      } else {
        if (dtab && g_mode == kModeSettings) {
          g_category = (g_category + dtab + kCategoryCount) % kCategoryCount, g_row = 0, g_scroll = 0;
          changed = true;
        }
        if (dv) {
          if (g_focus == 0) g_category = (g_category + dv + kCategoryCount) % kCategoryCount, g_row = 0, g_scroll = 0;
          else if (!rows.empty()) g_row = (g_row + dv + int(rows.size())) % int(rows.size());
          changed = true;
        }
        if (dh) {
          if (g_focus == 0 && dh > 0) g_focus = 1, g_row = 0, g_scroll = 0;
          else if (g_focus == 0 && g_mode == kModeSettings)
            g_category = (g_category + dh + kCategoryCount) % kCategoryCount, g_row = 0, g_scroll = 0;
          else if (g_focus == 1 && dh < 0 && sel_setting() && sel_setting()->kind != Kind::kChoice &&
                   g_mode == kModeSettings)
            g_focus = 0;
          else if (g_focus == 1 && sel_setting()) Change(*sel_setting(), dh);
          changed = true;
        }
      }
    }
  }
  if (!g_open && !g_puppet.on) return true;
  if (changed || g_dirty) {
    pcmenu::Show(BuildView());
    g_dirty = false;
  }
  return true;
}

}  // namespace rt::pcset
