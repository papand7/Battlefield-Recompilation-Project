// Bots as players on the host's own server (bot_players.cpp, from Bad
// Company's bots): for the director (bots.cpp), which gives them the brain's
// pad as it does the bot copies'.
#pragma once
#include <cstdint>

namespace rt {

int BotPlayerCount();             // BF_BOT_FAKE=N joined ones
uint32_t BotPlayerSoldier(int i);  // 0 while dead or not deployed
// The brain's pad: lx, ly -1..1 (ly forward), rx the turn (+ turns right),
// ry the look up, XInput buttons, triggers lt | rt << 8.
void SetBotPlayerInput(int i, float lx, float ly, float rx, float ry, uint32_t buttons, uint32_t triggers);

}  // namespace rt
