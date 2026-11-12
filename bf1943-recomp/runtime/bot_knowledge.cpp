// What the bots know of a map from the matches played on it: see
// bot_knowledge.h. A file per map:
//   # bot knowledge 1 <map>
//   c <ix> <iz> <visits 1> <visits 2> <deaths 1> <deaths 2> <kills 1> <kills 2>
// one line per 8 m cell (ix = floor(x / 8)) with anything in it; 1 and 2 the
// teams (US, Japan). Two layers: the shipped one (read only) and what this
// server learned (kept adding up over its matches, saved now and then).
#include "bot_knowledge.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rt::knowledge {
namespace {

constexpr float kCell = 8.0f;
using Cell = std::array<float, 6>;  // visits, deaths, kills; each for team 1, 2

struct Layer {
  std::unordered_map<uint64_t, Cell> cells;
};

std::mutex g_mutex;
std::string g_map;
Layer g_shipped, g_learned;
bool g_dirty = false;

uint64_t Key(int ix, int iz) { return uint64_t(uint32_t(ix)) << 32 | uint32_t(iz); }

std::filesystem::path ExeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  return n > 0 ? std::filesystem::path(std::string(buf, size_t(n))).parent_path() : std::filesystem::path(".");
#endif
}

std::filesystem::path LearnedPath(const std::string& map) { return g_user_root / "bot_knowledge" / (map + ".txt"); }

size_t Load(Layer& l, const std::filesystem::path& p) {
  l.cells.clear();
  FILE* f = fopen(p.string().c_str(), "r");
  if (!f) return 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    int ix, iz;
    Cell c{};
    if (sscanf(line, "c %d %d %f %f %f %f %f %f", &ix, &iz, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 8)
      l.cells[Key(ix, iz)] = c;
  }
  fclose(f);
  return l.cells.size();
}

void SaveLocked() {
  if (g_map.empty() || !g_dirty) return;
  std::filesystem::path p = LearnedPath(g_map);
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::filesystem::path tmp = p;
  tmp += ".tmp";
  FILE* f = fopen(tmp.string().c_str(), "w");
  if (!f) return;
  fprintf(f, "# bot knowledge 1 %s\n", g_map.c_str());
  for (auto& [k, c] : g_learned.cells)
    fprintf(f, "c %d %d %.2f %.2f %.2f %.2f %.2f %.2f\n", int32_t(k >> 32), int32_t(uint32_t(k)), c[0], c[1], c[2],
            c[3], c[4], c[5]);
  fclose(f);
  std::filesystem::rename(tmp, p, ec);
  g_dirty = false;
}

}  // namespace

void UseMap(const std::string& map) {
  std::lock_guard l(g_mutex);
  if (map == g_map) return;
  SaveLocked();
  g_map = map;
  size_t shipped = Load(g_shipped, ExeDir() / "bot_knowledge" / (map + ".txt"));
  size_t learned = Load(g_learned, LearnedPath(map));
  RT_INFO("bot knowledge: %s: %zu cells shipped, %zu learned here", map.c_str(), shipped, learned);
}

void Add(What what, int team, float x, float z, float weight) {
  if ((team != 1 && team != 2) || !std::isfinite(x) || !std::isfinite(z)) return;
  std::lock_guard l(g_mutex);
  if (g_map.empty()) return;
  int ix = int(std::floor(x / kCell)), iz = int(std::floor(z / kCell));
  g_learned.cells[Key(ix, iz)][size_t(what) * 2 + size_t(team - 1)] += weight;
  g_dirty = true;
}

float Get(What what, int team, float x, float z, float radius) {
  if (team != 1 && team != 2) return 0;
  std::lock_guard l(g_mutex);
  int r = int(std::ceil(radius / kCell));
  int cx = int(std::floor(x / kCell)), cz = int(std::floor(z / kCell));
  size_t i = size_t(what) * 2 + size_t(team - 1);
  float sum = 0;
  for (int dz = -r; dz <= r; ++dz)
    for (int dx = -r; dx <= r; ++dx) {
      float ox = (float(cx + dx) + 0.5f) * kCell - x, oz = (float(cz + dz) + 0.5f) * kCell - z;
      if (ox * ox + oz * oz > (radius + kCell * 0.5f) * (radius + kCell * 0.5f)) continue;
      uint64_t k = Key(cx + dx, cz + dz);
      if (auto it = g_shipped.cells.find(k); it != g_shipped.cells.end()) sum += it->second[i];
      if (auto it = g_learned.cells.find(k); it != g_learned.cells.end()) sum += it->second[i];
    }
  return sum;
}

void Save() {
  std::lock_guard l(g_mutex);
  SaveLocked();
}

}  // namespace rt::knowledge
