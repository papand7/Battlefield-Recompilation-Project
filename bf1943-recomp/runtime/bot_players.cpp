// Bots as players on the host's own server (Bad Company's bots, bfbc1-recomp/
// runtime/bc_bots.cpp), for BF1943: being ported. Bad Company's functions,
// found in BF1943 by the shape of their code (tools/match_functions.py):
//   82364C78 the player manager's join     -> 823B59E0 (0.84 alike)
//   82705810 the peer manager's create      -> 827559F0 (0.89)
//   82332350 deploy a soldier               -> 82382720 (0.71)
//   821BF2A8 a player's input to its soldier -> 822ED8D8 (0.79)
//   822D1AA0 damage a soldier               -> 823194B0 (0.76)
// BF_BOT_DISCOVER=1 (testing): logs the first calls of each, with the vtables
// (and type names) of their arguments, to take Bad Company's constants over.
// Seen (a dedicated server, one bot joining), BF1943 / Bad Company:
//   join: r3 the player manager 8201CC38 / 8201B828, r4 the peer's address
//     (vt 82001FF0), r5/r6 on the stack
//   create peer: r3 the player manager + 8 (vt 82056D38), r4 the peer index
//     (1, 2, ...), r5 1, r6 the player's connection part 8201C728 / 8201B140
//   deploy: r3 the spawn object 8201A990 / 82019490; r4 45, r5 1, r6 1 (Bad
//     Company: 0 any spawn point, 800, 20)
//   input: r3 the soldier's input state 82012320 / 82010C98, r4 the received
//     input record 8200E48C / 8200DC48 (the client's: r3 vt 82005914)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "bot_players.h"
#include "guest.h"
#include "kernel.h"
#include "../tools/msvc_compat/crash_diag.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace rt;

#define GAME_PATCH(addr)                  \
  extern "C" PPC_FUNC(__imp__sub_##addr); \
  PPC_FUNC(sub_##addr)

namespace {

const bool g_discover = getenv("BF_BOT_DISCOVER") != nullptr;

// A class's engine type name (its vtable's +8 is getType: lis / addi r3 / blr).
std::string TypeName(uint32_t vt) {
  if (vt < 0x82000000 || vt >= 0x82180000 || !IsCommitted(vt + 8)) return {};
  uint32_t fn = Ld32(vt + 8);
  if (fn < 0x82180000 || fn >= 0x82D00000) return {};
  uint32_t i0 = Ld32(fn), i1 = Ld32(fn + 4), i2 = Ld32(fn + 8);
  uint32_t rx = (i0 >> 21) & 31;
  if ((i0 & 0xFC1F0000) != 0x3C000000 || (i1 & 0xFFE00000) != 0x38600000 || ((i1 >> 16) & 31) != rx ||
      i2 != 0x4E800020)
    return {};
  uint32_t type = (i0 << 16) + uint32_t(int32_t(int16_t(i1 & 0xFFFF)));
  uint32_t rec = IsCommitted(type + 4) ? Ld32(type + 4) : 0;
  uint32_t name = rec >= 0x82000000 && rec < 0x83000000 ? Ld32(rec) : 0;
  if (name < 0x82000000 || name >= 0x83000000) return {};
  return std::string(G<char>(name)).substr(0, 40);
}

// "r3 A3B4C5D0 (vt 82013A40 ServerSoldierEntity)" for each of r3..r6.
std::string Args(PPCContext& ctx) {
  std::string out;
  uint32_t r[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  for (int i = 0; i < 4; ++i) {
    char buf[120];
    uint32_t a = r[i], vt = (a >= 0x40000000 && a < 0xC0000000 && !(a & 3) && IsCommitted(a)) ? Ld32(a) : 0;
    if (vt >= 0x82000000 && vt < 0x82180000)
      snprintf(buf, sizeof(buf), " r%d %08X (vt %08X %s)", i + 3, a, vt, TypeName(vt).c_str());
    else
      snprintf(buf, sizeof(buf), " r%d %08X", i + 3, a);
    out += buf;
  }
  return out;
}

void Note(const char* what, std::atomic<int>& n, PPCContext& ctx) {
  if (!g_discover || n++ >= 6) return;
  RT_INFO("bot players: %s:%s", what, Args(ctx).c_str());
}

std::atomic<int> g_joins{0}, g_peers{0}, g_deploys{0}, g_inputs{0}, g_damages{0};

// BF1943's classes for the bot players (Bad Company's in brackets).
constexpr uint32_t kSpawnObject = 0x8201A990;     // the player's soldier spawner (82019490)
constexpr uint32_t kConnectionPart = 0x8201C728;  // a joined player's connection part (8201B140)
constexpr uint32_t kPeer = 0x82056C00;            // a network peer (820550C0): the part's +14 and +28
constexpr uint32_t kSoldier = 0x82013A40;         // ServerSoldierEntity (820118D0)
constexpr uint32_t kNetPlayer = 0x8201C6C8;       // a player's network object: +28 its connection part,
                                                  // +2FC its move receiver (823B5330 -> 823B2C80)
// The spawn object: +30 its team, +120 its soldier once deployed, +1F4 and
// +1F8 the kit the spawn screen chose (SoldierEntityData, SoldierWeaponData;
// Bad Company +1D4, +1D8).

// Every live object with this vtable in the heaps.
std::vector<uint32_t> Scan(uint32_t vtable) {
  std::vector<uint32_t> out;
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    for (uint32_t i = 0; i < 0x1000; i += 4)
      if (Ld32(uint32_t(page) + i) == vtable) out.push_back(uint32_t(page) + i);
  }
  return out;
}
float LdF(uint32_t a) {
  uint32_t u = Ld32(a);
  float f;
  memcpy(&f, &u, 4);
  return f;
}
uint32_t FindNetPlayer(uint32_t conn) {
  for (uint32_t o : Scan(kNetPlayer))
    if (Ld32(o + 0x28) == conn) return o;
  return 0;
}
// A float as an IEEE half (for the hold times; small positive values).
uint16_t HalfFloat(float f) {
  if (f <= 0) return 0;
  uint32_t u;
  memcpy(&u, &f, 4);
  int e = int((u >> 23) & 0xFF) - 127 + 15;
  if (e <= 0) return 0;
  if (e >= 31) return 0x7BFF;
  return uint16_t((e << 10) | ((u >> 13) & 0x3FF));
}
template <class T>
bool Has(const std::vector<T>& v, T x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}

// A bot player: joined as a fake network player (no client, no traffic), with
// its own spawn object.
struct BotPlayer {
  int index = 0;
  uint32_t team = 0;
  uint32_t conn = 0;     // its connection part
  uint32_t spawner = 0;  // its spawn object
  uint32_t soldier = 0;
  int64_t died_ms = 0;
  uint32_t move_msg = 0;  // its own move message (a copy of a real one)
  uint32_t mover = 0;     // its move receiver (823B2C80's r3)
  uint32_t ghost_id = 0;  // its soldier's network id in the host's table (0: not found yet)
  // The brain's pad (SetBotPlayerInput), applied in its soldier's step.
  float lx = 0, ly = 0, rx = 0, ry = 0;
  uint32_t buttons = 0, triggers = 0;
  // Where it aims (the move records' +88 is minus the yaw, +8C the pitch),
  // turned by rx / ry: written into every record it is driven by.
  float heading = 0, pitch = 0;
  bool aim_set = false;
  uint64_t bits = 0;  // the button bits of its last step (the move messages carry them: entering a vehicle is one)
  float held[64] = {};
  int e_steps = 0;  // steps B has been held
  bool press13 = false;  // a ChangeVehicle press not yet seen by 822BD2B0  // how long each digital action has been held (s)
  int64_t e_until = 0;
  uint32_t ghost_for = 0; // the soldier that id is for
  uint32_t move_seq = 0;
};
// A player's move receiver (823B2C80's r3): +14 1E, +18 3, +30 its spawn
// object, then a ring of move numbers (-1 when empty).
uint32_t FindMover(uint32_t spawner) {
  for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
    if (page >= 0x80000000 && page < 0xA0000000) continue;
    if (!IsCommitted(uint32_t(page))) continue;
    for (uint32_t i = 0x30; i < 0x1000; i += 4) {
      uint32_t a = uint32_t(page) + i;
      if (Ld32(a) != spawner) continue;
      uint32_t o = a - 0x30;
      if (Ld32(o + 0x14) == 0x1E && Ld32(o + 0x18) == 3) return o;
    }
  }
  return 0;
}
// A real player's move message (823B2C80's r4: vtable 8200E490, +4 its
// number, +10 the input record, vtable 8200E48C: +8 forward ...), kept as
// the template for the bots'.
uint8_t g_move_template[0x100];
bool g_have_template = false;
uint32_t g_real_mover = 0;  // a real player's move receiver (to clone for the bots)
std::deque<BotPlayer> g_players;
const int g_fake_count = getenv("BF_BOT_FAKE") ? std::clamp(atoi(getenv("BF_BOT_FAKE")), 1, 16) : 0;

}  // namespace

// Words around a guest address (vtable-looking ones with their type names).
std::string Around(uint32_t a, int32_t from, int32_t to) {
  std::string out;
  for (int32_t off = from; off < to; off += 4) {
    uint32_t p = a + off;
    if (!IsCommitted(p)) continue;
    uint32_t v = Ld32(p);
    std::string tn = TypeName(v);
    char buf[96];
    snprintf(buf, sizeof(buf), " %+X:%08X%s%s", off, v, tn.empty() ? "" : ":", tn.c_str());
    out += buf;
  }
  return out;
}

int rt::BotPlayerCount() { return int(g_players.size()); }
uint32_t rt::BotPlayerSoldier(int i) {
  if (i < 0 || i >= int(g_players.size())) return 0;
  uint32_t so = g_players[size_t(i)].soldier;
  return so && IsCommitted(so + 0x400) && Ld32(so) == kSoldier ? so : 0;
}
void rt::SetBotPlayerInput(int i, float lx, float ly, float rx, float ry, uint32_t buttons, uint32_t triggers) {
  if (i < 0 || i >= int(g_players.size())) return;
  BotPlayer& b = g_players[size_t(i)];
  b.lx = lx, b.ly = ly, b.rx = rx, b.ry = ry, b.buttons = buttons, b.triggers = triggers;
}

extern "C" PPC_FUNC(__imp__sub_827559F0);
GAME_PATCH(823B59E0) {  // the player manager's join
  if (g_discover && g_joins < 6) {
    RT_INFO("bot players: join r4 %08X:%s", ctx.r4.u32, Around(ctx.r4.u32, -0x10, 0x60).c_str());
    RT_INFO("bot players: join r5 %08X:%s", ctx.r5.u32, Around(ctx.r5.u32, 0, 0x40).c_str());
  }
  Note("join", g_joins, ctx);
  // BF_BOT_FAKE=N: before the first join (the host's own client), N players
  // joined with copies of its connection object (0x60 bytes from r4 - 4; r4
  // its address object, vtable 82001FF0: family 2, the port at +6, the IP at
  // +8), each on port 1599 + i (nobody's) and on the join's working record
  // (r5 and what follows) as it was; then each one's network peer, as after a
  // real join: 827559F0 on the peer manager (the player manager + 8) with the
  // player's index (7 + i), 1 and its connection part.
  static bool done = false;
  if (g_fake_count && !done) {
    done = true;
    const char* team_env = getenv("BF_BOT_TEAM");
    int one_team = team_env ? atoi(team_env) : 0;
    std::vector<uint8_t> snap(0x800);
    memcpy(snap.data(), G<uint8_t>(ctx.r5.u32), snap.size());
    for (int i = 0; i < g_fake_count; ++i) {
      BotPlayer& b = g_players.emplace_back();
      b.index = i;
      b.team = one_team == 1 || one_team == 2 ? uint32_t(one_team) : uint32_t(i % 2 + 1);
      uint32_t conn = g_system_heap->Alloc(0x60, 16);
      memcpy(G<uint8_t>(conn), G<uint8_t>(ctx.r4.u32 - 4), 0x60);
      uint32_t addr = conn + 4;
      St16(addr + 6, uint16_t(1599 + i));
      memcpy(G<uint8_t>(ctx.r5.u32), snap.data(), snap.size());
      PPCContext c = ctx;
      c.r4.u64 = addr;
      std::vector<uint32_t> conn_before = Scan(kConnectionPart), spawn_before = Scan(kSpawnObject);
      __imp__sub_823B59E0(c, base);
      for (uint32_t o : Scan(kSpawnObject))
        if (!Has(spawn_before, o)) b.spawner = o;
      for (uint32_t o : Scan(kConnectionPart)) {
        if (Has(conn_before, o)) continue;
        PPCContext pc = ctx;
        pc.r3.u64 = ctx.r3.u32 + 8;
        pc.r4.u64 = uint64_t(7 + i);
        pc.r5.u64 = 1;
        pc.r6.u64 = o;
        // Its peer, linked at the part's +14 and +28 as a real player's once
        // the handshake is done (which never comes for a bot: without it the
        // server's update crashes in 823A2378, as Bad Company's in 8234E390).
        std::vector<uint32_t> peers_before = Scan(kPeer);
        __imp__sub_827559F0(pc, base);
        for (uint32_t peer : Scan(kPeer)) {
          if (Has(peers_before, peer)) continue;
          if (!Ld32(o + 0x14)) St32(o + 0x14, peer);
          if (!Ld32(o + 0x28)) St32(o + 0x28, peer);
        }
        // EXPERIMENT (BF_BOT_READY): a real player's part after the handshake
        // also has +10 | 01000000, +38 010B0003 and +3C 1 (its soldier only
        // gets updated then?).
        if (getenv("BF_BOT_READY")) {
          St32(o + 0x10, Ld32(o + 0x10) | 0x01000000);
          St32(o + 0x38, 0x010B0003);
          St32(o + 0x3C, 1);
        }
        b.conn = o;
      }
      RT_INFO("bot %d: joined at 127.0.0.1:%d (team %u): connection %08X, spawn object %08X", i, 1599 + i, b.team,
              b.conn, b.spawner);
    }
    memcpy(G<uint8_t>(ctx.r5.u32), snap.data(), snap.size());
    StartBotDirector(0);  // the brain drives them (bots.cpp), as the bot copies
  }
  __imp__sub_823B59E0(ctx, base);
}
GAME_PATCH(827559F0) {  // the peer manager's create
  Note("create peer", g_peers, ctx);
  uint32_t conn = ctx.r6.u32;
  bool show = g_discover && g_peers <= 6;
  __imp__sub_827559F0(ctx, base);
  // BF_BOT_DISCOVER: a real joiner's network object, its +2FC (the move
  // receiver) watched: who stores it (the handshake).
  if (g_discover && Ld32(conn) == kConnectionPart) {
    bool bot = false;
    for (BotPlayer& b : g_players) bot |= b.conn == conn;
    uint32_t np = FindNetPlayer(conn);
    RT_INFO("bot players: %s connection %08X: network object %08X, +2FC %08X", bot ? "bot" : "real", conn, np,
            np ? Ld32(np + 0x2FC) : 0);
    static bool watching = false;
    if (!bot && np && !watching && g_peers >= 3) {
      watching = true;
      StartWriteWatch(np + 0x2FC, 4, 0);
    }
  }
  // What it left in the player's connection part (Bad Company: the peer at +14 and +28),
  // and again 20 s later (after the handshake).
  if (show && IsCommitted(conn)) {
    RT_INFO("bot players: after create peer, connection %08X:%s", conn, Around(conn, 0, 0x40).c_str());
    std::thread([conn] {
      std::this_thread::sleep_for(std::chrono::seconds(20));
      RT_INFO("bot players: 20 s after create peer, connection %08X:%s", conn, Around(conn, 0, 0x40).c_str());
      for (uint32_t off : {0x14u, 0x28u, 0x18u, 0x38u}) {
        uint32_t p = Ld32(conn + off);
        if (p >= 0x40000000 && p < 0xC0000000 && IsCommitted(p))
          RT_INFO("bot players:   +%X -> %08X:%s", off, p, Around(p, 0, 0x20).c_str());
      }
    }).detach();
  }
}
GAME_PATCH(82382720) {  // deploy a soldier
  Note("deploy", g_deploys, ctx);
  __imp__sub_82382720(ctx, base);
}
GAME_PATCH(822ED8D8) {  // a player's received input to its soldier
  Note("input", g_inputs, ctx);
  // A bot soldier's input: written into the record before it is copied into
  // the soldier's input state (r3: vtable 82012320, +4 the soldier, +14
  // forward; r4 the record: +8 forward), as Bad Company's bots.
  if (Ld32(ctx.r3.u32) == 0x82012320) {
    uint32_t so = Ld32(ctx.r3.u32 + 4);
    for (BotPlayer& b : g_players)
      if (b.soldier && b.soldier == so && (g_fake_count > 0)) {
        // The whole record from a moving player's (its frame time +80/+84,
        // heading +88, +90 ...), keeping its network id; then our input.
        if (g_have_template && getenv("BF_BOT_FULL_RECORD")) {
          uint32_t id = Ld32(ctx.r4.u32 + 0x9C);
          memcpy(G<uint8_t>(ctx.r4.u32 + 4), g_move_template + 0x10 + 4, 0xB0);
          St32(ctx.r4.u32 + 0x9C, id);
        }
        float fwd = 0.8f;
        uint32_t u;
        memcpy(&u, &fwd, 4);
        St32(ctx.r4.u32 + 8, u);
        static int64_t next = 0;
        int64_t t = int64_t(GetTickCount64());
        if (t >= next) {
          next = t + 3000;
          uint32_t is = ctx.r3.u32, rec = ctx.r4.u32;
          __imp__sub_822ED8D8(ctx, base);
          RT_INFO("bot %d: input copied: state +14 %.2f, record %08X (vt %08X) +8 %.2f", b.index, LdF(is + 0x14), rec,
                  Ld32(rec), LdF(rec + 8));
          // The bot's soldier against a real one: words that are small
          // numbers (flags, states) and differ.
          for (uint32_t real : Scan(kSoldier)) {
            if (real == b.soldier) continue;
            std::string d;
            for (uint32_t off = 0; off < 0x600; off += 4) {
              uint32_t x = Ld32(b.soldier + off), y = Ld32(real + off);
              if (x == y || x > 0xFFFF0000u || y > 0xFFFF0000u) continue;
              if ((x < 0x10000 && y < 0x10000) || (x >> 24) == 0x01 || (y >> 24) == 0x01) {
                char buf[40];
                snprintf(buf, sizeof(buf), " +%X:%X/%X", off, x, y);
                d += buf;
              }
            }
            RT_INFO("bot %d: its soldier vs %08X (small words differing, bot/real):%s", b.index, real, d.c_str());
            break;
          }
          return;
        }
      }
  }
  // BF_BOT_DISCOVER: every 3 s, a real player's received record (r4) as
  // floats, with its input state's soldier (r3 +4) and where it is, and
  // where the bot players' soldiers are.
  static int64_t next = 0;
  int64_t now = int64_t(GetTickCount64());
  if (g_discover && now >= next && Ld32(ctx.r3.u32) == 0x82012320) {
    uint32_t so = Ld32(ctx.r3.u32 + 4);
    bool bot = false;
    for (BotPlayer& b : g_players) bot |= b.soldier == so;
    if (!bot) {
      next = now + 3000;
      std::string f;
      for (uint32_t off = 0; off < 0xB8; off += 4) {
        float v = LdF(ctx.r4.u32 + off);
        uint32_t u = Ld32(ctx.r4.u32 + off);
        char buf[40];
        if (u && std::isfinite(v) && std::fabs(v) > 1e-4f && std::fabs(v) < 1e4f) snprintf(buf, sizeof(buf), " %X:%.2f", off, v);
        else if (u) snprintf(buf, sizeof(buf), " %X:%08X", off, u);
        else buf[0] = 0;
        f += buf;
      }
      RT_INFO("bot players: input of soldier %08X (vt %08X) at %.1f %.1f %.1f:%s", so, Ld32(so), LdF(so + 0x180),
              LdF(so + 0x184), LdF(so + 0x188), f.c_str());
      for (BotPlayer& b : g_players)
        if (b.soldier)
          RT_INFO("bot players: bot %d soldier at %.1f %.1f %.1f", b.index, LdF(b.soldier + 0x180), LdF(b.soldier + 0x184),
                  LdF(b.soldier + 0x188));
    }
  }
  __imp__sub_822ED8D8(ctx, base);
}
// BF_BOT_DISCOVER: 60 s in, every spawn object's first 0x220 bytes, and the
// connection parts, to see which spawn object is whose.
const bool g_spawn_dump = [] {
  if (!g_discover) return false;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(60));
    for (uint32_t o : Scan(kConnectionPart)) RT_INFO("bot players: connection part %08X", o);
    for (uint32_t o : Scan(kSpawnObject)) {
      RT_INFO("bot players: spawn object %08X:%s", o, Around(o, 0, 0x220).c_str());
      std::string named;
      for (uint32_t off = 0; off < 0x300; off += 4) {
        uint32_t p = Ld32(o + off);
        if (p < 0x40000000 || p >= 0xC0000000 || (p & 3) || !IsCommitted(p)) continue;
        std::string tn = TypeName(Ld32(p));
        if (tn.empty()) continue;
        char buf[96];
        snprintf(buf, sizeof(buf), " +%X->%08X:%s", off, p, tn.c_str());
        named += buf;
      }
      RT_INFO("bot players:   its fields pointing at named objects:%s", named.c_str());
    }
  }).detach();
  return true;
}();

// The message after a join that gives a player its spawn object (Bad
// Company's 82359A58; found by its callees, tools/match_by_callees.py): r3 the
// player's connection part, r4 a record (+8 begin, +C end, +10 capacity of a
// small payload; +28 a pointer), r5 0x21, r6 1. BF_BOT_FAKE: when the host's
// arrives, the same message (a copy of the record and its payload) is given to
// each bot player's connection part, which makes it its spawn object.
GAME_PATCH(823AEF90) {
  static std::atomic<int> n{0};
  if (g_discover && n < 6) {
    Note("spawn message", n, ctx);
    RT_INFO("bot players:   its r4 %08X:%s", ctx.r4.u32, Around(ctx.r4.u32, 0, 0x40).c_str());
  }
  PPCContext saved = ctx;
  __imp__sub_823AEF90(ctx, base);
  static bool replayed = false;
  bool from_bot = false;
  for (BotPlayer& b : g_players) from_bot |= b.conn == saved.r3.u32;
  if (g_players.empty() || replayed || from_bot) return;
  replayed = true;
  uint32_t rec = saved.r4.u32;
  uint32_t b0 = Ld32(rec + 8), e0 = Ld32(rec + 0xC);
  uint32_t len = e0 >= b0 && e0 - b0 < 0x1000 ? e0 - b0 : 0;
  for (BotPlayer& b : g_players) {
    if (!b.conn) continue;
    uint32_t copy = g_system_heap->Alloc(0x40, 16);
    uint32_t payload = g_system_heap->Alloc(0x1000, 16);
    memcpy(G<uint8_t>(copy), G<uint8_t>(rec), 0x40);
    if (len) memcpy(G<uint8_t>(payload), G<uint8_t>(b0), len);
    St32(copy + 8, payload), St32(copy + 0xC, payload + len), St32(copy + 0x10, payload + len + 1);
    PPCContext c = saved;
    c.r3.u64 = b.conn;
    c.r4.u64 = copy;
    std::vector<uint32_t> spawners = Scan(kSpawnObject);
    __imp__sub_823AEF90(c, base);
    for (uint32_t o : Scan(kSpawnObject))
      if (!Has(spawners, o)) b.spawner = o;
    RT_INFO("bot %d: the host's message (%u bytes) replayed on its connection %08X; spawn object %08X", b.index, len,
            b.conn, b.spawner);
  }
}

GAME_PATCH(8237EE38) {  // the spawn object's constructor (Bad Company's 8232ECB0)
  static std::atomic<int> n{0};
  if (g_discover && n++ < 3) {
    RT_INFO("bot players: spawn object made:%s", Args(ctx).c_str());
    bf_crash_diag::PrintStack(nullptr);
  }
  __imp__sub_8237EE38(ctx, base);
}
// The server's update (Bad Company's 823D1DD0, 0.91 alike): once a second,
// each bot player without a soldier is deployed (8 s after a death): its
// spawn object's team (+30), a kit borrowed from a deployed player's spawn
// object (+1F4, +1F8), then deploy (82382720) with no spawn point (r4 0: the
// game picks one of the team's).
extern "C" PPC_FUNC(__imp__sub_82382720);
extern "C" PPC_FUNC(__imp__sub_823B2C80);
PPC_FUNC(sub_8275D2F8);
// A move record names the entity it moves by its network id (+9C; 823B2C80
// reads it with 8251A288), looked up per connection: 8275D2F8(*(receiver),
// id) -> the entity + 80. A bot's own connection never gets its soldier (no
// client): the host's local client's table has it (the dedicated server's
// own player sees every soldier).
uint32_t HostTable() {
  for (uint32_t o : Scan(kNetPlayer)) {
    bool bot = false;
    for (BotPlayer& b : g_players) bot |= Ld32(o + 0x28) == b.conn;
    uint32_t rcv = bot ? 0 : Ld32(o + 0x2FC);
    if (rcv && IsCommitted(rcv) && Ld32(rcv)) return Ld32(rcv);
  }
  return 0;
}
uint32_t GhostId(PPCContext& ctx, uint8_t* base, uint32_t table, uint32_t entity) {
  for (uint32_t id = 1; id < 0x4000; ++id) {
    PPCContext c = ctx;
    c.r3.u64 = table;
    c.r4.u64 = id;
    sub_8275D2F8(c, base);
    if (c.r3.u32 == entity + 80) return id;
  }
  return 0;
}
GAME_PATCH(82414430) {
  __imp__sub_82414430(ctx, base);
  if (g_players.empty()) return;
  int64_t now = int64_t(GetTickCount64());
  static int64_t last = 0;
  bool second = now - last >= 1000;
  if (second) last = now;
  uint32_t kit = 0, kit2 = 0;
  if (second)
    for (uint32_t o : Scan(kSpawnObject)) {
      bool bots = false;
      for (BotPlayer& b : g_players) bots |= b.spawner == o;
      if (!bots && Ld32(o + 0x1F4)) kit = Ld32(o + 0x1F4), kit2 = Ld32(o + 0x1F8);
    }
  // EXPERIMENT (BF_BOT_DRIVE): a move message for each bot soldier every
  // tick, as its client would send (the server simulates a soldier per move
  // message): 823B2C80 on its spawn object + 180.
  static const bool drive = g_fake_count > 0;
  // Without a moving player to copy (a server of bots): a move message made
  // from what a real one holds (vtables, the button words +78/+7C, +80/+84,
  // the 1.0 at +90).
  if (drive && !g_have_template) {
    auto put32 = [](uint8_t* p, uint32_t off, uint32_t v) {
      v = _byteswap_ulong(v);
      memcpy(p + off, &v, 4);
    };
    memset(g_move_template, 0, sizeof(g_move_template));
    put32(g_move_template, 0, 0x8200E490);
    put32(g_move_template, 0x10, 0x8200E48C);
    put32(g_move_template, 0x10 + 0x78,
          getenv("BF_BOT_TPL78") ? uint32_t(strtoul(getenv("BF_BOT_TPL78"), nullptr, 16)) : 0x900);
    put32(g_move_template, 0x10 + 0x7C, 0x00400000);
    put32(g_move_template, 0x10 + 0x80, 0x3C23D70A);  // 0.01
    put32(g_move_template, 0x10 + 0x84, 0x3C23D70A);
    put32(g_move_template, 0x10 + 0x90, 0x3F800000);  // 1.0
    g_have_template = true;
  }
  if (drive && g_have_template)
    for (BotPlayer& b : g_players) {
      if (!b.soldier || Ld32(b.soldier) != kSoldier) continue;
      if (!b.mover) {
        // Its network object's +2FC (made with the join).
        if (uint32_t np = FindNetPlayer(b.conn)) b.mover = Ld32(np + 0x2FC);
        if (b.mover)
          RT_INFO("bot %d: move receiver %08X from its network object:%s", b.index, b.mover,
                  Around(b.mover, 0, 0x40).c_str());
        if (!b.mover) b.mover = FindMover(b.spawner);
        // None (made in a real client's handshake): a copy of a real one,
        // pointing at the bot's spawn object, its move ring and numbers clear.
        if (!b.mover && g_real_mover && getenv("BF_BOT_CLONE_MOVER")) {
          b.mover = g_system_heap->Alloc(0x200, 16);
          memcpy(G<uint8_t>(b.mover), G<uint8_t>(g_real_mover), 0x200);
          St32(b.mover + 0x30, b.spawner);
          for (uint32_t k = 0x34; k < 0xB4; k += 4) St32(b.mover + k, 0xFFFFFFFF);
          St32(b.mover + 180, 0), St32(b.mover + 184, 0), St32(b.mover + 192, 0);
        }
        RT_INFO("bot %d: move receiver %08X:%s", b.index, b.mover, b.mover ? Around(b.mover, 0, 0x40).c_str() : "");
        if (!b.mover) continue;
      }
      if (!b.move_msg) b.move_msg = g_system_heap->Alloc(0x100, 16);
      if (b.ghost_for != b.soldier) {
        uint32_t table = HostTable();
        b.ghost_id = table ? GhostId(ctx, base, table, b.soldier) : 0;
        b.ghost_for = b.soldier;
        RT_INFO("bot %d: soldier %08X is network id %X in the host's table %08X", b.index, b.soldier, b.ghost_id, table);
        if (b.ghost_id) St32(b.mover, table);  // look it up there
      }
      if (!b.ghost_id) continue;
      memcpy(G<uint8_t>(b.move_msg), g_move_template, sizeof(g_move_template));
      St32(b.move_msg + 4, ++b.move_seq);
      float fwd = 0.8f;
      uint32_t u;
      memcpy(&u, &fwd, 4);
      St32(b.move_msg + 0x10 + 8, u);
      St32(b.move_msg + 0x10 + 0x9C, b.ghost_id);
      if (b.aim_set) {
        auto putf = [](uint32_t a, float v) {
          uint32_t w;
          memcpy(&w, &v, 4);
          St32(a, w);
        };
        putf(b.move_msg + 0x10 + 0x88, b.heading);
        putf(b.move_msg + 0x10 + 0x8C, b.pitch);
        putf(b.move_msg + 0x10 + 0x90, 1.0f);
        putf(b.move_msg + 0x10 + 8, b.ly);
        putf(b.move_msg + 0x10 + 0xC, b.lx);
        uint64_t m = b.bits ? b.bits : (uint64_t(1) << 40 | uint64_t(1) << 43);
        St32(b.move_msg + 0x10 + 0x78, uint32_t(m >> 32)), St32(b.move_msg + 0x10 + 0x7C, uint32_t(m));
      }
      // EXPERIMENT (BF_BOT_CONTROL): the receiver's +118 is the entity it
      // controls (823B2C80 compares it with the one the move names).
      static int64_t said = 0;
      if (now - said > 5000) {
        said = now;
        RT_INFO("bot %d: receiver +118 %08X (vt %08X), its soldier %08X", b.index, Ld32(b.mover + 0x118),
                IsCommitted(Ld32(b.mover + 0x118)) ? Ld32(Ld32(b.mover + 0x118)) : 0, b.soldier);
      }
      if (getenv("BF_BOT_CONTROL")) St32(b.mover + 0x118, b.soldier);
      // The entry it is in (+11C: 0 its own, as a real player on foot; FFFF
      // is "none", and then the move handler enters nothing).
      static int64_t said11c = 0;
      if (now - said11c > 5000) {
        said11c = now;
        RT_INFO("bot %d: receiver +118 %08X +11C %08X", b.index, Ld32(b.mover + 0x118), Ld32(b.mover + 0x11C));
      }
      if (Ld32(b.mover + 0x118) == b.soldier && Ld32(b.mover + 0x11C) == 0xFFFF && !getenv("BF_BOT_NO_11C"))
        St32(b.mover + 0x11C, 0);
      // EXPERIMENT (BF_BOT_YAW_SPIN): turn the soldier through its aim (the
      // character entity, soldier +130: heading +134), a little each tick.
      if (getenv("BF_BOT_YAW_SPIN")) {
        uint32_t aim = Ld32(b.soldier + 0x130);
        if (aim && IsCommitted(aim + 0x140)) {
          float yaw = LdF(aim + 0x134) + 0.02f;
          uint32_t yu;
          memcpy(&yu, &yaw, 4);
          St32(aim + 0x134, yu);
        }
      }
      // BF_BOT_VEC_WATCH: the 16-byte vectors at the bot soldier's position,
      // read again 4 s later: does anything (its physics) move?
      static std::vector<uint32_t> vecs;
      static int64_t vec_t = 0;
      if (getenv("BF_BOT_VEC_WATCH")) {
        float px = LdF(b.soldier + 0x180), py = LdF(b.soldier + 0x184), pz = LdF(b.soldier + 0x188);
        if (vecs.empty() && px != 0) {
          for (uint64_t page = 0x40000000; page < 0xC0000000; page += 0x1000) {
            if (page >= 0x80000000 && page < 0xA0000000) continue;
            if (!IsCommitted(uint32_t(page))) continue;
            for (uint32_t k = 0; k < 0x1000; k += 16) {
              uint32_t va = uint32_t(page) + k;
              if (std::fabs(LdF(va) - px) < 0.05f && std::fabs(LdF(va + 4) - py) < 0.5f && std::fabs(LdF(va + 8) - pz) < 0.05f)
                vecs.push_back(va);
            }
          }
          vec_t = now;
          RT_INFO("bot %d: %zu vectors at its position %.1f %.1f %.1f", b.index, vecs.size(), px, py, pz);
        } else if (!vecs.empty() && now - vec_t > 4000 && vec_t) {
          vec_t = 0;
          std::string moved;
          for (uint32_t va : vecs) {
            float dx = LdF(va) - px, dz = LdF(va + 8) - pz;
            if (std::fabs(dx) > 0.05f || std::fabs(dz) > 0.05f) {
              char buf[64];
              snprintf(buf, sizeof(buf), " %08X(%.1f %.1f)", va, LdF(va), LdF(va + 8));
              moved += buf;
            }
          }
          RT_INFO("bot %d: 4 s later, moved vectors:%s", b.index, moved.empty() ? " none" : moved.c_str());
        }
      }
      // BF_BOT_CHAR: the character entities (820332B8) that point at a
      // soldier (within 0x200), the bot's and the real ones, with their words.
      static int64_t char_said = 0;
      if (getenv("BF_BOT_CHAR") && now - char_said > 8000) {
        char_said = now;
        for (uint32_t ce : Scan(0x820332B8)) {
          for (uint32_t off = 0; off < 0x200; off += 4) {
            uint32_t v = Ld32(ce + off);
            if (v < 0x40000000 || v >= 0xC0000000 || (v & 3) || !IsCommitted(v) || Ld32(v) != kSoldier) continue;
            RT_INFO("bot players: character %08X%s +%X -> soldier %08X:%s", ce, v == b.soldier ? " (bot)" : "", off, v,
                    Around(ce, 0, 0x100).c_str());
            break;
          }
        }
        // what sits at soldier + 1000 (the real one's character points there, +A4)
        for (uint32_t so : Scan(kSoldier)) {
          uint32_t o = so + 0x1000;
          RT_INFO("bot players: soldier %08X%s + 1000 (vt %08X %s):%s", so, so == b.soldier ? " (bot)" : "", Ld32(o),
                  TypeName(Ld32(o)).c_str(), Around(o, 0, 0x60).c_str());
        }
        // and the soldier's pointers to it
        for (uint32_t off = 0; off < 0x600; off += 4) {
          uint32_t v = Ld32(b.soldier + off);
          if (v >= 0x40000000 && v < 0xC0000000 && IsCommitted(v) && Ld32(v) == 0x820332B8)
            RT_INFO("bot players: bot soldier +%X -> character %08X", off, v);
        }
      }
      // EXPERIMENT (BF_BOT_FLAGS): a walking soldier's +34C ends in 00 (the
      // bot's 56) and its +21C is 0 (the bot's 1D00).
      if (getenv("BF_BOT_FLAGS")) St32(b.soldier + 0x34C, Ld32(b.soldier + 0x34C) & 0xFFFFFF00), St32(b.soldier + 0x21C, 0);
      // EXPERIMENT (BF_BOT_STATE6): a walking soldier has 6 at +1BC, +224
      // and +228 where the bot's has 0, 1 and 1.
      if (getenv("BF_BOT_STATE6")) St32(b.soldier + 0x1BC, 6), St32(b.soldier + 0x224, 6), St32(b.soldier + 0x228, 6);
      PPCContext c = ctx;
      c.r3.u64 = b.mover;
      c.r4.u64 = b.move_msg;
      c.r5.u64 = 1;
      c.r6.u64 = 0;
      __imp__sub_823B2C80(c, base);
      static int64_t next_log = 0;
      if (now >= next_log) {
        next_log = now + 2000;
        if (g_discover)
          for (uint32_t is : Scan(0x82012320))
            RT_INFO("bot players: input state %08X soldier %08X%s:%s", is, Ld32(is + 4),
                    Ld32(is + 4) == b.soldier ? " (bot)" : "", Around(is, 4, 0x40).c_str());
        if (getenv("BF_BOT_RCV_DUMP")) {
          RT_INFO("bot %d: its receiver %08X:%s", b.index, b.mover, Around(b.mover, 0xB0, 0x130).c_str());
          if (g_real_mover) RT_INFO("bot %d: a real receiver %08X:%s", b.index, g_real_mover, Around(g_real_mover, 0xB0, 0x130).c_str());
        }
        RT_INFO("bot %d: heading %.2f pitch %.2f", b.index, LdF(Ld32(b.soldier + 0x130) + 0x134),
                LdF(Ld32(b.soldier + 0x130) + 0x138));
        RT_INFO("bot %d: driven soldier %08X at %.1f %.1f %.1f (move %u)", b.index, b.soldier, LdF(b.soldier + 0x180),
                LdF(b.soldier + 0x184), LdF(b.soldier + 0x188), b.move_seq);
      }
    }
  // No player deployed yet (a server of bots): the level's own kit data.
  if (second && !kit) {
    std::vector<uint32_t> se = Scan(0x8205AD34), sw = Scan(0x8205F284);
    if (!se.empty() && !sw.empty()) kit = se[0], kit2 = sw[0];
  }
  if (!kit) return;
  for (BotPlayer& b : g_players) {
    // Its move receiver (its network object's +2FC, made with the join) and
    // the spawn object that one drives (+30): deploy on that one, so the
    // move messages reach the soldier.
    if (!b.mover)
      if (uint32_t np = FindNetPlayer(b.conn)) {
        b.mover = Ld32(np + 0x2FC);
        uint32_t sp = b.mover ? Ld32(b.mover + 0x30) : 0;
        RT_INFO("bot %d: network object %08X, receiver %08X, its spawn object %08X (vt %08X), ours %08X", b.index, np,
                b.mover, sp, sp && IsCommitted(sp) ? Ld32(sp) : 0, b.spawner);
        if (sp && IsCommitted(sp) && Ld32(sp) == kSpawnObject && sp != b.spawner) {
          RT_INFO("bot %d: its move receiver %08X drives spawn object %08X (deploying there, not %08X)", b.index,
                  b.mover, sp, b.spawner);
          b.spawner = sp;
        }
      }
    if (!b.spawner) continue;
    bool alive = b.soldier && IsCommitted(b.soldier + 0x400) && Ld32(b.soldier) == kSoldier;
    if (alive) continue;
    if (b.soldier) {
      RT_INFO("bot %d: its soldier %08X is gone", b.index, b.soldier);
      b.soldier = 0, b.died_ms = now;
    }
    if (b.died_ms && now - b.died_ms < 8000) continue;
    St32(b.spawner + 0x30, b.team);
    St32(b.spawner + 0x1F4, kit), St32(b.spawner + 0x1F8, kit2);
    std::vector<uint32_t> before = Scan(kSoldier);
    PPCContext c = ctx;
    c.r3.u64 = b.spawner;
    c.r4.u64 = 0;
    c.r5.u64 = 1;
    c.r6.u64 = 1;
    __imp__sub_82382720(c, base);
    for (uint32_t o : Scan(kSoldier))
      if (!Has(before, o)) b.soldier = o;
    b.died_ms = b.soldier ? 0 : now;
    static bool watched = false;  // BF_BOT_WATCH_POS (testing): who writes the bot soldier's position
    if (b.soldier && !watched && getenv("BF_BOT_WATCH_POS")) watched = true, StartWriteWatch(b.soldier + 0x180, 12, 0);
    if (g_discover && b.soldier) {
      uint32_t sp = Ld32(b.spawner + 0x1F0), so = b.soldier;
      bool ptr = sp >= 0x40000000 && sp < 0xC0000000 && IsCommitted(sp);
      RT_INFO("bot %d: spawn point +1F0 %08X (vt %08X %s); spawner:%s", b.index, sp, ptr ? Ld32(sp) : 0,
              ptr ? TypeName(Ld32(sp)).c_str() : "", Around(b.spawner, 0x100, 0x130).c_str());
      std::thread([so, i = b.index] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        RT_INFO("bot %d: 5 s later soldier %08X at %.1f %.1f %.1f, vt %08X", i, so, LdF(so + 0x180), LdF(so + 0x184),
                LdF(so + 0x188), Ld32(so));
      }).detach();
    }
    RT_INFO("bot %d: deployed on team %u: soldier %08X (team %u) at %.1f %.1f %.1f", b.index, b.team, b.soldier,
            b.soldier ? Ld32(b.soldier + 0x48) : 0, b.soldier ? LdF(b.soldier + 0x180) : 0.0f,
            b.soldier ? LdF(b.soldier + 0x184) : 0.0f, b.soldier ? LdF(b.soldier + 0x188) : 0.0f);
  }
}

// BF_BOT_DISCOVER: the call stacks that make soldiers (the two functions that
// set ServerSoldierEntity's vtable), the bots' and a real player's.
GAME_PATCH(8231C770) {
  static std::atomic<int> n{0};
  if (g_discover && n++ < 4) {
    RT_INFO("bot players: soldier made (8231C770):%s", Args(ctx).c_str());
    bf_crash_diag::PrintStack(nullptr);
  }
  __imp__sub_8231C770(ctx, base);
}
GAME_PATCH(8231CD38) {
  static std::atomic<int> n{0};
  if (g_discover && n++ < 4) {
    RT_INFO("bot players: soldier made (8231CD38):%s", Args(ctx).c_str());
    bf_crash_diag::PrintStack(nullptr);
  }
  __imp__sub_8231CD38(ctx, base);
}

// A real player's soldier is simulated per input message from its client:
// 8274BDA8 (dispatch) -> 827600A8 -> 823B2C80 -> 822F05C8 -> the soldier's
// update (82308978). BF_BOT_DISCOVER: their arguments.
// The stream reader before it (8274BDA8 -> 827600A8): its this, a channel
// of the connection. BF_BOT_DISCOVER: what it holds.
GAME_PATCH(827600A8) {
  static std::atomic<int> n{0};
  if (g_discover && n == 0) {
    std::string v;
    for (uint32_t k = 0; k < 0x40; k += 4) {
      char buf[24];
      snprintf(buf, sizeof(buf), " +%X:%08X", k, Ld32(0x8201C6C8 + k));
      v += buf;
    }
    RT_INFO("bot players: vtable 8201C6C8:%s", v.c_str());
  }
  if (g_discover && n++ < 3) {
    uint32_t ch = ctx.r3.u32;
    RT_INFO("bot players: channel %08X (vt %08X):%s", ch, Ld32(ch), Around(ch, 0, 0x90).c_str());
    for (uint32_t off : {48u, 52u, 56u, 28u, 36u}) {
      uint32_t o = Ld32(ch + off);
      if (o >= 0x40000000 && o < 0xC0000000 && IsCommitted(o))
        RT_INFO("bot players:   +%u -> %08X (vt %08X %s):%s", off, o, Ld32(o), TypeName(Ld32(o)).c_str(), Around(o, 0, 0x40).c_str());
    }
  }
  __imp__sub_827600A8(ctx, base);
}

GAME_PATCH(823B2C80) {
  // BF_BOT_MSG_DUMP: a moving player's whole move message (0x300 bytes) as floats, with
  // its soldier's position, to see whether the client sends where it is.
  static int dumps = 0;
  if (getenv("BF_BOT_MSG_DUMP") && dumps < 2 && Ld32(ctx.r4.u32) == 0x8200E490 && LdF(ctx.r4.u32 + 0x10 + 8) > 0.5f) {
    ++dumps;
    std::string f;
    for (uint32_t off = 0; off < 0x300; off += 4) {
      float v = LdF(ctx.r4.u32 + off);
      uint32_t u = Ld32(ctx.r4.u32 + off);
      if (!u) continue;
      char buf[40];
      if (std::isfinite(v) && std::fabs(v) > 1e-3f && std::fabs(v) < 1e5f) snprintf(buf, sizeof(buf), " %X:%.2f", off, v);
      else snprintf(buf, sizeof(buf), " %X:%08X", off, u);
      f += buf;
    }
    uint32_t so = Ld32(ctx.r3.u32 + 0x118);
    RT_INFO("bot players: move message of soldier %08X at %.1f %.1f %.1f:%s", so, LdF(so + 0x180), LdF(so + 0x184),
            LdF(so + 0x188), f.c_str());
  }
  // (From a player who is moving: the host's own on the spawn screen is mostly zeros.)
  if (!g_have_template && Ld32(ctx.r4.u32) == 0x8200E490 && LdF(ctx.r4.u32 + 0x10 + 8) > 0.5f) {
    memcpy(g_move_template, G<uint8_t>(ctx.r4.u32), sizeof(g_move_template));
    g_have_template = true;
  }
  // BF_BOT_BITS: a real player's button bits (record +78) when they change.
  if (getenv("BF_BOT_BITS") && Ld32(ctx.r4.u32) == 0x8200E490) {
    static uint64_t last = 0;
    uint32_t rec = ctx.r4.u32 + 0x10;
    uint64_t m = uint64_t(Ld32(rec + 0x78)) << 32 | Ld32(rec + 0x7C);
    if (m != last) {
      last = m;
      std::string on;
      for (int i = 0; i < 64; ++i)
        if (m >> i & 1) on += " " + std::to_string(i);
      RT_INFO("bot players: real buttons %016llX:%s (fire %.1f pitch +8C %.2f)", (unsigned long long)m, on.c_str(),
              LdF(rec + 0x24), LdF(rec + 0x8C));
    }
  }
  // BF_BOT_ENTRYLOG: a real player's record +98..+B8 and its receiver's
  // +118/+11C at every move whose words there change (entering a vehicle).
  if (getenv("BF_BOT_ENTRYLOG") && Ld32(ctx.r4.u32) == 0x8200E490) {
    static uint32_t last[12];
    uint32_t rec = ctx.r4.u32 + 0x10, now_w[12];
    for (int k = 0; k < 8; ++k) now_w[k] = Ld32(rec + 0x98 + 4 * k);
    now_w[8] = Ld32(ctx.r3.u32 + 0x118), now_w[9] = Ld32(ctx.r3.u32 + 0x11C);
    now_w[10] = Ld32(rec + 0x78), now_w[11] = Ld32(rec + 0x7C);
    bool changed = false;
    for (int k = 0; k < 12; ++k)
      if (k != 0 && k != 3 && now_w[k] != last[k]) changed = true;  // (+98, +A4: tick counters)
    if (changed) {
      memcpy(last, now_w, sizeof(last));
      uint32_t so = now_w[8];
      RT_INFO("bot players: entry: rec 98:%08X 9C:%08X A0:%08X A4:%08X A8:%08X AC:%08X B0:%08X B4:%08X | rcv 118:%08X 11C:%08X | bits %08X%08X | on foot %u",
              now_w[0], now_w[1], now_w[2], now_w[3], now_w[4], now_w[5], now_w[6], now_w[7], now_w[8], now_w[9],
              now_w[10], now_w[11], so && IsCommitted(so + 0x400) ? Ld32(so + 0x34C) >> 24 : 9);
    }
  }
  // BF_BOT_WATCH_SEAT: a write watch on a real soldier's on-foot byte (+34C):
  // the call stack that puts it in a vehicle seat.
  if (getenv("BF_BOT_WATCH_SEAT") && Ld32(ctx.r4.u32) == 0x8200E490) {
    static bool armed = false;
    uint32_t so = Ld32(ctx.r3.u32 + 0x118);
    if (!armed && so && IsCommitted(so + 0x400) && Ld32(so) == kSoldier) {
      armed = true;
      RT_INFO("bot players: watching soldier %08X +34C", so);
      StartWriteWatch(so + 0x34C, 4, 0);
    }
  }
  // BF_BOT_AIMLOG: a real player's record +80..+94 beside its soldier's aim
  // (yaw +134, pitch +138), once a second: which field is the pitch.
  if (getenv("BF_BOT_AIMLOG") && Ld32(ctx.r4.u32) == 0x8200E490) {
    static int64_t next = 0;
    int64_t t = int64_t(GetTickCount64());
    uint32_t so = Ld32(ctx.r3.u32 + 0x118);
    if (t >= next && so && IsCommitted(so + 0x400) && Ld32(so) == kSoldier) {
      next = t + 1000;
      uint32_t rec = ctx.r4.u32 + 0x10, aim = Ld32(so + 0x130);
      RT_INFO("bot players: aim yaw %.2f pitch %.2f | record 80:%.2f 84:%.2f 88:%.2f 8C:%.2f 90:%.2f 94:%.2f | 10:%.2f 14:%.2f",
              LdF(aim + 0x134), LdF(aim + 0x138), LdF(rec + 0x80), LdF(rec + 0x84), LdF(rec + 0x88), LdF(rec + 0x8C),
              LdF(rec + 0x90), LdF(rec + 0x94), LdF(rec + 0x10), LdF(rec + 0x14));
    }
  }
  bool bot_mover = false;
  for (BotPlayer& b : g_players) bot_mover |= b.mover == ctx.r3.u32;
  if (!bot_mover && LdF(ctx.r4.u32 + 0x10 + 8) > 0.5f) g_real_mover = ctx.r3.u32;
  static std::atomic<int> n{0};
  if (g_discover && n < 4 && Ld32(ctx.r3.u32 + 0x10)) {
    Note("move message 823B2C80", n, ctx);
    uint32_t pl = ctx.r3.u32;
    uint32_t t = Ld32(pl + 0x10);
    if (t >= 0x40000000 && t < 0xC0000000 && IsCommitted(t))
      RT_INFO("bot players:   receiver +10 -> %08X (vt %08X %s):%s", t, Ld32(t), TypeName(Ld32(t)).c_str(),
              Around(t, 0, 0x20).c_str());
    RT_INFO("bot players:   r3 %08X vt %08X %s:%s", pl, Ld32(pl), TypeName(Ld32(pl)).c_str(), Around(pl, 0, 0x80).c_str());
    RT_INFO("bot players:   r4 %08X:%s", ctx.r4.u32, Around(ctx.r4.u32, 0, 0x40).c_str());
    RT_INFO("bot players:   r5 %08X:%s", ctx.r5.u32, Around(ctx.r5.u32, 0, 0x40).c_str());
  }
  __imp__sub_823B2C80(ctx, base);
}
// The input a step uses: 8236F238 on the seat's controller (entry + 284).
// BF_BOT_STEP_INPUT: for the bot soldier's steps, what that controller and
// input are.
uint32_t g_step_soldier = 0;
GAME_PATCH(8236F238) {
  uint32_t ctl = ctx.r3.u32;
  __imp__sub_8236F238(ctx, base);
  // A bot soldier's step: its input record is the spawn object's (made with
  // the join, never filled in without a client): our input goes there, over
  // a moving player's record (frame time +80/+84, heading +88, +90).
  if ((g_fake_count > 0) && g_step_soldier)
    for (BotPlayer& b : g_players)
      if (b.soldier == g_step_soldier && Ld32(ctx.r3.u32) == 0x8200E48C) {
        // The actions are read from the record it points at, +160
        // (822BD248: analog action i at +8 + 4 i, buttons as bits at +78).
        uint32_t rec = ctx.r3.u32;
        if (true) {
          uint32_t p = Ld32(rec + 0x160);
          static int said = 0;
          if (said++ < 3)
            RT_INFO("bot %d: step input %08X +160 -> %08X (vt %08X)", b.index, rec, p, IsCommitted(p) ? Ld32(p) : 0);
          if (p >= 0x40000000 && p < 0xC0000000 && IsCommitted(p) && Ld32(p) == 0x8200E48C) rec = p;
        }
        // The input object itself from a moving player's record (its button
        // and state words, frame time, ...: the heading at +88 only counts
        // with them), keeping its number and network id.
        if (g_have_template) {
          uint32_t in = ctx.r3.u32, keep4 = Ld32(in + 4), id = Ld32(in + 0x9C), keep160 = Ld32(in + 0x160);
          // BF_BOT_TPL_RANGE=from,to (hex, record offsets): which part of it.
          uint32_t lo = 0x78, hi = 0x78;  // (nothing: the bits, heading and pitch are written below)
          if (const char* rg = getenv("BF_BOT_TPL_RANGE")) sscanf(rg, "%x,%x", &lo, &hi);
          memcpy(G<uint8_t>(in + lo), g_move_template + 0x10 + lo, hi - lo);
          St32(in + 4, keep4), St32(in + 0x9C, id), St32(in + 0x160, keep160);
          for (uint32_t k = 8; k < 0x30; k += 4) St32(in + k, 0);
        }
        // The heading (+88) on the input object itself, every step.
        if (getenv("BF_BOT_HEADING_SPIN")) {
          static float h = 0;
          h += 0.01f;
          if (h > 6.2832f) h -= 6.2832f;
          uint32_t hu;
          memcpy(&hu, &h, 4);
          St32(ctx.r3.u32 + 0x88, hu);
          St32(ctx.r3.u32 + 0x90, 0x3F800000);  // 1.0 beside it in a real record
        }
        if (g_have_template && false) {
          uint32_t keep4 = Ld32(rec + 4), id = Ld32(rec + 0x9C);
          memcpy(G<uint8_t>(rec + 4), g_move_template + 0x10 + 4, 0xB0);
          St32(rec + 4, keep4), St32(rec + 0x9C, id);
        }
        // The sticks (action i at +8 + 4 i: 0 forward, 1 strafe, 4 turn,
        // 5 look; 822ED8D8 copies them into the input state): ours only.
        for (uint32_t k = 8; k < 0x30; k += 4) St32(rec + k, 0);
        uint32_t u;
        auto put = [&](uint32_t a, float v) {
          memcpy(&u, &v, 4);
          St32(a, u);
        };
        if (const char* f = getenv("BF_BOT_FWD")) {  // testing: a fixed forward
          put(rec + 8, float(atof(f)));
        } else {
          // The entry input actions (EntryInputActionEnum, the EIA* names in
          // the image in reverse order): 0 Throttle, 1 Strafe, 2 Brake,
          // 3 HandBrake, 4 Yaw, 5 Pitch, 6 Roll, 7 Fire (analog: +8 + 4 i),
          // then bits of the 64-bit mask at +78: 11 Zoom, 12 Jump,
          // 13 ChangeVehicle, 14 ChangeEntry, 15 ChangePose, 16 Interact,
          // 21 Reload, 23 Sprint.
          bool seated = (Ld32(b.soldier + 0x34C) >> 24) == 0;
          float lt = float(b.triggers & 0xFF) / 255.0f, rt = float((b.triggers >> 8) & 0xFF) / 255.0f;
          uint64_t bits = 0;
          // (A digital action's bit is its number: a real player's jump sets
          // 12; its sprint shows as 22. Bits 40 and 43 are set in nearly every
          // move of a live soldier, and the heading only counts with 40.)
          auto bit = [&](int a) { bits |= uint64_t(1) << a; };
          if (seated) {
            put(rec + 8, std::max(lt, b.ly));      // throttle (LT, as the PC keys)
            put(rec + 0xC, b.lx);                 // steer
            if (b.buttons & 0x0100) put(rec + 0x10, 1.0f);  // LB: brake / reverse
            put(rec + 0x18, -b.rx);               // yaw
            put(rec + 0x1C, b.ry);                // pitch
            if (b.buttons & 0x1000) bit(14);      // A: the next seat
          } else {
            put(rec + 8, b.ly);                   // forward
            put(rec + 0xC, b.lx);                 // strafe
            if (b.buttons & 0x1000) bit(12);      // A: jump
            if (b.buttons & 0x0040) bit(22);      // left stick click: sprint (22, as a real player's)
            if (lt > 0.5f) bit(11);               // LT: zoom
          }
          put(rec + 0x24, rt);                    // RT: fire
          static const int ebit = getenv("BF_BOT_E_BIT") ? atoi(getenv("BF_BOT_E_BIT")) : -1;
          // EXPERIMENT (BF_BOT_E_LATCH): E kept held 1.5 s after a press.
          static const bool latch = getenv("BF_BOT_E_LATCH") != nullptr;
          if (b.buttons & 0x2000) b.e_until = int64_t(GetTickCount64()) + 1500;
          bool e_on = (b.buttons & 0x2000) || (latch && int64_t(GetTickCount64()) < b.e_until);
          // B: in or out of a vehicle (13), use (16). Entering wants a press
          // (13 set in this step's record, clear in the last one: 822BD2B0),
          // so a held B pulses it: one step on, then off, again every 0.5 s.
          b.e_steps = e_on ? b.e_steps + 1 : 0;
          if (e_on) {
            if (ebit >= 0) bits |= uint64_t(1) << ebit;
            else {
              bit(16);
              if (b.e_steps % 15 == 1) bit(13), b.press13 = true;
            }
          }
          if (b.buttons & 0x4000) bit(21);        // X: reload
          // (with the flags every live soldier's moves carry)
          uint64_t base = getenv("BF_BOT_BASE_BITS") ? strtoull(getenv("BF_BOT_BASE_BITS"), nullptr, 16)
                                                     : (uint64_t(1) << 40 | uint64_t(1) << 43);
          uint64_t mask = base | bits;
          b.bits = mask;
          // How long each action has been held, as half floats at +(action
          // + 20) * 2 (822C1818: entering a vehicle wants Interact, 16, held
          // long enough; Bad Company's records the same).
          for (int a = 8; a < 40; ++a) {
            b.held[a] = (bits >> a & 1) ? b.held[a] + 1.0f / 30.0f : 0.0f;
            uint16_t h = HalfFloat(b.held[a]);
            for (uint32_t o : {rec, ctx.r3.u32}) St16(o + uint32_t(a + 20) * 2, h);
          }
          if (getenv("BF_BOT_ENTER_TRACE") && (bits >> 16 & 1)) {
            static int64_t nx = 0;
            int64_t t2 = int64_t(GetTickCount64());
            if (t2 >= nx) {
              nx = t2 + 500;
              uint32_t in = ctx.r3.u32;
              RT_INFO("bot %d: E held %.2f s: input %08X +352 %08X +356 %08X | rec %08X +48 %04X mask %08X%08X | target +356 %08X",
                      b.index, b.held[16], in, Ld32(in + 352), Ld32(in + 356), rec, Ld16(rec + 0x48), Ld32(rec + 0x78),
                      Ld32(rec + 0x7C), Ld32(b.spawner + 356));
            }
          }
          // (the input object too: the move handler enters vehicles from it, 8239D040)
          St32(ctx.r3.u32 + 0x78, uint32_t(mask >> 32)), St32(ctx.r3.u32 + 0x7C, uint32_t(mask));
          if (ebit == 43 && !e_on) mask &= ~(uint64_t(1) << 43);  // (43 only while E is held)
          St32(rec + 0x78, uint32_t(mask >> 32)), St32(rec + 0x7C, uint32_t(mask));
          // The heading (the input object's +88; the soldier's aim +134 is
          // minus it): turned by rx, as a pad's right stick turns the view.
          uint32_t aim = Ld32(b.soldier + 0x130);
          if (aim && IsCommitted(aim + 0x140) && !getenv("BF_BOT_HEADING_SPIN")) {
            if (!b.aim_set) b.heading = -LdF(aim + 0x134), b.pitch = LdF(aim + 0x138), b.aim_set = true;
            float yaw = LdF(aim + 0x134) - b.rx * 0.12f;
            put(ctx.r3.u32 + 0x88, -yaw);
            // The pitch beside it (+8C; the aim's +138, up positive), turned by ry.
            static const float psign = getenv("BF_BOT_PITCH_SIGN") ? float(atof(getenv("BF_BOT_PITCH_SIGN"))) : 1.0f;
            float pitch = std::clamp(LdF(aim + 0x138) + b.ry * 0.08f, -1.0f, 1.0f);
            (void)psign;
            b.heading = -yaw, b.pitch = pitch;
            put(ctx.r3.u32 + 0x8C, pitch);
            if (rec != ctx.r3.u32) put(rec + 0x88, -yaw), put(rec + 0x8C, pitch);
            static int64_t next = 0;
            int64_t t = int64_t(GetTickCount64());
            if (getenv("BF_BOT_AIMLOG") && t >= next) {
              next = t + 1000;
              RT_INFO("bot %d: rx %.2f ry %.2f, aim yaw %.2f pitch %.2f -> wrote heading %.2f pitch %.2f (input %08X, record %08X)",
                      b.index, b.rx, b.ry, LdF(aim + 0x134), LdF(aim + 0x138), -yaw, pitch, ctx.r3.u32, rec);
            }
            St32(ctx.r3.u32 + 0x90, 0x3F800000);
          }
        }
        // The client's tick counters (+98, +A4: about 60 a second), advanced
        // by two a step: the step's time comes from them.
        if (!getenv("BF_BOT_NO_TICKS")) {
          static std::map<uint32_t, uint32_t> ticks;
          uint32_t& t = ticks[rec];
          if (!t) t = Ld32(rec + 0x98) + 1;
          t += 2;
          St32(rec + 0x98, t), St32(rec + 0xA4, t);
        }
        if (const char* t = getenv("BF_BOT_TURN")) {  // testing: a constant turn (action 4)
          float turn = float(atof(t));
          memcpy(&u, &turn, 4);
          St32(rec + 0x18, u);
        }
      }
  static int64_t next = 0;
  int64_t t = int64_t(GetTickCount64());
  if (getenv("BF_BOT_STEP_INPUT") && g_step_soldier && t >= next) {
    bool bot = false;
    for (BotPlayer& b : g_players) bot |= b.soldier == g_step_soldier;
    if (bot) {
      next = t + 3000;
      uint32_t in = ctx.r3.u32;
      RT_INFO("bot players: bot step: controller %08X (vt %08X %s), input %08X (vt %08X %s) +8 %.2f:%s", ctl, Ld32(ctl),
              TypeName(Ld32(ctl)).c_str(), in, IsCommitted(in) ? Ld32(in) : 0,
              IsCommitted(in) ? TypeName(Ld32(in)).c_str() : "", IsCommitted(in) ? LdF(in + 8) : 0.f,
              IsCommitted(in) ? Around(in, 0, 0x40).c_str() : "");
    }
  }
}

// BF_BOT_ENTER_TRACE: how far down the vehicle entering chain (8239D040 ->
// 8239CEE8 -> 82398E08 -> 82398B98 -> 82387BB0 -> 822DDE58, seen with
// BF_BOT_WATCH_SEAT) calls get, and 8239D040's calls per controller.
std::atomic<int> g_enter_depth[4];
GAME_PATCH(8239CEE8) {
  if (getenv("BF_BOT_ENTER_TRACE")) g_enter_depth[0]++;
  __imp__sub_8239CEE8(ctx, base);
}
GAME_PATCH(82398E08) {
  if (getenv("BF_BOT_ENTER_TRACE")) g_enter_depth[1]++;
  __imp__sub_82398E08(ctx, base);
}
GAME_PATCH(82398B98) {
  if (getenv("BF_BOT_ENTER_TRACE")) g_enter_depth[2]++;
  __imp__sub_82398B98(ctx, base);
}
GAME_PATCH(82387BB0) {
  if (getenv("BF_BOT_ENTER_TRACE")) g_enter_depth[3]++;
  __imp__sub_82387BB0(ctx, base);
}
GAME_PATCH(8239D040) {
  if (getenv("BF_BOT_ENTER_TRACE")) {
    static std::map<uint32_t, int> calls;
    calls[ctx.r3.u32]++;
    static int64_t next = 0;
    int64_t t = int64_t(GetTickCount64());
    if (t >= next) {
      next = t + 5000;
      std::string out;
      for (auto& [k, v] : calls) {
        bool bot = false;
        for (BotPlayer& b : g_players) bot |= b.spawner == k;
        char buf[64];
        uint32_t tgt = Ld32(k + 356), so = Ld32(k + 288);
        snprintf(buf, sizeof(buf), " %08X%s:%d [+356 %08X(vt %08X) +520 %.2f soldier %08X +46 %04X]", k, bot ? "(bot)" : "", v,
                 tgt, tgt && IsCommitted(tgt) ? Ld32(tgt) : 0, LdF(k + 520), so, so && IsCommitted(so + 0x40) ? Ld16(so + 46) : 0);
        out += buf;
      }
      RT_INFO("bot players: 8239D040 calls:%s | deeper %d %d %d %d", out.c_str(), g_enter_depth[0].exchange(0),
              g_enter_depth[1].exchange(0), g_enter_depth[2].exchange(0), g_enter_depth[3].exchange(0));
      calls.clear();
    }
  }
  __imp__sub_8239D040(ctx, base);
}

// BF_BOT_CHECK_TRACE: the Interact checks entering a vehicle runs on an
// input object (822BD2B0 pressed, 822C1818 held long enough): what record
// they read, its bit and hold time, the time wanted, the answer.
bool BotInput(uint32_t in) {
  for (BotPlayer& b : g_players)
    if (b.spawner && Ld32(b.spawner + 280) == in) return true;
  return false;
}
GAME_PATCH(822BD2B0) {
  uint32_t in = ctx.r3.u32, act = ctx.r4.u32;
  if (getenv("BF_BOT_CHECK_TRACE")) {
    static std::map<uint32_t, int> by;
    static int64_t nx = 0;
    by[act]++;
    int64_t t = int64_t(GetTickCount64());
    if (t >= nx) {
      nx = t + 10000;
      std::string o;
      for (auto& [k, v] : by) o += " " + std::to_string(k) + ":" + std::to_string(v);
      RT_INFO("bot players: pressed() calls by action:%s", o.c_str());
    }
  }
  __imp__sub_822BD2B0(ctx, base);
  // A fake player's ChangeVehicle press: its records never show the edge
  // (something else writes bit 13 into both), so the check is answered from
  // the brain's pulse, once.
  if (act == 13)
    for (BotPlayer& b : g_players)
      if (b.spawner && Ld32(b.spawner + 280) == in) {
        ctx.r3.u64 = b.press13 ? 1 : 0;
        b.press13 = false;
      }
  if (getenv("BF_BOT_CHECK_TRACE") && act == 13) {
    static int n = 0;
    bool bot = BotInput(in), want = false;
    for (BotPlayer& b : g_players)
      if (b.spawner && Ld32(b.spawner + 280) == in) want = (b.bits >> 13) & 1;
    if ((bot && want && n++ < 40) || ctx.r3.u8)
      RT_INFO("bot players: pressed(13)%s in %08X cur %08X (mask %08X%08X) prev %08X (mask %08X%08X) -> %u", bot ? " BOT" : "",
              in, Ld32(in + 352), Ld32(Ld32(in + 352) + 0x78), Ld32(Ld32(in + 352) + 0x7C), Ld32(in + 356),
              Ld32(Ld32(in + 356) + 0x78), Ld32(Ld32(in + 356) + 0x7C), ctx.r3.u8);
  }
}
GAME_PATCH(822C1818) {
  uint32_t in = ctx.r3.u32, act = ctx.r4.u32;
  double want = ctx.f1.f64;
  __imp__sub_822C1818(ctx, base);
  if (getenv("BF_BOT_CHECK_TRACE") && act == 16) {
    static int n = 0;
    bool bot = BotInput(in);
    uint32_t rec = Ld32(in + 352);
    if (n++ < 40 || ctx.r3.u8)
      RT_INFO("bot players: held(16)%s in %08X rec %08X mask %08X%08X hold %04X want %.2f -> %u", bot ? " BOT" : "", in, rec,
              Ld32(rec + 0x78), Ld32(rec + 0x7C), Ld16(rec + 0x48), want, ctx.r3.u8);
  }
}

GAME_PATCH(822F05C8) {
  // BF_BOT_STEP_TRACE: steps per soldier, and how far each step moves it.
  g_step_soldier = ctx.r3.u32;
  if (getenv("BF_BOT_STEP_TRACE")) {
    uint32_t so = ctx.r3.u32;
    float x0 = LdF(so + 0x180), z0 = LdF(so + 0x188);
    PPCContext c = ctx;
    __imp__sub_822F05C8(ctx, base);
    static std::map<uint32_t, std::pair<int, float>> st;
    auto& e = st[so];
    e.first++, e.second += std::hypot(LdF(so + 0x180) - x0, LdF(so + 0x188) - z0);
    static int64_t next = 0;
    int64_t t = int64_t(GetTickCount64());
    if (t >= next) {
      next = t + 5000;
      std::string out;
      for (auto& [k, v] : st) {
        bool bot = false;
        for (BotPlayer& b : g_players) bot |= b.soldier == k;
        char buf[80];
        snprintf(buf, sizeof(buf), " %08X%s: %d steps, moved %.1f m;", k, bot ? "(bot)" : "", v.first, v.second);
        out += buf;
      }
      RT_INFO("bot players: soldier steps:%s r4 %08X r5 %08X", out.c_str(), c.r4.u32, c.r5.u32);
      st.clear();
    }
    return;
  }
  static std::atomic<int> n{0};
  if (g_discover && n < 4) {
    Note("soldier step 822F05C8", n, ctx);
    RT_INFO("bot players:   r4 %08X:%s", ctx.r4.u32, Around(ctx.r4.u32, 0, 0x40).c_str());
    RT_INFO("bot players:   r5 %08X:%s", ctx.r5.u32, Around(ctx.r5.u32, 0, 0x40).c_str());
  }
  __imp__sub_822F05C8(ctx, base);
}

// ServerSoldierEntity's update (its vtable's +114; writes its position through
// 82536548). BF_BOT_DISCOVER: which soldiers it runs for (every 10 s).
GAME_PATCH(82308978) {
  if (g_discover) {
    static std::map<uint32_t, int> calls;
    static int64_t next = 0;
    if (++calls[ctx.r3.u32] == 1) {
      static std::atomic<int> stacks{0};
      if (stacks++ < 2) {
        RT_INFO("bot players: first update of soldier %08X:", ctx.r3.u32);
        bf_crash_diag::PrintStack(nullptr);
      }
    }
    int64_t now = int64_t(GetTickCount64());
    if (now >= next) {
      next = now + 10000;
      std::string out;
      for (auto& [so, n] : calls) {
        bool bot = false;
        for (BotPlayer& b : g_players) bot |= b.soldier == so;
        char buf[64];
        snprintf(buf, sizeof(buf), " %08X%s:%d", so, bot ? "(bot)" : "", n);
        out += buf;
      }
      RT_INFO("bot players: soldier updates:%s", out.c_str());
      calls.clear();
    }
  }
  __imp__sub_82308978(ctx, base);
}

GAME_PATCH(823194B0) {  // damage a soldier
  Note("damage", g_damages, ctx);
  __imp__sub_823194B0(ctx, base);
}
