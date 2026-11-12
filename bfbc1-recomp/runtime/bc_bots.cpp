// Bot groundwork for Bad Company: the server's soldiers, vehicles and Gold Rush
// objectives, found by their vtables like bf1943's bots.cpp does.
//
// The vtables come from the type names in the image: each Server*Entity name
// has a record that a static initializer registers into a type object (for
// ServerSoldierEntity: record 82EA2DC0, type 82EA3460, at 82B10958); a getType
// function returns that type object (lis/addi r3, blr), and the class's
// vtable has it in slot 2, as in bf1943 (whose soldier vtable 82013A40 has
// its getType 822ED958 at +8).
//
// Confirmed on Valley Run (BF_AUTO_MP walk/turn/look steps, two players):
//   soldier: team +48 (1 attackers, 2 defenders), position +200/+204/+208
//     (copies at +330 and +370), world matrix rows at +300 (right), +310 (up),
//     +320 (forward: walking moves along (+320, +328) in x/z); +148 counts up.
//     Pitch isn't in it or in the objects it points at (looking up changed
//     nothing there; *(+A8) is scratch that changes every time).
//   vehicle: kind name *(*(+14) + C) as in bf1943 (Humvee, M3A3, Pbl, KORD,
//     Howitzer).
//   capture point: owner +80, position *(+14) + 60 as in bf1943; in Gold Rush
//     the teams' bases (the attackers' owned by 1).
//   mission objective: the gold crates. On Valley Run 8 objects of 0xD0 bytes
//     side by side, position +C0 (= *(+14) + 60, as for capture points), in
//     4 pairs about 5 m apart, one pair per base (-297,123,-177 / -302 by the
//     attackers' first target; then -286/-281,147,-4; -117/-122,139,19;
//     28/33,145,9). Their state (active, armed, destroyed) isn't read yet.
//
// BF_BOT_SCAN=1 (testing): every 10 s, logs how many live objects of each kind
// there are, and for the soldiers the words that look like a team (1 or 2) and
// float triplets that look like a world position, to find the fields to read.
#include "kernel.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "DetourCommon.h"
#include "../../bf1943-recomp/tools/msvc_compat/crash_diag.h"  // PrintStack

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <queue>
#include <filesystem>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {
uint32_t g_bot_soldier_for_trails = 0;  // the bot's soldier: its moves aren't learnt from
namespace {

struct Kind {
  const char* name;
  uint32_t vtable;
};
const Kind kKinds[] = {
    {"soldier", 0x820118D0},             // ServerSoldierEntity (getType 82329690)
    {"vehicle", 0x82011AA8},             // ServerVehicleEntity (822A8810)
    {"tactical objective", 0x82014910},  // ServerTacticalObjectiveEntity (822D8048)
    {"mission objective", 0x82013AF0},   // ServerMissionObjectiveEntity (822D79B0)
    {"capture point", 0x820168C0},       // ServerCapturePointEntity (822F4080)
    {"human player", 0x820149E8},        // ServerHumanPlayerEntity (822D76D0)
    {"team", 0x820133C8},                // ServerTeamEntity (822D8058)
    {"squad", 0x82047AA8},               // Squad, campaign AI (8263CA18)
};

// Every live object with this vtable in the heaps.
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  uint32_t want = _byteswap_ulong(vtable);
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i)
      if (p[i] == want) out.push_back(uint32_t(page) + i * 4);
  }
  return out;
}

float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// What in an object (its first 0x600 bytes) could be a team or a position.
void DescribeFields(const char* kind, uint32_t o) {
  std::string teams, positions;
  for (uint32_t off = 4; off < 0x600 && IsCommitted(o + off + 8); off += 4) {
    uint32_t w = Ld32(o + off);
    if (w == 1 || w == 2) teams += " +" + std::to_string(off) + "=" + std::to_string(w);
    float x = LdF(o + off), y = LdF(o + off + 4), z = LdF(o + off + 8);
    auto world = [](float v) { return std::isfinite(v) && std::fabs(v) > 1.0f && std::fabs(v) < 4000.0f; };
    if (world(x) && std::isfinite(y) && std::fabs(y) < 1000.0f && world(z)) {
      char buf[64];
      snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off, x, y, z);
      positions += buf;
    }
  }
  RT_INFO("bot scan: %s %08X teams:%s", kind, o, teams.substr(0, 300).c_str());
  RT_INFO("bot scan: %s %08X positions:%s", kind, o, positions.substr(0, 600).c_str());
}

// The soldier's float fields that changed since the last scan (its first
// 0x800 bytes): moving and turning it shows which are its position and aim.
void LogChanges(uint32_t o) {
  static std::map<uint32_t, std::vector<uint32_t>> seen;  // per soldier
  std::vector<uint32_t> now(0x200);
  for (uint32_t i = 0; i < 0x200; ++i) now[i] = IsCommitted(o + i * 4) ? Ld32(o + i * 4) : 0;
  auto it = seen.find(o);
  if (it != seen.end()) {
    const std::vector<uint32_t>& last = it->second;
    std::string changes;
    for (uint32_t i = 1; i < 0x200 && changes.size() < 900; ++i) {
      if (now[i] == last[i]) continue;
      float a, b;
      memcpy(&a, &last[i], 4), memcpy(&b, &now[i], 4);
      if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(a) > 1e5f || std::fabs(b) > 1e5f ||
          std::fabs(a - b) < 0.01f)
        continue;
      char buf[64];
      snprintf(buf, sizeof(buf), " +%X:%.2f>%.2f", i * 4, a, b);
      changes += buf;
    }
    RT_INFO("bot scan: soldier %08X changed:%s", o, changes.c_str());
  }
  seen[o] = now;
  // The objects it points at (aim, components): their float fields that changed.
  static std::map<uint64_t, std::vector<uint32_t>> parts;  // (soldier offset, object) -> words
  std::string part_changes;
  for (uint32_t i = 1; i < 0x200; ++i) {
    uint32_t q = now[i];
    if (q < 0x40000000 || q >= 0xC0000000 || (q & 3) || !IsCommitted(q) || !IsCommitted(q + 0x7FC)) continue;
    std::vector<uint32_t> words(0x200);
    for (uint32_t j = 0; j < 0x200; ++j) words[j] = Ld32(q + j * 4);
    uint64_t key = uint64_t(i) << 32 | q;
    auto pit = parts.find(key);
    if (pit != parts.end())
      for (uint32_t j = 1; j < 0x200 && part_changes.size() < 1500; ++j) {
        float a, b;
        memcpy(&a, &pit->second[j], 4), memcpy(&b, &words[j], 4);
        if (!std::isfinite(a) || !std::isfinite(b) || std::fabs(a) > 1e5f || std::fabs(b) > 1e5f ||
            std::fabs(a - b) < 0.01f)
          continue;
        char buf[80];
        snprintf(buf, sizeof(buf), " *(+%X)+%X:%.2f>%.2f", i * 4, j * 4, a, b);
        part_changes += buf;
      }
    parts[key] = std::move(words);
  }
  if (!part_changes.empty()) RT_INFO("bot scan: soldier %08X parts changed:%s", o, part_changes.c_str());
}

// An object's type name: its vtable's slot 2 is getType (lis rX; addi r3, rX;
// blr), the type object's +4 points at the record whose first word is the name.
}  // namespace
std::string TypeName(uint32_t o) {
  if (o < 0x40000000 || o >= 0xC0000000 || (o & 3) || !IsCommitted(o)) return {};
  uint32_t vt = Ld32(o);
  if (vt < 0x82000000 || vt >= 0x82F00000) return {};
  uint32_t get = Ld32(vt + 8);
  if (get < 0x82000000 || get >= 0x82F00000) return {};
  uint32_t w1 = Ld32(get), w2 = Ld32(get + 4), w3 = Ld32(get + 8);
  if ((w1 >> 26) != 15 || (w2 >> 26) != 14 || ((w2 >> 21) & 31) != 3 || w3 != 0x4E800020) return {};
  uint32_t type = ((w1 & 0xFFFF) << 16) + uint32_t(int16_t(w2 & 0xFFFF));
  if (!IsCommitted(type)) return {};
  uint32_t record = Ld32(type + 4);
  if (record < 0x82000000 || record >= 0x83000000) return {};
  uint32_t name = Ld32(record);
  if (name < 0x82000000 || name >= 0x83000000) return {};
  return std::string(G<char>(name)).substr(0, 48);
}

// The bytes of an object's first 0x800 that changed between two snapshots and
// stayed small (0-7 both times): flags and states, e.g. a soldier's pose.
static std::vector<uint8_t> Bytes(uint32_t o) {
  std::vector<uint8_t> v(0x800);
  for (uint32_t k = 0; k < 0x800 && IsCommitted(o + k); ++k) v[k] = *G<uint8_t>(o + k);
  return v;
}
static std::string SmallByteChanges(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  std::string d;
  for (size_t k = 0; k < a.size() && k < b.size(); ++k)
    if (a[k] != b[k] && a[k] < 8 && b[k] < 8) {
      char buf[24];
      snprintf(buf, sizeof(buf), " +%zX:%u>%u", k, a[k], b[k]);
      d += buf;
    }
  return d.empty() ? " none" : d;
}
// BF_BC_TYPES=1 (testing): 30 s after soldiers appear, every vtable in the heaps whose
// type name has Soldier, Vehicle, Player or Camera in it, with how many
// objects and a few of them, and for those objects the float triples that
// could be a place on the map (to find the client entities' positions).
static std::string VtName(uint32_t vt) {
  if (vt < 0x82000000 || vt >= 0x82F00000 || !IsCommitted(vt + 8)) return {};
  uint32_t get = Ld32(vt + 8);
  if (get < 0x82000000 || get >= 0x82F00000) return {};
  uint32_t w1 = Ld32(get), w2 = Ld32(get + 4), w3 = Ld32(get + 8);
  if ((w1 >> 26) != 15 || (w2 >> 26) != 14 || ((w2 >> 21) & 31) != 3 || w3 != 0x4E800020) return {};
  uint32_t type = ((w1 & 0xFFFF) << 16) + uint32_t(int16_t(w2 & 0xFFFF));
  if (!IsCommitted(type)) return {};
  uint32_t record = Ld32(type + 4);
  if (record < 0x82000000 || record >= 0x83000000) return {};
  uint32_t name = Ld32(record);
  if (name < 0x82000000 || name >= 0x83000000) return {};
  return std::string(G<char>(name)).substr(0, 48);
}
static const bool g_bc_types_started = [] {
  if (!getenv("BF_BC_TYPES")) return false;
  std::thread([] {
    // Once soldiers are in the level (and 30 s more).
    do std::this_thread::sleep_for(std::chrono::seconds(10));
    while (Scan(0x820118D0).empty());
    std::this_thread::sleep_for(std::chrono::seconds(30));
    std::map<uint32_t, std::vector<uint32_t>> by_vt;
    std::map<uint32_t, std::string> names;
    for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
      if (page >= 0x80000000 && page < 0xA0000000) continue;
      if (!IsCommitted(uint32_t(page))) continue;
      for (uint32_t i = 0; i < 0x1000; i += 4) {
        uint32_t v = Ld32(uint32_t(page) + i);
        if (v < 0x82000000 || v >= 0x82200000 || (v & 3)) continue;
        auto it = names.find(v);
        if (it == names.end()) it = names.emplace(v, VtName(v)).first;
        const std::string& n = it->second;
        if (n.empty()) continue;
        if (n.find("Soldier") == std::string::npos && n.find("Vehicle") == std::string::npos &&
            n.find("Player") == std::string::npos && n.find("Camera") == std::string::npos)
          continue;
        auto& list = by_vt[v];
        if (list.size() < 64) list.push_back(uint32_t(page) + i);
      }
    }
    for (auto& [vt, objs] : by_vt) {
      RT_INFO("bc types: %08X %s: %zu%s", vt, names[vt].c_str(), objs.size(), objs.size() >= 64 ? "+" : "");
      for (size_t k = 0; k < objs.size() && k < 3; ++k) {
        uint32_t o = objs[k];
        std::string places;
        for (uint32_t off = 0; off + 8 < 0x600 && IsCommitted(o + off + 8); off += 4) {
          float x = LdF(o + off), y = LdF(o + off + 4), z = LdF(o + off + 8);
          if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) > 20 && std::fabs(x) < 2048 &&
              y > 40 && y < 300 && std::fabs(z) > 20 && std::fabs(z) < 2048) {
            char buf[48];
            snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off, x, y, z);
            places += buf;
            off += 8;
          }
        }
        RT_INFO("bc types:   %08X:%s", o, places.substr(0, 600).c_str());
      }
    }
  }).detach();
  return true;
}();

namespace {
// A soldier's squad (campaign AI), once per squad: what it points at, two
// levels down.
void DescribeSquad(uint32_t o) {
  static std::set<uint32_t> squads;
  // Its squad (campaign AI): what that points at, two levels down.
  for (uint32_t off = 0; off < 0x800; off += 4) {
    uint32_t sq = off ? Ld32(o + off) : o;  // o itself may be the squad
    if (TypeName(sq) != "Squad" || !squads.insert(sq).second) continue;
    std::string sq_out;
    for (uint32_t so = 4; so < 0x400; so += 4) {
      uint32_t q = Ld32(sq + so);
      std::string n = TypeName(q);
      if (n.empty()) continue;
      sq_out += " +" + std::to_string(so) + ":" + n;
      std::string inner;
      for (uint32_t io = 4; io < 0x200; io += 4) {
        std::string m = TypeName(Ld32(q + io));
        if (!m.empty()) inner += " +" + std::to_string(io) + ":" + m;
      }
      if (!inner.empty()) sq_out += "{" + inner.substr(0, 300) + "}";
    }
    RT_INFO("bot scan: soldier %08X squad +%X %08X:%s", o, off, sq, sq_out.substr(0, 2500).c_str());
  }}

// The server's player for a soldier (BF_BOT_SCAN, testing): objects that hold
// the gamertag ("Host", inline or by pointer, BF_BOT_NAME to change it) and
// within 0x400 bytes a pointer to the soldier or one of the objects pointing at it.
void FindPlayer(uint32_t soldier) {
  const char* env = getenv("BF_BOT_NAME");
  std::string name = env ? env : "Host";
  std::vector<uint32_t> names;  // where the name string is
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const char* p = reinterpret_cast<const char*>(g_base + page);
    for (int i = 0; i + name.size() + 1 < 0x1000; ++i)
      if (!memcmp(p + i, name.c_str(), name.size() + 1) && (i == 0 || p[i - 1] == 0))
        names.push_back(uint32_t(page) + i);
  }
  std::string out;
  for (uint32_t at : names) {
    // Holders of a pointer to the name, plus the name's own place (inline).
    std::vector<uint32_t> holders = {at};
    uint32_t want = _byteswap_ulong(at);
    for (uint64_t page = 0x40000000; page < 0xC0000000 && holders.size() < 8; page += 0x1000) {
      if (page >= 0x80000000 && page < 0xA0000000) continue;
      if (!IsCommitted(uint32_t(page))) continue;
      const uint32_t* q = reinterpret_cast<const uint32_t*>(g_base + page);
      for (int i = 0; i < 1024; ++i)
        if (q[i] == want) holders.push_back(uint32_t(page) + i * 4);
    }
    for (uint32_t h : holders) {
      // Look around the holder (0x400 each way) for the soldier pointer.
      for (int64_t d = -0x400; d <= 0x400; d += 4) {
        uint32_t a2 = uint32_t(int64_t(h) + d);
        if (a2 < 0x40000000 || !IsCommitted(a2) || Ld32(a2) != soldier) continue;
        char buf[200];
        // The object's start: the nearest vtable-looking word below the soldier pointer.
        uint32_t start = 0, vt = 0;
        for (uint32_t back = 0; back <= 0x600; back += 4) {
          uint32_t w = Ld32(a2 - back);
          if (w >= 0x82000000 && w < 0x82180000 && Ld32(w) >= 0x82180000 && Ld32(w) < 0x82F00000) {
            start = a2 - back, vt = w;
            break;
          }
        }
        snprintf(buf, sizeof(buf), " name@%08X holder@%08X soldier-ptr@%08X; object %08X vt %08X: soldier at +%X, name at +%X",
                 at, h, a2, start, vt, a2 - start, at - start);
        out += buf;
      }
    }
  }
  RT_INFO("bot scan: player of soldier %08X: %zu names;%s", soldier, names.size(), out.substr(0, 1500).c_str());
}

// What a soldier points at (first 0x800 bytes) and what points at it, by type:
// tells an AI soldier (campaign) from a player's (BF_BOT_SCAN, once per soldier).
void DescribeLinks(uint32_t o) {
  static std::set<uint32_t> done;
  if (!done.insert(o).second) return;
  std::string out;
  for (uint32_t off = 4; off < 0x800; off += 4) {
    std::string n = TypeName(Ld32(o + off));
    if (!n.empty()) out += " +" + std::to_string(off) + ":" + n;
  }
  RT_INFO("bot scan: soldier %08X points at:%s", o, out.substr(0, 1500).c_str());

  std::string refs;
  uint32_t want = _byteswap_ulong(o);
  for (uint64_t page = 0x40000000; page < 0xC0000000 && refs.size() < 1500; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; ++i) {
      if (p[i] != want) continue;
      uint32_t at = uint32_t(page) + i * 4;
      // The object holding it: the nearest object start below (within 0x400): a
      // named one, or else the nearest word that looks like a vtable (82xxxxxx
      // in the data section, whose first entry is code).
      for (uint32_t back = 0; back <= 0x400 && back <= at - 0x40000000; back += 4) {
        std::string n = TypeName(at - back);
        uint32_t w = Ld32(at - back);
        bool vt = w >= 0x82000000 && w < 0x82180000 && (Ld32(w) >= 0x82180000 && Ld32(w) < 0x82F00000);
        if (n.empty() && !vt) continue;
        char buf[96];
        snprintf(buf, sizeof(buf), " %s(%08X)+%X", n.empty() ? ("vt" + std::to_string(w)).c_str() : n.c_str(),
                 at - back, back);
        if (n.empty()) snprintf(buf, sizeof(buf), " [vt %08X](%08X)+%X", w, at - back, back);
        refs += buf;
        break;
      }
    }
  }
  RT_INFO("bot scan: soldier %08X pointed at by:%s", o, refs.c_str());

}

// BF_BOT_CENSUS=1 (testing): every 10 s, the number of live objects per vtable
// (first words that point at a vtable: in the data section, first entry code),
// logged where it changed by 1 to 3: what appears when a player joins.
void Census() {
  static std::map<uint32_t, int> last;
  static bool first = true;
  std::map<uint32_t, int> now;
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    const uint32_t* q = reinterpret_cast<const uint32_t*>(g_base + page);
    for (int i = 0; i < 1024; i += 1) {
      uint32_t w = _byteswap_ulong(q[i]);
      if (w < 0x82000000 || w >= 0x82180000 || (w & 3)) continue;
      uint32_t e = Ld32(w);
      if (e < 0x82180000 || e >= 0x82F00000) continue;
      now[w]++;
    }
  }
  if (!first) {
    std::string out;
    for (auto& [vt, n] : now) {
      int d = n - (last.count(vt) ? last[vt] : 0);
      if (d >= 1 && d <= 3) {
        char buf[48];
        snprintf(buf, sizeof(buf), " %08X:+%d(%d)", vt, d, n);
        out += buf;
      }
    }
    for (auto& [vt, n] : last)
      if (!now.count(vt) || now[vt] < n) {
        int d = (now.count(vt) ? now[vt] : 0) - n;
        if (d >= -3) {
          char buf[48];
          snprintf(buf, sizeof(buf), " %08X:%d", vt, d);
          out += buf;
        }
      }
    RT_INFO("bot census:%s", out.substr(0, 3000).c_str());
  }
  first = false;
  last = std::move(now);
}

// BF_BOT_PLAYER_VTS="vt,vt,..." (testing): for every object with one of these
// vtables, where in its first 0x800 bytes it holds a player's name (inline or
// by pointer: "Host", "Joiner") or a soldier: which class is the player.
void CheckPlayerClasses() {
  const char* env = getenv("BF_BOT_PLAYER_VTS");
  if (!env) return;
  std::string list = env;
  for (size_t pos = 0; pos < list.size();) {
    size_t comma = list.find(',', pos);
    uint32_t vt = uint32_t(strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 16));
    pos = comma == std::string::npos ? list.size() : comma + 1;
    for (uint32_t o : Scan(vt)) {
      std::string hits;
      for (uint32_t off = 0; off < 0x800 && IsCommitted(o + off + 8); off += 4) {
        const char* inl = G<char>(o + off);
        for (const char* nm : {"Host", "Joiner"})
          if (!memcmp(inl, nm, strlen(nm) + 1)) hits += std::string(" +") + std::to_string(off) + "=\"" + nm + "\"";
        uint32_t q = Ld32(o + off);
        if (q >= 0x40000000 && q < 0xC0000000 && IsCommitted(q) && IsCommitted(q + 8)) {
          for (const char* nm : {"Host", "Joiner"})
            if (!memcmp(G<char>(q), nm, strlen(nm) + 1)) hits += std::string(" +") + std::to_string(off) + "->\"" + nm + "\"";
          if (Ld32(q) == 0x820118D0) hits += " +" + std::to_string(off) + "->soldier";
        }
      }
      if (!hits.empty()) RT_INFO("bot players: vt %08X object %08X:%s", vt, o, hits.c_str());
    }
  }
}

// BF_BOT_PLAYER_DUMP=1 (testing): each per-player object (vtable 8201B118),
// once, 0x600 bytes: non-zero words with the type names they point at, so a
// real player's can be compared with a fake one's.
void DumpPlayers() {
  if (!getenv("BF_BOT_PLAYER_DUMP")) return;
  static std::map<uint32_t, int> seen;
  for (uint32_t o : Scan(0x8201B118)) {
    if (seen[o]++ != 2) continue;  // the third scan after it appears: 20-30 s in
    std::string out;
    for (uint32_t off = 0; off < 0x600; off += 4) {
      uint32_t v = Ld32(o + off);
      if (!v) continue;
      std::string tn = TypeName(v);
      char buf[72];
      snprintf(buf, sizeof(buf), " +%X:%08X%s%s", off, v, tn.empty() ? "" : ":", tn.c_str());
      out += buf;
    }
    RT_INFO("bot player %08X:%s", o, out.substr(0, 6000).c_str());
  }
}

// BF_BOT_DUMP_VT="vt,vt" (testing): every object with one of these vtables,
// once (on the third scan it is seen), its first 0x60 bytes with type names.
void DumpVtables() {
  const char* env = getenv("BF_BOT_DUMP_VT");
  if (!env) return;
  static std::map<uint32_t, int> seen;
  std::string list = env;
  for (size_t pos = 0; pos < list.size();) {
    size_t comma = list.find(',', pos);
    uint32_t vt = uint32_t(strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 16));
    pos = comma == std::string::npos ? list.size() : comma + 1;
    for (uint32_t o : Scan(vt)) {
      if (seen[o]++ != 2) continue;
      std::string out;
      for (uint32_t off = 0; off < 0x60; off += 4) {
        uint32_t v = Ld32(o + off);
        std::string tn = TypeName(v);
        char buf[72];
        snprintf(buf, sizeof(buf), " +%X:%08X%s%s", off, v, tn.empty() ? "" : ":", tn.c_str());
        out += buf;
      }
      RT_INFO("bot dump vt %08X object %08X:%s", vt, o, out.c_str());
      // And what its +14 points at (vtable and first words).
      uint32_t t = Ld32(o + 0x14);
      if (t >= 0x40000000 && t < 0xC0000000 && IsCommitted(t)) {
        std::string w;
        for (uint32_t off = 0; off < 0x80; off += 4) {
          uint32_t v = Ld32(t + off);
          std::string tn = TypeName(v);
          char buf[72];
          snprintf(buf, sizeof(buf), " +%X:%08X%s%s", off, v, tn.empty() ? "" : ":", tn.c_str());
          w += buf;
        }
        RT_INFO("bot dump vt %08X object %08X +14 -> %08X:%s", vt, o, t, w.c_str());
      }
    }
  }
}

// BF_BOT_INPUT_PROBE=1 (testing): twice a second, the float fields (first
// 0x800 bytes) of every per-player object (BF_BOT_PLAYER_VTS) and of every
// soldier's entry component (soldier +2B0) that go from about 0 to about +-1
// or back: where the server keeps a player's received stick input.
void ProbeInput() {
  static std::map<uint32_t, std::vector<float>> last;
  std::vector<uint32_t> objs;
  if (const char* env = getenv("BF_BOT_PLAYER_VTS")) {
    std::string list = env;
    for (size_t pos = 0; pos < list.size();) {
      size_t comma = list.find(',', pos);
      uint32_t vt = uint32_t(strtoul(list.substr(pos, comma - pos).c_str(), nullptr, 16));
      pos = comma == std::string::npos ? list.size() : comma + 1;
      for (uint32_t o : Scan(vt)) objs.push_back(o);
    }
  }
  for (uint32_t so : Scan(0x820118D0)) {
    objs.push_back(so);
    uint32_t entry = Ld32(so + 0x2B0);
    if (entry >= 0x40000000 && entry < 0xC0000000 && IsCommitted(entry)) objs.push_back(entry);
  }
  for (uint32_t o : objs) {
    std::vector<float> now(0x200);
    for (uint32_t i = 0; i < 0x200; ++i) {
      uint32_t u = IsCommitted(o + i * 4) ? Ld32(o + i * 4) : 0;
      memcpy(&now[i], &u, 4);
    }
    auto it = last.find(o);
    if (it != last.end()) {
      std::string out;
      for (uint32_t i = 1; i < 0x200; ++i) {
        float a = it->second[i], b = now[i];
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        bool on = std::fabs(a) < 0.05f && std::fabs(b) > 0.9f && std::fabs(b) <= 1.0f;
        bool off = std::fabs(b) < 0.05f && std::fabs(a) > 0.9f && std::fabs(a) <= 1.0f;
        if (on || off) {
          char buf[48];
          snprintf(buf, sizeof(buf), " +%X:%.2f>%.2f", i * 4, a, b);
          out += buf;
        }
      }
      if (!out.empty()) RT_INFO("bot input: %08X (vt %08X):%s", o, Ld32(o), out.substr(0, 600).c_str());
    }
    last[o] = std::move(now);
  }
}

// BF_BOT_INPUT_WATCH=vt (testing): every 0.1 s, the non-zero floats in the first
// 0x200 bytes of each object with that vtable, when they changed: a closer look
// at a candidate input state while a joiner walks, turns and looks.
void WatchInput(uint32_t vt) {
  static std::map<uint32_t, std::string> last;
  for (uint32_t o : Scan(vt)) {
    std::string out;
    for (uint32_t off = 4; off < 0x200; off += 4) {
      uint32_t u = Ld32(o + off);
      float f;
      memcpy(&f, &u, 4);
      if (!std::isfinite(f) || std::fabs(f) < 0.001f || std::fabs(f) > 100.0f) continue;
      char buf[32];
      snprintf(buf, sizeof(buf), " +%X:%.2f", off, f);
      out += buf;
    }
    if (out != last[o]) RT_INFO("bot watch %08X:%s", o, out.substr(0, 900).c_str());
    last[o] = out;
  }
}

const bool g_input_watch_started = [] {
  const char* env = getenv("BF_BOT_INPUT_WATCH");
  if (!env) return false;
  uint32_t vt = uint32_t(strtoul(env, nullptr, 16));
  std::thread([vt] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      WatchInput(vt);
    }
  }).detach();
  return true;
}();

// BF_BOT_INPUT_HUNT=1 (testing): the whole server memory in three snapshots,
// taken when the files hunt1, hunt2 and hunt3 appear in the working folder
// (a joiner walking, stopped, walking again): the words that read 0.5-1.0,
// then under 0.05, then 0.5-1.0 again are where a player's stick input lives.
void HuntInput() {
  std::vector<uint32_t> cand;
  auto value = [](uint32_t a) {
    uint32_t u = Ld32(a);
    float f;
    memcpy(&f, &u, 4);
    return f;
  };
  auto walking = [](float f) { return std::isfinite(f) && f >= 0.5f && f <= 1.01f; };
  for (int step = 1; step <= 3; ++step) {
    std::string flag = "hunt" + std::to_string(step);
    while (!std::filesystem::exists(flag)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (step == 1) {
      for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
        if (page >= 0x80000000 && page < 0xA0000000) continue;
        if (!IsCommitted(uint32_t(page))) continue;
        for (uint32_t i = 0; i < 0x1000; i += 4)
          if (walking(value(uint32_t(page) + i))) cand.push_back(uint32_t(page) + i);
      }
    } else {
      std::vector<uint32_t> keep;
      for (uint32_t a : cand) {
        if (!IsCommitted(a)) continue;
        float f = value(a);
        if (step == 2 ? std::fabs(f) < 0.05f : walking(f)) keep.push_back(a);
      }
      cand.swap(keep);
    }
    RT_INFO("bot hunt: snapshot %d, %zu candidates", step, cand.size());
  }
  std::string out;
  for (size_t i = 0; i < cand.size() && i < 60; ++i) {
    uint32_t a = cand[i];
    // The object it is in: the nearest vtable-looking word below (0x400).
    uint32_t start = 0, vt = 0;
    for (uint32_t back = 0; back <= 0x400; back += 4) {
      uint32_t w = Ld32(a - back);
      if (w >= 0x82000000 && w < 0x82180000 && Ld32(w) >= 0x82180000 && Ld32(w) < 0x82F00000) {
        start = a - back, vt = w;
        break;
      }
    }
    char buf[96];
    snprintf(buf, sizeof(buf), " %08X=%.2f(obj %08X vt %08X +%X)", a, value(a), start, vt, a - start);
    out += buf;
  }
  RT_INFO("bot hunt: %s", out.c_str());
}

// BF_BOT_YAW_HUNT=1 (testing): at the files yaw1, yaw2, yaw3 (a joiner turned
// between them), every float in memory equal (within 0.002) to the turning
// player's aim yaw (the record whose +90 changed most since the last
// snapshot), or to minus it; the addresses that match all three times hold
// that player's heading.
}  // namespace
extern std::mutex g_record_yaw_mutex;
extern std::map<uint32_t, float> g_record_yaw;
namespace {
void HuntYaw() {
  std::map<uint32_t, float> prev;
  std::vector<std::pair<uint32_t, int>> cand;  // address, sign
  for (int step = 1; step <= 3; ++step) {
    std::string flag = "yaw" + std::to_string(step);
    while (!std::filesystem::exists(flag)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::map<uint32_t, float> now;
    {
      std::lock_guard l(g_record_yaw_mutex);
      now = g_record_yaw;
    }
    uint32_t rec = 0;
    float best = -1, yaw = 0;
    for (auto& [r, y] : now) {
      float d = prev.count(r) ? std::fabs(y - prev[r]) : (std::fabs(y) > 0.01f ? 0.001f : 0.0f);
      if (d > best || !rec) best = d, rec = r, yaw = y;
    }
    prev = now;
    auto value = [](uint32_t a) { return LdF(a); };
    if (step == 1) {
      for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
        if (page >= 0x80000000 && page < 0xA0000000) continue;
        if (!IsCommitted(uint32_t(page))) continue;
        for (uint32_t i = 0; i < 0x1000; i += 4) {
          float f = value(uint32_t(page) + i);
          if (std::fabs(f - yaw) < 0.002f) cand.push_back({uint32_t(page) + i, 1});
          else if (std::fabs(f + yaw) < 0.002f) cand.push_back({uint32_t(page) + i, -1});
        }
      }
    } else {
      std::vector<std::pair<uint32_t, int>> keep;
      for (auto& [a, sign] : cand)
        if (IsCommitted(a) && std::fabs(value(a) - sign * yaw) < 0.002f) keep.push_back({a, sign});
      cand.swap(keep);
    }
    RT_INFO("bot yaw hunt: snapshot %d, record %08X yaw %.3f (moved %.3f), %zu candidates", step, rec, yaw, best,
            cand.size());
  }
  std::string out;
  for (size_t i = 0; i < cand.size() && i < 40; ++i) {
    uint32_t a = cand[i].first, start = 0, vt = 0;
    for (uint32_t back = 0; back <= 0x600; back += 4) {
      uint32_t w = Ld32(a - back);
      if (w >= 0x82000000 && w < 0x82180000 && Ld32(w) >= 0x82180000 && Ld32(w) < 0x82F00000) {
        start = a - back, vt = w;
        break;
      }
    }
    char buf[96];
    snprintf(buf, sizeof(buf), " %08X%s(obj %08X vt %08X +%X)", a, cand[i].second < 0 ? "(-)" : "", start, vt, a - start);
    out += buf;
  }
  RT_INFO("bot yaw hunt:%s", out.c_str());
  // Who points into what holds them (the 0x600 bytes below each, aligned):
  // the objects (by vtable) owning an aim history.
  for (size_t i = 0; i < cand.size() && i < 12; ++i) {
    uint32_t a = cand[i].first, lo = a - 0x600, hi = a;
    std::string refs;
    int n = 0;
    for (uint64_t page = 0x40000000; page < 0xC0000000 && n < 8; page += 0x1000) {
      if (page >= 0x80000000 && page < 0xA0000000) continue;
      if (!IsCommitted(uint32_t(page))) continue;
      const uint32_t* q = reinterpret_cast<const uint32_t*>(g_base + page);
      for (int k = 0; k < 1024 && n < 8; ++k) {
        uint32_t v = _byteswap_ulong(q[k]);
        if (v < lo || v > hi || (v & 3)) continue;
        uint32_t at = uint32_t(page) + 4 * k, start = 0, vt = 0;
        for (uint32_t back = 0; back <= 0x600; back += 4) {
          uint32_t w = Ld32(at - back);
          if (w >= 0x82000000 && w < 0x82180000 && Ld32(w) >= 0x82180000 && Ld32(w) < 0x82F00000) {
            start = at - back, vt = w;
            break;
          }
        }
        char buf[96];
        snprintf(buf, sizeof(buf), " [%08X -> %08X: in vt %08X obj %08X +%X]", at, v, vt, start, at - start);
        refs += buf;
        ++n;
      }
    }
    RT_INFO("bot yaw hunt: %08X pointed at by:%s", a, refs.c_str());
  }
}
const bool g_yaw_hunt_started = [] {
  if (!getenv("BF_BOT_YAW_HUNT")) return false;
  std::thread(HuntYaw).detach();
  return true;
}();

const bool g_input_hunt_started = [] {
  if (!getenv("BF_BOT_INPUT_HUNT")) return false;
  std::thread(HuntInput).detach();
  return true;
}();

const bool g_input_probe_started = [] {
  if (!getenv("BF_BOT_INPUT_PROBE")) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      ProbeInput();
    }
  }).detach();
  return true;
}();

// Learning routes from play: every half second, each real player's soldier
// (not a bot) leaves a trail point every metre in nav/<map>.txt ("x y z" per
// line; "-" starts a new trail: a respawn or a jump of over 5 m). Bots route
// along these trails. On while hosting (--host); BF_BOT_NAV_RECORD=0 turns it off.
void RecordTrails() {
  static std::map<uint32_t, std::array<float, 3>> last;  // per soldier
  if (!g_lan_host || g_host_map.empty()) return;
  std::filesystem::create_directories("nav");
  FILE* f = fopen(("nav/" + g_host_map + ".txt").c_str(), "a");
  if (!f) return;
  std::set<uint32_t> alive;
  for (uint32_t so : Scan(0x820118D0)) {
    if (so == g_bot_soldier_for_trails) continue;
    alive.insert(so);
    float x = LdF(so + 0x200), y = LdF(so + 0x204), z = LdF(so + 0x208);
    if (!std::isfinite(x) || !std::isfinite(z)) continue;
    auto it = last.find(so);
    if (it == last.end()) {
      fprintf(f, "-\n%.2f %.2f %.2f\n", x, y, z);
      last[so] = {x, y, z};
      continue;
    }
    float d = std::hypot(x - it->second[0], z - it->second[2]);
    if (d < 1.0f) continue;
    if (d > 5.0f) fprintf(f, "-\n");
    fprintf(f, "%.2f %.2f %.2f\n", x, y, z);
    it->second = {x, y, z};
  }
  for (auto it = last.begin(); it != last.end();) it = alive.count(it->first) ? std::next(it) : last.erase(it);
  fclose(f);
}

const bool g_trails_started = [] {
  const char* env = getenv("BF_BOT_NAV_RECORD");
  if (env && !strcmp(env, "0")) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      RecordTrails();
    }
  }).detach();
  return true;
}();

// BF_BOT_NAVMESH_CHECK=1 (testing): 60 s and 120 s after start, how many
// Kynapse navigation mesh headers ("Kynogon AiMesh Array", "InPlace Mesh") are
// in memory: whether the game generated its navigation (campaign vs multiplayer).
const bool g_navmesh_check_started = [] {
  if (!getenv("BF_BOT_NAVMESH_CHECK")) return false;
  std::thread([] {
    for (int t : {60, 60}) {
      std::this_thread::sleep_for(std::chrono::seconds(t));
      for (const char* key : {"Kynogon AiMesh Array", "InPlace Mesh"}) {
        size_t n = strlen(key), hits = 0;
        std::string where;
        for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
          if (page >= 0x80000000 && page < 0xA0000000) continue;
          if (!IsCommitted(uint32_t(page))) continue;
          const char* q = reinterpret_cast<const char*>(g_base + page);
          for (int i = 0; i + int(n) < 0x1000; ++i)
            if (q[i] == key[0] && !memcmp(q + i, key, n)) {
              if (hits++ < 6) {
                char buf[16];
                snprintf(buf, sizeof(buf), " %08X", uint32_t(page) + i);
                where += buf;
              }
            }
        }
        RT_INFO("bot navmesh: \"%s\" %zu times in memory:%s", key, hits, where.c_str());
      }
    }
  }).detach();
  return true;
}();

// BF_BOT_DUMP_COLLISION=<file.obj> (the navigation tool, private: it is made
// from the player's own copy and stays on that PC): once the level is up,
// every Havok rigid body in memory (vtable 820BDE74, found through its RTTI)
// and its shapes, as triangles in map coordinates. A body's transform at +B0
// (rotation columns +B0/+C0/+D0, translation +E0), its shape at +10. The game
// fills in what the level files leave out (placements, mesh pointers), so the
// live world has the whole collision. Shapes (Havok 4.6, no RTTI) told apart
// by their fields: a MOPP tree (+14 its child, +18 its code), an extended mesh
// (+50 triangle subparts: 64 bytes, +10 triangles, +14 vertices, +18 vertex
// stride, +1C vertex count, +24 indices, +34 index stride, +38 1 for 16-bit;
// +58 shape subparts: 64 bytes, +10 child pointer array, +20 translation, +30
// rotation), a convex hull (+40 FourVectors vertices, +4C their count, +50
// plane equations, +54 their count).
static bool Ptr(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && !(a & 3) && IsCommitted(a); }
struct Mat { float c[3][3]; float t[3]; };
static void Apply(const Mat& m, const float v[3], float out[3]) {
  for (int i = 0; i < 3; ++i) out[i] = m.c[0][i] * v[0] + m.c[1][i] * v[1] + m.c[2][i] * v[2] + m.t[i];
}
static Mat FromQuat(const float q[4], const float t[3]) {
  float x = q[0], y = q[1], z = q[2], w = q[3];
  Mat m;
  m.c[0][0] = 1 - 2 * (y * y + z * z), m.c[0][1] = 2 * (x * y + z * w), m.c[0][2] = 2 * (x * z - y * w);
  m.c[1][0] = 2 * (x * y - z * w), m.c[1][1] = 1 - 2 * (x * x + z * z), m.c[1][2] = 2 * (y * z + x * w);
  m.c[2][0] = 2 * (x * z + y * w), m.c[2][1] = 2 * (y * z - x * w), m.c[2][2] = 1 - 2 * (x * x + y * y);
  for (int i = 0; i < 3; ++i) m.t[i] = t[i];
  return m;
}
static Mat Mul(const Mat& a, const Mat& b) {  // a after b
  Mat m;
  for (int j = 0; j < 3; ++j) Apply(Mat{{{a.c[0][0], a.c[0][1], a.c[0][2]}, {a.c[1][0], a.c[1][1], a.c[1][2]},
                                         {a.c[2][0], a.c[2][1], a.c[2][2]}}, {0, 0, 0}}, b.c[j], m.c[j]);
  Apply(a, b.t, m.t);
  return m;
}
struct CollisionOut {
  FILE* f;
  size_t verts = 0, tris = 0;
  float clip_r = 0, clip_x = 0, clip_z = 0;
  std::vector<std::array<float, 9>>* mem = nullptr;
  std::map<uint64_t, std::pair<int, uint32_t>> unhandled;  // shapes left out: (type, vtable) -> count, one of them  // clip_r > 0: only triangles touching that circle
  void Tri(const Mat& m, const float a[3], const float b[3], const float c[3]) {
    float w[3][3];
    Apply(m, a, w[0]), Apply(m, b, w[1]), Apply(m, c, w[2]);
    if (clip_r > 0 && std::hypot(w[0][0] - clip_x, w[0][2] - clip_z) > clip_r &&
        std::hypot(w[1][0] - clip_x, w[1][2] - clip_z) > clip_r && std::hypot(w[2][0] - clip_x, w[2][2] - clip_z) > clip_r)
      return;
    if (mem) {  // kept in memory instead (the bots' line of sight)
      mem->push_back({w[0][0], w[0][1], w[0][2], w[1][0], w[1][1], w[1][2], w[2][0], w[2][1], w[2][2]});
      ++tris;
      return;
    }
    for (auto& v : w) fprintf(f, "v %.3f %.3f %.3f\n", v[0], v[1], v[2]);
    fprintf(f, "f %zu %zu %zu\n", verts + 1, verts + 2, verts + 3);
    verts += 3, ++tris;
  }
};
static void ConvexOut(CollisionOut& out, const Mat& m, uint32_t s) {
  uint32_t vp = Ld32(s + 0x40), n = Ld32(s + 0x4C), pp = Ld32(s + 0x50), np = Ld32(s + 0x54);
  if (!Ptr(vp) || !Ptr(pp) || n < 3 || n > 4096 || np > 4096) return;
  std::vector<std::array<float, 3>> v(n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t blk = vp + 48 * (i / 4), j = i % 4;
    v[i] = {LdF(blk + 4 * j), LdF(blk + 16 + 4 * j), LdF(blk + 32 + 4 * j)};
  }
  float size = 0.01f;
  for (auto& q : v)
    for (float c : q) size = std::max(size, std::fabs(c));
  for (uint32_t k = 0; k < np; ++k) {
    float nx = LdF(pp + 16 * k), ny = LdF(pp + 16 * k + 4), nz = LdF(pp + 16 * k + 8), d = LdF(pp + 16 * k + 12);
    std::vector<std::array<float, 3>> on;
    for (auto& q : v)
      if (std::fabs(nx * q[0] + ny * q[1] + nz * q[2] + d) < 0.01f * size + 1e-3f) on.push_back(q);
    if (on.size() < 3) continue;
    float cx = 0, cy = 0, cz = 0;
    for (auto& q : on) cx += q[0], cy += q[1], cz += q[2];
    cx /= on.size(), cy /= on.size(), cz /= on.size();
    float ax[3] = {1, 0, 0};
    if (std::fabs(nx) > 0.9f) ax[0] = 0, ax[1] = 1;
    float ux[3] = {ny * ax[2] - nz * ax[1], nz * ax[0] - nx * ax[2], nx * ax[1] - ny * ax[0]};
    float ul = std::max(std::sqrt(ux[0] * ux[0] + ux[1] * ux[1] + ux[2] * ux[2]), 1e-6f);
    for (float& c : ux) c /= ul;
    float vx[3] = {ny * ux[2] - nz * ux[1], nz * ux[0] - nx * ux[2], nx * ux[1] - ny * ux[0]};
    std::sort(on.begin(), on.end(), [&](const auto& p, const auto& q) {
      auto ang = [&](const auto& r) {
        float d0 = r[0] - cx, d1 = r[1] - cy, d2 = r[2] - cz;
        return std::atan2(d0 * vx[0] + d1 * vx[1] + d2 * vx[2], d0 * ux[0] + d1 * ux[1] + d2 * ux[2]);
      };
      return ang(p) < ang(q);
    });
    for (size_t i = 1; i + 1 < on.size(); ++i) out.Tri(m, on[0].data(), on[i].data(), on[i + 1].data());
  }
}
// A box (half extents at +20) as 12 triangles.
static void BoxOut(CollisionOut& out, const Mat& m, uint32_t s) {
  float h[3] = {LdF(s + 0x20), LdF(s + 0x24), LdF(s + 0x28)};
  if (!(h[0] > 0 && h[1] > 0 && h[2] > 0 && h[0] < 1e4f && h[1] < 1e4f && h[2] < 1e4f)) return;
  float c[8][3];
  for (int i = 0; i < 8; ++i)
    c[i][0] = (i & 1) ? h[0] : -h[0], c[i][1] = (i & 2) ? h[1] : -h[1], c[i][2] = (i & 4) ? h[2] : -h[2];
  static const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
  for (auto& f : faces) out.Tri(m, c[f[0]], c[f[1]], c[f[2]]), out.Tri(m, c[f[0]], c[f[2]], c[f[3]]);
}
// Havok 4.6 shape types (hkShape +C): 3 sphere, 6 box, 7 capsule, 8 convex
// vertices, 10 list, 11 MOPP tree, 15 extended mesh, 0x18 the terrain's
// sampled heightfield (the terrain comes from its own file).
static void ShapeOut(CollisionOut& out, const Mat& m, uint32_t s, int depth) {
  if (!Ptr(s) || depth > 4) return;
  uint32_t type = Ld32(s + 0xC);
  if (type == 8) {
    ConvexOut(out, m, s);
    return;
  }
  if (type == 6) {
    BoxOut(out, m, s);
    return;
  }
  if (type == 11) {  // MOPP tree: its child
    ShapeOut(out, m, Ld32(s + 0x14), depth + 1);
    return;
  }
  if (type == 10) {  // list: +18 child entries (16 bytes, the shape first), +1C their count
    uint32_t arr = Ld32(s + 0x18), n = Ld32(s + 0x1C);
    for (uint32_t i = 0; Ptr(arr) && i < n && i < 4096; ++i) ShapeOut(out, m, Ld32(arr + 16 * i), depth + 1);
    return;
  }
  if (type != 15) {  // not turned into triangles: noted (type, vtable -> count, an example)
    float o[3] = {0, 0, 0}, w[3];
    if (type == 7) o[0] = LdF(s + 0x20), o[1] = LdF(s + 0x24), o[2] = LdF(s + 0x28);  // a capsule: its first end
    Apply(m, o, w);
    if (out.clip_r > 0 && std::hypot(w[0] - out.clip_x, w[2] - out.clip_z) > out.clip_r) return;
    auto& u = out.unhandled[(uint64_t(type) << 32) | Ld32(s)];
    if (!u.first++) u.second = s;
    return;
  }
  uint32_t tri = Ld32(s + 0x50), ntri = Ld32(s + 0x54), sub = Ld32(s + 0x58), nsub = Ld32(s + 0x5C);
  if (type == 15) {
    for (uint32_t e = 0; Ptr(tri) && e < ntri && e < 100000; ++e) {
      uint32_t t = tri + 64 * e;
      // A triangles subpart (Havok 4.6): +10 triangles, +14 vertices, +18 their
      // stride, +1C their count, +20 the extrusion (a vector), +30 indices, +34
      // their stride, +38 the index type (1: 16-bit).
      uint32_t n = Ld32(t + 0x10), vb = Ld32(t + 0x14), vs = Ld32(t + 0x18), ib = Ld32(t + 0x30), is = Ld32(t + 0x34);
      bool i16 = (Ld32(t + 0x38) >> 24) == 1;
      if (!Ptr(vb) || !Ptr(ib) || n > 1000000 || vs < 12 || is < 6) {
        auto& u = out.unhandled[(uint64_t(0x100 | (Ld32(t + 0x38) >> 24)) << 32) | is];  // skipped: 0x100 | index type, stride
        if (!u.first++) u.second = t;
        continue;
      }
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t idx[3];
        for (int j = 0; j < 3; ++j)
          idx[j] = i16 ? (Ld32(ib + is * k + 2 * j) >> ((j & 1) ? 0 : 16) & 0xFFFF) : Ld32(ib + is * k + 4 * j);
        if (i16) {  // two 16-bit indices per word, big-endian
          uint32_t w0 = Ld32(ib + is * k), w1 = Ld32(ib + is * k + 4);
          idx[0] = w0 >> 16, idx[1] = w0 & 0xFFFF, idx[2] = w1 >> 16;
        }
        float v[3][3];
        for (int j = 0; j < 3; ++j)
          for (int c = 0; c < 3; ++c) v[j][c] = LdF(vb + vs * idx[j] + 4 * c);
        out.Tri(m, v[0], v[1], v[2]);
      }
    }
    for (uint32_t e = 0; Ptr(sub) && e < nsub && e < 100000; ++e) {
      uint32_t t = sub + 64 * e;
      uint32_t arr = Ld32(t + 0x10);
      if (!Ptr(arr)) continue;
      float q[4] = {LdF(t + 0x30), LdF(t + 0x34), LdF(t + 0x38), LdF(t + 0x3C)};
      float tr[3] = {LdF(t + 0x20), LdF(t + 0x24), LdF(t + 0x28)};
      ShapeOut(out, Mul(m, FromQuat(q, tr)), Ld32(arr), depth + 1);
    }
  }
}
static void DumpCollision(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  CollisionOut out{f};
  size_t bodies = 0;
  std::map<uint32_t, std::array<uint32_t, 4>> kinds;
  size_t templates = 0;
  for (uint32_t b : Scan(0x820BDE74)) {
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    size_t before = out.tris;
    uint32_t shape = Ld32(b + 0x10);
    // Only bodies in the physics world (hkWorldObject +8, the hkWorld: vtable
    // 820BD8A0); the others are templates loaded with the level's assets.
    uint32_t world = Ld32(b + 8);
    bool in_world = Ptr(world) && Ld32(world) == 0x820BD8A0;
    if (in_world) ShapeOut(out, m, shape, 0);
    else ++templates;
    ++bodies;
    // What the shapes are: per shape vtable, bodies, triangles, and the child's vtable.
    uint32_t vt = Ptr(shape) ? Ld32(shape) : 0, child = Ptr(shape) ? Ld32(shape + 0x14) : 0;
    auto& k = kinds[vt];
    k[0]++, k[1] += out.tris - before;
    if (Ptr(child)) k[2] = Ld32(child);
    if (k[0] == 1) k[3] = b;
  }
  // Where the world transform is: in the first bodies of each kind, rotation
  // columns (orthonormal, 16 bytes apart) followed by a translation in the map.
  for (auto& [vt, k] : kinds) {
    uint32_t b = k[3];
    std::string hits;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      float c[3][3];
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) c[j][i] = LdF(b + off + 16 * j + 4 * i);
      bool ok = true;
      for (int j = 0; j < 3 && ok; ++j) {
        float l = c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2];
        ok = std::isfinite(l) && std::fabs(l - 1) < 1e-3f;
      }
      for (int i = 0; i < 3 && ok; ++i)
        for (int j = i + 1; j < 3 && ok; ++j)
          ok = std::fabs(c[i][0] * c[j][0] + c[i][1] * c[j][1] + c[i][2] * c[j][2]) < 1e-3f;
      if (!ok) continue;
      float t[3] = {LdF(b + off + 48), LdF(b + off + 52), LdF(b + off + 56)};
      char buf[80];
      snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off, t[0], t[1], t[2]);
      hits += buf;
    }
    RT_INFO("bot nav: kind %08X body %08X transforms:%s", vt, b, hits.substr(0, 400).c_str());
  }
  // A shape of each kind (and its child): its first 0x70 bytes.
  for (auto& [vt, k] : kinds) {
    uint32_t shape = Ld32(k[3] + 0x10);
    for (uint32_t obj : {shape, Ptr(shape) ? Ld32(shape + 0x14) : 0u}) {
      if (!Ptr(obj)) continue;
      std::string w;
      for (uint32_t off = 0; off < 0x70; off += 4) {
        uint32_t u = Ld32(obj + off);
        float fv = LdF(obj + off);
        char buf[32];
        if (std::isfinite(fv) && std::fabs(fv) > 1e-4f && std::fabs(fv) < 1e5f) snprintf(buf, sizeof(buf), " +%X:%.2f", off, fv);
        else snprintf(buf, sizeof(buf), " +%X:%08X", off, u);
        if (u) w += buf;
      }
      RT_INFO("bot nav: kind %08X %s %08X:%s", vt, obj == shape ? "shape" : "child", obj, w.c_str());
    }
  }
  for (auto& [vt, k] : kinds)
    RT_INFO("bot nav: shape vtable %08X: %u bodies, %u triangles, child vtable %08X, e.g. body %08X at %.0f %.0f %.0f", vt,
            k[0], k[1], k[2], k[3], LdF(k[3] + 0xE0), LdF(k[3] + 0xE4), LdF(k[3] + 0xE8));
  fclose(f);
  RT_INFO("bot nav: collision of %zu rigid bodies (%zu not in the world), %zu triangles, to %s", bodies, templates,
          out.tris, path);
}
// The physics world's triangles within r of (x, z), one .obj group per rigid
// body (named by its address, shape vtable and whether in the world).
static void DumpCollisionNear(const char* path, float x, float z, float r) {
  FILE* f = fopen(path, "w");
  if (!f) return;
  CollisionOut out{f};
  out.clip_r = r, out.clip_x = x, out.clip_z = z;
  for (uint32_t b : Scan(0x820BDE74)) {
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    uint32_t shape = Ld32(b + 0x10), world = Ld32(b + 8);
    bool in_world = Ptr(world) && Ld32(world) == 0x820BD8A0;
    fprintf(f, "g body_%08X_vt%08X%s_w%08X_wvt%08X_type%u\n", b, Ptr(shape) ? Ld32(shape) : 0, in_world ? "" : "_out",
            world, Ptr(world) ? Ld32(world) : 0, Ptr(shape) ? Ld32(shape + 0xC) : 0);
    ShapeOut(out, m, shape, 0);
  }
  fclose(f);
  RT_INFO("bot nav: %zu triangles within %.0f m of %.1f %.1f to %s", out.tris, r, x, z, path);
  for (auto& [k, u] : out.unhandled) {
    std::string w;
    for (uint32_t off = 0; off < 0x60 && Ptr(u.second + off); off += 4) {
      char buf[16];
      snprintf(buf, sizeof(buf), " %08X", Ld32(u.second + off));
      w += buf;
    }
    RT_INFO("bot nav: shapes left out: type %u vtable %08X: %d, e.g. %08X:%s", uint32_t(k >> 32), uint32_t(k), u.first,
            u.second, w.c_str());
  }
}

const bool g_dump_collision_started = [] {
  const char* path = getenv("BF_BOT_DUMP_COLLISION");
  if (!path) return false;
  std::string p = path;
  std::thread([p] {
    // When the level's bodies are up and their number stopped changing.
    size_t last = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      size_t n = Scan(0x820BDE74).size();
      if (n > 10 && n == last) break;
      last = n;
    }
    DumpCollision(p.c_str());
    // The play area for the navigation mesh: the crates' extent (every
    // base's interaction entities, +D0/+D8) and 350 m around it (the spawns).
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (uint32_t ie : Scan(0x82011680)) {
      float x = LdF(ie + 0xD0), z = LdF(ie + 0xD8);
      if (!std::isfinite(x) || !std::isfinite(z) || std::fabs(x) > 4096 || std::fabs(z) > 4096) continue;
      lo[0] = std::min(lo[0], x), lo[1] = std::min(lo[1], z), hi[0] = std::max(hi[0], x), hi[1] = std::max(hi[1], z);
    }
    if (lo[0] < 1e8f)
      RT_INFO("bot nav: play area %.0f %.0f %.0f %.0f", lo[0] - 350, lo[1] - 350, hi[0] + 350, hi[1] + 350);
  }).detach();
  return true;
}();

// BF_BOT_TYPES=Name,Name (testing): 60 s into the level, the vtables of the
// classes with those names and their objects (up to 8 each): the words of
// their first 0x100 bytes, and the mission objectives (crates) they point to
// within 0x400 bytes.
const bool g_types_started = [] {
  const char* names = getenv("BF_BOT_TYPES");
  if (!names) return false;
  std::string list = std::string(",") + names + ",";
  std::thread([list] {
    std::this_thread::sleep_for(std::chrono::seconds(150));
    std::map<uint32_t, int> vts;
    for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
      if (page >= 0x80000000 && page < 0xA0000000) continue;
      if (!IsCommitted(uint32_t(page))) continue;
      const uint32_t* q = reinterpret_cast<const uint32_t*>(g_base + page);
      for (int i = 0; i < 1024; ++i) {
        uint32_t w = _byteswap_ulong(q[i]);
        if (w < 0x82000000 || w >= 0x82180000 || (w & 3)) continue;
        uint32_t e = Ld32(w);
        if (e >= 0x82180000 && e < 0x82F00000 && !vts.count(w)) vts[w] = int(page + 4 * i);
      }
    }
    for (auto& [vt, example] : vts) {
      std::string name = TypeName(uint32_t(example));
      if (name.empty() || list.find("," + name + ",") == std::string::npos) continue;
      std::vector<uint32_t> objs = Scan(vt);
      RT_INFO("bot types: %s vtable %08X, %zu objects", name.c_str(), vt, objs.size());
      for (size_t i = 0; i < objs.size() && i < 24; ++i) {
        std::string w, crates;
        for (uint32_t off = 0; off < 0x100; off += 4) {
          char buf[16];
          snprintf(buf, sizeof(buf), " %08X", Ld32(objs[i] + off));
          w += buf;
        }
        for (uint32_t off = 0; off < 0x400; off += 4) {
          uint32_t p = Ld32(objs[i] + off);
          if (Ptr(p) && Ld32(p) == 0x82013AF0) crates += " +" + std::to_string(off) + "->" + std::to_string(p);
        }
        std::string where;  // transforms: three unit rows, then a translation
        for (uint32_t off = 0; off + 64 <= 0x400; off += 4) {
          bool ok = true;
          for (int j = 0; j < 3 && ok; ++j) {
            float a0 = LdF(objs[i] + off + 16 * j), a1 = LdF(objs[i] + off + 16 * j + 4), a2 = LdF(objs[i] + off + 16 * j + 8);
            float l = a0 * a0 + a1 * a1 + a2 * a2;
            ok = std::isfinite(l) && std::fabs(l - 1) < 1e-3f;
          }
          if (!ok) continue;
          char buf[64];
          snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off + 48, LdF(objs[i] + off + 48), LdF(objs[i] + off + 52),
                   LdF(objs[i] + off + 56));
          where += buf;
        }
        RT_INFO("bot types: %s %08X data %08X at%s | crates:%s", name.c_str(), objs[i], Ld32(objs[i] + 0x14), where.c_str(),
                crates.c_str());
      }
    }
  }).detach();
  return true;
}();

const bool g_census_started = [] {
  if (!getenv("BF_BOT_CENSUS") && !getenv("BF_BOT_PLAYER_DUMP") && !getenv("BF_BOT_DUMP_VT")) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      if (getenv("BF_BOT_CENSUS")) Census();
      CheckPlayerClasses();
      DumpPlayers();
      DumpVtables();
    }
  }).detach();
  return true;
}();

const bool g_scan_started = [] {
  if (!getenv("BF_BOT_SCAN")) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      std::string counts;
      for (const Kind& k : kKinds) counts += std::string(" ") + k.name + "=" + std::to_string(Scan(k.vtable).size());
      RT_INFO("bot scan:%s", counts.c_str());
      for (uint32_t o : Scan(kKinds[0].vtable)) {
        DescribeFields("soldier", o);
        LogChanges(o);
        DescribeLinks(o);
        DescribeSquad(o);
        FindPlayer(o);
      }
      for (uint32_t o : Scan(0x82047AA8)) DescribeSquad(o);
      for (uint32_t o : Scan(kKinds[2].vtable)) DescribeFields("tactical objective", o);
      for (uint32_t o : Scan(kKinds[3].vtable)) {
        DescribeFields("mission objective", o);
        uint32_t spatial = Ld32(o + 0x14);
        if (IsCommitted(spatial))
          RT_INFO("bot scan: mission objective %08X *(+14)+60: %.1f %.1f %.1f", o, LdF(spatial + 0x60),
                  LdF(spatial + 0x64), LdF(spatial + 0x68));
      }
      // Capture points as bf1943 reads them: owner +80, position *(+14) + 60.
      for (uint32_t o : Scan(kKinds[4].vtable)) {
        uint32_t spatial = Ld32(o + 0x14);
        if (!IsCommitted(spatial)) continue;
        RT_INFO("bot scan: capture point %08X owner %u at %.1f %.1f %.1f", o, Ld32(o + 0x80), LdF(spatial + 0x60),
                LdF(spatial + 0x64), LdF(spatial + 0x68));
      }
      // Vehicles: their kind's name as in bf1943 (*(*(+14) + C)), if it reads.
      for (uint32_t o : Scan(kKinds[1].vtable)) {
        uint32_t data = Ld32(o + 0x14), name = IsCommitted(data) ? Ld32(data + 0xC) : 0;
        RT_INFO("bot scan: vehicle %08X kind %s", o,
                name >= 0x40000000 && IsCommitted(name) ? std::string(G<char>(name)).substr(0, 40).c_str() : "?");
      }
    }
  }).detach();
  return true;
}();

}  // namespace

}  // namespace rt

// Game function hooks: at global scope (C++ names, as bfbc1_game.cpp's
// GAME_PATCH ones), so they replace the recompiled functions.
using namespace rt;
// How soldiers are made (BF_BOT_TRACE, campaign level 1 and Valley Run):
//   Every soldier, AI or human, comes from one factory, 822F5ED8, a virtual
//   function of the soldier blueprint (600C0074, vtable 820160E8; the same
//   object in the campaign and in multiplayer), called with a creation record
//   on the stack (r4: 5FFE5460 600262E0 <soldier + 2A0> <id> 0 0) and the new
//   soldier's memory (r5); it runs the constructor 822BD818 (822B8428 is its
//   destructor). The constructor's third argument (vtable 8206E890, a counter
//   at +8) is the same kind of object for every soldier.
//   Campaign: the first soldiers come straight from the level data while it
//   loads; later ones right after ServerSoldierSpawnEntity +14 (82321550,
//   probably its event handler). Multiplayer: the deploying player's soldier
//   from the same factory, with no soldier spawn entity call.
//   Campaign AI is organised in Squad objects (vtable 82047AA8, 5 on level 1)
//   holding inline SquadMember entries (0xD0 apart, +14 back to the squad).
//   An AI soldier is made in this order: 82638400 (r3 an AI object, r4 the
//   soldier's level data at 6005xxxx) -> 8264DA80, the squad hub (r3 the AI
//   system, r4 the squad, -1 for a new one; it makes Squads and SquadMembers)
//   -> ServerSoldierSpawnEntity +90 (82322508: r4 the spawn transform, r6
//   the spawn entity) -> the soldier factory. So the AI asks a soldier spawn
//   entity for its soldier. Multiplayer soldiers have no squad.
//   82638400's r5 is the spawn request: an id, a flag, -1, then among others
//   the soldier's SensingTemplateData and BehaviourTemplateData (its AI
//   templates). The campaign level has 12 and 14 of those; the multiplayer
//   levels only their type definitions (Valley Run checked), so an AI soldier
//   there needs templates from elsewhere.
// The server's players (BF_BOT_CENSUS / BF_BOT_PLAYER_VTS, host + joiner):
//   52 classes get a second object when a player joins. Two hold the
//   player's soldier once deployed: a large object (vtable 8201B118, soldier at
//   +500; 8201B108 at +4, 8201B0E4 at +330, 82055814 at +36C, 820553F4 at +4B4
//   are inside it) and a small one (8201D338, soldier at +4C; made through
//   ServerSoldierSpawnEntity +80). On join the server calls 82364C78 on the
//   player manager (vtable 8201B828, one object) with the new player's
//   connection-like object (vtable 82068EB0; the host has one too); that runs
//   8235FD08 -> 8235E9E8, which builds the large object. Neither object holds
//   the gamertag as a plain string.
//   The join's r4 is the peer's address inside its connection object (r4 - 4):
//   +4 family 2 and port (1501 for the host's own client, 1511 for a joiner),
//   +8 the IP (127.0.0.1). BF_BOT_FAKE joins a copy of only that record (port
//   1599): the join returns, then the server crashes on its next update
//   (826F3A40 <- 8234E390 <- 822D6CC8). The connection object is 0x60 bytes
//   (r4 - 4: vtable 82055080; +4 the address object, vtable 82068EB0; +48
//   flags, +4C a per-connection value, +50 a per-connection pointer; the
//   host's sit in an array of 0x60-byte slots). Joining a copy of the whole
//   object crashes the same way: 826F3A40 reads +14 of a per-player part
//   (vtable 8201B140, inside the 8201B118 object) that a real client's
//   messages after the join fill in and a fake player never gets.
//   That +14 (and +28) is the player's network peer (vtable 820550C0). After a
//   real join the server calls 82705810 on the peer manager (the player
//   manager + 8) with the player's index, 1 and its connection (vtable
//   8201B140), which builds the peer (826FD850); the link to the connection
//   comes later, from the network. BF_BOT_FAKE does the join, that call (index
//   7) and the link: the fake player then stays (two player objects and
//   connections with the host's, 4 minutes, no crash) and gets no traffic.
// Deploying (BF_BOT_STACK on the soldier factory: the call chain): the
//   player's message (8235FBD0 on its connection) -> 82332350 on its spawn
//   object (vtable 82019490; r4 the spawn point, 0 for any of its team's,
//   r5 0x800, r6 0x20) -> 823207F0 -> ... -> the soldier factory. The spawn
//   object comes from the client's message after the join (82359A58 on the
//   connection, r5 0x21): replaying the host's on the fake connection makes
//   one. 82332350 needs +1D4 (SoldierEntityData) and +1D8 (SoldierWeaponData),
//   the spawn screen's kit choice; +30 is the team, +104 the soldier once
//   deployed, +1D0 the chosen spawn point. With the host's kit borrowed, the
//   fake player deploys: a team 1 soldier at the attackers' spawn.
// Input (BF_BOT_INPUT_HUNT, then BF_BOT_INPUT_WATCH=82010C98 with a joiner's
//   stick): a soldier's input state is an object with vtable 82010C98, the
//   soldier at +4, analog actions as floats: +8 turn, +C look up/down, +14
//   forward (0.96 for a full stick). The fake soldier has one too, but values
//   written there before each server update (823D1DD0) are back at 0 by the
//   next one: the soldier update (822B92C8 <- 822C2C38 <- 822ACCD8 <- 82275A38,
//   seen with BF_BOT_WATCH_INPUT) copies them in every time with 821BF2A8 from
//   the player's received input (r4, vtable 8200DC48; +8 forward). Writing
//   forward there before that copy moves the fake soldier: it walked from its
//   spawn (-461, -221) to (-483, -188) on Valley Run.
//   The received input's fields (BF_BOT_INPUT_MAP, a joiner walking,
//   strafing, turning, looking, firing): +8 forward, +C strafe, +1C turn, +20
//   look, +28 fire; +2C.. how long each is held (half floats). Turn input
//   (with or without the held values) doesn't turn the fake soldier: its
//   heading (written by 822757E8 in the soldier update 822C2C38, seen with
//   BF_BOT_WATCH_HEADING) comes from somewhere else, not found yet (the yaw
//   BF_BOT_FIND_YAW matched once was a coincidence). 822757E8 only copies a
//   ready matrix in. Without turning, the brain moves with forward and strafe
//   (the direction split along the forward row +320/+328 and the right row
//   +300/+308; strafe right is minus that row): from the spawn the fake
//   soldier went from 165 m to 27 m from the first crate, then got stuck.
//   Buttons (hold:<button> automation steps with a joiner) show only as held
//   times (half floats, low half) at +40..+58: A +44, B +48/+4C, X +54, Y +40,
//   LB/RB +50, left stick click +58. Writing A's (+44) doesn't make the fake
//   soldier jump; sidestepping when stuck doesn't get it past the obstacle 26 m
//   from the crate: it needs routes (the levels' AI pathfinding data).
//   A press is a bit at +84 as well (A 0x2000, X 0x200000; the record is 0xB8
//   bytes). Holding X 0.8 m from the first crate for 30 s changes nothing in
//   the crate's object; interacting probably needs facing it
//   (PlayerInteractAimAngle), which needs turning.
//   A joiner's record also carries its aim: +90 yaw and +94 pitch (radians;
//   pitch rises looking up). Writing +90 for the fake soldier (BF_BOT_YAW_TEST:
//   0, 1.57, 3.14) doesn't turn it either.
//   What turns it: the soldier moves relative to its aim object (vtable
//   820264A8, minus the yaw at +134), and that object's setter 824ADEE8 takes
//   the new aim from a record (r4: +34 minus the yaw, +0/+8 its sine and
//   cosine). Filling that record for the fake soldier's aim object, with
//   atan2(dx, dz) + pi, makes forward walk toward the point.
//
// BF_BOT_TRACE=1 (testing): calls to ServerSoldierSpawnEntity's own virtual
// functions (the ones it overrides from ServerSpawnEntity), the first 30 of each
// with this and r4-r6, and how many soldiers there are after: to see which one
// spawns a soldier, in the campaign (AI) and in multiplayer.
static const bool g_trace = getenv("BF_BOT_TRACE") != nullptr;
static uint32_t Vt(uint32_t o) { return o >= 0x40000000 && o < 0xC0000000 && IsCommitted(o) ? Ld32(o) : 0u; }
static std::string Words(uint32_t a, int n) {
  std::string out;
  for (int i = 0; i < n && a >= 0x40000000 && a < 0xC0000000 && IsCommitted(a + i * 4); ++i) {
    char buf[12];
    snprintf(buf, sizeof(buf), " %08X", Ld32(a + i * 4));
    out += buf;
  }
  return out;
}
static void TraceSpawn(const char* fn, PPCContext& ctx) {
  static std::map<std::string, int> n;
  static std::mutex m;
  std::lock_guard l(m);
  if (n[fn]++ < 30)
    RT_INFO("bot trace: %s this=%08X r4=%08X[%s ] r5=%08X(vt %08X)[%s ] r6=%08X(vt %08X)", fn, ctx.r3.u32, ctx.r4.u32,
            Words(ctx.r4.u32, 6).c_str(), ctx.r5.u32, Vt(ctx.r5.u32), Words(ctx.r5.u32, 4).c_str(), ctx.r6.u32,
            Vt(ctx.r6.u32));
}
#define TRACE_VFN(addr, slot)                                  extern "C" PPC_FUNC(__imp__sub_##addr);                      PPC_FUNC(sub_##addr) {                                         if (g_trace) TraceSpawn("soldier spawn " slot " " #addr, ctx);     __imp__sub_##addr(ctx, base);                              }
// The soldier factory (the only caller of ServerSoldierEntity's constructor
// 822BD818; called through a vtable): what it gets and what it returns.
extern "C" PPC_FUNC(__imp__sub_822F5ED8);
PPC_FUNC(sub_822F5ED8) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  auto vt = [](uint32_t o) { return o >= 0x40000000 && o < 0xC0000000 && IsCommitted(o) ? Ld32(o) : 0u; };
  // BF_BOT_STACK=1: the call stack of the first 3 soldiers made (the recompiled
  // functions' names: the guest call chain, virtual calls included).
  static const bool stack = getenv("BF_BOT_STACK") != nullptr;
  static int stacks = 0;
  if (stack && stacks++ < 3) {
    RT_INFO("bot stack: soldier factory, call %d", stacks);
    bf_crash_diag::PrintStack(nullptr);
  }
  __imp__sub_822F5ED8(ctx, base);
  if (g_trace) {
    uint32_t made = ctx.r3.u32;
    RT_INFO("bot trace: soldier factory this=%08X(vt %08X) r4=%08X(vt %08X) r5=%08X(vt %08X) r6=%08X -> %08X(vt %08X team %u)",
            r3, vt(r3), r4, vt(r4), r5, vt(r5), r6, made, vt(made), vt(made) ? Ld32(made + 0x48) : 0);
  }
}
// The other one: 822BDCB8 calls the constructor 822B8428 (also through a vtable).
extern "C" PPC_FUNC(__imp__sub_822BDCB8);
PPC_FUNC(sub_822BDCB8) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  auto vt = [](uint32_t o) { return o >= 0x40000000 && o < 0xC0000000 && IsCommitted(o) ? Ld32(o) : 0u; };
  __imp__sub_822BDCB8(ctx, base);
  if (g_trace) {
    uint32_t made = ctx.r3.u32;
    RT_INFO("bot trace: soldier factory 2 this=%08X(vt %08X) r4=%08X(vt %08X) r5=%08X(vt %08X) r6=%08X -> %08X(vt %08X)",
            r3, vt(r3), r4, vt(r4), r5, vt(r5), r6, made, vt(made));
  }
}
// Logs an AI call: each argument with its vtable and type name, and the first
// 12 words of the r4/r5 records with the type names they point at.
static void TraceAi(const char* label, PPCContext& ctx) {
  auto one = [](uint32_t v) {
    std::string tn = TypeName(v);
    char buf[96];
    snprintf(buf, sizeof(buf), "%08X(vt %08X%s%s)", v, Vt(v), tn.empty() ? "" : " ", tn.c_str());
    return std::string(buf);
  };
  RT_INFO("bot trace: %s r3=%s r4=%s r5=%s r6=%08X", label, one(ctx.r3.u32).c_str(), one(ctx.r4.u32).c_str(),
          one(ctx.r5.u32).c_str(), ctx.r6.u32);
  for (uint32_t r : {ctx.r4.u32, ctx.r5.u32}) {
    std::string w;
    for (uint32_t k = 0; k < 12 && r >= 0x40000000 && r < 0xC0000000 && IsCommitted(r + k * 4); ++k) {
      uint32_t v = Ld32(r + k * 4);
      std::string tn = TypeName(v);
      char buf[80];
      snprintf(buf, sizeof(buf), " %08X%s%s", v, tn.empty() ? "" : ":", tn.c_str());
      w += buf;
    }
    if (!w.empty()) RT_INFO("bot trace:   %08X =%s", r, w.c_str());
  }
}
// The AI squad hub (makes squads and squad members) and its callers.
#define TRACE_AI(addr, label)                      \
  PPC_FUNC(sub_##addr) {                           \
    if (g_trace) {                                 \
      static int n = 0;                            \
      if (n++ < 40) TraceAi(label " " #addr, ctx); \
    }                                              \
    __imp__sub_##addr(ctx, base);                  \
  }
// The big per-player object (vtable 8201B118): its builders and their callers.
extern "C" PPC_FUNC(__imp__sub_823548E8);
extern "C" PPC_FUNC(__imp__sub_8235E9E8);
extern "C" PPC_FUNC(__imp__sub_82358908);
extern "C" PPC_FUNC(__imp__sub_8235FD08);
extern "C" PPC_FUNC(__imp__sub_82364C78);
TRACE_AI(823548E8, "player obj builder 1")
TRACE_AI(8235E9E8, "player obj builder 2")
TRACE_AI(82358908, "player obj caller 1 (virtual)")
TRACE_AI(8235FD08, "player obj caller 2")
// The join call itself: dump the object it gets (r4), 0x100 bytes, with the
// type names and text its words point at, and the stack record (r5).
// BF_BOT_FAKE: the fake player's per-player spawn object (vtable 82019490),
// and its deploy from the server update a few seconds after the host's
// soldier exists: 82332350 on it, with the host's deploy's arguments
// (1, 0x800, 0x1E), as the player's deploy message does (8235FBD0).
extern "C" PPC_FUNC(__imp__sub_82332350);
extern "C" PPC_FUNC(__imp__sub_823D1DD0);
// The bots (BF_BOT_FAKE=N, 1 to 16): fake players joined on the server (no
// client, no traffic), each with its connection, spawn object, soldier, the
// soldier's input state (vtable 82010C98, the soldier at +4) and its brain.
// Teams: BF_BOT_TEAM=1 or 2 puts them all on one, else they alternate from the
// attackers (1).
struct NavMemory {
  float last_main[3] = {0, 0, 0};  // its last place on the mesh's main piece
  bool have = false;
};
struct Bot {
  int index = 0;      // port 1599 + index, peer index 7 + index
  uint32_t team = 1;  // 1 attackers, 2 defenders
  uint32_t conn = 0, spawner = 0, soldier = 0, input = 0, record = 0, aim_obj = 0, aim_for = 0;
  int64_t died_at = 0, input_looked = 0;
  NavMemory nav;
  std::vector<std::array<float, 3>> route;
  size_t step = 0;
  int64_t planned = 0;
  float sx = 0, sz = 0, last_sx = 1e9f, last_sz = 1e9f;
  int64_t since = 0, unstick_until = 0, jump_until = 0;
  int side = 1;
  int stucks = 0;              // stuck events near the same place (last_sx, last_sz)
  int detours = 0;             // detours from around there so far (each one wider)
  float detour_from[2] = {1e9f, 1e9f};
  float detour[3] = {0, 0, 0};  // a way out: a reachable place nearby
  int64_t detour_until = 0;
  uint32_t enemy = 0, engaged = 0;
  float ex = 0, ey = 0, ez = 0, edist = 0;
  int64_t looked_at = 0, engaged_at = 0, reload_until = 0;
  bool fired = false;
  uint32_t use_entity = 0;  // an interaction to use (arm, defuse), from the input copy hook
  uint32_t hit = 0;         // a soldier its shot hit, for the input copy hook to damage
  int64_t next_shot = 0;
  std::set<uint32_t> used;  // the interactions it used
  float want_heading = 0;   // for the aim setter (824ADEE8)
  float want_pitch = 0;     // radians, up + (the aim record's +38)
  float face = 0;           // where its body should face: atan2(dx, dz)
  bool facing = false;
  bool crouched = false;    // as it made itself (the pose toggles)
  int64_t pose_at = 0;      // when it last toggled
  const char* kit_class = nullptr;  // its kit, chosen at the first deploy
  int64_t grenade_at = 0;           // when its last throw began
  float grenade_pos[3] = {0, 0, 0};  // where it lands (where the enemy stood)
  bool grenade_live = false;         // thrown, not gone off yet
  bool steer = false;
  int64_t logged = 0;
};
static std::deque<Bot> g_bots;
static Bot* BotWithSoldier(uint32_t so) {
  for (Bot& b : g_bots)
    if (b.soldier && b.soldier == so) return &b;
  return nullptr;
}
static Bot* BotWithSpawner(uint32_t sp) {
  for (Bot& b : g_bots)
    if (b.spawner && b.spawner == sp) return &b;
  return nullptr;
}
// A bot's soldier's input state: looked for once a second until found.
static void FindBotInputs() {
  int64_t now = int64_t(GetTickCount64());
  for (Bot& b : g_bots) {
    if (!b.soldier || b.input || now - b.input_looked < 1000) continue;
    b.input_looked = now;
    for (uint32_t o : Scan(0x82010C98))
      if (Ld32(o + 4) == b.soldier) b.input = o;
    if (b.input) RT_INFO("bot %d: input state of soldier %08X: %08X", b.index, b.soldier, b.input);
  }
}

// The game's own navigation is Kynapse path data, built offline and loaded
// from Output/xenon/ai/levels/<level>/{infantry,vehicle}/pathdata.hpd
// ("AI/%s/%s/PathDataDefinition", "/PathData.hpd" in the image): the disc has
// it for the 7 campaign levels only. BF_BOT_NAVMESH_CHECK finds its "InPlace
// Mesh" data in memory in the campaign, none in multiplayer. So multiplayer
// bots route along what they learn instead.
// Routes from the learnt trails (nav/<map>.txt, RecordTrails): points linked
// along each trail and to other trails' points within 2.5 m (height within
// 1.5 m); A* from the point nearest the bot to the one nearest its goal.
struct NavGraph {
  std::vector<std::array<float, 3>> p;
  std::vector<std::vector<int>> next;
  size_t file_size = 0;
};
static NavGraph g_nav;
static void LoadNav() {
  std::string path = "nav/" + g_host_map + ".txt";
  std::error_code ec;
  size_t size = size_t(std::filesystem::file_size(path, ec));
  if (ec || size == g_nav.file_size) return;
  NavGraph g;
  g.file_size = size;
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return;
  char line[96];
  int prev = -1;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '-' && (line[1] == '\n' || line[1] == '\r' || line[1] == 0)) {  // a lone "-": a new trail
      prev = -1;
      continue;
    }
    std::array<float, 3> q;
    if (sscanf(line, "%f %f %f", &q[0], &q[1], &q[2]) != 3) continue;
    g.p.push_back(q);
    g.next.emplace_back();
    int i = int(g.p.size()) - 1;
    if (prev >= 0) g.next[i].push_back(prev), g.next[prev].push_back(i);
    prev = i;
  }
  fclose(f);
  // Join trails where they pass close (a grid of 4 m cells).
  std::map<std::pair<int, int>, std::vector<int>> cells;
  for (int i = 0; i < int(g.p.size()); ++i) cells[{int(std::floor(g.p[i][0] / 4)), int(std::floor(g.p[i][2] / 4))}].push_back(i);
  for (int i = 0; i < int(g.p.size()); ++i) {
    int cx = int(std::floor(g.p[i][0] / 4)), cz = int(std::floor(g.p[i][2] / 4));
    for (int ox = -1; ox <= 1; ++ox)
      for (int oz = -1; oz <= 1; ++oz) {
        auto it = cells.find({cx + ox, cz + oz});
        if (it == cells.end()) continue;
        for (int j : it->second)
          if (j > i && std::hypot(g.p[i][0] - g.p[j][0], g.p[i][2] - g.p[j][2]) < 2.5f &&
              std::fabs(g.p[i][1] - g.p[j][1]) < 1.5f)
            g.next[i].push_back(j), g.next[j].push_back(i);
      }
  }
  RT_INFO("bot nav: %zu route points from %s", g.p.size(), path.c_str());
  g_nav = std::move(g);
}
static int NearestNav(float x, float z, float* dist) {
  int best = -1;
  float bd = 1e9f;
  for (int i = 0; i < int(g_nav.p.size()); ++i) {
    float d = std::hypot(g_nav.p[i][0] - x, g_nav.p[i][2] - z);
    if (d < bd) bd = d, best = i;
  }
  *dist = bd;
  return best;
}
static std::vector<int> RouteNav(int from, int to) {
  std::vector<float> cost(g_nav.p.size(), 1e30f);
  std::vector<int> came(g_nav.p.size(), -1);
  using Item = std::pair<float, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  auto h = [&](int i) { return std::hypot(g_nav.p[i][0] - g_nav.p[to][0], g_nav.p[i][2] - g_nav.p[to][2]); };
  cost[from] = 0;
  open.push({h(from), from});
  while (!open.empty()) {
    int i = open.top().second;
    open.pop();
    if (i == to) break;
    for (int j : g_nav.next[i]) {
      float c = cost[i] + std::hypot(g_nav.p[i][0] - g_nav.p[j][0], g_nav.p[i][2] - g_nav.p[j][2]);
      if (c < cost[j]) cost[j] = c, came[j] = i, open.push({c + h(j), j});
    }
  }
  std::vector<int> route;
  if (came[to] < 0 && from != to) return route;
  for (int i = to; i >= 0; i = came[i]) route.push_back(i);
  std::reverse(route.begin(), route.end());
  return route;
}

// The map's navigation mesh (tools/navgen: nav/<map>.nav, "BFNV", version, size,
// Detour data), routed with Detour: the nearest polygons to both ends (within
// 3 m across, 6 m up and down), A* between them, then the straight path's
// corners. Without one, the learnt trails.
static dtNavMesh* g_navmesh = nullptr;
static void LoadBlocked();
static dtNavMeshQuery* g_navquery = nullptr;
// The mesh's joined pieces (polygons reached through their links). Most of a
// map is one; the rest are islands Recast leaves on roofs, crates, ledges and
// spawn spots (Valley Run: 836 pieces, the main one 15432 of 23392 polygons).
// Routes run on the main piece: a goal snapped onto an island (base 2's first
// crate: 2 polygons) has no way to it, nor has a soldier standing on one.
static std::unordered_map<dtPolyRef, int> g_piece;
static int g_main_piece = -1;
static void FindPieces() {
  const dtNavMesh* mesh = g_navmesh;
  std::vector<int> sizes;
  int n = 0;
  for (int ti = 0; ti < mesh->getMaxTiles(); ++ti) {
    const dtMeshTile* tile = mesh->getTile(ti);
    if (!tile || !tile->header) continue;
    dtPolyRef base = mesh->getPolyRefBase(tile);
    for (int pi = 0; pi < tile->header->polyCount; ++pi) {
      ++n;
      dtPolyRef start = base | dtPolyRef(pi);
      if (g_piece.count(start)) continue;
      int id = int(sizes.size());
      sizes.push_back(0);
      std::vector<dtPolyRef> stack{start};
      g_piece[start] = id;
      while (!stack.empty()) {
        dtPolyRef r = stack.back();
        stack.pop_back();
        ++sizes[size_t(id)];
        const dtMeshTile* t = nullptr;
        const dtPoly* poly = nullptr;
        if (dtStatusFailed(mesh->getTileAndPolyByRef(r, &t, &poly))) continue;
        for (unsigned l = poly->firstLink; l != DT_NULL_LINK; l = t->links[l].next) {
          dtPolyRef o = t->links[l].ref;
          if (o && !g_piece.count(o)) g_piece[o] = id, stack.push_back(o);
        }
      }
    }
  }
  if (sizes.empty()) return;
  g_main_piece = int(std::max_element(sizes.begin(), sizes.end()) - sizes.begin());
  RT_INFO("bot nav: %d polygons in %zu pieces, the main one %d", n, sizes.size(), sizes[size_t(g_main_piece)]);
}
static bool InMainPiece(dtPolyRef r) {
  auto it = g_piece.find(r);
  return it != g_piece.end() && it->second == g_main_piece;
}
// The main piece's nearest place to p within the box ext (half sizes).
static bool NearestMain(const float p[3], const float ext[3], const dtQueryFilter& filter, dtPolyRef* ref,
                        float out[3]) {
  static dtPolyRef polys[2048];
  int n = 0;
  g_navquery->queryPolygons(p, ext, &filter, polys, &n, 2048);
  float best = 1e30f;
  *ref = 0;
  for (int i = 0; i < n; ++i) {
    if (!InMainPiece(polys[i])) continue;
    float c[3];
    if (dtStatusFailed(g_navquery->closestPointOnPoly(polys[i], p, c, nullptr))) continue;
    float d = dtVdistSqr(c, p);
    if (d < best) best = d, *ref = polys[i], dtVcopy(out, c);
  }
  return *ref != 0;
}
static bool LoadNavMesh() {
  static bool tried = false;
  if (tried) return g_navquery != nullptr;
  tried = true;
  std::string path = "nav/" + g_host_map + ".nav";
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  // Version 1: one tile (its data's size, the data); version 2 (navgen's
  // tiles): dtNavMeshParams, the tile count, then each tile's x, y, size, data.
  char magic[4];
  int version = 0;
  bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "BFNV", 4) && fread(&version, 4, 1, f) == 1;
  g_navmesh = dtAllocNavMesh();
  int size = 0, tiles = 0;
  if (ok && version == 1) {
    ok = fread(&size, 4, 1, f) == 1 && size > 0;
    unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(size, DT_ALLOC_PERM)) : nullptr;
    ok = ok && data && fread(data, 1, size_t(size), f) == size_t(size) &&
         dtStatusSucceed(g_navmesh->init(data, size, DT_TILE_FREE_DATA));
    tiles = 1;
  } else if (ok && version == 2) {
    dtNavMeshParams params;
    int count = 0;
    ok = fread(&params, sizeof(params), 1, f) == 1 && fread(&count, 4, 1, f) == 1 &&
         dtStatusSucceed(g_navmesh->init(&params));
    for (int i = 0; ok && i < count; ++i) {
      int tx, ty, ts;
      ok = fread(&tx, 4, 1, f) == 1 && fread(&ty, 4, 1, f) == 1 && fread(&ts, 4, 1, f) == 1 && ts > 0;
      unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(ts, DT_ALLOC_PERM)) : nullptr;
      ok = ok && data && fread(data, 1, size_t(ts), f) == size_t(ts) &&
           dtStatusSucceed(g_navmesh->addTile(data, ts, DT_TILE_FREE_DATA, 0, nullptr));
      size += ts, ++tiles;
    }
  } else {
    ok = false;
  }
  fclose(f);
  if (!ok) {
    RT_WARN("bot nav: %s isn't a navigation mesh Detour can read", path.c_str());
    dtFreeNavMesh(g_navmesh);
    g_navmesh = nullptr;
    return false;
  }
  g_navquery = dtAllocNavMeshQuery();
  g_navquery->init(g_navmesh, 4096);
  RT_INFO("bot nav: navigation mesh %s (%d bytes, %d tiles)", path.c_str(), size, tiles);
  FindPieces();
  return true;
}
// Learnt blockers: polygons a bot couldn't get onto (the navigation mesh can
// be wrong: a ledge it thinks a soldier steps up, a gap he doesn't fit), off
// for routing (flag 0) and kept in nav/<map>.blocked (one polygon reference
// per line) for later matches.
static void LoadBlocked() {
  FILE* f = fopen(("nav/" + g_host_map + ".blocked").c_str(), "r");
  if (!f) return;
  unsigned long long ref;
  int n = 0;
  while (fscanf(f, "%llx", &ref) == 1)
    if (dtStatusSucceed(g_navmesh->setPolyFlags(dtPolyRef(ref), 0))) ++n;
  fclose(f);
  RT_INFO("bot nav: %d blocked polygons learnt before", n);
}
static std::map<dtPolyRef, int64_t> g_blocked_until;
static void BlockPoly(const float at[3]) {
  if (!g_navquery) return;
  dtQueryFilter filter;
  const float ext[3] = {1.5f, 3, 1.5f};
  dtPolyRef ref = 0;
  float p[3];
  g_navquery->findNearestPoly(at, ext, &filter, &ref, p);
  if (!ref) return;
  // For a minute only, and not kept: a soldier in the way (bots on one route
  // bump into each other) looks the same, and blocks kept for good cut the
  // routes to the crates off.
  g_navmesh->setPolyFlags(ref, 0);
  g_blocked_until[ref] = int64_t(GetTickCount64()) + 60000;
  RT_INFO("bot nav: blocked polygon %llx at %.1f %.1f %.1f for a minute", (unsigned long long)ref, p[0], p[1], p[2]);
}
// Blocks whose minute is over open again (from the brain, once a second).
static void UnblockExpired() {
  static int64_t last = 0;
  int64_t now = int64_t(GetTickCount64());
  if (now - last < 1000 || !g_navmesh) return;
  last = now;
  for (auto it = g_blocked_until.begin(); it != g_blocked_until.end();)
    if (now > it->second) g_navmesh->setPolyFlags(it->first, 1), it = g_blocked_until.erase(it);
    else ++it;
}

// The corners of a route from a to b on the navigation mesh (empty if none).
static std::vector<std::array<float, 3>> RouteMesh(const float a[3], const float b[3], NavMemory* mem = nullptr) {
  std::vector<std::array<float, 3>> out;
  if (!LoadNavMesh()) return out;
  dtQueryFilter filter;
  const float ext[3] = {3, 6, 3};
  dtPolyRef sa = 0, sb = 0;
  float pa[3], pb[3];
  dtStatus s1 = g_navquery->findNearestPoly(a, ext, &filter, &sa, pa);
  // The goal (a crate) can be up on a structure or in a spot too tight to walk:
  // the nearest walkable place within 8 m across and 60 m up or down.
  const float goal_ext[3] = {8, 60, 8};
  dtStatus s2 = g_navquery->findNearestPoly(b, goal_ext, &filter, &sb, pb);
  static int said = 0;
  bool log = said++ < 4;
  if (log)
    RT_INFO("bot nav: from %.1f %.1f %.1f: poly %llx (%08X); to %.1f %.1f %.1f: poly %llx (%08X) at %.1f %.1f %.1f", a[0], a[1],
            a[2], (unsigned long long)sa, s1, b[0], b[1], b[2], (unsigned long long)sb, s2, pb[0], pb[1], pb[2]);
  // The goal on an island: the main piece's nearest place instead. The soldier
  // on one: first to the main piece's nearest place (a drop or a jump away).
  if (sb && !InMainPiece(sb)) {
    float q[3];
    dtPolyRef r = 0;
    if (NearestMain(b, goal_ext, filter, &r, q)) {
      if (log) RT_INFO("bot nav: goal on an island; to %.1f %.1f %.1f instead", q[0], q[1], q[2]);
      sb = r, dtVcopy(pb, q);
    }
  }
  // Where the soldier last was on the main piece: the way off an island is
  // the way it came on (the nearest main place can be behind a wall).
  static NavMemory shared;  // without a bot's own
  NavMemory& memory = mem ? *mem : shared;
  float* last_main = memory.last_main;
  bool& have_last = memory.have;
  // An island the main piece touches (the same floor, only not joined there,
  // a route along its edge can step onto it): on the main piece already.
  if (sa && !InMainPiece(sa)) {
    const float touch_ext[3] = {1.5f, 1, 1.5f};
    float q[3];
    dtPolyRef r = 0;
    if (NearestMain(a, touch_ext, filter, &r, q)) sa = r, dtVcopy(pa, q);
  }
  if (sa && InMainPiece(sa)) dtVcopy(last_main, pa), have_last = true;
  if (sa && !InMainPiece(sa)) {
    const float near_ext[3] = {6, 4, 6};
    const float back_ext[3] = {1, 2, 1};
    float q[3];
    dtPolyRef r = 0;
    bool back = have_last && dtVdist(last_main, a) < 30.0f &&
                (g_navquery->findNearestPoly(last_main, back_ext, &filter, &r, q), r && InMainPiece(r));
    if (back) RT_INFO("bot nav: back the way it came");
    if (back || NearestMain(a, near_ext, filter, &r, q)) {
      RT_INFO("bot nav: on an island at %.1f %.1f %.1f; first to %.1f %.1f %.1f", a[0], a[1], a[2], q[0], q[1], q[2]);
      out.push_back({q[0], q[1], q[2]});
      sa = r, dtVcopy(pa, q);
    }
  }
  if (!sa || !sb) return out;
  dtPolyRef polys[512];
  int npolys = 0;
  dtStatus s3 = g_navquery->findPath(sa, sb, pa, pb, &filter, polys, &npolys, 512);
  if (log) RT_INFO("bot nav: path %d polygons (%08X)", npolys, s3);
  if (!npolys) return out;
  // Partial (the soldier on a piece of the mesh not joined to the goal's, as
  // up on a ledge it can drop from; the mesh has no links down): first to the
  // nearest place around, up to 5 m away and 4 m lower, that has a whole way.
  if (s3 & DT_PARTIAL_RESULT) {
    const float near_ext[3] = {0.5f, 4, 0.5f};
    for (float r : {1.5f, 3.0f, 5.0f}) {
      for (int k = 0; k < 16; ++k) {
        float q[3] = {a[0] + r * std::sin(k * 0.3927f), a[1] - 2, a[2] + r * std::cos(k * 0.3927f)}, pq[3];
        dtPolyRef sq = 0;
        g_navquery->findNearestPoly(q, near_ext, &filter, &sq, pq);
        if (!sq || sq == sa) continue;
        int nq = 0;
        dtStatus s4 = g_navquery->findPath(sq, sb, pq, pb, &filter, polys, &nq, 512);
        if (!nq || (s4 & DT_PARTIAL_RESULT)) continue;
        RT_INFO("bot nav: no whole way from %.1f %.1f %.1f; down to %.1f %.1f %.1f first", a[0], a[1], a[2], pq[0], pq[1],
                pq[2]);
        out.push_back({pq[0], pq[1], pq[2]});
        npolys = nq;
        memcpy(pa, pq, sizeof(pa));
        break;
      }
      if (!out.empty()) break;
    }
  }
  float corners[256 * 3];
  unsigned char flags[256];
  dtPolyRef refs[256];
  int n = 0;
  g_navquery->findStraightPath(pa, pb, polys, npolys, corners, flags, refs, &n, 256);
  for (int i = 0; i < n; ++i) out.push_back({corners[3 * i], corners[3 * i + 1], corners[3 * i + 2]});
  return out;
}

extern uint32_t g_bot_aim_obj;
extern std::atomic<int> g_bot_setter_calls;
extern float g_bot_want_heading;
extern bool g_bot_steer;
// The aim object's +134 holds minus the heading (atan2 of the forward row).
static void set_aim(uint32_t aim_obj, float heading) {
  float v = -heading;
  uint32_t u;
  memcpy(&u, &v, 4);
  St32(aim_obj + 0x134, u);
}

// BF_BOT_HOST_TEAM=1 (testing): the players' spawn objects (vtable 82019490)
// put on team 1, the attackers, until they deploy (+104 the soldier).
const bool g_host_team_started = [] {
  if (!getenv("BF_BOT_HOST_TEAM")) return false;
  static const int team = atoi(getenv("BF_BOT_HOST_TEAM")) == 2 ? 2 : 1;  // BF_BOT_HOST_TEAM=1 or 2
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      for (uint32_t o : Scan(0x82019490))
        if (!Ld32(o + 0x104) && Ld32(o + 0x30) != uint32_t(team) && !BotWithSpawner(o)) {  // the host's, not a bot's
          St32(o + 0x30, uint32_t(team));
          RT_INFO("bot pilot: spawn object %08X put on team %d", o, team);
        }
    }
  }).detach();
  return true;
}();

// BF_BOT_HOST_PILOT=1 (testing): the host's own soldier driven through its pad
// (sticks and buttons, bfbc1_game.cpp) to the nearest crate along the
// navigation mesh; there it holds X for 10 s at a time while the interaction
// entities within 3 m (server 82011680, client 82008478) are watched.
extern std::atomic<bool> g_arm_trace_on;
namespace rt {
extern std::atomic<int16_t> g_pilot_lx, g_pilot_ly, g_pilot_rx, g_pilot_ry;
extern std::atomic<uint16_t> g_pilot_buttons;
extern std::atomic<uint8_t> g_pilot_rt;
}  // namespace rt
static bool InSight(float ax, float ay, float az, float bx, float by, float bz);
static bool SightReady();
const bool g_host_pilot_started = [] {
  if (!getenv("BF_BOT_HOST_PILOT")) return false;
  std::thread([] {
    std::vector<std::array<float, 3>> route;
    size_t step = 0;
    int64_t planned = 0, hold_since = 0, logged = 0;
    std::map<uint32_t, std::vector<uint32_t>> seen;
    uint32_t so = 0;
    std::vector<uint32_t> crates;
    int64_t scanned = 0;
    // BF_BOT_PILOT_VEHICLE=1: to the nearest vehicle (ClientVehicleEntity,
    // its place +5A0) instead, the buttons tried there, and what changes in the
    // client soldier, that vehicle and the seats (ClientPlayerEntryComponent).
    static const bool pilot_vehicle = getenv("BF_BOT_PILOT_VEHICLE") != nullptr;
    const uint32_t at_off = pilot_vehicle ? 0x5A0 : 0xD0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      int16_t lx = 0, ly = 0, rx = 0;
      struct Apply {  // the sticks, set once per pass (heap scans take long)
        int16_t &lx, &ly, &rx;
        ~Apply() { g_pilot_lx = lx, g_pilot_ly = ly, g_pilot_rx = rx; }
      } apply{lx, ly, rx};
      if (int64_t(GetTickCount64()) - scanned > 2000 || (so && Ld32(so) != 0x820118D0)) {
        scanned = int64_t(GetTickCount64());
        so = 0;
        for (uint32_t o : Scan(0x820118D0))
          if (!BotWithSoldier(o) && (Ld32(o + 0x48) == 1 || Ld32(o + 0x48) == 2) &&
              std::fabs(LdF(o + 0x200)) + std::fabs(LdF(o + 0x208)) > 1.0f)
            so = o;  // a real soldier (a stray word can match the vtable)
        crates = Scan(pilot_vehicle ? 0x820082E8 : 0x82011680);
      }
      if (!so || !LoadNavMesh()) {
        g_pilot_buttons = 0;
        continue;
      }
      float x = LdF(so + 0x200), y = LdF(so + 0x204), z = LdF(so + 0x208);
      float heading = std::atan2(LdF(so + 0x320), LdF(so + 0x328));
      float best = 1e9f, tx = 0, ty = 0, tz = 0;
      uint32_t target = 0;
      for (uint32_t c : crates) {
        if (pilot_vehicle && std::fabs(LdF(c + at_off + 4) - y) > 2.5f) continue;  // one on the ground, at its height
        float d = std::hypot(LdF(c + at_off) - x, LdF(c + at_off + 8) - z);
        if (d < best) best = d, tx = LdF(c + at_off), ty = LdF(c + at_off + 4), tz = LdF(c + at_off + 8), target = c;
      }
      if (best > 1e8f) continue;
      int64_t now = int64_t(GetTickCount64());
      if (now - logged > 3000) {
        logged = now;
        RT_INFO("bot pilot: soldier %08X team %u at %.1f %.1f %.1f heading %.2f, crate %.1f m (%.1f %.1f)", so,
                Ld32(so + 0x48), x, y, z, heading, best, tx, tz);
      }
      // BF_BOT_PILOT_SHOOT=1: the nearest bot of the other team within 25 m in
      // sight is turned to and fired at (RT); the first one's health (+3C)
      // writes are logged with their call stacks: the server's damage path.
      static const bool pilot_shoot = getenv("BF_BOT_PILOT_SHOOT") != nullptr;
      if (pilot_shoot) {
        uint32_t target = 0;
        float td = 45.0f;
        for (Bot& b : g_bots) {
          uint32_t t = b.soldier;
          if (!t || !IsCommitted(t + 0x400) || Ld32(t) != 0x820118D0 || Ld32(t + 0x48) == Ld32(so + 0x48) ||
              LdF(t + 0x3C) <= 0)
            continue;
          float d = std::hypot(LdF(t + 0x200) - x, LdF(t + 0x208) - z);
          if (d < td && SightReady() && InSight(x, y, z, LdF(t + 0x200), LdF(t + 0x204), LdF(t + 0x208))) td = d, target = t;
        }
        g_pilot_rt = target ? 255 : 0;
        if (target) {
          static uint32_t watched = 0;
          if (!watched) {
            watched = target;
            RT_INFO("bot pilot: shooting at bot soldier %08X (%.1f m); watching its health", target, td);
            StartWriteWatch(target + 0x3C, 4, 0);
          }
          static int64_t health_log = 0;
          if (now - health_log > 1000) {
            health_log = now;
            RT_INFO("bot pilot: firing at %08X, %.1f m, its health %.0f", target, td, LdF(target + 0x3C));
          }
          float want = std::atan2(LdF(target + 0x200) - x, LdF(target + 0x208) - z), diff = want - heading;
          while (diff > 3.14159265f) diff -= 6.2831853f;
          while (diff < -3.14159265f) diff += 6.2831853f;
          rx = int16_t(std::clamp(-diff * 3.0f, -1.0f, 1.0f) * 20000);
          // BF_BOT_PILOT_GRENADE=1: LB (the grenade) for 0.3 s every 8 s too.
          g_pilot_buttons = getenv("BF_BOT_PILOT_GRENADE") && now % 8000 < 300 ? 0x0100 : 0;
          continue;
        }
      }
      // BF_BOT_PILOT_STAY=1: it stays where it spawned (only turns and fires).
      if (getenv("BF_BOT_PILOT_STAY")) {
        g_pilot_buttons = 0;
        continue;
      }
      if (best < (pilot_vehicle ? 3.0f : 1.0f)) {  // at the crate: each button in turn, 6 s held then 2 s up
        if (pilot_vehicle) {
          static int64_t last_diff = 0;
          static std::map<uint32_t, std::vector<uint32_t>> was;
          if (now - last_diff > 1000) {
            last_diff = now;
            std::vector<std::pair<const char*, uint32_t>> objs{{"vehicle", target}};
            for (uint32_t c : Scan(0x82008D48)) objs.push_back({"client soldier", c});
            for (uint32_t c : Scan(0x82003A70)) objs.push_back({"seat", c});
            for (auto& [what, o] : objs) {
              std::vector<uint32_t> w(0x100);
              for (uint32_t k = 0; k < 0x100; ++k) w[k] = Ld32(o + 4 * k);
              auto& before = was[o];
              std::string d;
              for (uint32_t k = 0; k < 0x100 && !before.empty(); ++k)
                if (w[k] != before[k] && (Ptr(w[k]) || Ptr(before[k]) || w[k] < 16 || before[k] < 16)) {
                  char buf[64];
                  snprintf(buf, sizeof(buf), " +%X:%08X>%08X(%s)", 4 * k, before[k], w[k], TypeName(w[k]).c_str());
                  d += buf;
                }
              if (!d.empty()) RT_INFO("bot pilot: %s %08X changed:%s", what, o, d.substr(0, 900).c_str());
              before = w;
            }
          }
        }
        if (!hold_since) hold_since = now;
        // B arms ("B ARM CHARGE" on the HUD); the others were tried as well.
        static const uint16_t buttons[] = {0x2000, 0x4000, 0x8000, 0x0200, 0x0100, 0x0080, 0x0040, 0x2000};
        static const char* names[] = {"B", "X", "Y", "RB", "LB", "RS", "LS", "B looking down"};
        // BF_BOT_WATCH_ARM=1: who writes the arm entity's state (+8C): the
        // server interaction entity sharing its data (+14) with a client one.
        g_arm_trace_on = true;
        static bool watching = false;
        if (!watching && getenv("BF_BOT_WATCH_ARM")) {
          for (uint32_t sv : crates) {
            if (std::hypot(LdF(sv + 0xD0) - x, LdF(sv + 0xD8) - z) > 3.0f) continue;
            for (uint32_t cl : Scan(0x82008478))
              if (Ld32(cl + 0x14) == Ld32(sv + 0x14) && !watching) {
                watching = true;
                RT_INFO("bot pilot: watching arm entity %08X (+8C)", sv);
                StartWriteWatch(sv + 0x8C, 4, 0);
              }
          }
        }
        // BF_BOT_PILOT_HOLD=ms: B alone, held that long once.
        static const int hold_ms = getenv("BF_BOT_PILOT_HOLD") ? atoi(getenv("BF_BOT_PILOT_HOLD")) : 0;
        int phase = hold_ms ? 0 : int((now - hold_since) / 8000) % 8;
        bool hold = hold_ms ? now - hold_since < hold_ms : (now - hold_since) % 8000 < 6000;
        static int last_phase = -1;
        if (phase != last_phase) last_phase = phase, RT_INFO("bot pilot: holding %s", names[phase]);
        g_pilot_buttons = hold ? buttons[phase] : 0;
        g_pilot_ry = phase == 7 && (now - hold_since) % 8000 < 1500 ? -20000 : 0;  // look down first
        float want = std::atan2(tx - x, tz - z), diff = want - heading;
        while (diff > 3.14159265f) diff -= 6.2831853f;
        while (diff < -3.14159265f) diff += 6.2831853f;
        if (std::fabs(diff) > 0.1f) rx = int16_t(std::clamp(-diff * 3.0f, -1.0f, 1.0f) * 20000);
        // What changes in the interaction entities around.
        static int64_t last_look = 0;
        if (now - last_look > 1000) {
          last_look = now;
          for (uint32_t vt : {0x82011680u, 0x82008478u})
            for (uint32_t ie : Scan(vt)) {
              uint32_t at = vt == 0x82011680u ? 0xD0 : 0xF0;
              if (std::hypot(LdF(ie + at) - x, LdF(ie + at + 8) - z) > 3.0f) continue;
              std::vector<uint32_t> w(0x80);
              for (uint32_t k = 0; k < 0x80; ++k) w[k] = Ld32(ie + 4 * k);
              auto& before = seen[ie];
              std::string d;
              for (uint32_t k = 0; k < 0x80 && !before.empty(); ++k)
                if (w[k] != before[k]) {
                  char buf[40];
                  snprintf(buf, sizeof(buf), " +%X:%08X>%08X", 4 * k, before[k], w[k]);
                  d += buf;
                }
              if (before.empty() || !d.empty())
                RT_INFO("bot pilot: %s interaction %08X%s (X %s)", vt == 0x82011680u ? "server" : "client", ie,
                        d.empty() ? " first seen" : d.c_str(), hold ? "held" : "up");
              before = w;
            }
        }
        continue;
      }
      hold_since = 0, g_pilot_buttons = 0;
      // BF_BOT_PILOT_CROUCH=1: the crouch button (the right stick's click) for
      // 0.2 s every 4 s; the soldier's small bytes that changed 1 s later.
      if (getenv("BF_BOT_PILOT_CROUCH")) {
        static std::vector<uint8_t> before;
        static int64_t pressed = 0;
        if (now - pressed > 4000) pressed = now, before = Bytes(so);
        if (now - pressed < 200) g_pilot_buttons = 0x0080;
        static int64_t reported = 0;
        if (now - pressed > 1000 && reported != pressed) {
          reported = pressed;
          RT_INFO("bot pilot: crouch pressed: soldier bytes changed:%s", SmallByteChanges(before, Bytes(so)).substr(0, 500).c_str());
        }
      }
      // BF_BOT_PILOT_LOOK=1: looking up 1.5 s, then down 1.5 s, while walking.
      if (getenv("BF_BOT_PILOT_LOOK")) {
        int64_t ph = (now / 3000) % 2;
        g_pilot_ry = ph ? -20000 : 20000;
        static int64_t said_ph = -1;
        if (ph != said_ph) said_ph = ph, RT_INFO("bot pilot: looking %s", ph ? "down" : "up");
      }
      if (now - planned > 3000) {
        planned = now;
        float from[3] = {x, y, z}, to[3] = {tx, ty, tz};
        route = RouteMesh(from, to);
        step = 0;
      }
      while (step < route.size() && std::hypot(route[step][0] - x, route[step][2] - z) < 1.0f) ++step;
      float px = step < route.size() ? route[step][0] : tx, pz = step < route.size() ? route[step][2] : tz;
      float want = std::atan2(px - x, pz - z), diff = want - heading;
      while (diff > 3.14159265f) diff -= 6.2831853f;
      while (diff < -3.14159265f) diff += 6.2831853f;
      rx = int16_t(std::clamp(-diff * 3.0f, -1.0f, 1.0f) * 32000);
      ly = std::fabs(diff) < 1.0f ? 32000 : 8000;
    }
  }).detach();
  return true;
}();

// Line of sight: the physics world's triangles (the bodies in the world, as
// DumpCollision; the terrain heightfield is left out), binned once on a 4 m
// grid by their extent across, and a segment tested against those in the
// cells it crosses (Moller-Trumbore).
struct SightGrid {
  float x0 = 0, z0 = 0, cell = 4.0f;
  int nx = 0, nz = 0;
  std::vector<std::array<float, 9>> tris;
  std::vector<std::vector<uint32_t>> cells;
  std::vector<uint32_t> stamp;
  uint32_t visit = 0;
};
static SightGrid* g_sight = nullptr;
static void BuildSight() {
  auto* g = new SightGrid;
  CollisionOut out{nullptr};
  out.mem = &g->tris;
  for (uint32_t b : Scan(0x820BDE74)) {
    uint32_t world = Ld32(b + 8);
    if (!Ptr(world) || Ld32(world) != 0x820BD8A0) continue;
    Mat m;
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
    for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
    ShapeOut(out, m, Ld32(b + 0x10), 0);
  }
  float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
  for (auto& t : g->tris)
    for (int v = 0; v < 3; ++v) {
      lo[0] = std::min(lo[0], t[3 * v]), hi[0] = std::max(hi[0], t[3 * v]);
      lo[1] = std::min(lo[1], t[3 * v + 2]), hi[1] = std::max(hi[1], t[3 * v + 2]);
    }
  lo[0] = std::max(lo[0], -4096.0f), lo[1] = std::max(lo[1], -4096.0f);
  hi[0] = std::min(hi[0], 4096.0f), hi[1] = std::min(hi[1], 4096.0f);
  g->x0 = lo[0], g->z0 = lo[1];
  g->nx = std::max(1, int((hi[0] - lo[0]) / g->cell) + 1), g->nz = std::max(1, int((hi[1] - lo[1]) / g->cell) + 1);
  g->cells.resize(size_t(g->nx) * g->nz);
  for (uint32_t i = 0; i < g->tris.size(); ++i) {
    auto& t = g->tris[i];
    float a0 = std::min({t[0], t[3], t[6]}), a1 = std::max({t[0], t[3], t[6]});
    float b0 = std::min({t[2], t[5], t[8]}), b1 = std::max({t[2], t[5], t[8]});
    int i0 = std::clamp(int((a0 - g->x0) / g->cell), 0, g->nx - 1), i1 = std::clamp(int((a1 - g->x0) / g->cell), 0, g->nx - 1);
    int j0 = std::clamp(int((b0 - g->z0) / g->cell), 0, g->nz - 1), j1 = std::clamp(int((b1 - g->z0) / g->cell), 0, g->nz - 1);
    for (int j = j0; j <= j1; ++j)
      for (int k = i0; k <= i1; ++k) g->cells[size_t(j) * g->nx + k].push_back(i);
  }
  g->stamp.assign(g->tris.size(), 0);
  g_sight = g;
  RT_INFO("bot fight: line of sight from %zu triangles, %d x %d cells of %.0f m", g->tris.size(), g->nx, g->nz, g->cell);
}
static bool SegmentHitsTri(const float o[3], const float d[3], const std::array<float, 9>& t) {
  float e1[3] = {t[3] - t[0], t[4] - t[1], t[5] - t[2]}, e2[3] = {t[6] - t[0], t[7] - t[1], t[8] - t[2]};
  float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
  float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::fabs(det) < 1e-9f) return false;
  float inv = 1.0f / det, s[3] = {o[0] - t[0], o[1] - t[1], o[2] - t[2]};
  float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return false;
  float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
  float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return false;
  float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
  return tt > 0.02f && tt < 0.98f;  // not at either end (where the soldiers stand)
}
// From eye to chest (a and b are the soldiers' feet).
static bool SightReady() { return g_sight != nullptr; }
static bool InSight(float ax, float ay, float az, float bx, float by, float bz) {
  if (!g_sight) BuildSight();
  SightGrid& g = *g_sight;
  float o[3] = {ax, ay + 1.6f, az}, d[3] = {bx - ax, by + 1.2f - (ay + 1.6f), bz - az};
  float len = std::hypot(d[0], d[2]);
  ++g.visit;
  int steps = int(len / (g.cell * 0.25f)) + 1;
  int last = -1;
  for (int k = 0; k <= steps; ++k) {
    float f = float(k) / steps;
    int i = int((ax + d[0] * f - g.x0) / g.cell), j = int((az + d[2] * f - g.z0) / g.cell);
    if (i < 0 || j < 0 || i >= g.nx || j >= g.nz) continue;
    int c = j * g.nx + i;
    if (c == last) continue;
    last = c;
    for (uint32_t ti : g.cells[size_t(c)]) {
      if (g.stamp[ti] == g.visit) continue;
      g.stamp[ti] = g.visit;
      if (SegmentHitsTri(o, d, g.tris[ti])) return false;
    }
  }
  return true;
}
// The crates' interaction entities (a heap scan, far too slow for every
// frame: once every 2 s).
static const std::vector<uint32_t>& CrateEntities() {
  static std::vector<uint32_t> list;
  static int64_t at = 0;
  int64_t now = int64_t(GetTickCount64());
  if (now - at > 2000) at = now, list = Scan(0x82011680);
  return list;
}
// Interactions: a crate has two ServerInteractionEntity (vtable 82011680,
// place +D0/+D4/+D8); the enabled one (+4C 1) is what can be done there now.
// The first enabled at a place arms (attackers, B); the one enabled there
// after it defuses (defenders), and each keeps its role.
static std::map<uint32_t, int> g_role;  // 1 arm, 2 defuse
static bool Enabled(uint32_t ie) { return IsCommitted(ie + 0xDC) && Ld32(ie) == 0x82011680 && Ld32(ie + 0x4C) == 1; }
static void NoteInteractions() {
  const auto& all = CrateEntities();
  for (uint32_t ie : all) {
    if (!Enabled(ie) || g_role.count(ie)) continue;
    int role = 1;
    for (uint32_t o : all)
      if (o != ie && g_role.count(o) && g_role[o] == 1 &&
          std::hypot(LdF(o + 0xD0) - LdF(ie + 0xD0), LdF(o + 0xD8) - LdF(ie + 0xD8)) < 1.5f)
        role = 2;
    g_role[ie] = role;
    RT_INFO("bot brain: crate interaction %08X at %.1f %.1f: %s", ie, LdF(ie + 0xD0), LdF(ie + 0xD8),
            role == 1 ? "arms" : "defuses");
  }
}
static bool CanArmNow(uint32_t ie) { return Enabled(ie) && g_role.count(ie) && g_role[ie] == 1; }
static bool CanDefuseNow(uint32_t ie) { return Enabled(ie) && g_role.count(ie) && g_role[ie] == 2; }

// A bot's brain, run on its soldier's input (r4 of the input copy, vtable
// 8200DC48: +8 forward, +C strafe, +1C turn, +28 fire, +2C.. held times as
// half floats, +84 button bits, +90/+94 the aim's yaw and pitch).
//   Goal: attackers the nearest crate to arm; defenders the nearest armed
//   crate to defuse, else the nearest crate to guard (within 6 m); attackers
//   with nothing to arm guard an armed one. Arming and defusing: 822C4F48 on
//   the interaction with the soldier, once, from the input copy hook.
//   Route: the navigation mesh (RouteMesh), re-planned every 5 s; stuck (under
//   0.3 m in 1.5 s): jump and sidestep for 2 s, and twice near the same spot
//   the polygon ahead is blocked for routing.
//   Fighting: the nearest enemy (another team's soldier, +48) within 30 m and
//   in sight (InSight), looked for every 250 ms: standing, aimed at (the aim
//   object and +90/+94) and fired at (+28); X (reload) tapped after a fight.
//   Walking: the soldier moves relative to its aim object (vtable 820264A8),
//   which the aim setter turns toward the next corner.
static void BotBrain(Bot& b, uint32_t in) {
  auto set = [in](uint32_t off, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(in + off, u);
  };
  uint32_t so = b.soldier;
  if (!so || !IsCommitted(so + 0x400) || Ld32(so) != 0x820118D0) return;
  int64_t now = int64_t(GetTickCount64());
  float x = LdF(so + 0x200), y = LdF(so + 0x204), z = LdF(so + 0x208);
  NoteInteractions();
  UnblockExpired();
  uint32_t goal = 0;
  float best = 1e9f;
  bool guard = false;
  auto consider = [&](uint32_t ie, bool as_guard) {
    float d = std::hypot(LdF(ie + 0xD0) - x, LdF(ie + 0xD8) - z);
    if (d < best) best = d, goal = ie, guard = as_guard;
  };
  for (uint32_t ie : CrateEntities())
    if (b.team == 1 ? CanArmNow(ie) : CanDefuseNow(ie)) consider(ie, false);
  if (!goal)
    for (uint32_t ie : CrateEntities())
      if (b.team == 1 ? CanDefuseNow(ie) : CanArmNow(ie)) consider(ie, true);
  float tx = x, ty = y, tz = z;
  if (goal) tx = LdF(goal + 0xD0), ty = LdF(goal + 0xD4), tz = LdF(goal + 0xD8);
  if (goal && now - b.planned > 5000) {
    b.planned = now;
    float from[3] = {x, y, z}, to[3] = {tx, ty, tz};
    b.route = RouteMesh(from, to, &b.nav);
    b.step = 0;
  }
  while (b.step < b.route.size() && std::hypot(b.route[b.step][0] - x, b.route[b.step][2] - z) < 1.5f) ++b.step;
  float px = tx, pz = tz;
  if (b.step < b.route.size()) px = b.route[b.step][0], pz = b.route[b.step][2];
  bool arrived = !goal || (guard ? best < 6.0f : best < 1.0f && b.step >= b.route.size());
  // Out of a spot it keeps getting stuck in: to the detour first.
  if (now < b.detour_until) {
    if (std::hypot(b.detour[0] - x, b.detour[2] - z) < 1.5f) b.detour_until = 0, b.planned = 0;
    else px = b.detour[0], pz = b.detour[2], arrived = false;
  }
  float forward = arrived ? 0.0f : 1.0f, strafe = 0.0f;

  // Fighting.
  static std::vector<uint32_t> soldiers;
  static int64_t soldiers_at = 0;
  if (now - soldiers_at > 1000) soldiers_at = now, soldiers = Scan(0x820118D0);
  if (b.enemy && (!IsCommitted(b.enemy + 0x20C) || Ld32(b.enemy) != 0x820118D0 || LdF(b.enemy + 0x3C) <= 0)) b.enemy = 0;
  if (now - b.looked_at > 250) {
    b.looked_at = now, b.enemy = 0, b.edist = 30.0f;
    for (uint32_t e : soldiers) {
      if (e == so || !IsCommitted(e + 0x20C) || Ld32(e) != 0x820118D0) continue;
      uint32_t team = Ld32(e + 0x48);
      if ((team != 1 && team != 2) || team == b.team || LdF(e + 0x3C) <= 0) continue;
      float ex = LdF(e + 0x200), ey = LdF(e + 0x204), ez = LdF(e + 0x208);
      if (!std::isfinite(ex) || !std::isfinite(ez) || std::fabs(ex) + std::fabs(ez) < 1.0f) continue;
      float d = std::hypot(ex - x, ez - z);
      if (d >= b.edist || std::fabs(ey - y) > 15.0f || !InSight(x, y, z, ex, ey, ez)) continue;
      b.enemy = e, b.edist = d;
    }
  }
  if (b.enemy) b.ex = LdF(b.enemy + 0x200), b.ey = LdF(b.enemy + 0x204), b.ez = LdF(b.enemy + 0x208);
  if (b.enemy != b.engaged) {
    b.engaged = b.enemy, b.engaged_at = now;
    if (b.enemy)
      RT_INFO("bot %d: enemy %08X (team %u, health %.0f) at %.1f m", b.index, b.enemy, Ld32(b.enemy + 0x48),
              LdF(b.enemy + 0x3C), b.edist);
    else if (b.fired)
      b.fired = false, b.reload_until = now + 300;  // the fight is over: reload
  }

  // Stuck (not while standing on purpose).
  if (std::hypot(x - b.sx, z - b.sz) > 0.3f || b.enemy || arrived) b.sx = x, b.sz = z, b.since = now;
  if (now - b.since > 1500 && now > b.unstick_until) {
    b.side = -b.side;
    b.unstick_until = now + 2000, b.jump_until = now + 300, b.since = now;
    RT_INFO("bot %d: stuck at %.1f %.1f: jump and sidestep %s", b.index, x, z, b.side > 0 ? "right" : "left");
    // Stuck twice near the same spot on a route: the polygon ahead (1.5 m
    // toward the next corner) is somewhere it can't get to.
    bool same_place = std::hypot(x - b.last_sx, z - b.last_sz) < 5.0f;
    b.stucks = same_place ? b.stucks + 1 : 1;
    if (same_place && b.step < b.route.size() && g_navquery) {
      float dx = b.route[b.step][0] - x, dz = b.route[b.step][2] - z, l = std::max(std::hypot(dx, dz), 0.001f);
      float ahead[3] = {x + 1.5f * dx / l, y, z + 1.5f * dz / l};
      BlockPoly(ahead);
      b.planned = 0;
    }
    // Stuck three times around here: a detour to a reachable place on the
    // mesh's main piece (a random one) 4 to 10 m away, then the route again;
    // from the same place again, wider each time (20, 30, 40 m).
    if (b.stucks >= 3 && g_navquery) {
      if (std::hypot(x - b.detour_from[0], z - b.detour_from[1]) < 8.0f) ++b.detours;
      else b.detours = 0, b.detour_from[0] = x, b.detour_from[1] = z;
      const float radius = 10.0f * float(1 + std::min(b.detours, 3));
      dtQueryFilter filter;
      const float ext[3] = {2, 4, 2};
      float at[3] = {x, y, z}, onmesh[3], pt[3];
      dtPolyRef start = 0, ref = 0;
      g_navquery->findNearestPoly(at, ext, &filter, &start, onmesh);
      for (int tries = 0; start && tries < 12; ++tries) {
        if (dtStatusFailed(g_navquery->findRandomPointAroundCircle(start, onmesh, radius, &filter,
                                                                   [] { return float(rand()) / RAND_MAX; }, &ref, pt)))
          break;
        float d = std::hypot(pt[0] - x, pt[2] - z);
        if (d < std::max(4.0f, radius * 0.4f) || !InMainPiece(ref)) continue;
        memcpy(b.detour, pt, sizeof(pt));
        b.detour_until = now + int64_t(radius * 600.0f), b.stucks = 0;
        RT_INFO("bot %d: stuck here again; out by %.1f %.1f first", b.index, pt[0], pt[2]);
        break;
      }
    }
    b.last_sx = x, b.last_sz = z;
  }
  if (now < b.unstick_until && !b.enemy) forward = 0.3f, strafe = float(b.side);

  // Arming or defusing at the crate.
  if (goal && !guard && best < 2.0f && !b.used.count(goal)) b.use_entity = goal;

  // The pad.
  bool jump = now < b.jump_until, reload = now < b.reload_until;
  uint32_t extra_bits = 0;
  // BF_BOT_SPRINT_TEST=1 (testing): while walking, each button bit of +84 in
  // turn (4 s each; A's and X's known) with the left stick click's held time
  // (+58) at 1.0, and how far it went: which bit sprints.
  static const bool sprint_test = getenv("BF_BOT_SPRINT_TEST") != nullptr;
  if (sprint_test && b.index == 0 && forward > 0.9f && !b.enemy) {
    static int64_t seg_at = 0;
    static int seg = -1;
    static float seg_x = 0, seg_z = 0;
    if (now - seg_at > 4000) {
      static const int kNames[4] = {-1, 23, 24, 25};
      if (seg >= 0) RT_INFO("bot sprint test: bit %d: %.2f m in 4 s", kNames[seg], std::hypot(x - seg_x, z - seg_z));
      seg = (seg + 1) % 4, seg_at = now, seg_x = x, seg_z = z;
    }
    static const int kBits[4] = {-1, 23, 24, 25};  // none, then the candidates around EIASprint (24)
    if (kBits[seg] >= 0) extra_bits = 1u << kBits[seg];
    St32(in + 0x58, 0x3C003C00u);
  }
  // Crouching (bit 16, a toggle: heights of 1.38 fell to 1.00, BF_BOT_POSE_TEST)
  // while fighting an enemy over 8 m off; standing again to move on.
  bool want_crouch = b.enemy && b.edist > 8.0f;
  bool toggle = want_crouch != b.crouched && now - b.pose_at > 800;
  if (toggle) b.crouched = want_crouch, b.pose_at = now;
  if (now - b.pose_at < 200) extra_bits |= 1u << 16;
  if (b.crouched && !b.enemy) forward = 0;  // until it has stood up
  // Buttons are bits of +84: 13 jump (A), 16 crouch, 21 reload (X), 23 sprint
  // (the left stick's click: 21 m in 4 s against 15 walking, BF_BOT_SPRINT_TEST).
  // Sprinting on the way when the goal is over 20 m off.
  bool sprint = forward > 0.9f && !b.enemy && best > 20.0f && now >= b.unstick_until && !sprint_test;
  St32(in + 0x84, (jump ? 0x2000u : 0u) | (reload ? 0x200000u : 0u) | (sprint ? 0x800000u : 0u) | extra_bits);
  St32(in + 0x44, jump ? 0x00003C00u : 0u);
  St32(in + 0x54, reload ? 0x00003C00u : 0u);
  // A grenade every 15 s at an enemy 12 to 35 m off. A fake player's throw
  // makes no grenade (no button tried did: BF_BOT_GRENADE_TEST), so it is
  // the server's: 2.5 s later everyone within 6 m of where the enemy stood
  // takes up to 60 damage (falling off with distance), credited to the bot.
  // No explosion is drawn.
  if (b.enemy && b.edist > 12.0f && b.edist < 35.0f && now - b.grenade_at > 15000 && now - b.engaged_at > 600) {
    b.grenade_at = now, b.grenade_live = true;
    b.grenade_pos[0] = b.ex, b.grenade_pos[1] = b.ey, b.grenade_pos[2] = b.ez;
    RT_INFO("bot %d: grenade at %08X, %.1f m", b.index, b.enemy, b.edist);
  }
  if (b.enemy) {
    forward = 0, strafe = 0;
    // The shot's direction: the record's aim (+90 yaw: atan2(dx, dz) + pi, as
    // the aim object's +134 negated; +94 pitch, up +), eye to chest.
    // BF_BOT_YAW_FLIP=1 (testing): the old half-turn (bullets went behind?).
    static const bool flip = getenv("BF_BOT_YAW_FLIP") != nullptr;
    float yaw = std::atan2(b.ex - x, b.ez - z) + (flip ? 3.14159265f : 0.0f);
    if (yaw > 3.14159265f) yaw -= 6.2831853f;
    set(0x90, yaw);
    set(0x94, std::atan2(b.ey + 1.2f - (y + 1.6f), std::max(b.edist, 0.5f)));
    // Not through a teammate: none within 1 m of the line of fire.
    bool clear = true;
    float lx = b.ex - x, lz = b.ez - z, ll = std::max(lx * lx + lz * lz, 0.01f);
    for (uint32_t o : soldiers) {
      if (o == so || o == b.enemy || !IsCommitted(o + 0x20C) || Ld32(o) != 0x820118D0 || Ld32(o + 0x48) != b.team ||
          LdF(o + 0x3C) <= 0)
        continue;
      float ox = LdF(o + 0x200) - x, oz = LdF(o + 0x208) - z, t = (ox * lx + oz * lz) / ll;
      if (t > 0 && t < 1 && std::hypot(ox - t * lx, oz - t * lz) < 1.0f) clear = false;
    }
    if (now - b.engaged_at > 400 && clear) {
      set(0x28, 1.0f), St32(in + 0x3C, 0x3C000000u | (Ld32(in + 0x3C) & 0xFFFFu));
      b.fired = true;
      // Its own bullets hit (12 a hit through 822D1AA0, credited to it) now that
      // its aim has the yaw and pitch toward the target; they didn't while the
      // gun followed a body that never turned. BF_BOT_ROLL=1: rolled hits as
      // before (one every 150 ms, 90 % at 5 m down to 15 % from 40 m).
      static const bool roll = getenv("BF_BOT_ROLL") != nullptr;
      if (roll && now >= b.next_shot) {
        b.next_shot = now + 150;
        float chance = std::clamp(1.05f - b.edist / 40.0f, 0.15f, 0.9f);
        if (float(rand()) / RAND_MAX < chance) b.hit = b.enemy;
      }
    }
  }
  // BF_BOT_POSE_TEST=1 (testing): bot 0 stands; each crouch candidate (bits
  // 15, 16, 14, 17 of +84) pressed once, then again 4 s later, 4 s each: the
  // floats of 0.5-2 m in the soldier and what it points at that moved by over
  // 0.2 (a height that drops when crouched).
  static const bool pose_test = getenv("BF_BOT_POSE_TEST") != nullptr;
  if (pose_test && b.index == 0) {
    forward = 0, strafe = 0;
    static int64_t t0 = 0;
    static int step = -1;
    static std::map<uint32_t, float> before;
    static const int kCands[] = {15, 15, 16, 16, 14, 14, 17, 17};
    auto heights = [&](std::map<uint32_t, float>& out) {
      out.clear();
      std::vector<uint32_t> objs{so};
      for (uint32_t k = 0; k < 0x800; k += 4)
        if (Ptr(Ld32(so + k)) && IsCommitted(Ld32(so + k) + 0x3FC)) objs.push_back(Ld32(so + k));
      for (size_t n = 0; n < objs.size(); ++n)
        for (uint32_t k = 0; k < (n ? 0x400u : 0x800u); k += 4) {
          float f = LdF(objs[n] + k);
          if (std::isfinite(f) && f > 0.5f && f < 2.0f) out[uint32_t(n) << 16 | k] = f;
        }
    };
    if (!t0 || now - t0 > 4000) {
      if (step >= 0) {
        std::map<uint32_t, float> after;
        heights(after);
        std::string d;
        for (auto& [key, f] : before) {
          auto it = after.find(key);
          if (it != after.end() && std::fabs(it->second - f) > 0.2f) {
            char buf[48];
            snprintf(buf, sizeof(buf), " obj%u+%X:%.2f>%.2f", key >> 16, key & 0xFFFF, f, it->second);
            d += buf;
          }
        }
        RT_INFO("bot pose test: bit %d (press %d): heights moved:%s", kCands[step], step % 2 + 1, d.empty() ? " none" : d.substr(0, 500).c_str());
      }
      t0 = now, step = (step + 1) % 8;
      heights(before);
    }
    if (now - t0 < 300) St32(in + 0x84, Ld32(in + 0x84) | (1u << kCands[step]));
  }
  // BF_BOT_BULLET_TEST=1 (testing): while bot 0 fights, every 2 s: its body's
  // facing (forward row +320/+328), its aim's (-(+134)), the wanted one, and
  // the classes with Bullet or Projectile in their name that exist (once), with
  // each one's float triples of 50-2000 in length (a velocity) near the bot.
  static const bool bullet_test = getenv("BF_BOT_BULLET_TEST") != nullptr;
  if (bullet_test && b.index == 0 && b.enemy) {
    static int64_t last = 0;
    static std::vector<uint32_t> vts;
    if (vts.empty()) {
      std::set<uint32_t> seen;
      for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
        if (page >= 0x80000000 && page < 0xA0000000) continue;
        if (!IsCommitted(uint32_t(page))) continue;
        for (uint32_t i = 0; i < 0x1000; i += 4) {
          uint32_t v = Ld32(uint32_t(page) + i);
          if (v < 0x82000000 || v >= 0x82100000 || (v & 3) || !seen.insert(v).second) continue;
          std::string n = VtName(v);
          if (n.find("Bullet") != std::string::npos || n.find("Projectile") != std::string::npos)
            vts.push_back(v), RT_INFO("bot bullet test: class %08X %s", v, n.c_str());
        }
      }
      if (vts.empty()) vts.push_back(0);
    }
    if (now - last > 2000) {
      last = now;
      float bodyh = std::atan2(LdF(so + 0x320), LdF(so + 0x328));
      std::string seen;
      for (uint32_t v : vts) {
        if (!v) continue;
        for (uint32_t o : Scan(v)) {
          for (uint32_t k = 0; k + 8 < 0x200 && IsCommitted(o + k + 8); k += 4) {
            float vx = LdF(o + k), vy = LdF(o + k + 4), vz = LdF(o + k + 8), sp = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (!std::isfinite(sp) || sp < 50 || sp > 2000 || std::fabs(vx) + std::fabs(vz) < 1) continue;
            char buf[96];
            snprintf(buf, sizeof(buf), " [%s %08X +%X dir %.2f speed %.0f]", VtName(v).c_str(), o, k, std::atan2(vx, vz), sp);
            seen += buf;
            break;
          }
          if (seen.size() > 600) break;
        }
      }
      RT_INFO("bot bullet test: to the enemy %.2f, body %.2f, aim %.2f;%s", std::atan2(b.ex - x, b.ez - z), bodyh,
              b.aim_obj ? -LdF(b.aim_obj + 0x134) : 0.0f, seen.empty() ? " no bullets seen" : seen.c_str());
    }
  }
  // BF_BOT_GRENADE_TEST=1 (testing): bot 0 stands; every 8 s one candidate
  // weapon slot button (bits 19, 18, 20, 29, 30, 31 of +84, bits 0-1 of +88)
  // for 0.2 s, fire at 1.2 s for 0.4 s, then at 4 s: how many objects of the
  // projectile and explosion classes there are, and what of the soldier changed.
  static const bool grenade_test = getenv("BF_BOT_GRENADE_TEST") != nullptr;
  if (grenade_test && b.index == 0) {
    forward = 0, strafe = 0;
    static int64_t t0 = 0;
    static int phase = -1;
    static std::vector<uint32_t> was;
    static std::vector<uint32_t> vts;  // the projectile/explosion/grenade classes' vtables
    if (vts.empty()) {
      std::set<uint32_t> seen;
      for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
        if (page >= 0x80000000 && page < 0xA0000000) continue;
        if (!IsCommitted(uint32_t(page))) continue;
        for (uint32_t i = 0; i < 0x1000; i += 4) {
          uint32_t v = Ld32(uint32_t(page) + i);
          if (v < 0x82000000 || v >= 0x82100000 || (v & 3) || !seen.insert(v).second) continue;
          std::string n = VtName(v);
          if (n.find("Projectile") != std::string::npos || n.find("Grenade") != std::string::npos ||
              n.find("Explosion") != std::string::npos)
            vts.push_back(v), RT_INFO("bot grenade test: class %08X %s", v, n.c_str());
        }
      }
    }
    static const int kCands[] = {10, 9, 11, 12, 10, 9, 11, 12};  // EIAAltFire (10) and its neighbours
    if (!t0 || now - t0 > 8000) {
      t0 = now, phase = (phase + 1) % 8;
      was.clear();
      for (uint32_t k = 0; k < 0x800; k += 4) was.push_back(Ld32(so + k));
      RT_INFO("bot grenade test: trying bit %d", kCands[phase]);
    }
    int64_t t = now - t0;
    int bit = kCands[phase];
    if (t < 1000) {
      if (bit < 32) St32(in + 0x84, Ld32(in + 0x84) | (1u << bit));
      else St32(in + 0x88, Ld32(in + 0x88) | (1u << (bit - 32)));
    }
    static int reported = -1;
    if (t >= 4000 && reported != phase) {
      reported = phase;
      std::string counts, d;
      for (uint32_t v : vts) counts += " " + VtName(v) + "=" + std::to_string(Scan(v).size());
      for (uint32_t k = 0; k < 0x800; k += 4)
        if (Ld32(so + k) != was[k / 4] && (Ptr(Ld32(so + k)) || Ptr(was[k / 4]))) {
          char buf[48];
          snprintf(buf, sizeof(buf), " +%X:%08X>%08X", k, was[k / 4], Ld32(so + k));
          d += buf;
        }
      RT_INFO("bot grenade test: bit %d: objects%s; soldier pointers changed:%s", bit, counts.c_str(), d.substr(0, 600).c_str());
    }
  }
  set(0x8, forward);
  set(0xC, strafe);
  set(0x1C, 0.0f);

  // The aim object (vtable 820264A8, two levels of pointers from the soldier).
  if (b.aim_for != so) {
    b.aim_for = so, b.aim_obj = 0;
    std::function<void(uint32_t, int)> find = [&](uint32_t o, int depth) {
      for (uint32_t off = 0; off < 0x800 && !b.aim_obj && IsCommitted(o + off); off += 4) {
        uint32_t q = Ld32(o + off);
        if (q < 0x40000000 || q >= 0xC0000000 || (q & 3) || !IsCommitted(q)) continue;
        if (Ld32(q) == 0x820264A8) b.aim_obj = q;
        else if (depth < 2 && IsCommitted(q + 0x7FC)) find(q, depth + 1);
      }
    };
    find(so, 0);
    RT_INFO("bot %d: aim object of soldier %08X: %08X", b.index, so, b.aim_obj);
  }
  // Facing the enemy or the next point (the aim's convention is half a turn
  // from atan2(dx, dz)).
  float want = b.enemy ? std::atan2(b.ex - x, b.ez - z) : std::atan2(px - x, pz - z);
  b.want_heading = want + 3.14159265f, b.steer = b.aim_obj != 0;
  b.face = want, b.facing = true;
  // Looking up or down: at the enemy's chest from the eye, else level.
  b.want_pitch = b.enemy ? std::atan2(b.ey + 1.2f - (y + 1.6f), std::max(b.edist, 0.5f)) : 0.0f;
  if (now - b.logged > 3000) {
    b.logged = now;
    RT_INFO("bot %d (team %u): at %.1f %.1f %.1f, health %.0f; %s %.1f m%s", b.index, b.team, x, y, z, LdF(so + 0x3C),
            !goal ? "no goal" : guard ? "guarding a crate" : b.team == 1 ? "to arm a crate" : "to defuse a crate", best,
            b.enemy ? ", fighting" : "");
  }
}

// The aim's writer (BF_BOT_WATCH_AIM: 824ADEE8 <- 822BDE38 <- 822ACB88 <-
// 822C6170 <- 82275A38): its arguments, to steer the bot through it.
// Its r4 record: +0/+8 a direction (minus sin, cos of +34), +34 the value it
// stores at +134 (minus the soldier's heading atan2(fx, fz)). For the bot's
// aim object, the heading its brain wants goes in there first.
uint32_t g_bot_aim_obj = 0;
float g_bot_want_heading = 0;
bool g_bot_steer = false;
std::atomic<int> g_bot_setter_calls{0};  // the setter steering the bot (counted for BF_BOT_TRACE_AT)
// BF_BOT_AIM_CHAIN=1 (testing): the aim's update chain (822C6170 -> 822ACB88 ->
// 822BDE38 -> 824ADEE8): every 5 s, per function, the distinct first
// arguments it ran with and how often, the bot's soldier and aim object
// marked: does it run for the bot at all?
static const bool g_aim_chain = getenv("BF_BOT_AIM_CHAIN") != nullptr;
static std::mutex g_aim_chain_mutex;
static std::map<std::string, std::map<uint32_t, int>> g_aim_chain_calls;
static void AimChainNote(const char* fn, uint32_t r3) {
  std::lock_guard l(g_aim_chain_mutex);
  g_aim_chain_calls[fn][r3]++;
  static int64_t last = 0;
  int64_t now = int64_t(GetTickCount64());
  if (now - last < 5000) return;
  last = now;
  for (auto& [name, calls] : g_aim_chain_calls) {
    std::string out;
    for (auto& [obj, n] : calls) {
      char buf[64];
      snprintf(buf, sizeof(buf), " %08X(vt %08X)x%d%s", obj, IsCommitted(obj) ? Ld32(obj) : 0, n,
               obj == g_bot_aim_obj ? "=BOT AIM" : obj == g_bot_soldier_for_trails ? "=BOT SOLDIER" : "");
      out += buf;
    }
    RT_INFO("bot aim chain %s:%s", name.c_str(), out.substr(0, 700).c_str());
  }
  g_aim_chain_calls.clear();
}
#define AIM_CHAIN(addr)                                    \
  extern "C" PPC_FUNC(__imp__sub_##addr);                  \
  PPC_FUNC(sub_##addr) {                                   \
    if (g_aim_chain) AimChainNote(#addr, ctx.r3.u32);      \
    __imp__sub_##addr(ctx, base);                          \
  }
AIM_CHAIN(822C6170)
AIM_CHAIN(822ACB88)
AIM_CHAIN(822BDE38)
extern "C" PPC_FUNC(__imp__sub_824ADEE8);
PPC_FUNC(sub_824ADEE8) {
  if (g_aim_chain) AimChainNote("824ADEE8", ctx.r3.u32);
  static const bool trace = getenv("BF_BOT_AIM_TRACE") != nullptr;
  static int n = 0;
  uint32_t r3 = ctx.r3.u32;
  const Bot* steered = nullptr;
  for (const Bot& b : g_bots)
    if (b.steer && b.aim_obj == r3) steered = &b;
  if (steered && IsCommitted(ctx.r4.u32 + 0x38)) {
    auto put = [](uint32_t a, float f) {
      uint32_t u;
      memcpy(&u, &f, 4);
      St32(a, u);
    };
    float h = steered->want_heading;
    put(ctx.r4.u32 + 0x38, steered->want_pitch);  // pitch, up + (BF_BOT_PITCH_TRACE: -1.4 looking down)
    put(ctx.r4.u32 + 0x34, -h);
    put(ctx.r4.u32 + 0x0, std::sin(h));
    put(ctx.r4.u32 + 0x8, std::cos(h));
    ++g_bot_setter_calls;
  }
  // BF_BOT_PITCH_TRACE=1 (testing): a player's (not a bot's) aim record once
  // a second: the three vectors (+0..+2C) and +30..+40, to find its pitch.
  static const bool pitch_trace = getenv("BF_BOT_PITCH_TRACE") != nullptr;
  if (pitch_trace && !steered && IsCommitted(ctx.r4.u32 + 0x48)) {
    static int64_t last = 0;
    int64_t t = int64_t(GetTickCount64());
    if (t - last > 300) {
      last = t;
      RT_INFO("bot pitch trace: aim %08X: +38 %.3f +3C %.3f (yaw +34 %.3f)", r3, LdF(ctx.r4.u32 + 0x38),
              LdF(ctx.r4.u32 + 0x3C), LdF(ctx.r4.u32 + 0x34));
    }
  }
  float before = IsCommitted(r3 + 0x134) ? LdF(r3 + 0x134) : 0;
  PPCContext in = ctx;
  __imp__sub_824ADEE8(ctx, base);
  if (g_bot_steer && r3 == g_bot_aim_obj) {
    static int64_t last = 0;
    int64_t now = int64_t(GetTickCount64());
    if (now - last > 3000) {
      last = now;
      uint32_t so = g_bot_soldier_for_trails;
      RT_INFO("bot aim check: wanted heading %.2f, stored +134 %.2f (minus it %.2f), soldier heading %.2f", g_bot_want_heading,
              LdF(r3 + 0x134), -LdF(r3 + 0x134), so ? std::atan2(LdF(so + 0x320), LdF(so + 0x328)) : 0.0f);
    }
  }
  if (trace && n++ < 6) {
    RT_INFO("bot aim set: r3=%08X(vt %08X) r4=%08X r5=%08X r6=%08X f1=%.3f f2=%.3f f3=%.3f; +134 %.3f -> %.3f", r3,
            IsCommitted(r3) ? Ld32(r3) : 0, in.r4.u32, in.r5.u32, in.r6.u32, in.f1.f64, in.f2.f64, in.f3.f64, before,
            IsCommitted(r3 + 0x134) ? LdF(r3 + 0x134) : 0);
    std::string w;
    for (uint32_t k = 0; k < 0x80 && IsCommitted(in.r4.u32 + k); k += 4) {
      float fv = LdF(in.r4.u32 + k);
      char buf[32];
      if (std::isfinite(fv) && std::fabs(fv) > 1e-3f && std::fabs(fv) < 1e4f) snprintf(buf, sizeof(buf), " +%X:%.3f", k, fv);
      else snprintf(buf, sizeof(buf), " +%X:%08X", k, Ld32(in.r4.u32 + k));
      w += buf;
    }
    RT_INFO("bot aim set: r4 record:%s", w.c_str());
  }
}

// The soldier update (822B92C8 <- 822C2C38 <- 822ACCD8 <- 82275A38, BF_BOT_WATCH_INPUT)
// fills a soldier's input state from its player's received input through
// 821BF2A8, which writes all its analog fields: for the fake soldier, the
// bot's values go in right after that, before the soldier uses them.
extern "C" PPC_FUNC(__imp__sub_821BF2A8);
namespace rt {
std::mutex g_record_yaw_mutex;
std::map<uint32_t, float> g_record_yaw;  // received input record -> its aim yaw (+90)
}  // namespace rt
extern "C" PPC_FUNC(__imp__sub_822C4F48);
extern "C" PPC_FUNC(__imp__sub_822D1AA0);
static void DamageSoldier(PPCContext& ctx, uint8_t* base, uint32_t soldier, float amount, uint32_t attacker);
PPC_FUNC(sub_821BF2A8) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  if (IsCommitted(r4) && Ld32(r4) == 0x8200DC48) {
    std::lock_guard l(g_record_yaw_mutex);
    g_record_yaw[r4] = LdF(r4 + 0x90);
  }
  // The source is the player's received input (vtable 8200DC48): for a bot's
  // soldier, its brain fills it before the copy; then the interaction it
  // asked for (arming, defusing: 822C4F48 needs the game's context).
  if (IsCommitted(r4) && Ld32(r4) == 0x8200DC48)
    for (Bot& b : g_bots) {
      if (!b.input || r3 != b.input) continue;
      b.record = r4;
      BotBrain(b, r4);
      if (uint32_t target = b.hit) {
        b.hit = 0;
        DamageSoldier(ctx, base, target, 9.0f, b.spawner);
      }
      if (b.grenade_live && int64_t(GetTickCount64()) - b.grenade_at > 2500) {
        b.grenade_live = false;
        for (uint32_t o : Scan(0x820118D0)) {
          if (!IsCommitted(o + 0x400) || LdF(o + 0x3C) <= 0) continue;
          uint32_t t = Ld32(o + 0x48);
          if ((t != 1 && t != 2) || t == b.team) continue;
          float d = std::hypot(LdF(o + 0x200) - b.grenade_pos[0], LdF(o + 0x208) - b.grenade_pos[2]);
          if (d < 6.0f && std::fabs(LdF(o + 0x204) - b.grenade_pos[1]) < 3.0f)
            DamageSoldier(ctx, base, o, 60.0f * (1.0f - d / 6.0f) + 10.0f, b.spawner);
        }
      }
      if (uint32_t ie = b.use_entity) {
        b.use_entity = 0;
        b.used.insert(ie);
        PPCContext c = ctx;
        c.r3.u64 = ie, c.r4.u64 = b.soldier, c.r5.u64 = 0, c.r6.u64 = 0;
        __imp__sub_822C4F48(c, base);
        RT_INFO("bot %d: %s crate %08X: 822C4F48 -> %u", b.index, CanDefuseNow(ie) || b.team == 2 ? "defusing" : "arming",
                ie, c.r3.u32);
        // BF_BOT_WATCH_HEALTH=1 (testing): the first bot to arm a crate stays by
        // it: who writes its health (+3C) when the charge goes off (the server's
        // damage path).
        static bool watching = false;
        if (!watching && b.team == 1 && getenv("BF_BOT_WATCH_HEALTH")) {
          watching = true;
          RT_INFO("bot %d: watching its health (soldier %08X +3C)", b.index, b.soldier);
          StartWriteWatch(b.soldier + 0x3C, 4, 0);
        }
      }
      break;
    }
  __imp__sub_821BF2A8(ctx, base);
  // BF_BOT_INPUT_MAP=1 (testing): a player's received input (r4) whenever its
  // first 0x60 bytes change, the non-zero words shown: which field is which.
  static const bool map_input = getenv("BF_BOT_INPUT_MAP") != nullptr;
  if (map_input && IsCommitted(r4) && Ld32(r4) == 0x8200DC48) {
    static std::map<uint32_t, std::string> last;
    std::string w;
    for (uint32_t k = 1; k < 64; ++k) {  // the first 0x100 bytes
      uint32_t u = Ld32(r4 + k * 4);
      if (!u) continue;
      float f;
      memcpy(&f, &u, 4);
      char buf[40];
      if (std::isfinite(f) && std::fabs(f) > 0.001f && std::fabs(f) < 1000.0f) snprintf(buf, sizeof(buf), " +%X:%.2f", k * 4, f);
      else snprintf(buf, sizeof(buf), " +%X:%08X", k * 4, u);
      w += buf;
    }
    if (w != last[r4]) RT_INFO("bot input map %08X:%s", r4, w.c_str());
    last[r4] = w;
  }
}
// Kits (SoldierEntityData, vtable 8206E890: KIT_ASSAULT...): one of each
// class per side, told apart by their SoldierAsset (vtable 8207B2FC, its kit
// at +3C, its name at +C: Objects/Soldiers/US/Assault, .../RU/Recon). A
// team's side: its soldiers' kits (a soldier's +14). Each bot gets its team's
// side and a class in turn (assault, support, demolition, recon, specialist);
// the weapon choice (+1D8) is left to the kit (0) unless it is the host's kit.
struct KitInfo { uint32_t kit; std::string side, cls; };
static std::vector<KitInfo> g_kits;
static void FindKits() {
  if (!g_kits.empty()) return;
  for (uint32_t a : Scan(0x8207B2FC)) {
    uint32_t k = Ld32(a + 0x3C), name = Ld32(a + 0xC);
    if (!Ptr(k) || Ld32(k) != 0x8206E890 || !Ptr(name)) continue;
    std::string n = std::string(G<char>(name)).substr(0, 80);
    size_t p = n.find("/Soldiers/");
    if (p == std::string::npos) continue;
    std::string rest = n.substr(p + 10);
    size_t slash = rest.find('/');
    if (slash == std::string::npos) continue;
    g_kits.push_back({k, rest.substr(0, slash), rest.substr(slash + 1)});
  }
  for (auto& k : g_kits) RT_INFO("bot kits: %08X %s %s", k.kit, k.side.c_str(), k.cls.c_str());
}
static std::string KitSide(uint32_t kit) {
  for (auto& k : g_kits)
    if (k.kit == kit) return k.side;
  return {};
}
static uint32_t BotKit(Bot& b, uint32_t host_kit) {
  FindKits();
  // The bot's team's side: from a soldier of that team, else the host's kit's
  // side for the host's team and the other side for the other.
  std::string side;
  for (uint32_t so : Scan(0x820118D0))
    if (Ld32(so + 0x48) == b.team && Ptr(Ld32(so + 0x14)) && !(side = KitSide(Ld32(so + 0x14))).empty()) break;
  if (side.empty()) {
    std::string host_side = KitSide(host_kit);
    uint32_t host_team = 0;
    for (uint32_t o : Scan(0x82019490))
      if (Ld32(o + 0x1D4) == host_kit && !BotWithSpawner(o)) host_team = Ld32(o + 0x30);
    for (auto& k : g_kits)
      if ((host_team == b.team) == (k.side == host_side)) side = k.side;
  }
  static const char* kClasses[] = {"Assault", "Support", "Demolition", "Recon", "Specialist"};
  if (!b.kit_class) b.kit_class = kClasses[b.index % 5];
  for (auto& k : g_kits)
    if (k.side == side && k.cls == b.kit_class) {
      static std::map<int, uint32_t> said;
      if (said[b.index] != k.kit) said[b.index] = k.kit, RT_INFO("bot %d: kit %s/%s", b.index, side.c_str(), b.kit_class);
      return k.kit;
    }
  return host_kit;
}
// Damage to a soldier, as the server applies a hit (seen with BF_BOT_WATCH_HEALTH
// and BF_BOT_DAMAGE_TRACE: its health +3C set by 822B29A8 on its health part
// +34, from 822D1AA0, which also kills through 822CC5D8): 822D1AA0(soldier,
// damage record, giver). The record: +28 the amount, +2C and +3B read too
// (zero here); the giver: a pointer to a word pointing at the attacking
// player + 0xC4 (82328920), the player being its spawn object (vtable
// 82019490: BF_BOT_DAMAGE_TRACE on a crate's blast), so the kill is the bot's.
static void DamageSoldier(PPCContext& ctx, uint8_t* base, uint32_t soldier, float amount, uint32_t attacker) {
  if (!IsCommitted(soldier + 0x400) || Ld32(soldier) != 0x820118D0 || LdF(soldier + 0x3C) <= 0) return;
  static uint32_t record = 0, giver = 0;
  if (!record) record = g_system_heap->Alloc(0x100, 16), giver = g_system_heap->Alloc(0x10, 16);
  memset(G<uint8_t>(record), 0, 0x100);
  St32(giver, attacker ? giver + 4 : 0);
  St32(giver + 4, attacker ? attacker + 0xC4 : 0);
  uint32_t u;
  memcpy(&u, &amount, 4);
  St32(record + 0x28, u);
  float before = LdF(soldier + 0x3C);
  std::vector<uint32_t> was;
  if (attacker && before <= amount)  // a kill: what of the attacker changes (its score)
    for (uint32_t k = 0; k < 0x200; k += 4) was.push_back(Ld32(attacker + k));
  PPCContext c = ctx;
  c.r3.u64 = soldier, c.r4.u64 = record, c.r5.u64 = giver;
  __imp__sub_822D1AA0(c, base);
  if (!was.empty()) {
    std::string d;
    for (uint32_t k = 0; k < 0x200; k += 4)
      if (Ld32(attacker + k) != was[k / 4]) {
        char buf[40];
        snprintf(buf, sizeof(buf), " +%X:%X>%X", k, was[k / 4], Ld32(attacker + k));
        d += buf;
      }
    RT_INFO("bot fight: kill by spawn object %08X; it changed:%s", attacker, d.empty() ? " nothing" : d.c_str());
  }
  static int logged = 0;
  if (logged++ < 20 || LdF(soldier + 0x3C) <= 0)
    RT_INFO("bot fight: hit soldier %08X: health %.0f -> %.0f", soldier, before, LdF(soldier + 0x3C));
}
// Deploying the bots (once the host's soldier has been in the level 5 s) and
// again 8 s after each death (its soldier gone, or health +3C at 0): the
// spawn object's team (+30) set, the kit choices a real client's spawn screen
// fills in (+1D4, +1D8) borrowed from the host's, then 82332350 on it with no
// spawn point (the game picks one of the team's), 0x800 and 0x20, as the
// player's deploy message does (8235FBD0).
PPC_FUNC(sub_823D1DD0) {
  FindBotInputs();
  __imp__sub_823D1DD0(ctx, base);
  if (g_bots.empty()) return;
  static int64_t host_seen_ms = 0, last_check = 0;
  int64_t now = int64_t(GetTickCount64());
  if (now - last_check < 1000) return;
  last_check = now;
  if (!host_seen_ms) {
    if (!Scan(0x820118D0).empty()) host_seen_ms = now;
    return;
  }
  if (now - host_seen_ms < 5000) return;
  uint32_t kit = 0, kit2 = 0;
  for (uint32_t o : Scan(0x82019490))
    if (!BotWithSpawner(o) && Ld32(o + 0x1D4)) kit = Ld32(o + 0x1D4), kit2 = Ld32(o + 0x1D8);
  if (!kit) return;
  // BF_BC_KITS=1 (testing): the kits (SoldierEntityData, vtable 8206E890) once:
  // each one's words that point at a SoldierWeaponData (82074ACC) or at an
  // array of them, its text, and the host's choice.
  static bool kits_logged = false;
  if (!kits_logged && getenv("BF_BC_KITS")) {
    kits_logged = true;
    RT_INFO("bc kits: the host's choice: kit %08X, weapon %08X", kit, kit2);
    for (uint32_t k : Scan(0x8206E890)) {
      std::string out;
      for (uint32_t off = 0; off < 0x200; off += 4) {
        uint32_t v = Ld32(k + off);
        if (!Ptr(v)) continue;
        if (Ld32(v) == 0x82074ACC) out += " +" + std::to_string(off) + "=weapon " + std::to_string(v);
        else if (IsCommitted(v + 4) && Ptr(Ld32(v)) && Ld32(Ld32(v)) == 0x82074ACC)
          out += " +" + std::to_string(off) + "=weapons[]@" + std::to_string(v);
        else {
          const char* c = G<char>(v);
          int n = 0;
          while (n < 40 && c[n] >= 32 && c[n] < 127) ++n;
          if (n >= 4 && c[n] == 0) out += " +" + std::to_string(off) + "='" + std::string(c, n) + "'";
        }
      }
      RT_INFO("bc kits: kit %08X:%s", k, out.substr(0, 900).c_str());
      // What points at it (the side's kit list, its asset): the objects with
      // a vtable, their class and text.
      uint32_t want = _byteswap_ulong(k);
      std::string refs;
      int found = 0;
      for (uint64_t page = 0x40000000; page < 0xC0000000 && found < 12; page += 0x1000) {
        if (page >= 0x80000000 && page < 0xA0000000) continue;
        if (!IsCommitted(uint32_t(page))) continue;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(g_base + page);
        for (int i = 0; i < 1024 && found < 12; ++i) {
          if (w[i] != want) continue;
          uint32_t at = uint32_t(page) + i * 4;
          // the object around it: the nearest word before that is a vtable
          for (uint32_t o = at; o + 0x400 > at && o >= 0x40000000; o -= 4) {
            uint32_t vt = Ld32(o);
            if (vt >= 0x82000000 && vt < 0x82180000 && !TypeName(o).empty()) {
              char buf[160];
              snprintf(buf, sizeof(buf), " [%08X+%X %s %s]", o, at - o, TypeName(o).c_str(),
                       Ptr(Ld32(o + 0xC)) ? std::string(G<char>(Ld32(o + 0xC))).substr(0, 40).c_str() : "");
              refs += buf;
              break;
            }
          }
          ++found;
        }
      }
      RT_INFO("bc kits:   pointed at by:%s", refs.c_str());
    }
  }
  for (Bot& b : g_bots) {
    if (!b.spawner) continue;
    uint32_t so = b.soldier;
    bool alive = so && IsCommitted(so + 0x400) && Ld32(so) == 0x820118D0 && LdF(so + 0x3C) > 0;
    if (alive) continue;
    if (so) {
      RT_INFO("bot %d: its soldier %08X is dead", b.index, so);
      b.soldier = b.input = b.aim_obj = b.aim_for = 0, b.steer = false, b.enemy = 0, b.crouched = false;
      b.route.clear(), b.died_at = now;
    }
    if (b.died_at && now - b.died_at < 8000) continue;
    St32(b.spawner + 0x30, b.team);
    uint32_t bot_kit = BotKit(b, kit), bot_weapon = bot_kit == kit ? kit2 : 0;
    St32(b.spawner + 0x1D4, bot_kit), St32(b.spawner + 0x1D8, bot_weapon);
    std::vector<uint32_t> before = Scan(0x820118D0);
    PPCContext c = ctx;
    c.r3.u64 = b.spawner;
    c.r4.u64 = 0;
    c.r5.u64 = 0x800;
    c.r6.u64 = 0x20;
    __imp__sub_82332350(c, base);
    for (uint32_t o : Scan(0x820118D0))
      if (std::find(before.begin(), before.end(), o) == before.end()) b.soldier = o;
    b.died_at = b.soldier ? 0 : now - 6000;  // refused (the game's respawn wait): again in 2 s
    if (b.soldier) g_bot_soldier_for_trails = b.soldier;
    RT_INFO("bot %d: deployed on team %u: soldier %08X (team %u) at %.1f %.1f", b.index, b.team, b.soldier,
            b.soldier ? Ld32(b.soldier + 0x48) : 0, b.soldier ? LdF(b.soldier + 0x200) : 0.0f,
            b.soldier ? LdF(b.soldier + 0x208) : 0.0f);
  }
}
extern "C" PPC_FUNC(__imp__sub_82705810);
PPC_FUNC(sub_82364C78) {
  if (g_trace) {
    TraceAi("player join 82364C78", ctx);
    for (uint32_t r : {ctx.r4.u32, ctx.r5.u32}) {
      std::string out;
      for (uint32_t off = 0; off < 0x100 && r >= 0x40000000 && IsCommitted(r + off); off += 4) {
        uint32_t v = Ld32(r + off);
        std::string tn = TypeName(v);
        std::string text;
        if (v >= 0x40000000 && v < 0xC0000000 && IsCommitted(v)) {
          const char* c = G<char>(v);
          int n = 0;
          while (n < 24 && c[n] >= 32 && c[n] < 127) ++n;
          if (n >= 3 && c[n] == 0) text = std::string(c, n);
        }
        char buf[96];
        snprintf(buf, sizeof(buf), " +%X:%08X%s%s%s%s", off, v, tn.empty() ? "" : ":", tn.c_str(),
                 text.empty() ? "" : "=", text.c_str());
        out += buf;
      }
      RT_INFO("bot trace:   join arg %08X:%s", r, out.c_str());
    }
  }
  // The connection object around the address record: r4 - 0x40 .. r4 + 0x400,
  // vtable-looking words and named types marked.
  if (g_trace) {
    std::string out;
    for (int32_t off = -0x40; off < 0x400; off += 4) {
      uint32_t a2 = ctx.r4.u32 + off;
      if (!IsCommitted(a2)) continue;
      uint32_t v = Ld32(a2);
      if (!v) continue;
      bool vt = v >= 0x82000000 && v < 0x82180000 && Ld32(v) >= 0x82180000 && Ld32(v) < 0x82F00000;
      std::string tn = TypeName(v);
      char buf[80];
      snprintf(buf, sizeof(buf), " %+X:%08X%s%s%s", off, v, vt ? "(vt)" : "", tn.empty() ? "" : ":", tn.c_str());
      out += buf;
    }
    RT_INFO("bot trace:   connection around %08X:%s", ctx.r4.u32, out.substr(0, 3500).c_str());
  }
  // BF_BOT_FAKE=N: before the first join (the host's own client), N players
  // joined with copies of its connection object (0x60 bytes from r4 - 4, its
  // address record on port 1599 + i, nobody's), each on the join's working
  // record (r5 and what follows) as it was, restored for the real join; then
  // each one's network peer, as a real join is followed by: 82705810 on the
  // peer manager (the player manager + 8) with the player's index (7 + i), 1
  // and its connection object (vtable 8201B140), linked at the connection's
  // +14 and +28.
  static const int fake_count = getenv("BF_BOT_FAKE") ? std::clamp(atoi(getenv("BF_BOT_FAKE")), 1, 16) : 0;
  static bool done = false;
  if (fake_count && !done) {
    done = true;
    const char* team_env = getenv("BF_BOT_TEAM");
    int one_team = team_env ? atoi(team_env) : 0;
    std::vector<uint8_t> snap(0x800);
    memcpy(snap.data(), G<uint8_t>(ctx.r5.u32), snap.size());
    for (int i = 0; i < fake_count; ++i) {
      Bot& b = g_bots.emplace_back();
      b.index = i;
      b.team = one_team == 1 || one_team == 2 ? uint32_t(one_team) : uint32_t(i % 2 + 1);
      uint32_t conn = g_system_heap->Alloc(0x60, 16);
      memcpy(G<uint8_t>(conn), G<uint8_t>(ctx.r4.u32 - 4), 0x60);
      uint32_t copy = conn + 4;
      St16(copy + 6, uint16_t(1599 + i));  // the port (sockaddr: family 2, port, address)
      memcpy(G<uint8_t>(ctx.r5.u32), snap.data(), snap.size());
      PPCContext c = ctx;
      c.r4.u64 = copy;
      std::vector<uint32_t> before = Scan(0x8201B140);
      std::vector<uint32_t> spawn_before = Scan(0x82019490);
      __imp__sub_82364C78(c, base);
      for (uint32_t o : Scan(0x82019490))
        if (std::find(spawn_before.begin(), spawn_before.end(), o) == spawn_before.end()) b.spawner = o;
      for (uint32_t o : Scan(0x8201B140)) {
        if (std::find(before.begin(), before.end(), o) != before.end()) continue;
        PPCContext pc = ctx;
        pc.r3.u64 = ctx.r3.u32 + 8;
        pc.r4.u64 = uint64_t(7 + i);
        pc.r5.u64 = 1;
        pc.r6.u64 = o;
        std::vector<uint32_t> peers_before = Scan(0x820550C0);
        __imp__sub_82705810(pc, base);
        for (uint32_t peer : Scan(0x820550C0)) {
          if (std::find(peers_before.begin(), peers_before.end(), peer) != peers_before.end()) continue;
          if (!Ld32(o + 0x14)) St32(o + 0x14, peer);
          if (!Ld32(o + 0x28)) St32(o + 0x28, peer);
        }
        b.conn = o;
      }
      RT_INFO("bot %d: joined at 127.0.0.1:%d (team %u): connection %08X, spawn object %08X", i, 1599 + i, b.team,
              b.conn, b.spawner);
    }
    memcpy(G<uint8_t>(ctx.r5.u32), snap.data(), snap.size());
  }
  __imp__sub_82364C78(ctx, base);
}
// The deploy path (BF_BOT_STACK on the soldier factory): the player's network
// message (8235FBD0) and the spawn logic below it.
extern "C" PPC_FUNC(__imp__sub_8235FBD0);
extern "C" PPC_FUNC(__imp__sub_82332350);
extern "C" PPC_FUNC(__imp__sub_823207F0);
extern "C" PPC_FUNC(__imp__sub_82359C68);
extern "C" PPC_FUNC(__imp__sub_82359A58);
TRACE_AI(82359C68, "spawn object: player 1")
// The message after a join that gives a player its spawn object (vtable
// 82019490): 82359A58 on its connection, r4 a record (+8 begin, +C end, +10
// capacity of a small payload; +28 a pointer), r5 0x21, r6 1. BF_BOT_FAKE:
// when the host's arrives, the same message (a copy of the record and its
// payload) is given to the fake player's connection.
PPC_FUNC(sub_82359A58) {
  if (g_trace) TraceAi("spawn object: player 2 82359A58", ctx);
  PPCContext saved = ctx;
  __imp__sub_82359A58(ctx, base);
  static bool replayed = false;
  bool from_bot = false;
  for (Bot& b : g_bots) from_bot |= b.conn == saved.r3.u32;
  if (g_bots.empty() || replayed || from_bot) return;
  replayed = true;
  uint32_t rec = saved.r4.u32;
  uint32_t b0 = Ld32(rec + 8), e0 = Ld32(rec + 0xC);
  uint32_t len = e0 >= b0 && e0 - b0 < 0x1000 ? e0 - b0 : 0;
  for (Bot& b : g_bots) {
    if (!b.conn) continue;
    uint32_t copy = g_system_heap->Alloc(0x40, 16);
    uint32_t payload = g_system_heap->Alloc(0x1000, 16);
    memcpy(G<uint8_t>(copy), G<uint8_t>(rec), 0x40);
    if (len) memcpy(G<uint8_t>(payload), G<uint8_t>(b0), len);
    St32(copy + 8, payload), St32(copy + 0xC, payload + len), St32(copy + 0x10, payload + len + 1);
    PPCContext c = saved;
    c.r3.u64 = b.conn;
    c.r4.u64 = copy;
    std::vector<uint32_t> spawners = Scan(0x82019490);
    __imp__sub_82359A58(c, base);
    for (uint32_t o : Scan(0x82019490))
      if (std::find(spawners.begin(), spawners.end(), o) == spawners.end()) b.spawner = o;
    RT_INFO("bot %d: the host's message (%u bytes) replayed on its connection %08X; spawn object %08X", b.index, len,
            b.conn, b.spawner);
  }
}
TRACE_AI(8235FBD0, "deploy: player message")
TRACE_AI(82332350, "deploy: spawn 1")
TRACE_AI(823207F0, "deploy: spawn 2")
// The network peer object (vtable 820550C0) behind a player's connection.
extern "C" PPC_FUNC(__imp__sub_826FB8F0);
extern "C" PPC_FUNC(__imp__sub_826FD9F0);
extern "C" PPC_FUNC(__imp__sub_826FD850);
extern "C" PPC_FUNC(__imp__sub_82705810);
TRACE_AI(826FB8F0, "peer builder 1")
TRACE_AI(826FD9F0, "peer caller 1 (virtual)")
TRACE_AI(826FD850, "peer builder 2")
TRACE_AI(82705810, "peer caller 2")
extern "C" PPC_FUNC(__imp__sub_8264DA80);
extern "C" PPC_FUNC(__imp__sub_82614F78);
extern "C" PPC_FUNC(__imp__sub_82638400);
extern "C" PPC_FUNC(__imp__sub_82638520);
TRACE_AI(8264DA80, "ai hub")
TRACE_AI(82614F78, "ai caller 1")
TRACE_AI(82638400, "ai caller 2")
TRACE_AI(82638520, "ai caller 3 (virtual)")
TRACE_VFN(822B8428, "ctor A")
TRACE_VFN(822BD818, "ctor B")
TRACE_VFN(82321550, "+14")
TRACE_VFN(822F4348, "+44")
TRACE_VFN(82321820, "+80")
TRACE_VFN(822FD978, "+84")
TRACE_VFN(82322508, "+90")
TRACE_VFN(822F7D50, "+94")

// BF_BOT_CONN_COUNT=1 (testing): how often each of the connection's virtual
// functions (vtables 8201B140, 8201B134, 8201B124) runs on a connection,
// logged every 5 s: the per-tick ones carry the client's input.
static const bool g_conn_count = getenv("BF_BOT_CONN_COUNT") != nullptr;
static std::atomic<uint32_t> g_conn_calls[64];
static const char* g_conn_names[64];
static bool IsConnection(uint32_t o) {
  if (o < 0x40000000 || o >= 0xC0000000 || !IsCommitted(o)) return false;
  uint32_t v = Ld32(o);
  return v == 0x8201B140 || v == 0x8201B134 || v == 0x8201B124;
}
#define COUNT_CONN(i, addr)                                                    extern "C" PPC_FUNC(__imp__sub_##addr);                                     PPC_FUNC(sub_##addr) {                                                        if (g_conn_count && IsConnection(ctx.r3.u32)) {                               g_conn_names[i] = #addr;                                                    g_conn_calls[i]++;                                                        }                                                                           __imp__sub_##addr(ctx, base);                                             }
COUNT_CONN(0, 82325EB0)
COUNT_CONN(1, 82325EB8)
COUNT_CONN(2, 82325EC0)
COUNT_CONN(3, 82349F80)
COUNT_CONN(4, 8234A350)
COUNT_CONN(5, 8234A358)
COUNT_CONN(6, 8234A360)
COUNT_CONN(7, 8234A468)
COUNT_CONN(8, 8234A470)
// Hot on every connection (about 30 a second): every 10th call, r4/r5 and
// the 16 words they point at (BF_BOT_CONN_COUNT): does a field follow the stick?
extern "C" PPC_FUNC(__imp__sub_8234A478);
PPC_FUNC(sub_8234A478) {
  if (g_conn_count && IsConnection(ctx.r3.u32)) {
    g_conn_names[9] = "8234A478";
    if (g_conn_calls[9]++ % 10 == 0) {
      std::string w;
      for (uint32_t r : {ctx.r4.u32, ctx.r5.u32}) {
        w += " |";
        for (uint32_t k = 0; k < 16 && r >= 0x40000000 && r < 0xC0000000 && IsCommitted(r + k * 4); ++k) {
          char buf[12];
          snprintf(buf, sizeof(buf), " %08X", Ld32(r + k * 4));
          w += buf;
        }
      }
      RT_INFO("bot hot 8234A478 conn %08X r4=%08X r5=%08X r6=%08X:%s", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, w.c_str());
    }
  }
  __imp__sub_8234A478(ctx, base);
}
COUNT_CONN(10, 8234A508)
COUNT_CONN(11, 8234AC08)
COUNT_CONN(12, 8234AE40)
COUNT_CONN(13, 8234D900)
COUNT_CONN(14, 82351920)
COUNT_CONN(15, 82351D30)
COUNT_CONN(16, 82354F00)
COUNT_CONN(17, 82354FD0)
COUNT_CONN(18, 82355098)
COUNT_CONN(19, 82355128)
COUNT_CONN(20, 82358958)
COUNT_CONN(21, 8235A1E0)
COUNT_CONN(22, 8235FEE8)
COUNT_CONN(23, 82364618)
// Hot on every connection (about 30 a second): every 10th call, r4/r5 and
// the 16 words they point at (BF_BOT_CONN_COUNT): does a field follow the stick?
extern "C" PPC_FUNC(__imp__sub_82368478);
PPC_FUNC(sub_82368478) {
  if (g_conn_count && IsConnection(ctx.r3.u32)) {
    g_conn_names[24] = "82368478";
    if (g_conn_calls[24]++ % 10 == 0) {
      std::string w;
      for (uint32_t r : {ctx.r4.u32, ctx.r5.u32}) {
        w += " |";
        for (uint32_t k = 0; k < 16 && r >= 0x40000000 && r < 0xC0000000 && IsCommitted(r + k * 4); ++k) {
          char buf[12];
          snprintf(buf, sizeof(buf), " %08X", Ld32(r + k * 4));
          w += buf;
        }
      }
      RT_INFO("bot hot 82368478 conn %08X r4=%08X r5=%08X r6=%08X:%s", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, w.c_str());
    }
  }
  __imp__sub_82368478(ctx, base);
}
COUNT_CONN(25, 824BD728)
COUNT_CONN(26, 8250E7D0)
COUNT_CONN(27, 825FE488)
COUNT_CONN(28, 8267A318)
COUNT_CONN(29, 826F3A68)
COUNT_CONN(30, 826F3A80)
COUNT_CONN(31, 826F3A98)
COUNT_CONN(32, 826F3AD8)
COUNT_CONN(33, 826F3B00)
COUNT_CONN(34, 826F3B18)
COUNT_CONN(35, 826F3BE8)
COUNT_CONN(36, 826F3BF0)
COUNT_CONN(37, 826F62B8)
COUNT_CONN(38, 826F6A28)
COUNT_CONN(39, 826F9528)
COUNT_CONN(40, 826FDF08)
COUNT_CONN(41, 826FF510)
COUNT_CONN(42, 82701FD0)
static const bool g_conn_count_started = [] {
  if (!g_conn_count) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      std::string out;
      for (int i = 0; i < 64; ++i)
        if (uint32_t n = g_conn_calls[i].exchange(0)) out += std::string(" ") + g_conn_names[i] + "=" + std::to_string(n);
      RT_INFO("bot conn calls (5 s):%s", out.c_str());
    }
  }).detach();
  return true;
}();

// BF_BOT_INPUT_FNS=1 (testing): the input state's virtual functions (vtable
// 82010C98): calls on an input state counted every 5 s, and the first call of
// each with its arguments and call stack: who clears and who reads the input.
static const bool g_input_fns = getenv("BF_BOT_INPUT_FNS") != nullptr;
static std::atomic<uint32_t> g_input_calls[32];
static const char* g_input_names[32];
#define INPUT_FN(i, addr)                                                          extern "C" PPC_FUNC(__imp__sub_##addr);                                         PPC_FUNC(sub_##addr) {                                                            if (g_input_fns && ctx.r3.u32 >= 0x40000000 && ctx.r3.u32 < 0xC0000000 &&           IsCommitted(ctx.r3.u32) && Ld32(ctx.r3.u32) == 0x82010C98) {                  g_input_names[i] = #addr;                                                       if (g_input_calls[i]++ == 0) {                                                    RT_INFO("bot input fn %s on %08X r4=%08X r5=%08X r6=%08X", #addr,                       ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);                        bf_crash_diag::PrintStack(nullptr);                                           }                                                                             }                                                                               __imp__sub_##addr(ctx, base);                                                 }
INPUT_FN(0, 822AB128)
INPUT_FN(1, 822AB140)
INPUT_FN(2, 822AB150)
INPUT_FN(3, 822AB198)
INPUT_FN(4, 822AB1B0)
INPUT_FN(5, 822AB1B8)
INPUT_FN(6, 822AB1C8)
INPUT_FN(7, 822AB1E0)
INPUT_FN(8, 822B2618)
INPUT_FN(9, 822B2678)
INPUT_FN(10, 822B82B8)
INPUT_FN(11, 825165C8)
INPUT_FN(12, 825165F0)
INPUT_FN(13, 825E8378)
INPUT_FN(14, 825E9178)
INPUT_FN(15, 825E91A0)
INPUT_FN(16, 82679EA0)
static const bool g_input_fns_started = [] {
  if (!g_input_fns) return false;
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      std::string out;
      for (int i = 0; i < 32; ++i)
        if (uint32_t n = g_input_calls[i].exchange(0)) out += std::string(" ") + g_input_names[i] + "=" + std::to_string(n);
      RT_INFO("bot input fn calls (5 s):%s", out.c_str());
    }
  }).detach();
  return true;
}();

// BF_BOT_RAY_STACK=1 (testing): Havok's ray casters' virtual functions
// (hkWorldRayCaster 820BD6A8, hkSimpleWorldRayCaster 820BD73C, found through
// their RTTI): the first call of each prints its call stack, which shows the
// world's cast function and the game code that casts rays.
static const bool g_ray_stack = getenv("BF_BOT_RAY_STACK") != nullptr;
#define RAY_FN(addr)                                                  extern "C" PPC_FUNC(__imp__sub_##addr);                            PPC_FUNC(sub_##addr) {                                               static std::atomic<int> n{0};                                      if (g_ray_stack && n++ < 2) {                                        RT_INFO("bot ray: %s r3=%08X r4=%08X r5=%08X r6=%08X", #addr,               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);           bf_crash_diag::PrintStack(nullptr);                              }                                                                  __imp__sub_##addr(ctx, base);                                    }
RAY_FN(82B09FE8)
RAY_FN(82A00B88)
RAY_FN(82B0A510)
RAY_FN(82A020B0)
RAY_FN(82A02168)
RAY_FN(82A068B0)
RAY_FN(82A068B8)

// The game's ray casts: 824BA540 -> 824BA3B8 -> 829E8CD0 (builds an
// hkWorldRayCaster) and 824BA4F8 -> 824BA238 -> 829E98C8 (hkSimpleWorldRayCaster).
#define RAY_TRACE(addr)                                                             extern "C" PPC_FUNC(__imp__sub_##addr);                                          PPC_FUNC(sub_##addr) {                                                             static std::atomic<int> n{0};                                                    if (g_ray_stack && n++ < 3) {                                                      TraceAi("ray " #addr, ctx);                                                      if (n == 1) bf_crash_diag::PrintStack(nullptr);                                }                                                                                __imp__sub_##addr(ctx, base);                                                  }
RAY_TRACE(824BA540)
RAY_TRACE(824BA3B8)
RAY_TRACE(824BA4F8)
RAY_TRACE(824BA238)

// BF_BOT_ARM_TRACE=1 (testing): the server's arming path (BF_BOT_WATCH_ARM: the
// arm entity's state +8C written by 822C4F48 < 82345AD0 < 82349920 < 82349A30
// < 823CDCD0 < 823CEAD8): each call's r3-r7 (a pointer's vtable and class)
// and r3 after, the first 30 per function.
static const bool g_arm_trace = getenv("BF_BOT_ARM_TRACE") != nullptr;
std::atomic<bool> g_arm_trace_on{false};  // from when the host's autopilot is at the crate
static std::string ArmArg(uint32_t v) {
  char buf[96];
  if (Ptr(v) && Ld32(v) >= 0x82000000 && Ld32(v) < 0x82180000)
    snprintf(buf, sizeof(buf), "%08X(%08X %s)", v, Ld32(v), TypeName(v).c_str());
  else
    snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}
#define ARM_TRACE(addr)                                                                                       \
  extern "C" PPC_FUNC(__imp__sub_##addr);                                                                     \
  PPC_FUNC(sub_##addr) {                                                                                      \
    static int n = 0;                                                                                         \
    bool log = g_arm_trace && g_arm_trace_on && n++ < 30 && (std::string(#addr) == "822C4F48" || n < 6);                                                                       \
    std::string args;                                                                                         \
    if (log)                                                                                                  \
      args = ArmArg(ctx.r3.u32) + " " + ArmArg(ctx.r4.u32) + " " + ArmArg(ctx.r5.u32) + " " +                 \
             ArmArg(ctx.r6.u32) + " " + ArmArg(ctx.r7.u32);                                                    \
    __imp__sub_##addr(ctx, base);                                                                             \
    if (log) RT_INFO("bot arm trace: %s(%s) -> %08X", #addr, args.c_str(), ctx.r3.u32);                      \
  }
ARM_TRACE(822C4F48)
ARM_TRACE(82345AD0)
ARM_TRACE(82349920)
ARM_TRACE(82349A30)

// BF_BOT_DAMAGE_TRACE=1 (testing): the server's damage path (BF_BOT_WATCH_HEALTH:
// a soldier's health +3C written by 82275C48 < 822B29A8 < 822D1AA0 < 82316510,
// the killing write through 822CC5D8): each call whose r3 or r4 is a soldier,
// r3-r7 (a pointer's vtable and class) and f1/f2, the record r4 and r5 point
// to (0x60 bytes), and the soldier's health before and after; the first 25 per
// function.
static const bool g_damage_trace = getenv("BF_BOT_DAMAGE_TRACE") != nullptr;
static std::string DamageWords(uint32_t a) {
  if (!Ptr(a)) return "-";
  std::string w;
  for (uint32_t k = 0; k < 0x60 && IsCommitted(a + k + 3); k += 4) {
    uint32_t u = Ld32(a + k);
    float f;
    memcpy(&f, &u, 4);
    char buf[40];
    if (std::isfinite(f) && std::fabs(f) > 1e-3f && std::fabs(f) < 1e5f) snprintf(buf, sizeof(buf), " +%X:%.2f", k, f);
    else snprintf(buf, sizeof(buf), " +%X:%08X%s", k, u, Ptr(u) && Ld32(u) >= 0x82000000 && Ld32(u) < 0x82180000 ? "*" : "");
    w += buf;
  }
  return w;
}
#define DAMAGE_TRACE(addr)                                                                                        \
  extern "C" PPC_FUNC(__imp__sub_##addr);                                                                         \
  PPC_FUNC(sub_##addr) {                                                                                          \
    static int n = 0;                                                                                             \
    uint32_t so = Ptr(ctx.r3.u32) && Ld32(ctx.r3.u32) == 0x820118D0   ? ctx.r3.u32                                \
                  : Ptr(ctx.r4.u32) && Ld32(ctx.r4.u32) == 0x820118D0 ? ctx.r4.u32                                \
                                                                       : 0;                                       \
    bool log = g_damage_trace && n < 25;                                                                          \
    std::string args;                                                                                             \
    float before = so ? LdF(so + 0x3C) : 0;                                                                       \
    if (log)                                                                                                      \
      args = ArmArg(ctx.r3.u32) + " " + ArmArg(ctx.r4.u32) + " " + ArmArg(ctx.r5.u32) + " " + ArmArg(ctx.r6.u32) + \
             " " + ArmArg(ctx.r7.u32) + " f1 " + std::to_string(ctx.f1.f64) + " f2 " + std::to_string(ctx.f2.f64) +   \
             " | r4:" + DamageWords(ctx.r4.u32) + " | r5:" + DamageWords(ctx.r5.u32);                             \
    __imp__sub_##addr(ctx, base);                                                                                 \
    if (log && (!so || LdF(so + 0x3C) != before || n < 3)) {                                                      \
      ++n;                                                                                                        \
      RT_INFO("bot damage trace: %s(%s) -> %08X; health %.1f -> %.1f", #addr, args.substr(0, 1400).c_str(),       \
              ctx.r3.u32, before, so ? LdF(so + 0x3C) : 0.0f);                                                     \
    }                                                                                                             \
  }
DAMAGE_TRACE(82275C48)
DAMAGE_TRACE(822B29A8)
DAMAGE_TRACE(822CC5D8)
// 822D1AA0 itself: its giver (r5 -> word -> the attacking player + 0xC4,
// 82328920): what that player is (its vtable and class) and what of it points
// at the attacker's soldier, for the bots' kill credit.
extern "C" PPC_FUNC(__imp__sub_822D1AA0);
PPC_FUNC(sub_822D1AA0) {
  static int n = 0;
  if (g_damage_trace && n < 20 && Ptr(ctx.r5.u32)) {
    uint32_t w = Ld32(ctx.r5.u32), q = Ptr(w) ? Ld32(w) : 0, player = q ? q - 0xC4 : 0;
    if (Ptr(player)) {
      ++n;
      std::string soldiers;
      for (uint32_t off = 0; off < 0x800 && IsCommitted(player + off + 3); off += 4)
        if (Ptr(Ld32(player + off)) && Ld32(Ld32(player + off)) == 0x820118D0) soldiers += " +" + std::to_string(off);
      if (!BotWithSpawner(player) && n <= 4) bf_crash_diag::PrintStack(nullptr);  // a player's hit: how it gets here
      RT_INFO("bot damage trace: 822D1AA0 victim %08X (%s), amount %.1f, giver %08X -> %08X -> %08X: player %08X (vt %08X %s); "
              "soldier pointers at (decimal):%s",
              ctx.r3.u32, TypeName(ctx.r3.u32).c_str(), Ptr(ctx.r4.u32) ? LdF(ctx.r4.u32 + 0x28) : 0.0f, ctx.r5.u32, w, q,
              player, Ld32(player), TypeName(player).c_str(), soldiers.c_str());
    }
  }
  __imp__sub_822D1AA0(ctx, base);
}

// BF_BOT_GRENADE_TRACE=1 (testing): ServerGrenadeEntity's constructor (it
// stores its vtable 82017440): its arguments (a pointer's class, and the
// record r4/r5 point to) and the call stack, the first 4: how a throw makes one.
extern "C" PPC_FUNC(__imp__sub_82303C20);
PPC_FUNC(sub_82303C20) {
  static const bool trace = getenv("BF_BOT_GRENADE_TRACE") != nullptr;
  static int n = 0;
  if (trace && n++ < 4) {
    RT_INFO("bot grenade trace: 82303C20(%s %s %s %s %s) | r4:%s | r5:%s", ArmArg(ctx.r3.u32).c_str(), ArmArg(ctx.r4.u32).c_str(),
            ArmArg(ctx.r5.u32).c_str(), ArmArg(ctx.r6.u32).c_str(), ArmArg(ctx.r7.u32).c_str(),
            DamageWords(ctx.r4.u32).c_str(), DamageWords(ctx.r5.u32).c_str());
    bf_crash_diag::PrintStack(nullptr);
  }
  __imp__sub_82303C20(ctx, base);
}

// A player's connection dropped (8235FD80 on the player manager, r4 the
// connection: out of its list at +520, then the peer's disconnect 826FF630).
// A bot's connection has no traffic, so the network layer drops it after a
// few minutes, and the disconnect reads what only a real client's peer has
// (a fault at 00000096 in 82700480): a bot's is kept instead.
extern "C" PPC_FUNC(__imp__sub_8235FD80);
PPC_FUNC(sub_8235FD80) {
  for (const Bot& b : g_bots)
    if (b.conn && (ctx.r4.u32 == b.conn || ctx.r4.u32 == b.conn + 4 || ctx.r4.u32 == Ld32(b.conn + 0x14))) {
      static int said = 0;
      if (said++ < 8) RT_INFO("bot %d: its connection %08X kept (the network would drop it)", b.index, ctx.r4.u32);
      return;
    }
  static int seen = 0;
  if (seen++ < 8) RT_INFO("net: player connection %08X (vt %08X) dropped", ctx.r4.u32, Ptr(ctx.r4.u32) ? Ld32(ctx.r4.u32) : 0);
  __imp__sub_8235FD80(ctx, base);
}

// The body's facing: a fake soldier's never turns (its aim does, and walking
// follows the aim), and its bullets fly where the body faces, not where it
// aims (bots fighting at 8 m hurt each other only when standing in that
// line). Its transform is copied in each update by 822757E8 (r3 the soldier's,
// r4 the new matrix: three rotation rows and the position, 16 bytes apart):
// for a bot's soldier the rows are first turned about the vertical so the
// forward row (+20) faces where the bot wants.
extern "C" PPC_FUNC(__imp__sub_822757E8);
PPC_FUNC(sub_822757E8) {
  uint32_t r3 = ctx.r3.u32, m = ctx.r4.u32;
  // The matrix goes to r3's +30 (the storage; seen at the soldier's +300 with
  // BF_BOT_WATCH_HEADING), so the bot is the one whose soldier holds it.
  uint32_t dest = Ptr(r3) && IsCommitted(r3 + 0x34) ? Ld32(r3 + 0x30) : 0;
  for (const Bot& b : g_bots) {
    bool mine = b.soldier && ((r3 >= b.soldier && r3 < b.soldier + 0x800) || (dest >= b.soldier && dest < b.soldier + 0x800));
    static int said = 0;
    if (mine && said < 4) ++said, RT_INFO("bot %d: its body's transform (822757E8 r3 %08X, to %08X; soldier %08X)", b.index, r3, dest, b.soldier);
    if (!b.facing || !mine || !IsCommitted(m + 0x3C)) continue;
    float fx = LdF(m + 0x20), fz = LdF(m + 0x28);
    if (!std::isfinite(fx) || !std::isfinite(fz) || std::fabs(fx) + std::fabs(fz) < 0.1f) break;
    float d = b.face - std::atan2(fx, fz), c = std::cos(d), sn = std::sin(d);
    for (uint32_t row = 0; row < 0x30; row += 0x10) {
      float x = LdF(m + row), z = LdF(m + row + 8);
      float nx = x * c + z * sn, nz = z * c - x * sn;
      uint32_t ux, uz;
      memcpy(&ux, &nx, 4), memcpy(&uz, &nz, 4);
      St32(m + row, ux), St32(m + row + 8, uz);
    }
    break;
  }
  __imp__sub_822757E8(ctx, base);
}
