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
// Answers server browsers (the dedicated server, from the start).
void StartBeacon();
// --join: everything for joining but starting it (that follows the title).
bool PrepareJoin(const std::string& address, const std::string& map);
// Joins the host at the address (IPv4), which is on the map.
void Join(const std::string& address, const std::string& map);

}  // namespace rt::lan
