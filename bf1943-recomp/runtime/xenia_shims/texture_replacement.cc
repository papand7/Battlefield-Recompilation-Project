// bf1943: replacement texture index and .rtex loading (see texture_replacement.h).
//
// replacements.txt: one line per texture, "<hash hex> <width> <height> <file>",
// where width/height are the guest size and file is relative to the folder.
// .rtex: "RTX1", u32 width, height, levels (little endian), then each level's
// RGBA8 pixels, largest first.
#include "texture_replacement.h"

#include "xenia/base/logging.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace xe::gpu::texrep {
namespace {

struct Key {
  uint64_t hash;
  uint32_t width, height;
  bool operator==(const Key& o) const {
    return hash == o.hash && width == o.width && height == o.height;
  }
};
struct KeyHasher {
  size_t operator()(const Key& k) const {
    return size_t(k.hash ^ (uint64_t(k.width) << 40) ^ (uint64_t(k.height) << 20));
  }
};

std::unordered_map<Key, Replacement, KeyHasher> g_entries;
bool g_log_misses = false;
std::mutex g_log_mutex;
std::set<Key, bool (*)(const Key&, const Key&)> g_logged(
    [](const Key& a, const Key& b) {
      return std::tie(a.hash, a.width, a.height) < std::tie(b.hash, b.width, b.height);
    });

// Loaded pixels by entry, filled by the prefetch thread and by Load.
std::mutex g_cache_mutex;
std::unordered_map<const Replacement*, std::shared_ptr<const Pixels>> g_cache;
size_t g_cache_bytes = 0;
constexpr size_t kCacheBudget = size_t(6) << 30;  // more than a whole pack

std::shared_ptr<const Pixels> ReadFile(const Replacement& r) {
  std::ifstream in(r.file, std::ios::binary);
  if (!in) return nullptr;
  in.seekg(16);
  auto p = std::make_shared<Pixels>();
  size_t total = 0;
  for (uint32_t l = 0; l < r.levels; ++l) {
    p->level_offsets.push_back(total);
    total += size_t(std::max(r.width >> l, 1u)) * std::max(r.height >> l, 1u) * 4;
  }
  p->data.resize(total);
  in.read(reinterpret_cast<char*>(p->data.data()), std::streamsize(total));
  if (size_t(in.gcount()) != total) return nullptr;
  return p;
}

void Keep(const Replacement* r, std::shared_ptr<const Pixels> p) {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  if (g_cache.count(r) || g_cache_bytes + p->data.size() > kCacheBudget) return;
  g_cache_bytes += p->data.size();
  g_cache.emplace(r, std::move(p));
}

void PrefetchThread(std::vector<const Replacement*> entries) {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"texture prefetch");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  pthread_setname_np(pthread_self(), "tex prefetch");
#endif
  size_t n = 0;
  for (const Replacement* r : entries) {
    {
      std::lock_guard<std::mutex> lock(g_cache_mutex);
      if (g_cache.count(r)) continue;
      if (g_cache_bytes >= kCacheBudget) break;
    }
    if (auto p = ReadFile(*r)) Keep(r, std::move(p)), ++n;
  }
  XELOGI("textures: {} replacements preloaded ({} MB)", n, g_cache_bytes >> 20);
}

}  // namespace

uint64_t Hash(const uint8_t* data, size_t size) {
  uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a
  for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
  return h;
}

size_t SetDirectory(const std::filesystem::path& dir) {
  g_entries.clear();
  std::ifstream in(dir / "replacements.txt");
  if (!in) {
    XELOGE("textures: no replacements.txt in {}", dir.string());
    return 0;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string hash, file;
    Key k{};
    if (!(s >> hash >> k.width >> k.height) || !std::getline(s >> std::ws, file)) continue;
    k.hash = std::stoull(hash, nullptr, 16);
    Replacement r{};
    r.file = dir / file;
#ifdef _WIN32
    FILE* f = _wfopen(r.file.c_str(), L"rb");
#else
    FILE* f = fopen(r.file.c_str(), "rb");
#endif
    uint32_t hdr[4] = {};
    if (!f) continue;
    bool ok = fread(hdr, 4, 4, f) == 4 && !memcmp(hdr, "RTX1", 4);
    fclose(f);
    if (!ok || !hdr[1] || !hdr[2] || !hdr[3]) continue;
    r.width = hdr[1];
    r.height = hdr[2];
    r.levels = hdr[3];
    r.pbr = file.size() > 9 && file.compare(file.size() - 9, 9, ".pbr.rtex") == 0;
    g_entries[k] = std::move(r);
  }
  XELOGI("textures: {} replacements from {}", g_entries.size(), dir.string());
  std::vector<const Replacement*> entries;
  for (auto& [k, r] : g_entries) entries.push_back(&r);
  if (!entries.empty()) std::thread(PrefetchThread, std::move(entries)).detach();
  return g_entries.size();
}

bool Enabled() { return !g_entries.empty() || g_log_misses; }
void SetLogMisses(bool log) { g_log_misses = log; }

const Replacement* Find(const uint8_t* guest, uint32_t width, uint32_t height,
                        uint32_t guest_format) {
  Key k{Hash(guest, kHashBytes), width, height};
  auto it = g_entries.find(k);
  if (g_log_misses) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_logged.insert(k).second) {
      XELOGI("textures: {:016X} {}x{} fmt {} {}", k.hash, width, height, guest_format,
             it != g_entries.end() ? it->second.file.filename().string() : "(none)");
      // Misses: save the tiled base level (textures_missing/<hash>_<w>x<h>_<fmt>.bin)
      // for tools/texpack/missing.py to decode and add to the pack.
      if (it == g_entries.end()) {
        // DXT1 / DXT5A: 8 bytes per 4x4 block, DXT2-5 / DXN: 16; tiled storage
        // is padded to 32x32 blocks.
        uint32_t block_bytes = (guest_format == 18 || guest_format == 59) ? 8 : 16;
        uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        size_t bytes = size_t((bw + 31) & ~31u) * ((bh + 31) & ~31u) * block_bytes;
        std::filesystem::create_directories("textures_missing");
        char name[96];
        snprintf(name, sizeof(name), "textures_missing/%016llX_%ux%u_%u.bin",
                 (unsigned long long)k.hash, width, height, guest_format);
        std::ofstream(name, std::ios::binary).write(reinterpret_cast<const char*>(guest),
                                                    std::streamsize(bytes));
      }
    }
  }
  return it != g_entries.end() ? &it->second : nullptr;
}

std::shared_ptr<const Pixels> Load(const Replacement& r) {
  {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto it = g_cache.find(&r);
    if (it != g_cache.end()) return it->second;
  }
  auto p = ReadFile(r);
  if (p) Keep(&r, p);
  return p;
}

static std::string g_trace;
void SetTrace(const std::string& name_part) { g_trace = name_part; }
const std::string& Trace() { return g_trace; }

}  // namespace xe::gpu::texrep
