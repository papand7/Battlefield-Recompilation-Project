// Self-play tuning of the bots' behaviour: see bot_tuning.h. A file per map:
//   # bot tuning 1 <map>: generation <n>, <tried> candidates tried, <taken> taken
//   <setting> <value>     (bots/bot_api.h's kTuneInfo names; missing: the default)
#include "bot_tuning.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>

#include "bots/bot_api.h"
#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rt::tuning {
namespace {

using botapi::kTuneCount;
using botapi::kTuneInfo;
using Settings = std::array<float, kTuneCount>;

constexpr double kRound = 600;     // a round of play: 10 minutes of the game's time
constexpr int kMinEvents = 5;      // kills, deaths and flags taken per half before judging
constexpr double kMargin = 0.03;   // per bot and minute: the candidate must beat the champion by this

struct Score { int kills = 0, deaths = 0, captures = 0; };

std::mutex g_mutex;
std::string g_map;
bool g_training = false;
Settings g_champion, g_candidate;
int g_generation = 0, g_tried = 0, g_taken = 0;
std::array<Score, botapi::kMaxBots> g_score;
double g_round_start = -1;
std::mt19937 g_rng{std::random_device{}()};

// Half of each team plays the candidate (teams alternate by slot: slots 2-3,
// 6-7, ... are one bot of each).
bool OnCandidate(int slot) { return g_training && (slot / 2) % 2 == 1; }

Settings Defaults() {
  Settings s;
  for (int i = 0; i < kTuneCount; ++i) s[i] = kTuneInfo[i].def;
  return s;
}

FILE* Open(const std::filesystem::path& p, const wchar_t* wmode, const char* mode) {
#ifdef _WIN32
  (void)mode;
  return _wfopen(p.c_str(), wmode);
#else
  (void)wmode;
  return fopen(p.c_str(), mode);
#endif
}

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

std::filesystem::path LearnedPath(const std::string& map) { return g_user_root / "bot_knowledge" / (map + ".tune"); }

bool Load(const std::filesystem::path& p, Settings& s, int& generation) {
  FILE* f = Open(p, L"r", "r");
  if (!f) return false;
  s = Defaults();
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char name[64];
    float v;
    if (const char* g = strstr(line, "generation ")) generation = atoi(g + 11);
    if (line[0] == '#' || sscanf(line, "%63s %f", name, &v) != 2) continue;
    for (int i = 0; i < kTuneCount; ++i)
      if (!strcmp(name, kTuneInfo[i].name)) s[i] = std::clamp(v, kTuneInfo[i].lo, kTuneInfo[i].hi);
  }
  fclose(f);
  return true;
}

void SaveLocked() {
  std::filesystem::path p = LearnedPath(g_map);
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::filesystem::path tmp = p;
#ifdef _WIN32
  tmp += ".tmp" + std::to_string(GetCurrentProcessId());
#else
  tmp += ".tmp" + std::to_string(getpid());
#endif
  FILE* f = Open(tmp, L"w", "w");
  if (!f) return;
  fprintf(f, "# bot tuning 1 %s: generation %d, %d candidates tried, %d taken\n", g_map.c_str(), g_generation,
          g_tried, g_taken);
  for (int i = 0; i < kTuneCount; ++i) fprintf(f, "%s %.4g\n", kTuneInfo[i].name, g_champion[i]);
  bool ok = !ferror(f);
  ok = fclose(f) == 0 && ok;
  if (ok) std::filesystem::rename(tmp, p, ec);
  if (!ok || ec) std::filesystem::remove(tmp, ec);
}

// The champion with two or three settings moved (each by about an eighth of
// its range, either way).
Settings Mutate(const Settings& from) {
  Settings s = from;
  int n = 2 + int(g_rng() % 2);
  for (int k = 0; k < n; ++k) {
    int i = int(g_rng() % kTuneCount);
    const auto& t = kTuneInfo[i];
    std::normal_distribution<float> step(0.0f, 0.125f * (t.hi - t.lo));
    s[i] = std::clamp(s[i] + step(g_rng), t.lo, t.hi);
  }
  return s;
}

std::string Describe(const Settings& s) {
  std::string out;
  for (int i = 0; i < kTuneCount; ++i)
    if (s[i] != g_champion[i]) {
      char buf[96];
      snprintf(buf, sizeof(buf), " %s %.3g->%.3g", kTuneInfo[i].name, g_champion[i], s[i]);
      out += buf;
    }
  return out.empty() ? " (none)" : out;
}

void NewRound(double now) {
  g_score = {};
  g_round_start = now;
  if (g_training) {
    g_candidate = Mutate(g_champion);
    RT_INFO("bot tuning: %s, generation %d: trying%s", g_map.c_str(), g_generation, Describe(g_candidate).c_str());
  }
}

}  // namespace

void UseMap(const std::string& map, bool training) {
  std::lock_guard l(g_mutex);
  if (map == g_map) return;
  g_map = map, g_training = training;
  g_champion = Defaults(), g_generation = g_tried = g_taken = 0;
  const char* from = "the defaults";
  if (Load(LearnedPath(map), g_champion, g_generation)) from = "learned here";
  else if (Load(ExeDir() / "bot_knowledge" / (map + ".tune"), g_champion, g_generation)) from = "shipped";
  RT_INFO("bot tuning: %s: generation %d (%s)%s", map.c_str(), g_generation, from,
          training ? "; training: half of each team tries a candidate" : "");
  g_candidate = g_champion;
  g_round_start = -1;
}

const float* For(int slot) {
  std::lock_guard l(g_mutex);
  return OnCandidate(slot) ? g_candidate.data() : g_champion.data();
}

void Kill(int slot) {
  std::lock_guard l(g_mutex);
  if (slot >= 0 && slot < botapi::kMaxBots) ++g_score[slot].kills;
}
void Death(int slot) {
  std::lock_guard l(g_mutex);
  if (slot >= 0 && slot < botapi::kMaxBots) ++g_score[slot].deaths;
}
void Capture(int slot) {
  std::lock_guard l(g_mutex);
  if (slot >= 0 && slot < botapi::kMaxBots) ++g_score[slot].captures;
}

void Update(double now, int bot_count) {
  std::lock_guard l(g_mutex);
  if (!g_training || g_map.empty()) return;
  if (g_round_start < 0 || now < g_round_start) return NewRound(now);
  double minutes = (now - g_round_start) / 60.0;
  if (minutes * 60.0 < kRound) return;
  // Each half: its points per bot and minute, and how much happened.
  double points[2] = {0, 0};
  int bots[2] = {0, 0}, events[2] = {0, 0};
  for (int i = 0; i < std::min(bot_count, botapi::kMaxBots); ++i) {
    const Score& s = g_score[i];
    int half = OnCandidate(i) ? 1 : 0;
    points[half] += s.kills - s.deaths + 2.0 * s.captures;
    events[half] += s.kills + s.deaths + s.captures;
    ++bots[half];
  }
  if (!bots[0] || !bots[1]) return NewRound(now);
  if (events[0] < kMinEvents || events[1] < kMinEvents) {
    if (minutes * 60.0 < 3 * kRound) return;  // a quiet round: longer
    RT_INFO("bot tuning: %s: too little happened in %.0f minutes, candidate dropped", g_map.c_str(), minutes);
    return NewRound(now);
  }
  double champion = points[0] / bots[0] / minutes, candidate = points[1] / bots[1] / minutes;
  ++g_tried;
  bool taken = candidate > champion + kMargin;
  RT_INFO("bot tuning: %s: candidate %.3f vs champion %.3f a bot a minute (%d / %d things happened): %s",
          g_map.c_str(), candidate, champion, events[1], events[0], taken ? "taken" : "dropped");
  if (taken) g_champion = g_candidate, ++g_generation, ++g_taken;
  SaveLocked();
  NewRound(now);
}

}  // namespace rt::tuning
