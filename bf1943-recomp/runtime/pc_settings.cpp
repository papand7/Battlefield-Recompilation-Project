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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>
#else
#include "posix_gdiplus.h"  // (Linux: key names, XInput, the launcher's folder)
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
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
enum Category { kGraphics, kEffects, kControls, kKeys, kSound, kProfile, kCategoryCount };
const wchar_t* const kCategoryNames[kCategoryCount] = {L"GRAPHICS", L"EFFECTS", L"CONTROLS",
                                                       L"KEYS", L"SOUND", L"PROFILE"};

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
                     L"Enhanced: every effect. Vivid is enhanced, brighter and more colourful; Toon, Noir and WW2 are enhanced with a film look. "
                     L"Stock: the game as it was (from stock or to it on the next start).",
                     {L"STOCK", L"ENHANCED", L"VIVID", L"TOON", L"NOIR", L"WW2"},
                     {"stock", "enhanced", "vivid", "toon", "noir", "ww2"}, 1, false, "preset"));
  v.push_back(Choice(kGraphics, "resolution", L"RESOLUTION SCALE",
                     L"The game drawn at 1, 2 or 3 times its own 1280x720.",
                     {L"1X (720P)", L"2X (1440P)", L"3X (2160P)"}, {"1", "2", "3"}, 1, true,
                     "resolution"));
  v.push_back(Choice(kGraphics, "upscaling", L"UPSCALING",
                     L"How the frame is fitted to the window. FSR 3 and DLSS (NVIDIA RTX; AUTO picks): "
                     L"temporal, from the resolution scale up to a larger window (1X to 1440P or 4K).",
                     {L"FSR", L"CAS SHARPEN", L"BILINEAR", L"FSR 3", L"DLSS", L"AUTO (DLSS / FSR 3)"},
                     {"fsr", "cas", "none", "fsr3", "dlss", "auto"}, 0, true,
                     "upscaling"));
  v.push_back(Choice(kGraphics, "upscale_quality", L"UPSCALE QUALITY",
                     L"With FSR 3, DLSS or AUTO: picks the resolution scale for your window or screen. "
                     L"QUALITY renders about 1.5x smaller, PERFORMANCE about 2x. OFF: the RESOLUTION SCALE above.",
                     {L"OFF", L"QUALITY", L"BALANCED", L"PERFORMANCE"}, {"off", "quality", "balanced", "performance"}, 0,
                     true, "upscaleQuality"));
  v.push_back(Choice(kGraphics, "window", L"WINDOW SIZE", L"The game window's size.",
                     {L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440", L"3840 x 2160"},
                     {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, 2, true,
                     "window"));
  v.push_back(Choice(kGraphics, "display", L"DISPLAY MODE",
                     L"Borderless fullscreen on the window's monitor, or a window. Alt+Enter switches.",
                     {L"WINDOWED", L"FULLSCREEN"}, {"windowed", "fullscreen"}, 0, false, "display"));
  v.push_back(Choice(kGraphics, "fov", L"FIELD OF VIEW",
                     L"How wide you see, in degrees across a 16:9 screen (the game's own: 86). "
                     L"Aiming and vehicles zoom as before.",
                     {L"86 (GAME)", L"90", L"95", L"100", L"105", L"110"}, {"0", "90", "95", "100", "105", "110"}, 0,
                     false, "fov"));
  v.push_back(Choice(kGraphics, "camera_shake", L"CAMERA SHAKE",
                     L"How hard the view shakes from blasts and hits.",
                     {L"OFF", L"25%", L"50%", L"75%", L"100%", L"150%", L"200%"},
                     {"0", "0.25", "0.5", "0.75", "1", "1.5", "2"}, 4));
  v.push_back(Choice(kGraphics, "motion_blur", L"MOTION BLUR",
                     L"The game's blur from explosions, gunfire and low health.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kGraphics, "fps", L"FRAME RATE LIMIT", L"Most frames a second.",
                     {L"30", L"60", L"90", L"120", L"144", L"150", L"165", L"180", L"240", L"UNLIMITED"},
                     {"30", "60", "90", "120", "144", "150", "165", "180", "240", "0"}, 3, false, "frameRate"));
  v.push_back(Choice(kGraphics, "skip_intro", L"SKIP INTRO",
                     L"Fast-forwards the logos and cards before the title screen.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kGraphics, "fps_overlay", L"FPS COUNTER",
                     L"In the top-left corner: the frame rate, or FULL with frame times and the 1% low. F2 switches.",
                     {L"OFF", L"FPS", L"FULL"}, {"0", "1", "2"}, 0));
  v.push_back(Choice(kGraphics, "vsync", L"VSYNC",
                     L"Frames wait for the screen's refresh: no tearing, a little more delay. Off: as fast as the "
                     L"frame rate limit (a G-SYNC / FreeSync screen avoids tearing anyway).",
                     kOffOn, {"0", "1"}, 0, true));
  v.push_back(Choice(kGraphics, "brightness", L"BRIGHTNESS", L"Lifts or darkens the whole picture.",
                     {L"-30%", L"-20%", L"-10%", L"0", L"+10%", L"+20%", L"+30%"},
                     {"-0.3", "-0.2", "-0.1", "0", "0.1", "0.2", "0.3"}, 3));
  v.push_back(Choice(kGraphics, "gamma", L"GAMMA",
                     L"Brightens or darkens the midtones, keeping black and white (the game's own: 1.0).",
                     {L"0.7", L"0.8", L"0.9", L"1.0", L"1.1", L"1.2", L"1.3"},
                     {"0.7", "0.8", "0.9", "1", "1.1", "1.2", "1.3"}, 3));
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
  v.push_back(Choice(kEffects, "quality", L"QUALITY PRESET",
                     L"Sets every effect below at once. LOW: the game's own look with a few extras; "
                     L"ULTRA: more rays and steps. Changing an effect makes it CUSTOM.",
                     {L"LOW", L"MEDIUM", L"HIGH", L"ULTRA", L"CUSTOM"}, {"low", "medium", "high", "ultra", "custom"},
                     2));
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
  v.push_back(Choice(kEffects, "particle_quality", L"PARTICLE QUALITY",
                     L"The game's smoke, dust and sparks. High: full resolution, finer shading.",
                     {L"GAME", L"HIGH"}, {"0", "1"}, 1));
  v.push_back(Choice(kEffects, "soft_particles", L"SOFT PARTICLES",
                     L"Smoke fades where it meets the ground and walls (next start).", kOffOn,
                     {"0", "1"}, 1, true));
  v.push_back(Choice(kEffects, "explosion_smoke", L"EXPLOSION SMOKE",
                     L"Lingering, lit smoke and bouncing debris at explosions.", kOffOn, {"0", "1"}, 0));
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
  v.push_back(Choice(kControls, "aim_sens", L"AIM SENSITIVITY",
                     L"The mouse sensitivity while aiming down the sights, as a share of the one above.",
                     {L"25%", L"50%", L"60%", L"70%", L"80%", L"90%", L"100%", L"120%", L"150%"},
                     {"0.25", "0.5", "0.6", "0.7", "0.8", "0.9", "1", "1.2", "1.5"}, 6));
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
  // What the effects cost (measured at 2X, 1440p, on an RTX 5060).
  {
    struct Cost { const char* key; const wchar_t* text; };
    const Cost kCosts[] = {
        {"rt", L" The biggest cost: most of the main pass (about 3.4 ms in all)."},
        {"gi", L" About 0.8 ms."}, {"ao", L" About 1 ms."},
        {"density", L" The haze: about 0.6 ms."}, {"shafts", L" Part of the haze (0.6 ms)."},
        {"clouds", L" About 0.2 ms."}, {"water_foam", L" The water: about 0.3 ms in all."},
        {"water_refract", L" The water: about 0.3 ms in all."}, {"water_shallow", L" The water: about 0.3 ms in all."},
    };
    static std::list<std::wstring> texts;  // (desc points into these)
    for (Setting& s : v)
      for (const Cost& c : kCosts)
        if (s.key && !std::strcmp(s.key, c.key)) s.desc = texts.emplace_back(std::wstring(s.desc) + c.text).c_str();
  }
  // Sound, and the window in the background.
  {
    std::vector<std::wstring> names;
    std::vector<std::string> values;
    for (int i = 0; i <= 10; ++i) {
      names.push_back(std::to_wstring(i * 10) + L"%");
      values.push_back(i == 10 ? "1" : "0." + std::to_string(i));
    }
    v.push_back(Choice(kSound, "master_volume", L"MASTER VOLUME", L"Everything the game plays.", names, values, 10));
    v.push_back(Choice(kSound, "music_volume", L"MUSIC VOLUME", L"The game's music.", names, values, 10));
    v.push_back(Choice(kSound, "voice_volume", L"VOICE VOLUME", L"The soldiers' and the narrator's voices.", names,
                       values, 10));
  }
  v.push_back(Choice(kSound, "background_mute", L"MUTE IN BACKGROUND",
                     L"No sound while another window is in front.", kOffOn, {"0", "1"}, 1));
  v.push_back(Choice(kSound, "background_fps", L"BACKGROUND FRAME RATE",
                     L"Most frames a second while another window is in front (saves power and heat).",
                     {L"15", L"30", L"60", L"AS IN FRONT"}, {"15", "30", "60", "0"}, 1));
  v.push_back(Choice(kControls, "prompts", L"BUTTON PROMPTS",
                     L"The buttons the game shows. AUTO: your keys, or the Xbox 360's with a controller plugged in. "
                     L"Shows from the next menu or map.",
                     {L"AUTO", L"KEYBOARD", L"XBOX 360", L"XBOX SERIES", L"PLAYSTATION 3", L"PLAYSTATION 5",
                      L"STEAM DECK"},
                     {"0", "1", "2", "3", "4", "5", "6"}, 0));
  v.push_back(Choice(kControls, "aim_toggle", L"AIM", L"Hold the aim key, or press it to aim and again to stop.",
                     {L"HOLD", L"TOGGLE"}, {"0", "1"}, 0));
  v.push_back(Choice(kControls, "sprint_toggle", L"SPRINT",
                     L"Hold the sprint key, or press it to sprint until you stop or press it again.",
                     {L"HOLD", L"TOGGLE"}, {"0", "1"}, 0));
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
    if (s.category != kEffects || !std::strcmp(s.key, "quality")) continue;
    fx += std::string(fx.empty() ? "" : ",") + s.key + "=" + s.values[s.index];
  }
  // The preset's rays and steps (no rows of their own).
  {
    const std::string& q = Value("quality");
    fx += q == "low"      ? ",gi_rays=1,steps=16,cloud_steps=12,shadow_steps=24"
          : q == "medium" ? ",gi_rays=2,steps=24,cloud_steps=16,shadow_steps=32"
          : q == "ultra"  ? ",gi_rays=4,steps=48,cloud_steps=32,shadow_steps=64"
                          : ",gi_rays=2,steps=32,cloud_steps=24,shadow_steps=48";
  }
  xgpu::ApplySceneFx(fx.c_str());
  // Particles: the game's renderer settings, ours.
  {
    // (Not in the stock look: the game as it was.)
    static std::string last_emitter;
    if (g_running_look != 0) {
      g_emitter = Value("particle_quality") == "0" ? "QuadHalfResEnable=1,QuadSimpleRenderingEnable=1"
                                                   : "QuadHalfResEnable=0,QuadSimpleRenderingEnable=0";
      if (g_emitter != last_emitter) last_emitter = g_emitter, ApplyEmitterNow();
    }
    xgpu::SetExplosionEffects(Value("explosion_smoke") == "1");
    xgpu::SetSoftParticles(Value("soft_particles") != "0");
  }
  const std::string& look = Value("preset");
  if (g_running_look != 0 && look != "stock") {
    xgpu::SetLook(look == "toon" ? 1 : look == "noir" ? 2 : look == "ww2" ? 3 : look == "vivid" ? 4 : 0);
  }
  g_max_fps = uint32_t(std::atoi(Value("fps").c_str()));
  xgpu::SetPicture(float(std::atof(Value("brightness").c_str())), float(std::atof(Value("gamma").c_str())));
  xgpu::SetVsync(Value("vsync") == "1" || g_present_vsync);
  xgpu::SetFullscreen(Value("display") == "fullscreen");
  {
    static const float command_line_fov = g_fov;  // --fov, when the setting is the game's
    float fov = float(std::atof(Value("fov").c_str()));
    g_fov = fov > 0.0f ? fov : command_line_fov;
  }
  xgpu::SetHdrWhite(float(std::atof(Value("hdr_white").c_str())));
  g_mouse_sensitivity = float(std::atof(Value("mouse_sens").c_str()));
  g_mouse_invert = Value("invert_mouse") == "1";
  g_plane_mouse_sensitivity = float(std::atof(Value("plane_sens").c_str()));
  g_aim_mouse_sensitivity = float(std::atof(Value("aim_sens").c_str()));
  g_plane_mouse_invert = Value("plane_invert") == "1";
  g_aim_toggle = Value("aim_toggle") == "1";
  g_camera_shake = float(std::atof(Value("camera_shake").c_str()));
  g_fps_overlay = std::atoi(Value("fps_overlay").c_str());
  g_skip_intro = Value("skip_intro") != "0";
  g_prompt_style = std::atoi(Value("prompts").c_str());
  g_motion_blur = Value("motion_blur") != "0";
  g_sprint_toggle = Value("sprint_toggle") == "1";
  g_background_fps = uint32_t(std::atoi(Value("background_fps").c_str()));
  g_background_mute = Value("background_mute") == "1";
  g_master_volume = float(std::atof(Value("master_volume").c_str()));
  g_music_volume = float(std::atof(Value("music_volume").c_str()));
  g_voice_volume = float(std::atof(Value("voice_volume").c_str()));
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
bool g_suppress_main = false;
bool g_show_failure = false;
bool g_lost_notice = false;   // the main menu tells why the last match ended (the connection closed)  // the main menu came back because the game did not start    // a game is starting: no main menu until the next front end
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
    // A notice (why the last game ended or didn't start) in place of the
    // button's note, until another button is chosen.
    static int notice_button = -1;
    if (g_status.empty()) notice_button = -1;
    else if (notice_button < 0) notice_button = g_button;
    if (!g_status.empty() && g_button != notice_button) g_status.clear(), notice_button = -1;
    if (main && !g_status.empty()) v.desc = g_status;
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

bool Down(int vk) {
  if (vk == VK_RETURN && (GetAsyncKeyState(VK_MENU) & 0x8000)) return false;  // Alt+Enter: fullscreen
  return (GetAsyncKeyState(vk) & 0x8000) != 0 || SimulatedKeyDown(vk);
}

// The QUALITY PRESET's effect values (key, value; the rest keep theirs).
struct PresetValue { const char* key; const char* low; const char* medium; const char* high; const char* ultra; };
const PresetValue kPresets[] = {
    {"rt", "0", "0", "1", "1"},                   {"shadows", "0", "1", "1", "1"},
    {"gi", "0", "0", "1", "1"},                   {"ao", "0", "0.5", "1", "1"},
    {"clouds", "0.3", "0.45", "0.45", "0.45"},    {"cloud_shadows", "0", "0.85", "0.85", "0.85"},
    {"shafts", "0", "0.6", "0.6", "0.6"},         {"reflect", "0", "1", "1", "1"},
    {"gloss", "0", "0", "1", "1"},                {"water_foam", "0", "0", "1", "1"},
    {"water_refract", "0", "1", "1", "1"},        {"water_shallow", "0", "1", "1", "1"},
    {"detail", "0", "1", "1", "1.5"},             {"particle_quality", "0", "1", "1", "1"},
    {"soft_particles", "0", "1", "1", "1"},       {"explosion_smoke", "0", "0", "0", "1"},
};
void ApplyPreset(const std::string& q) {
  for (const PresetValue& p : kPresets) {
    Setting* s = Find(p.key);
    if (!s) continue;
    Select(*s, q == "low" ? p.low : q == "medium" ? p.medium : q == "ultra" ? p.ultra : p.high);
    s->touched = true;
  }
}

void Change(Setting& s, int dir) {
  if (s.kind != Kind::kChoice) return;
  int n = int(s.values.size());
  s.index = (s.index + dir + n) % n;
  s.touched = true;
  // The preset sets the effects; an effect changed by hand makes it CUSTOM.
  if (!std::strcmp(s.key, "quality")) {
    if (s.values[s.index] == "custom") s.index = (s.index + dir + n) % n;  // (not picked, only shown)
    ApplyPreset(s.values[s.index]);
  } else if (s.category == kEffects) {
    if (Setting* q = Find("quality")) Select(*q, "custom");
  }
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

// What was last started, for the message if it never does.
std::wstring g_start_failed;

// A game is starting: the menus give way to the game's loading screen.
void StartingGame() {
  g_suppress_main = true;
  g_level_files = false;
  Close();
}

void Activate(Setting& s) {
  if (s.kind == Kind::kButton) {
    if (s.button == kHostStart) {
      std::wstring problem = lan::HostProblem();
      if (!problem.empty()) {
        g_status = problem;
        g_dirty = true;
        return;
      }
      lan::SetPublish(PageValue("online") == "1");
      lan::Host(PageValue("host_map"), std::atoi(PageValue("bots").c_str()));
      g_start_failed = L"Hosting " + Wide(PageValue("host_map")) +
                       L" did not start: the map did not load. Try again, or another map.";
      StartingGame();
    } else if (s.button == kJoinServer) {
      if (lan::Join(s.address, s.map)) {
        g_start_failed = L"No answer from the game at " + Wide(s.address) +
                         L". Check it is still running, on the same network, and that its firewall lets the game through.";
        StartingGame();
      }
    } else if (s.button == kJoinAddress) {
      if (!PageText("address").empty()) {
        if (lan::Join(PageText("address"), PageValue("join_map"))) {
          g_start_failed = L"No answer from " + Wide(PageText("address")) +
                           L". Check the address, that the game there is hosting, and its firewall.";
          StartingGame();
        } else {
          g_status = L"That is not an address: type one like 192.168.1.20 (or 192.168.1.20:1003).";
          g_dirty = true;
        }
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
               (sv.online ? L" (on the internet)." : L" (" + std::to_wstring(sv.ping_ms) + L" ms).");
    if (sv.online) labels[k] += L"  [INTERNET]";
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
                                 L"Computer players in your game, half on each team.",
                                 {L"NONE", L"2", L"4", L"6", L"8", L"12", L"15"},
                                 {"0", "2", "4", "6", "8", "12", "15"}, 0));
    {
      Setting online = Choice(kGraphics, "online", L"LIST ON THE INTERNET",
                              L"Shows your game in everyone's JOIN GAME list (the master server). Your router must "
                              L"pass UDP ports 1003 and 19430 to this PC. Your IP address is shown in the list.",
                              {L"NO", L"YES"}, {"0", "1"}, lan::Publish() ? 1 : 0);
      g_host_rows.push_back(online);
    }
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
    g_status = L"Looking for games on your network and the internet...";
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
  // Alt+Enter switched fullscreen: the setting follows (and is saved).
  static int toggles = 0;
  if (int t = xgpu::FullscreenToggles(); t != toggles) {
    toggles = t;
    if (Setting* s = Find("display")) {
      Select(*s, xgpu::IsFullscreen() ? "fullscreen" : "windowed");
      s->touched = true;
      Save();
      g_dirty = true;
    }
  }
  const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
  for (int vk = 1; vk < 256; ++vk) {
    bool d = (window && (GetAsyncKeyState(vk) & 0x8000) != 0) || SimulatedKeyDown(vk);
    if (vk == VK_RETURN && alt) d = false;  // Alt+Enter: fullscreen
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
  static bool first = true;
  if (first) first = false, g_suppress_main = !g_host_map.empty();  // --host-map hosts by itself
  if (!g_in_menus) g_suppress_main = false;
  static double title_at = 0;
  if (title_passed && title_at == 0) title_at = now;
  int dialogs = GameDialogs();
  if (dialogs < 0 && title_at > 0 && now - title_at > 6) dialogs = 0;  // never asked: none
  // The connection to the server closed in a match and the game, back in its
  // front end, shows its notice (it says "kicked" whatever happened): OK is
  // pressed for the player, and our main menu says what happened instead.
  bool notice_wait = false;  // a connection just closed: the game's notice may still come
  {
    static bool pending = false;
    static int64_t next_press = 0;
    static int presses = 0;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t at = g_disconnect_ms.load();
    notice_wait = (at && now_ms - at < 8000) || pending;
    if (at && now_ms - at < 20000 && g_in_menus && dialogs > 0 && !pending && g_disconnect_reason.load() >= 0) {
      pending = true;
      g_disconnect_ms = 0;
      next_press = now_ms + 1000, presses = 0;  // once it has come in
    }
    // OK held 150 ms, again every 1.5 s until the dialog is gone (5 tries).
    if (pending && dialogs > 0 && now_ms >= next_press && presses < 5) {
      g_press_a_until = now_ms + 150;
      next_press = now_ms + 1500, ++presses;
      RT_INFO("menus: OK on the game's connection notice (reason %d)", g_disconnect_reason.load());
    }
    if (pending && (dialogs == 0 || presses >= 5) && now_ms >= next_press - 1300) {
      pending = false;
      g_lost_notice = true;
    }
  }
  // A game that never started (the join failed, the host could not be
  // reached): still in the front end with no dialog up for a while and no
  // level file opened, the main menu comes back instead of leaving the
  // game's own on screen. (Once a level's files load, a slow load is left
  // alone.)
  static double idle_since = -1;
  if (CustomMenus() && g_suppress_main && g_in_menus && !g_level_files && title_passed && dialogs == 0 && !g_open) {
    if (idle_since < 0) idle_since = now;
    if (now - idle_since > (g_host_map.empty() ? 15.0 : 25.0)) {
      RT_INFO("menus: the game did not start, back to the main menu");
      g_suppress_main = false;
      g_show_failure = true;
    }
  } else {
    idle_since = -1;
  }
  if (CustomMenus() && !g_open && g_in_menus && title_passed && dialogs == 0 && !g_suppress_main && !notice_wait) {
    OpenMode(kModeMain, kModeSettings);
    g_wait_release = false;
    RT_INFO("menus: our main menu shown");
    // Back from a game whose connection was lost: why.
    if (g_lost_notice) {
      int reason = g_disconnect_reason.exchange(-1);
      g_status = reason == 4 ? L"You were kicked from the server."
                             : L"The connection to the server was lost: the host left, or the network dropped.";
      g_show_failure = false;
      g_lost_notice = false;
    }
    // Back from a game that never started: why.
    if (g_show_failure && !g_start_failed.empty()) g_status = g_start_failed;
    g_show_failure = false;
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
    // F2: the FPS counter off, FPS, FULL.
    if (pressed[VK_F2]) {
      if (Setting* s = Find("fps_overlay")) {
        s->index = (s->index + 1) % int(s->values.size());
        s->touched = true;
        ApplyLive();
        Save();
        g_dirty = true;
      }
    }
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
