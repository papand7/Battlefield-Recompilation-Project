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
#include "../../bf1943-recomp/tools/msvc_compat/crash_diag.h"  // PrintStack

#include <algorithm>
#include <chrono>
#include <filesystem>
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
static void DriveFake() {
  if (!g_fake_soldier) return;
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

// The soldier update (822B92C8 <- 822C2C38 <- 822ACCD8 <- 82275A38, BF_BOT_WATCH_INPUT)
// fills a soldier's input state from its player's received input through
// 821BF2A8, which writes all its analog fields: for the fake soldier, the
// bot's values go in right after that, before the soldier uses them.
extern "C" PPC_FUNC(__imp__sub_821BF2A8);
PPC_FUNC(sub_821BF2A8) {
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  // The source is the player's received input (vtable 8200DC48; a walking
  // joiner's has 0.96 at +8): for the fake soldier, fill it before the copy.
  if (g_fake_input && r3 == g_fake_input && IsCommitted(r4) && Ld32(r4) == 0x8200DC48) {
    float f = 1.0f;
    uint32_t u;
    memcpy(&u, &f, 4);
    St32(r4 + 8, u);
  }
  __imp__sub_821BF2A8(ctx, base);
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

