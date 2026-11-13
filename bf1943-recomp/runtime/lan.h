// LAN games for HOST GAME and JOIN GAME (lan.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rt::lan {

struct Server {
  std::string name, map, address;
  uint16_t port;
  int ping_ms;
  bool online = false;  // from the master server's list (not this network)
  int players = 0;
};

// The game's 4-letter tag in browser packets and the beacon's UDP port
// (bf1943: "BF43", 19430), so two games on one network don't list each other.
void SetGame(const char* tag, uint16_t beacon_port);
// Searches the network for hosts (in the background, about a second).
void Refresh();
bool Browsing();
std::vector<Server> Found();
// Hosts the map as a LAN server with bots joining it.
void Host(const std::string& map, int bots);
// Why hosting would fail right now (empty: it can go ahead): the server's
// ports already taken by another copy of the game on this PC.
std::wstring HostProblem();
// Answers server browsers (the dedicated server, from the start).
void StartBeacon();
// The master server (an api.php like masterserver.dankelsoft.dk's): JOIN GAME
// lists its servers beside the network's; a host with publishing on tells it
// every 30 s while its level runs (the master checks the host's beacon
// itself, so the game port and the beacon's must be open to the internet).
void SetMaster(const std::string& url);
void SetPublish(bool on);
bool Publish();
// While hosting (in a level): the announcing thread (does nothing unless
// publishing is on).
void StartPublishing();
// --join: everything for joining but starting it (that follows the title).
bool PrepareJoin(const std::string& address, const std::string& map);
// Joins the host at the address (IPv4), which is on the map.
// False if the address is not one (nothing happens then).
bool Join(const std::string& address, const std::string& map);

}  // namespace rt::lan
