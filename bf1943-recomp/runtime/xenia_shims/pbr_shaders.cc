// bf1943: PBR shader table (see pbr_shaders.h).
#include "pbr_shaders.h"

#include "xenia/base/logging.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace xe::gpu::pbr {
namespace {

std::unordered_map<uint64_t, ShaderConfig> g_shaders;

// "r3.yzw@47"
bool ParseTap(const std::string& s, Tap& tap) {
  size_t dot = s.find('.'), at = s.find('@');
  if (s.size() < 5 || s[0] != 'r' || dot == std::string::npos ||
      at == std::string::npos || at < dot + 2 || at - dot - 1 > 4) {
    return false;
  }
  tap.reg = uint32_t(std::atoi(s.c_str() + 1));
  tap.count = 0;
  for (size_t i = dot + 1; i < at; ++i) {
    const char* xyzw = "xyzw";
    const char* c = std::strchr(xyzw, s[i]);
    if (!c || !*c) return false;
    tap.comps[tap.count++] = uint32_t(c - xyzw);
  }
  tap.address = s.compare(at + 1, std::string::npos, "s") == 0
                    ? kTapAtStart
                    : uint32_t(std::atoi(s.c_str() + at + 1));
  return true;
}

// "8" or "c250.x"
bool ParseScalar(const std::string& s, Scalar& v) {
  if (s.empty()) return false;
  if (s[0] != 'c') {
    v.value = float(std::atof(s.c_str()));
    return true;
  }
  const char* xyzw = "xyzw";
  size_t dot = s.find('.');
  const char* c = dot != std::string::npos && dot + 1 < s.size()
                      ? std::strchr(xyzw, s[dot + 1]) : nullptr;
  if (!c || !*c) return false;
  v.constant = uint32_t(std::atoi(s.c_str() + 1));
  v.comp = uint32_t(c - xyzw);
  return true;
}

uint32_t ParseConstant(const std::string& s) {
  return uint32_t(std::atoi(s.c_str() + (s[0] == 'c' ? 1 : 0)));
}

}  // namespace

size_t Load(const std::filesystem::path& file) {
  g_shaders.clear();
  std::ifstream in(file);
  if (!in) {
    XELOGE("pbr: cannot read {}", file.string());
    return 0;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string hash_text, item;
    if (!(ls >> hash_text) || hash_text[0] == '#') continue;
    uint64_t hash = std::strtoull(hash_text.c_str(), nullptr, 16);
    ShaderConfig c;
    if (const char* pom = std::getenv("BF_POM")) c.pom = float(std::atof(pom));
    bool ok = true;
    while (ls >> item) {
      size_t eq = item.find('=');
      if (eq == std::string::npos) { ok = false; break; }
      std::string key = item.substr(0, eq), value = item.substr(eq + 1);
      Tap* tap = key == "n" ? &c.n : key == "v" ? &c.v : key == "a" ? &c.albedo
               : key == "lit" ? &c.lit : key == "env" ? &c.env
               : key == "so" ? &c.so : key == "ao" ? &c.ao : key == "out" ? &c.out
               : key == "wp" ? &c.wp : nullptr;
      if (tap) ok &= ParseTap(value, *tap);
      else if (key == "rm") c.rm_fetch = uint32_t(std::atoi(value.c_str()));
      else if (key == "env_scale") ok &= ParseScalar(value, c.env_scale);
      else if (key == "pow") ok &= ParseScalar(value, c.pow) && c.pow.is_constant();
      else if (key == "blur") c.blur = float(std::atof(value.c_str()));
      else if (key == "rough_scale") c.rough_scale = float(std::atof(value.c_str()));
      else if (key == "bump") c.bump = float(std::atof(value.c_str()));
      else if (key == "spec_max") c.spec_max = float(std::atof(value.c_str()));
      else if (key == "show") c.show_fetch = uint32_t(std::atoi(value.c_str()));
      else if (key == "show_size") {
        c.show_width = float(std::atof(value.c_str()));
        size_t x = value.find('x');
        if (x != std::string::npos) c.show_height = float(std::atof(value.c_str() + x + 1));
        size_t plus = value.find('+');
        if (plus != std::string::npos) c.show_y0 = float(std::atof(value.c_str() + plus + 1));
        size_t at = value.find('@');
        if (at != std::string::npos) c.show_x0 = float(std::atof(value.c_str() + at + 1));
      }
      else if (key == "rough") c.rough = float(std::atof(value.c_str()));
      else if (key == "metal") c.metal = float(std::atof(value.c_str()));
      else if (key == "l") c.c_light = ParseConstant(value);
      else if (key == "up") c.c_up = ParseConstant(value);
      else if (key == "sky") c.c_sky = ParseConstant(value);
      else if (key == "ground") c.c_ground = ParseConstant(value);
      else if (key == "sun") c.c_sun = ParseConstant(value);
      else if (key == "cam") c.c_cam = ParseConstant(value);
      else if (key == "pom") c.pom = float(std::atof(value.c_str()));
      else ok = false;
    }
    if (!ok || !c.n.valid() || !c.v.valid() || !c.albedo.valid() ||
        !c.out.valid() || c.n.count < 3 || c.v.count < 3 || c.albedo.count < 3) {
      XELOGE("pbr: bad line: {}", line);
      continue;
    }
    g_shaders[hash] = c;
  }
  XELOGI("pbr: {} shaders from {}", g_shaders.size(), file.string());
  return g_shaders.size();
}

bool Enabled() { return !g_shaders.empty(); }

namespace {
bool g_material_output = false;
bool g_game_shadows_off = false;
}  // namespace
void SetGameShadowsOff(bool off) { g_game_shadows_off = off; }
bool GameShadowsOff() { return g_game_shadows_off; }
void SetMaterialOutput(bool on) { g_material_output = on; }
namespace {
float g_pom_scale = 1.0f;
}  // namespace
void SetPomScale(float scale) { g_pom_scale = scale; }
float PomScale() { return g_pom_scale; }
bool MaterialOutput() { return g_material_output; }

const ShaderConfig* Find(uint64_t pixel_shader_hash) {
  auto it = g_shaders.find(pixel_shader_hash);
  return it == g_shaders.end() ? nullptr : &it->second;
}

}  // namespace xe::gpu::pbr
