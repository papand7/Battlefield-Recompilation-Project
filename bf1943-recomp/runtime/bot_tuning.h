// The bots tune their own behaviour by self-play (bots/bot_api.h's Tune: how
// far they fight from, strafe, aim, choose flags, sprint). A map's best
// settings so far, the champion, are what every bot plays with. On a training
// server (--train) half of each team plays a candidate instead (the champion
// with a few settings moved); after a round of play (10 minutes of game time)
// the two halves' scores in that same match are compared (kills - deaths, +2
// per flag taken, per bot and minute), and a clearly better candidate becomes
// the champion. Saved per map as <user>/bot_knowledge/<map>.tune (shipped:
// data/bot_knowledge/<map>.tune next to the exe, tools/bot_knowledge.py).
#pragma once
#include <cstdint>
#include <string>

namespace rt::tuning {

// A game's own defaults (kTuneCount values; null: kTuneInfo's, bf1943's
// brain's) and which settings its bots use (bit 1 << Tune: only those are
// moved in a candidate).
void UseMap(const std::string& map, bool training, const float* defaults = nullptr, uint32_t used = ~0u);
// The settings that bot slot plays with (botapi::kTuneCount values).
const float* For(int slot);
// What a bot did (from any thread): a kill, a death, a flag its team took
// with it there.
void Kill(int slot);
void Death(int slot);
void Capture(int slot);
// The director's pass (the game's time, in seconds; bot_count bots playing):
// ends a round when due.
void Update(double now, int bot_count);

}  // namespace rt::tuning
