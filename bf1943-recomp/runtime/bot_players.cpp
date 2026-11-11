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
  }
  __imp__sub_823B59E0(ctx, base);
}
GAME_PATCH(827559F0) {  // the peer manager's create
  Note("create peer", g_peers, ctx);
  uint32_t conn = ctx.r6.u32;
  bool show = g_discover && g_peers <= 6;
  __imp__sub_827559F0(ctx, base);
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
  static const bool drive = getenv("BF_BOT_DRIVE") != nullptr;
  if (drive && g_have_template)
    for (BotPlayer& b : g_players) {
      if (!b.soldier || Ld32(b.soldier) != kSoldier) continue;
      if (!b.mover) {
        b.mover = FindMover(b.spawner);
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
      memcpy(G<uint8_t>(b.move_msg), g_move_template, sizeof(g_move_template));
      St32(b.move_msg + 4, ++b.move_seq);
      float fwd = 0.8f;
      uint32_t u;
      memcpy(&u, &fwd, 4);
      St32(b.move_msg + 0x10 + 8, u);
      PPCContext c = ctx;
      c.r3.u64 = b.mover;
      c.r4.u64 = b.move_msg;
      c.r5.u64 = 1;
      c.r6.u64 = 0;
      __imp__sub_823B2C80(c, base);
      static int64_t next_log = 0;
      if (now >= next_log) {
        next_log = now + 2000;
        RT_INFO("bot %d: driven soldier %08X at %.1f %.1f %.1f (move %u)", b.index, b.soldier, LdF(b.soldier + 0x180),
                LdF(b.soldier + 0x184), LdF(b.soldier + 0x188), b.move_seq);
      }
    }
  if (!kit) return;
  for (BotPlayer& b : g_players) {
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
GAME_PATCH(823B2C80) {
  if (!g_have_template && Ld32(ctx.r4.u32) == 0x8200E490) {
    memcpy(g_move_template, G<uint8_t>(ctx.r4.u32), sizeof(g_move_template));
    g_have_template = true;
  }
  bool bot_mover = false;
  for (BotPlayer& b : g_players) bot_mover |= b.mover == ctx.r3.u32;
  if (!bot_mover) g_real_mover = ctx.r3.u32;
  static std::atomic<int> n{0};
  if (g_discover && n < 4) {
    Note("move message 823B2C80", n, ctx);
    uint32_t pl = ctx.r3.u32;
    RT_INFO("bot players:   r3 %08X vt %08X %s:%s", pl, Ld32(pl), TypeName(Ld32(pl)).c_str(), Around(pl, 0, 0x80).c_str());
    RT_INFO("bot players:   r4 %08X:%s", ctx.r4.u32, Around(ctx.r4.u32, 0, 0x40).c_str());
    RT_INFO("bot players:   r5 %08X:%s", ctx.r5.u32, Around(ctx.r5.u32, 0, 0x40).c_str());
  }
  __imp__sub_823B2C80(ctx, base);
}
GAME_PATCH(822F05C8) {
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
