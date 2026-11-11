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
#include "../../bf1943-recomp/tools/msvc_compat/crash_diag.h"  // PrintStack

#include <algorithm>
#include <array>
#include <chrono>
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
  std::map<uint64_t, std::pair<int, uint32_t>> unhandled;  // shapes left out: (type, vtable) -> count, one of them  // clip_r > 0: only triangles touching that circle
  void Tri(const Mat& m, const float a[3], const float b[3], const float c[3]) {
    float w[3][3];
    Apply(m, a, w[0]), Apply(m, b, w[1]), Apply(m, c, w[2]);
    if (clip_r > 0 && std::hypot(w[0][0] - clip_x, w[0][2] - clip_z) > clip_r &&
        std::hypot(w[1][0] - clip_x, w[1][2] - clip_z) > clip_r && std::hypot(w[2][0] - clip_x, w[2][2] - clip_z) > clip_r)
      return;
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
static uint32_t g_fake_spawner = 0;
static uint32_t g_fake_conn = 0;  // its connection (vtable 8201B140)
static uint32_t g_fake_conn_for_replay = 0;  // the same, once its peer is linked
extern "C" PPC_FUNC(__imp__sub_82332350);
extern "C" PPC_FUNC(__imp__sub_823D1DD0);
// The fake player's soldier and its input state (vtable 82010C98, the soldier at
// +4; analog actions: +8 turn, +C look up/down, +14 forward, as a joiner's
// stick showed), driven every server update: forward and a gentle turn.
static uint32_t g_fake_soldier = 0, g_fake_input = 0;
static uint32_t g_fake_record = 0;  // its received input record (vtable 8200DC48)
static void DriveFake() {
  if (!g_fake_soldier) return;
  // BF_BOT_YAW_EARLY=1 (test): the record's aim (+90) set at the start of the
  // server update, before anything reads it: 0, 1.57, 3.14 in turn, 5 s each.
  static const bool early = getenv("BF_BOT_YAW_EARLY") != nullptr;
  if (early && g_fake_record && IsCommitted(g_fake_record) && Ld32(g_fake_record) == 0x8200DC48) {
    static int64_t t0 = int64_t(GetTickCount64());
    int64_t now = int64_t(GetTickCount64());
    float test = float(((now - t0) / 5000) % 3) * 1.5708f;
    uint32_t u;
    memcpy(&u, &test, 4);
    St32(g_fake_record + 0x90, u);
    static int64_t last = 0;
    if (now - last > 2500) {
      last = now;
      RT_INFO("bot yaw early: +90 = %.2f, heading %.2f", test,
              std::atan2(LdF(g_fake_soldier + 0x320), LdF(g_fake_soldier + 0x328)));
    }
  }
  if (!g_fake_input) {
    for (uint32_t o : Scan(0x82010C98))
      if (Ld32(o + 4) == g_fake_soldier) g_fake_input = o;
    static int64_t logged = 0;
    if (g_fake_input || int64_t(GetTickCount64()) - logged > 5000) {
      logged = int64_t(GetTickCount64());
      RT_INFO("bot fake: input state of soldier %08X: %08X", g_fake_soldier, g_fake_input);
    }
    if (!g_fake_input) return;
    // BF_BOT_WATCH_INPUT=1: log the call stack of every write to its analog
    // fields (the runtime's --watch-write): what clears them each update.
    if (getenv("BF_BOT_WATCH_INPUT")) StartWriteWatch(g_fake_input + 8, 0x10, 0);
    // BF_BOT_FIND_YAW=1: floats in the soldier and the objects it points at (two
    // levels, 0x800 / 0x400 bytes) equal to its heading in some convention.
    if (getenv("BF_BOT_FIND_YAW")) {
      float fx = LdF(g_fake_soldier + 0x320), fz = LdF(g_fake_soldier + 0x328);
      float h = std::atan2(fx, fz);
      const float pi = 3.14159265f;
      float wants[] = {h, -h, pi - h, h - pi, h + pi / 2, h - pi / 2, -h + pi / 2, -h - pi / 2};
      std::string out;
      std::function<void(uint32_t, uint32_t, std::string, int)> look = [&](uint32_t o, uint32_t len, std::string path,
                                                                           int depth) {
        for (uint32_t off = 0; off < len && IsCommitted(o + off + 4); off += 4) {
          float f = LdF(o + off);
          for (float w : wants)
            if (std::isfinite(f) && std::fabs(f - w) < 0.01f && std::fabs(f) > 0.05f) {
              char buf[96];
              snprintf(buf, sizeof(buf), " %s+%X=%.3f", path.c_str(), off, f);
              out += buf;
              break;
            }
          uint32_t q = Ld32(o + off);
          if (depth < 2 && q >= 0x40000000 && q < 0xC0000000 && !(q & 3) && IsCommitted(q) && IsCommitted(q + 0x3FC)) {
            char sub[32];
            snprintf(sub, sizeof(sub), "%s*(+%X)", path.c_str(), off);
            look(q, 0x400, sub, depth + 1);
          }
        }
      };
      look(g_fake_soldier, 0x800, "", 0);
      RT_INFO("bot fake: heading %.3f (fx %.2f fz %.2f); matching floats:%s", h, fx, fz, out.substr(0, 2500).c_str());
    }
    // BF_BOT_WATCH_HEADING=1: the writes to its soldier's rotation rows (+300..+32F).
    if (getenv("BF_BOT_WATCH_HEADING")) StartWriteWatch(g_fake_soldier + 0x300, 0x30, 0);
  }
  auto set = [](uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(a, u);
  };
  static int64_t last_log = 0;
  if (int64_t(GetTickCount64()) - last_log > 2000) {
    last_log = int64_t(GetTickCount64());
    float fwd, turn;
    uint32_t u = Ld32(g_fake_input + 0x14), v = Ld32(g_fake_input + 0x8);
    memcpy(&fwd, &u, 4), memcpy(&turn, &v, 4);
    RT_INFO("bot fake: before writing: forward %.2f turn %.2f; soldier at %.1f %.1f", fwd, turn,
            LdF(g_fake_soldier + 0x200), LdF(g_fake_soldier + 0x208));
  }
  (void)set;
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
static bool LoadNavMesh() {
  static bool tried = false;
  if (tried) return g_navquery != nullptr;
  tried = true;
  std::string path = "nav/" + g_host_map + ".nav";
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  char magic[4];
  int version = 0, size = 0;
  bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "BFNV", 4) && fread(&version, 4, 1, f) == 1 &&
            version == 1 && fread(&size, 4, 1, f) == 1 && size > 0;
  unsigned char* data = ok ? static_cast<unsigned char*>(dtAlloc(size, DT_ALLOC_PERM)) : nullptr;
  ok = ok && data && fread(data, 1, size_t(size), f) == size_t(size);
  fclose(f);
  if (!ok) {
    if (data) dtFree(data);
    RT_WARN("bot nav: %s isn't a navigation mesh", path.c_str());
    return false;
  }
  g_navmesh = dtAllocNavMesh();
  if (dtStatusFailed(g_navmesh->init(data, size, DT_TILE_FREE_DATA))) {
    RT_WARN("bot nav: Detour couldn't read %s", path.c_str());
    return false;
  }
  g_navquery = dtAllocNavMeshQuery();
  g_navquery->init(g_navmesh, 4096);
  RT_INFO("bot nav: navigation mesh %s (%d bytes)", path.c_str(), size);
  LoadBlocked();
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
static void BlockPoly(const float at[3]) {
  if (!g_navquery) return;
  dtQueryFilter filter;
  const float ext[3] = {1.5f, 3, 1.5f};
  dtPolyRef ref = 0;
  float p[3];
  g_navquery->findNearestPoly(at, ext, &filter, &ref, p);
  if (!ref) return;
  g_navmesh->setPolyFlags(ref, 0);
  if (FILE* f = fopen(("nav/" + g_host_map + ".blocked").c_str(), "a")) {
    fprintf(f, "%llx\n", (unsigned long long)ref);
    fclose(f);
  }
  RT_INFO("bot nav: blocked polygon %llx at %.1f %.1f %.1f", (unsigned long long)ref, p[0], p[1], p[2]);
}

// The corners of a route from a to b on the navigation mesh (empty if none).
static std::vector<std::array<float, 3>> RouteMesh(const float a[3], const float b[3]) {
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
  if (!sa || !sb) return out;
  dtPolyRef polys[512];
  int npolys = 0;
  dtStatus s3 = g_navquery->findPath(sa, sb, pa, pb, &filter, polys, &npolys, 512);
  if (log) RT_INFO("bot nav: path %d polygons (%08X)", npolys, s3);
  if (!npolys) return out;
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

// A first brain for the fake soldier: to the nearest gold crate. The crates
// are where their interaction entities are (ServerInteractionEntity, vtable
// 82011680, two per crate, position +D0/+D4/+D8); a mission objective's +C0
// is its marker, above the base. Heading from the soldier's world matrix (forward
// row +320/+328 in x/z); turning right lowers atan2(x, z).
static void FakeBrain(uint32_t in) {
  auto set = [in](uint32_t off, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(in + off, u);
  };
  uint32_t so = g_fake_soldier;
  if (!so || !IsCommitted(so) || Ld32(so) != 0x820118D0) return;
  float x = LdF(so + 0x200), z = LdF(so + 0x208), fx = LdF(so + 0x320), fz = LdF(so + 0x328);
  float best = 1e9f, tx = 0, tz = 0;
  float ty = 0;
  for (uint32_t crate : Scan(0x82011680)) {
    float cx = LdF(crate + 0xD0), cz = LdF(crate + 0xD8);
    float d = std::hypot(cx - x, cz - z);
    if (d < best) best = d, tx = cx, tz = cz, ty = LdF(crate + 0xD4);
  }
  if (best > 1e8f) return;
  // BF_BOT_CRATE_PROBE=1 (testing): every 20 s, each mission objective's +C0
  // triple and the transforms (three orthonormal rows, then a translation in
  // the map) in its first 0x400 bytes and in the objects it points to.
  static int64_t crate_probe = 0;
  if (getenv("BF_BOT_CRATE_PROBE") && int64_t(GetTickCount64()) - crate_probe > 20000) {
    crate_probe = int64_t(GetTickCount64());
    auto transforms = [](uint32_t o, uint32_t size) {
      std::string hits;
      for (uint32_t off = 0; off + 64 <= size && IsCommitted(o + off + 60); off += 4) {
        float c[3][3];
        for (int j = 0; j < 3; ++j)
          for (int i = 0; i < 3; ++i) c[j][i] = LdF(o + off + 16 * j + 4 * i);
        bool ok = true;
        for (int j = 0; j < 3 && ok; ++j) {
          float l = c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2];
          ok = std::isfinite(l) && std::fabs(l - 1) < 1e-3f;
        }
        float t[3] = {LdF(o + off + 48), LdF(o + off + 52), LdF(o + off + 56)};
        if (!ok || !(std::fabs(t[0]) > 1 && std::fabs(t[0]) < 2000 && std::fabs(t[2]) < 2000)) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), " +%X(%.1f %.1f %.1f)", off, t[0], t[1], t[2]);
        hits += buf;
      }
      return hits;
    };
    for (uint32_t crate : Scan(0x82013AF0)) {
      std::string out = transforms(crate, 0x400);
      for (uint32_t off = 0; off < 0x400; off += 4) {
        uint32_t q = Ld32(crate + off);
        if (!Ptr(q) || q == crate) continue;
        std::string h = transforms(q, 0x200);
        if (!h.empty()) out += " | +" + std::to_string(off) + "->" + TypeName(q) + ":" + h;
      }
      RT_INFO("bot crate probe: %08X C0 %.1f %.1f %.1f:%s", crate, LdF(crate + 0xC0), LdF(crate + 0xC4),
              LdF(crate + 0xC8), out.substr(0, 1200).c_str());
    }
  }
  // Along the learnt routes when there are some near both ends: the next route
  // point instead of the crate (re-planned every 5 s, next point within 1.5 m).
  static std::vector<std::array<float, 3>> route;  // the route's points
  static size_t step = 0;
  static int64_t planned = 0;
  int64_t now_ms = int64_t(GetTickCount64());
  if (now_ms - planned > 5000) {
    planned = now_ms;
    route.clear(), step = 0;
    float from[3] = {x, LdF(so + 0x204), z}, to[3] = {tx, LdF(so + 0x204), tz};
    to[1] = ty;
    route = RouteMesh(from, to);
    if (!route.empty()) {
      RT_INFO("bot nav: navigation mesh route of %zu corners to the crate", route.size());
      static int corners_logged = 0;
      if (corners_logged++ < 2) {
        std::string c;
        for (auto& q : route) {
          char buf[48];
          snprintf(buf, sizeof(buf), " (%.1f %.1f %.1f)", q[0], q[1], q[2]);
          c += buf;
        }
        RT_INFO("bot nav: corners:%s", c.c_str());
      }
    } else {
      LoadNav();
      float d_bot, d_goal;
      int a = NearestNav(x, z, &d_bot), b = NearestNav(tx, tz, &d_goal);
      if (a >= 0 && b >= 0 && d_bot < 15.0f && d_goal < 25.0f)
        for (int i : RouteNav(a, b)) route.push_back(g_nav.p[i]);
      if (!route.empty()) RT_INFO("bot nav: trail route of %zu points", route.size());
    }
  }
  while (step < route.size() && std::hypot(route[step][0] - x, route[step][2] - z) < 1.5f) ++step;
  if (step < route.size()) tx = route[step][0], tz = route[step][2];
  float diff = std::atan2(tx - x, tz - z) - std::atan2(fx, fz);
  while (diff > 3.14159265f) diff -= 6.2831853f;
  while (diff < -3.14159265f) diff += 6.2831853f;
  float turn = std::clamp(-diff * 2.0f, -1.0f, 1.0f);
  bool arrived = best < 1.0f && step >= route.size();
  // BF_BOT_TRACE_AT=x,z (testing): every tick within 4 m of there, the
  // soldier's position, height, aim and target, the first 400 times.
  static float trace_x = 1e9f, trace_z = 1e9f;
  static int traced = 0;
  if (trace_x > 1e8f && getenv("BF_BOT_TRACE_AT")) sscanf(getenv("BF_BOT_TRACE_AT"), "%f,%f", &trace_x, &trace_z);
  if (traced < 400 && std::hypot(x - trace_x, z - trace_z) < 4.0f) {
    ++traced;
    RT_INFO("bot trace: %lld at %.2f %.3f %.2f target %.1f %.1f forward row %.2f %.2f aim %.2f setter calls %d",
            (long long)GetTickCount64(), x, LdF(so + 0x204), z, tx, tz, fx, fz,
            g_bot_aim_obj ? -LdF(g_bot_aim_obj + 0x134) : 0.0f, g_bot_setter_calls.exchange(0));
  }
  // The soldier moves relative to where it looks (its aim object, steered
  // toward the next point below), not to its body: aim there, walk forward.
  float forward = arrived ? 0.0f : 1.0f;
  float strafe = 0.0f;
  // Stuck (under 0.3 m in 1.5 s while trying to move): jump (A: its held time
  // at +44, low half) and go sideways for 2 s.
  static float sx = 0, sz = 0;
  static int64_t since = 0, unstick_until = 0, jump_until = 0;
  static int side = 1;
  int64_t now = int64_t(GetTickCount64());
  if (std::hypot(x - sx, z - sz) > 0.3f) sx = x, sz = z, since = now;
  if (best >= 3.0f && now - since > 1500 && now > unstick_until) {
    side = -side;
    unstick_until = now + 2000, jump_until = now + 300, since = now;
    RT_INFO("bot brain: stuck at %.1f %.1f: jump and sidestep %s", x, z, side > 0 ? "right" : "left");
    // BF_BOT_STUCK_PROBE=1 (testing): the rigid bodies placed within 8 m (shape
    // vtable and type, its child's, whether in the world): what holds it.
    if (getenv("BF_BOT_STUCK_PROBE")) {
      std::string close;
      for (uint32_t b : Scan(0x820BDE74)) {
        float bx = LdF(b + 0xE0), by = LdF(b + 0xE4), bz = LdF(b + 0xE8);
        if (std::hypot(bx - x, bz - z) > 8.0f) continue;
        uint32_t s = Ld32(b + 0x10), w = Ld32(b + 8), c = Ptr(s) ? Ld32(s + 0x14) : 0;
        char buf[128];
        snprintf(buf, sizeof(buf), " [%08X at %.1f %.1f %.1f shape vt %08X type %u child %08X%s]", b, bx, by, bz,
                 Ptr(s) ? Ld32(s) : 0, Ptr(s) ? Ld32(s + 0xC) : 0, Ptr(c) ? Ld32(c) : 0,
                 Ptr(w) && Ld32(w) == 0x820BD8A0 ? "" : " not in world");
        close += buf;
      }
      RT_INFO("bot brain: bodies near:%s", close.substr(0, 1500).c_str());
      std::string soldiers;
      for (uint32_t o : Scan(0x820118D0)) {
        char buf[64];
        snprintf(buf, sizeof(buf), " %08X%s at %.1f %.1f %.1f", o, o == so ? "(bot)" : "", LdF(o + 0x200), LdF(o + 0x204),
                 LdF(o + 0x208));
        soldiers += buf;
      }
      RT_INFO("bot brain: soldiers:%s", soldiers.c_str());
      static int stuck_dumps = 0;
      if (stuck_dumps < 12) DumpCollisionNear(("stuck" + std::to_string(stuck_dumps++) + ".obj").c_str(), x, z, 6.0f);
      // Everything in the physics world, whatever its class: objects whose +8
      // is the hkWorld, per vtable (count, shape type at +10, an example).
      static bool world_census = false;
      if (!world_census) {
        world_census = true;
        uint32_t world = 0;
        for (uint32_t b : Scan(0x820BDE74))
          if (Ptr(Ld32(b + 8)) && Ld32(Ld32(b + 8)) == 0x820BD8A0) world = Ld32(b + 8);
        std::map<uint32_t, std::pair<int, uint32_t>> kinds;
        uint32_t want = _byteswap_ulong(world);
        for (uint64_t page = 0x40000000; world && page < 0xC0000000; page += 0x1000) {
          if (page >= 0x80000000 && page < 0xA0000000) continue;
          if (!IsCommitted(uint32_t(page))) continue;
          const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + page);
          for (int i = 2; i < 1024; ++i) {
            if (p[i] != want) continue;
            uint32_t o = uint32_t(page) + (i - 2) * 4, vt = Ld32(o);
            if (vt < 0x82000000 || vt >= 0x83000000) continue;
            auto& k = kinds[vt];
            if (!k.first++) k.second = o;
          }
        }
        // Shapes left out of the triangles, over all bodies in any world.
        if (FILE* nul = fopen("NUL", "w")) {
          CollisionOut all{nul};
          for (uint32_t b : Scan(0x820BDE74)) {
            uint32_t w = Ld32(b + 8);
            if (!Ptr(w) || Ld32(w) != 0x820BD8A0) continue;
            Mat m;
            for (int j = 0; j < 3; ++j)
              for (int i = 0; i < 3; ++i) m.c[j][i] = LdF(b + 0xB0 + 16 * j + 4 * i);
            for (int i = 0; i < 3; ++i) m.t[i] = LdF(b + 0xE0 + 4 * i);
            ShapeOut(all, m, Ld32(b + 0x10), 0);
          }
          fclose(nul);
          for (auto& [k, u] : all.unhandled)
            RT_INFO("bot brain: level shapes left out: type %u vtable %08X: %d, e.g. %08X", uint32_t(k >> 32), uint32_t(k),
                    u.first, u.second);
        }
        for (auto& [vt, k] : kinds) {
          uint32_t o = k.second, s = Ld32(o + 0x10);
          RT_INFO("bot brain: world %08X holds vtable %08X (%s) x%d, e.g. %08X: shape %08X type %u, +E0 %.1f %.1f %.1f", world,
                  vt, TypeName(o).c_str(), k.first, o, s, Ptr(s) ? Ld32(s + 0xC) : 0, LdF(o + 0xE0), LdF(o + 0xE4),
                  LdF(o + 0xE8));
        }
      }
    }
    // Stuck twice near the same spot while following a navigation mesh route:
    // the corner it was heading for is somewhere it can't get to.
    static float last_sx = 1e9f, last_sz = 1e9f;
    if (std::hypot(x - last_sx, z - last_sz) < 3.0f && step < route.size() && g_navquery) {
      // Where it can't get through: 1.5 m ahead, toward the corner it is heading for.
      float dx2 = route[step][0] - x, dz2 = route[step][2] - z, l2 = std::max(std::hypot(dx2, dz2), 0.001f);
      float ahead[3] = {x + 1.5f * dx2 / l2, LdF(so + 0x204), z + 1.5f * dz2 / l2};
      BlockPoly(ahead);
      planned = 0;  // plan again now
    }
    last_sx = x, last_sz = z;
  }
  if (now < unstick_until) forward = 0.3f, strafe = float(side);
  // A button pressed: its bit at +84 (A 0x2000, X 0x200000) and its held time
  // (A +44, X +54; half float, low half), as a joiner's pad showed.
  bool jump = now < jump_until;
  static int64_t held_x_since = 0;
  bool at_crate = best < 2.0f;
  if (at_crate && !held_x_since) held_x_since = now;
  if (!at_crate) held_x_since = 0;
  bool arm = at_crate;
  St32(in + 0x84, (jump ? 0x2000u : 0u) | (arm ? 0x200000u : 0u));
  St32(in + 0x44, jump ? 0x00003C00u : 0u);
  St32(in + 0x54, arm ? 0x00003C00u : 0u);
  // BF_BOT_ARM_SWEEP=1 (testing): at the crate every button bit but A's, and
  // every held time at 1.0 (both halves); the interaction entities within 6 m
  // (a transform at +40..+400 there) logged when their first 0x200 bytes change.
  if (arm && getenv("BF_BOT_ARM_SWEEP")) {
    St32(in + 0x84, 0xFFFFFFFFu & ~0x2000u);
    for (uint32_t off = 0x40; off <= 0x58; off += 4) St32(in + off, 0x3C003C00u);
    static std::map<uint32_t, std::vector<uint32_t>> seen;
    static int64_t last_look = 0;
    if (now - last_look > 1000) {
      last_look = now;
      for (uint32_t ie : Scan(0x82011680)) {
        bool close = false;
        for (uint32_t off = 0x40; off < 0x400 && !close; off += 4) {
          float tx2 = LdF(ie + off), ty2 = LdF(ie + off + 4), tz2 = LdF(ie + off + 8);
          close = std::isfinite(tx2) && std::fabs(ty2) < 300 && ty2 > 50 && std::hypot(tx2 - x, tz2 - z) < 6.0f;
        }
        if (!close) continue;
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
          RT_INFO("bot brain: interaction entity %08X near (radius %.1f)%s", ie,
                  Ptr(Ld32(ie + 0x14)) ? LdF(Ld32(ie + 0x14) + 0x84) : -1.0f, d.empty() ? " first seen" : d.c_str());
        before = w;
      }
    }
  }
  // At the crate: what of its first 0x200 bytes changes while X is held.
  static uint32_t watched = 0;
  static std::vector<uint32_t> before_arm;
  if (arm) {
    uint32_t crate = 0;
    for (uint32_t c : Scan(0x82011680))
      if (std::hypot(LdF(c + 0xD0) - x, LdF(c + 0xD8) - z) < 3.0f) crate = c;
    if (crate && crate != watched) {
      watched = crate;
      before_arm.assign(0x80, 0);
      for (uint32_t k = 0; k < 0x80; ++k) before_arm[k] = Ld32(crate + 4 * k);
      RT_INFO("bot brain: at crate %08X, holding X", crate);
    }
    static int64_t last_diff = 0;
    if (crate && crate == watched && now - last_diff > 2000) {
      last_diff = now;
      std::string d;
      for (uint32_t k = 0; k < 0x80; ++k) {
        uint32_t v = Ld32(crate + 4 * k);
        if (v != before_arm[k]) {
          char buf[40];
          snprintf(buf, sizeof(buf), " +%X:%08X>%08X", 4 * k, before_arm[k], v);
          d += buf;
        }
      }
      RT_INFO("bot brain: crate %08X after %lld ms of X:%s", crate, (long long)(now - held_x_since), d.c_str());
      // And the mission objectives: did one change (armed)?
      static std::map<uint32_t, std::vector<uint32_t>> objectives;
      for (uint32_t o : Scan(0x82013AF0)) {
        auto& was = objectives[o];
        std::string od;
        for (uint32_t k = 0; k < 0x80; ++k) {
          uint32_t v = Ld32(o + 4 * k);
          if (!was.empty() && v != was[k]) {
            char buf[40];
            snprintf(buf, sizeof(buf), " +%X:%08X>%08X", 4 * k, was[k], v);
            od += buf;
          }
        }
        if (!od.empty()) RT_INFO("bot brain: objective %08X changed:%s", o, od.c_str());
        was.resize(0x80);
        for (uint32_t k = 0; k < 0x80; ++k) was[k] = Ld32(o + 4 * k);
      }
    }
  }
  set(0x8, forward);
  set(0xC, strafe);
  // The turn stick stays centred: the aim setter below turns it (the stick,
  // full over whenever the target was far from the fixed body heading, turned
  // the aim against it).
  (void)turn;
  set(0x1C, 0.0f);
  // The soldier's aim (BF_BOT_YAW_HUNT on a turning joiner): an object with
  // vtable 820264A8 holds minus its yaw at +134. The bot's, found from its
  // soldier (two levels of pointers), gets the yaw toward its target.
  static uint32_t aim_obj = 0;
  static uint32_t aim_for = 0;
  if (aim_for != so) {
    aim_for = so, aim_obj = 0;
    std::function<void(uint32_t, int)> find = [&](uint32_t o, int depth) {
      for (uint32_t off = 0; off < 0x800 && !aim_obj && IsCommitted(o + off); off += 4) {
        uint32_t q = Ld32(o + off);
        if (q < 0x40000000 || q >= 0xC0000000 || (q & 3) || !IsCommitted(q)) continue;
        if (Ld32(q) == 0x820264A8) aim_obj = q;
        else if (depth < 2 && IsCommitted(q + 0x7FC)) find(q, depth + 1);
      }
    };
    find(so, 0);
    RT_INFO("bot brain: aim object of soldier %08X: %08X (yaw %.2f)", so, aim_obj, aim_obj ? -LdF(aim_obj + 0x134) : 0.0f);
    // BF_BOT_WATCH_AIM=1: who writes it (the call stacks), instead of steering.
    if (aim_obj && getenv("BF_BOT_WATCH_AIM")) StartWriteWatch(aim_obj + 0x134, 4, 0);
  }
  if (aim_obj && !getenv("BF_BOT_WATCH_AIM")) {
    // Facing the next point: the yaw whose heading points there. Heading
    // atan2(fx, fz) was minus the aim object's value (-2.57 vs 2.57).
    float want = std::atan2(tx - x, tz - z);
    // BF_BOT_DIR_TEST=1 (testing): 8 fixed directions, 4 s each, and how far
    // each one went: does the speed depend on the direction?
    if (getenv("BF_BOT_DIR_TEST")) {
      static int64_t t0 = now;
      static float px = x, pz = z;
      static int seg = -1;
      int s = int((now - t0) / 4000) % 8;
      if (s != seg) {
        if (seg >= 0)
          RT_INFO("bot dir test: direction %.2f went %.2f m (dir %.2f)", seg * 0.785398f - 3.14159f,
                  std::hypot(x - px, z - pz), std::atan2(x - px, z - pz));
        seg = s, px = x, pz = z;
      }
      want = s * 0.785398f - 3.14159f;
    }
    // Walking forward with the aim at "want" went the opposite way: the aim's
    // convention is half a turn from atan2(dx, dz).
    g_bot_aim_obj = aim_obj, g_bot_want_heading = want + 3.14159265f, g_bot_steer = true;
    static int64_t aim_log = 0;
    if (now - aim_log > 3000) {
      aim_log = now;
      RT_INFO("bot brain: aim %.2f wanted, heading now %.2f", want, std::atan2(fx, fz));
    }
  }
  // BF_BOT_YAW_TEST=1: the absolute aim (+90 yaw, +94 pitch, radians, as a
  // joiner's showed) set to 0, 1.57 and 3.14 in turn, 5 s each, with the
  // soldier's heading logged: how yaw maps to facing.
  static const bool yaw_test = getenv("BF_BOT_YAW_TEST") != nullptr;
  if (yaw_test) {
    static int64_t t0 = now;
    float test = float(((now - t0) / 5000) % 3) * 1.5708f;
    set(0x90, test);
    set(0x94, 0.0f);
    set(0x8, 0.0f), set(0xC, 0.0f);
    static int64_t last_log = 0;
    if (now - last_log > 2500) {
      last_log = now;
      RT_INFO("bot yaw test: +90 = %.2f, heading atan2(fx, fz) = %.2f (fx %.2f fz %.2f)", test, std::atan2(fx, fz), fx, fz);
    }
  }
  // The server takes a remote soldier's heading from a stored yaw (minus the
  // heading: *(*(soldier + C)) + 198, BF_BOT_FIND_YAW), not from the turn input:
  // turned there directly.
  uint32_t a1 = Ld32(so + 0xC);
  uint32_t a2 = a1 >= 0x40000000 && a1 < 0xC0000000 && IsCommitted(a1) ? Ld32(a1) : 0;
  uint32_t yaw_at = a2 >= 0x40000000 && a2 < 0xC0000000 && IsCommitted(a2 + 0x198) ? a2 + 0x198 : 0;
  if (yaw_at) {
    float yaw = LdF(yaw_at) + turn * 0.05f;
    uint32_t u;
    memcpy(&u, &yaw, 4);
    St32(yaw_at, u);
  }
  static bool said = false;
  if (!said) {
    said = true;
    RT_INFO("bot brain: yaw at %08X (soldier +C -> %08X -> %08X), %.3f", yaw_at, a1, a2, yaw_at ? LdF(yaw_at) : 0.0f);
  }
  // While a stick is held its "held" values ramp up (two half floats; for turn
  // and look at +34 low half and +38 high half): turning needs them, at 1.0.
  if (std::fabs(turn) > 0.05f) {
    St32(in + 0x34, 0x00003C00);
    St32(in + 0x38, 0x3C000000);
  }
  static int64_t last = 0;
  if (int64_t(GetTickCount64()) - last > 2000) {
    last = int64_t(GetTickCount64());
    RT_INFO("bot brain: at %.1f %.1f height %.1f, crate at %.1f %.1f, %.1f m, forward %.2f strafe %.2f", x, z,
            LdF(so + 0x204), tx, tz, best, forward, strafe);
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
  if (g_bot_steer && r3 == g_bot_aim_obj && IsCommitted(ctx.r4.u32 + 0x38)) {
    auto put = [](uint32_t a, float f) {
      uint32_t u;
      memcpy(&u, &f, 4);
      St32(a, u);
    };
    float h = g_bot_want_heading;
    put(ctx.r4.u32 + 0x34, -h);
    put(ctx.r4.u32 + 0x0, std::sin(h));
    put(ctx.r4.u32 + 0x8, std::cos(h));
    ++g_bot_setter_calls;
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
PPC_FUNC(sub_821BF2A8) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  if (IsCommitted(r4) && Ld32(r4) == 0x8200DC48) {
    std::lock_guard l(g_record_yaw_mutex);
    g_record_yaw[r4] = LdF(r4 + 0x90);
  }
  // The source is the player's received input (vtable 8200DC48; a walking
  // joiner's has 0.96 at +8): for the fake soldier, fill it before the copy.
  // Its fields (BF_BOT_INPUT_MAP with a joiner): +8 forward, +C strafe, +1C
  // turn (right +), +20 look (up +), +28 fire; +2C.. how long each is held.
  if (g_fake_input && r3 == g_fake_input && IsCommitted(r4) && Ld32(r4) == 0x8200DC48) {
    g_fake_record = r4;
    FakeBrain(r4);
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
  // The source (r4) of every copy, when a word in it looks like a full stick
  // (the host walking), and for the fake soldier's: its layout.
  {
    static int src_logged = 0;
    bool stick = false;
    for (uint32_t k = 0; k < 24; ++k) {
      float f;
      uint32_t u = IsCommitted(r4 + k * 4) ? Ld32(r4 + k * 4) : 0;
      memcpy(&f, &u, 4);
      stick |= f > 0.9f && f < 1.01f;
    }
    if ((stick || r3 == g_fake_input) && src_logged < 12) {
      ++src_logged;
      std::string w;
      for (uint32_t k = 0; k < 24; ++k) {
        char buf[12];
        snprintf(buf, sizeof(buf), " %08X", IsCommitted(r4 + k * 4) ? Ld32(r4 + k * 4) : 0);
        w += buf;
      }
      RT_INFO("bot fake: copy source %08X -> %08X%s:%s", r4, r3, r3 == g_fake_input ? " (fake)" : "", w.c_str());
    }
  }
  static int logged = 0;
  if (g_fake_input && logged < 3 && (r3 == g_fake_input || r4 == g_fake_input || r5 == g_fake_input)) {
    ++logged;
    RT_INFO("bot fake: input copy 821BF2A8 r3=%08X r4=%08X r5=%08X (input %08X)", r3, r4, r5, g_fake_input);
  }
  if (!g_fake_input || (r3 != g_fake_input && r4 != g_fake_input && r5 != g_fake_input)) return;
  auto set = [](uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(a, u);
  };
  set(g_fake_input + 0x14, 1.0f);  // forward
  set(g_fake_input + 0x8, 0.3f);   // turn right
}
PPC_FUNC(sub_823D1DD0) {
  DriveFake();
  __imp__sub_823D1DD0(ctx, base);
  static int64_t host_seen_ms = 0;
  static bool deployed = false;
  if (!g_fake_conn || deployed) return;
  static int64_t last_check = 0;
  int64_t now = int64_t(GetTickCount64());
  if (now - last_check < 1000) return;
  last_check = now;
  if (!host_seen_ms) {
    if (!Scan(0x820118D0).empty()) host_seen_ms = now;
    return;
  }
  if (now - host_seen_ms < 5000) return;
  deployed = true;
  // The spawn objects and which connection (vtable 8201B140) each points to
  // within its first 0x400 bytes; the fake player's is the one pointing at its
  // connection.
  for (uint32_t o : Scan(0x82019490)) {
    std::string links;
    for (uint32_t off = 0; off < 0x400; off += 4) {
      uint32_t v = Ld32(o + off);
      if (v >= 0x40000000 && v < 0xC0000000 && IsCommitted(v) && Ld32(v) == 0x8201B140) {
        char buf[48];
        snprintf(buf, sizeof(buf), " +%X->conn %08X%s", off, v, v == g_fake_conn ? "(fake)" : "");
        links += buf;

      }
    }
    RT_INFO("bot fake: spawn object %08X:%s +30=%08X +104=%08X +158=%02X +1D0=%08X +1D4=%08X +1D8=%08X", o,
            links.c_str(), Ld32(o + 0x30), Ld32(o + 0x104), *G<uint8_t>(o + 0x158), Ld32(o + 0x1D0), Ld32(o + 0x1D4),
            Ld32(o + 0x1D8));
  }
  if (!g_fake_spawner) {
    RT_INFO("bot fake: no spawn object for the fake player");
    return;
  }
  // The deploy needs +1D4 (with +1D8 and the spawn point at +1D0): what a real
  // client's spawn screen choices (kit) fill in. Borrowed from the host's.
  for (uint32_t o : Scan(0x82019490)) {
    if (o == g_fake_spawner || !Ld32(o + 0x1D4)) continue;
    RT_INFO("bot fake: taking the host's choices: +1D4 %08X(%s) +1D8 %08X(%s)", Ld32(o + 0x1D4),
            TypeName(Ld32(o + 0x1D4)).c_str(), Ld32(o + 0x1D8), TypeName(Ld32(o + 0x1D8)).c_str());
    St32(g_fake_spawner + 0x1D4, Ld32(o + 0x1D4));
    St32(g_fake_spawner + 0x1D8, Ld32(o + 0x1D8));
    break;
  }
  size_t soldiers = Scan(0x820118D0).size();
  PPCContext c = ctx;
  c.r3.u64 = g_fake_spawner;
  c.r4.u64 = 0;  // no spawn point: the game picks one of the player's team's
  c.r5.u64 = 0x800;
  c.r6.u64 = 0x20;
  RT_INFO("bot fake: deploying (spawn object %08X, %zu soldiers before)", g_fake_spawner, soldiers);
  std::vector<uint32_t> soldiers_before = Scan(0x820118D0);
  __imp__sub_82332350(c, base);
  for (uint32_t so : Scan(0x820118D0))
    if (std::find(soldiers_before.begin(), soldiers_before.end(), so) == soldiers_before.end()) g_fake_soldier = so;
  RT_INFO("bot fake: its soldier %08X", g_fake_soldier);
  g_bot_soldier_for_trails = g_fake_soldier;
  RT_INFO("bot fake: deploy returned r3=%08X, %zu soldiers now", c.r3.u32, Scan(0x820118D0).size());
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
  // BF_BOT_FAKE=1 (experiment): before the first join (the host's own client),
  // join a player with a copy of its address record on port 1599, nobody's,
  // on a snapshot of the join's working record (r5 and what follows, restored
  // for the real join): does the server keep a player with no client?
  static const bool fake = getenv("BF_BOT_FAKE") != nullptr;
  static bool done = false;
  if (fake && !done) {
    done = true;
    // The whole connection object (0x60, from r4 - 4: its vtable 82055080, then
    // the address object the join gets), with another port.
    static uint32_t conn = g_system_heap->Alloc(0x60, 16);
    memcpy(G<uint8_t>(conn), G<uint8_t>(ctx.r4.u32 - 4), 0x60);
    uint32_t copy = conn + 4;
    St16(copy + 6, 1599);  // the port (sockaddr: family 2, port, address)
    std::vector<uint8_t> snap(0x800);
    memcpy(snap.data(), G<uint8_t>(ctx.r5.u32), snap.size());
    PPCContext c = ctx;
    c.r4.u64 = copy;
    std::vector<uint32_t> before = Scan(0x8201B140);
    std::vector<uint32_t> spawn_before = Scan(0x82019490);
    RT_INFO("bot fake: joining a player at 127.0.0.1:1599 (record %08X)", copy);
    __imp__sub_82364C78(c, base);
    RT_INFO("bot fake: join returned r3=%08X", c.r3.u32);
    for (uint32_t o : Scan(0x82019490))
      if (std::find(spawn_before.begin(), spawn_before.end(), o) == spawn_before.end()) {
        g_fake_spawner = o;
        RT_INFO("bot fake: its spawn object %08X", o);
      }
    // A real join is followed by the peer's creation: 82705810 on the peer
    // manager (the player manager + 8) with the player's index, 1 and its
    // connection object (vtable 8201B140); that fills the connection's +14.
    for (uint32_t o : Scan(0x8201B140)) {
      if (std::find(before.begin(), before.end(), o) != before.end()) continue;
      PPCContext pc = ctx;
      pc.r3.u64 = ctx.r3.u32 + 8;
      pc.r4.u64 = 7;
      pc.r5.u64 = 1;
      pc.r6.u64 = o;
      RT_INFO("bot fake: creating its peer (connection %08X, +14 %08X)", o, Ld32(o + 0x14));
      std::vector<uint32_t> peers_before = Scan(0x820550C0);
      __imp__sub_82705810(pc, base);
      RT_INFO("bot fake: peer created, connection +14 now %08X", Ld32(o + 0x14));
      // Real connections hold their peer (vtable 820550C0) at +14 and +28.
      for (uint32_t peer : Scan(0x820550C0)) {
        if (std::find(peers_before.begin(), peers_before.end(), peer) != peers_before.end()) continue;
        if (!Ld32(o + 0x14)) St32(o + 0x14, peer);
        if (!Ld32(o + 0x28)) St32(o + 0x28, peer);
        RT_INFO("bot fake: new peer %08X linked into connection %08X", peer, o);
        g_fake_conn = o;
        g_fake_conn_for_replay = o;
      }
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
  uint32_t fake = g_fake_conn_for_replay;
  if (!fake || replayed || saved.r3.u32 == fake) return;
  replayed = true;
  uint32_t rec = saved.r4.u32;
  uint32_t b0 = Ld32(rec + 8), e0 = Ld32(rec + 0xC);
  uint32_t len = e0 >= b0 && e0 - b0 < 0x1000 ? e0 - b0 : 0;
  static uint32_t copy = g_system_heap->Alloc(0x40, 16);
  static uint32_t payload = g_system_heap->Alloc(0x1000, 16);
  memcpy(G<uint8_t>(copy), G<uint8_t>(rec), 0x40);
  if (len) memcpy(G<uint8_t>(payload), G<uint8_t>(b0), len);
  St32(copy + 8, payload), St32(copy + 0xC, payload + len), St32(copy + 0x10, payload + len + 1);
  PPCContext c = saved;
  c.r3.u64 = fake;
  c.r4.u64 = copy;
  RT_INFO("bot fake: replaying the host's message (%u bytes) on connection %08X", len, fake);
  std::vector<uint32_t> spawners = Scan(0x82019490);
  __imp__sub_82359A58(c, base);
  for (uint32_t o : Scan(0x82019490))
    if (std::find(spawners.begin(), spawners.end(), o) == spawners.end()) g_fake_spawner = o;
  RT_INFO("bot fake: replayed; its spawn object %08X", g_fake_spawner);
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
