// The bots' advisor (--bot-llm, optional): a small vision model run by
// llama.cpp's llama-server (Qwen3.5 4B by default; tools/setup_bot_llm.py
// downloads both) is shown a top-down map around a bot in a difficult spot
// (stuck, losing a fight) with numbered places it can walk to, and picks one.
// Asked off the game thread: the bot carries on as it would without it and
// takes the answer when it comes (a few seconds on a GPU, longer on a CPU).
// Both games use it (BC: bc_bots.cpp; BF1943: bots.cpp for the brain).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rt::botllm {

// --bot-llm <folder with bot_llm.ini | http://host:port>: starts llama-server
// from the folder's bot_llm.ini (setup_bot_llm.py writes it) on 127.0.0.1, or
// uses a server already there (another process's, or any OpenAI-style one).
void Configure(const std::string& where);
bool Enabled();
void StopServer();  // ends the llama-server it started (also gone with this process)

// What the map shows (north up, +z; east right, +x).
struct Marker {
  enum Kind { kSelf, kFriend, kEnemy, kObjective, kObjectiveEnemy, kVehicle, kPlace } kind;
  float x, z;
  float yaw = 0;   // kSelf: where it faces (atan2(dx, dz))
  int label = 0;   // kPlace: its number (1..9)
  std::string note;  // kPlace: what the bot's code knows of it ("an enemy can see it"), in the words
};
struct MapView {
  float cx = 0, cz = 0, half = 60;  // centre and half its width, metres
  std::function<float(float x, float z)> ground;  // terrain height (NaN where unknown)
  // Whether a soldier can walk there (the navigation mesh), near height y.
  std::function<bool(float x, float y, float z)> walkable;
  float y = 0;  // the bot's height, for walkable
  std::vector<Marker> markers;
  // The eye view (optional, a second picture): what the bot sees ahead, 90
  // degrees wide, traced with sight (whether nothing is between two points)
  // from eye, facing eye_yaw (atan2(dx, dz)).
  std::function<bool(const float* a, const float* b)> sight;
  float eye[3] = {0, 0, 0}, eye_yaw = 0;
  int kind = 0;  // what the question is about (1 stuck, 2 cover, ...: the training record)
};

// Asks for bot `bot` (one question at a time each; not again within
// `cooldown_ms` of the last). The map is drawn here (call it on the thread that
// may use ground/walkable); the rest happens on the advisor's thread. places:
// how many kPlace markers there are (the answer is one of them, or 0: none).
bool Ask(int bot, const std::string& situation, const MapView& map, int places, int cooldown_ms = 15000);
// Whether Ask would take a question for it now (cheap: before making the map).
bool CanAsk(int bot, int cooldown_ms = 15000);
// The answer, once: true with the place's number (0: none fits), why, and the
// case's id (for Learn).
bool Answer(int bot, int* place, std::string* why = nullptr, int64_t* case_id = nullptr);

// ---- learning from the answers -------------------------------------------------
// What came of an answer (judged by the bot's code: out of the stuck spot,
// alive in its cover) is kept per map in <user>/bot_advice/<map>.txt. A place
// that worked more often than not for the same kind of situation (1 stuck, 2
// cover, 3 a vehicle stuck, 4 standing idle, 5 where to fight from) within 6 m and 60 degrees (dir: where it was going, or the enemy) is
// used again without asking: the bots learn the maps, and servers without the
// model use what servers with it learned (the files can be shipped). Every
// question also goes to <user>/bot_advice/cases/ with its map image, the
// answer and what came of it: a data set for training a bot model.
void UseMap(const std::string& map);
bool Recall(int kind, const float at[3], float dir, float place[3]);
void Learn(int kind, const float at[3], float dir, const float place[3], bool worked, int64_t case_id = 0);

// ---- the training run's record (--train) ----------------------------------------
// Every bot's track (once a game second), each question with its pictures,
// answer and outcome, and the learned places reused, for a viewer:
// <user>/bot_advice/runs/<map>_<date>/index.html, a top-down map of the whole
// level with all of it over time (a slider, play, each question to click).
// SetLevelMap starts a map's run (the level drawn once; bounds: cx, cz, half),
// Track feeds the tracks; data.js is written every 20 s and by FinishRun
// (--train-minutes: then the server exits).
void SetLevelMap(const std::string& map, const MapView& level);
void Track(int bot, int team, float x, float z);
// A bot's shot (from its eyes to what it hit, or as far as it reaches) and a
// soldier killed (where it fell; killer: the bot's slot, -1 a real player).
void Shot(int bot, const float from[3], const float to[3], bool hit);
void Killed(float x, float z, int team, int killer);
void FinishRun();

}  // namespace rt::botllm
