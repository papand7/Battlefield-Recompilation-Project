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

// Searches the network for hosts (in the background, about a second).
void Refresh();
bool Browsing();
std::vector<Server> Found();
// Hosts the map as a LAN server with bots joining it.
void Host(const std::string& map, int bots);
// Joins the host at the address (IPv4), which is on the map.
void Join(const std::string& address, const std::string& map);

}  // namespace rt::lan
