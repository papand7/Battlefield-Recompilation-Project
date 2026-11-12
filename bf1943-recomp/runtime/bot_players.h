// Bots as players on the host's own server (bot_players.cpp, from Bad
// Company's bots): for the director (bots.cpp), which gives them the brain's pad.
#pragma once
#include <cstdint>
#include <string>

namespace rt {

int BotPlayerCount();             // --bots N (BF_BOT_FAKE=N) joined ones
uint32_t BotPlayerSoldier(int i);  // 0 while dead or not deployed
// The brain's pad: lx, ly -1..1 (ly forward), rx the turn (+ turns right),
// ry the look up, XInput buttons, triggers lt | rt << 8.
void SetBotPlayerInput(int i, float lx, float ly, float rx, float ry, uint32_t buttons, uint32_t triggers);
void SetBotPlayerVehicle(int i, uint32_t vehicle);  // the vehicle the director sees it in (0: none found)

// Line of sight (bot_collision.cpp: the map's collision and terrain, built in
// the background for a new map). InSight is true while it isn't built yet.
void SightForMap(const std::string& map);
bool InSight(const float a[3], const float b[3]);
float TerrainHeight(float x, float z);  // -1e9 off the terrain or before it is known

}  // namespace rt
