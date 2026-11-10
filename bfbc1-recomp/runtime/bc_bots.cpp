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

#include <chrono>
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
      // The object holding it: the nearest named object start below (within 0x400).
      for (uint32_t back = 0; back <= 0x400 && back <= at - 0x40000000; back += 4) {
        std::string n = TypeName(at - back);
        if (!n.empty()) {
          char buf[96];
          snprintf(buf, sizeof(buf), " %s(%08X)+%X", n.c_str(), at - back, back);
          refs += buf;
          break;
        }
      }
    }
  }
  RT_INFO("bot scan: soldier %08X pointed at by:%s", o, refs.c_str());
}

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

