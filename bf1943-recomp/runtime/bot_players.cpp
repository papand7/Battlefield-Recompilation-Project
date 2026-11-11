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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "guest.h"
#include "kernel.h"

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

}  // namespace

GAME_PATCH(823B59E0) {  // the player manager's join
  Note("join", g_joins, ctx);
  __imp__sub_823B59E0(ctx, base);
}
GAME_PATCH(827559F0) {  // the peer manager's create
  Note("create peer", g_peers, ctx);
  __imp__sub_827559F0(ctx, base);
}
GAME_PATCH(82382720) {  // deploy a soldier
  Note("deploy", g_deploys, ctx);
  __imp__sub_82382720(ctx, base);
}
GAME_PATCH(822ED8D8) {  // a player's received input to its soldier
  Note("input", g_inputs, ctx);
  __imp__sub_822ED8D8(ctx, base);
}
GAME_PATCH(823194B0) {  // damage a soldier
  Note("damage", g_damages, ctx);
  __imp__sub_823194B0(ctx, base);
}
