// What the bots know of a map from the matches played on it (bot_knowledge.cpp):
// on a grid of 8 m cells, per team, where players go, where soldiers die and
// where kills are made from. Each server adds what its matches show to
// <user>/bot_knowledge/<map>.txt; the copy that ships with the game
// (bot_knowledge/<map>.txt next to the exe, from data/bot_knowledge, merged
// by tools/bot_knowledge.py) is what everyone starts from.
#pragma once
#include <string>

namespace rt::knowledge {

enum What { kVisits = 0, kDeaths = 1, kKills = 2 };

void UseMap(const std::string& map);  // the map being played: its knowledge loaded (the last one's saved)
void Add(What what, int team, float x, float z, float weight = 1.0f);  // team 1 or 2
// The sum over the cells within radius of x, z (shipped and learned).
float Get(What what, int team, float x, float z, float radius);
void Save();  // what this server learned, to <user>/bot_knowledge/<map>.txt

}  // namespace rt::knowledge
