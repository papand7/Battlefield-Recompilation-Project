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

namespace {
uint64_t g_table_hash = 0;
}  // namespace

size_t Load(const std::filesystem::path& file) {
  g_shaders.clear();
  std::ifstream in(file);
  if (!in) {
    XELOGE("pbr: cannot read {}", file.string());
    return 0;
  }
  {
    std::ifstream raw(file, std::ios::binary);
    uint64_t h = 1469598103934665603ull;
    for (char c; raw.get(c);) h = (h ^ uint8_t(c)) * 1099511628211ull;
    g_table_hash = h;
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
               : key == "wp" ? &c.wp : key == "tonemap" ? &c.tonemap : nullptr;
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
      else if (key == "tm_exposure") c.tm_exposure = float(std::atof(value.c_str()));
      else if (key == "soft") c.soft = float(std::atof(value.c_str()));
      else if (key == "camv") c.cam_from_buffer = value == "1";
      else if (key == "pl") c.point_lights = value == "1";
      else if (key == "tm_saturation") c.tm_saturation = float(std::atof(value.c_str()));
      else ok = false;
    }
    if (ok && !c.out.valid() && (c.soft > 0.0f || (c.tonemap.valid() && c.tonemap.count >= 3))) {
      g_shaders[hash] = c;
      continue;
    }
    bool v_ok = (c.v.valid() && c.v.count >= 3) || c.cam_from_buffer;
    if (!ok || !c.n.valid() || !v_ok || !c.albedo.valid() ||
        !c.out.valid() || c.n.count < 3 || c.albedo.count < 3) {
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
float g_tonemap = std::getenv("BF_TONEMAP") ? float(std::atof(std::getenv("BF_TONEMAP"))) : 1.0f;
void SetTonemap(float strength) { g_tonemap = strength; }
bool g_soft_particles = true;
void SetSoftParticles(bool on) { g_soft_particles = on; }
bool SoftParticles() { return g_soft_particles; }
bool g_fsr_capture = false;
void SetFsrCapture(bool on) { g_fsr_capture = on; }
bool FsrCapture() { return g_fsr_capture; }
float Tonemap() { return g_tonemap; }
bool Filmic() {
  if (g_tonemap < 1.0f) return false;
  for (const auto& [hash, c] : g_shaders)
    if (c.tonemap.valid()) return true;
  return false;
}
void SetGameShadowsOff(bool off) { g_game_shadows_off = off; }
bool GameShadowsOff() { return g_game_shadows_off; }
void SetMaterialOutput(bool on) { g_material_output = on; }
namespace {
float g_pom_scale = 1.0f;
}  // namespace
void SetPomScale(float scale) { g_pom_scale = scale; }
float PomScale() { return g_pom_scale; }
bool MaterialOutput() { return g_material_output; }

uint64_t TranslationKey() {
  uint64_t h = g_table_hash ^ 0x9E3779B97F4A7C15ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  uint32_t u;
  std::memcpy(&u, &g_tonemap, 4), mix(u);
  std::memcpy(&u, &g_pom_scale, 4), mix(u);
  mix(uint64_t(g_soft_particles) | uint64_t(g_fsr_capture) << 1 | uint64_t(g_game_shadows_off) << 2 |
      uint64_t(g_material_output) << 3);
  return h;
}

const ShaderConfig* Find(uint64_t pixel_shader_hash) {
  auto it = g_shaders.find(pixel_shader_hash);
  return it == g_shaders.end() ? nullptr : &it->second;
}

}  // namespace xe::gpu::pbr
