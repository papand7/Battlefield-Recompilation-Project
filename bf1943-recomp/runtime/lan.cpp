// LAN games from the main menu's HOST GAME and JOIN GAME (pc_settings.cpp
// draws their pages). A host answers browsers on UDP 19430 with its name, map
// and game port; a joiner hosts the same map locally (the game's own way to
// load a level and start a client) and its client's packets to its "own"
// server (0.0.0.0:1003) go to the host's address instead (net.cpp
// FixAnyDestination). On the same PC the joiner's ports are offset, as for
// --bots.
#include "kernel.h"
#include "lan.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rt;

namespace rt {
uint32_t g_join_address = 0;  // network order: the host's address while joining (0 none)
extern uint32_t g_lan_ip;     // net.cpp
}

namespace rt::lan {

namespace {
constexpr uint16_t kBeaconPort = 19430;
std::mutex g_mutex;
std::vector<Server> g_found;
std::atomic<bool> g_browsing{false}, g_beacon{false};

void Beacon() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kBeaconPort);
  if (s == INVALID_SOCKET || bind(s, (sockaddr*)&a, sizeof(a))) {
    RT_WARN("lan: beacon port %u busy, this game will not show in browsers", kBeaconPort);
    return;
  }
  RT_INFO("lan: answering server browsers on UDP %u", kBeaconPort);
  char buf[256];
  for (;;) {
    sockaddr_in from{};
    int len = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
    if (n < 5 || memcmp(buf, "BF43?", 5)) continue;
    std::string reply = "BF43!" + g_gamertag + "|" + g_host_map + "|" + std::to_string(1003 + g_port_offset);
    sendto(s, reply.data(), int(reply.size()), 0, (sockaddr*)&from, len);
  }
}
}  // namespace

// Browses: broadcasts a query (and to this PC), collects answers for a second.
void Refresh() {
  if (g_browsing.exchange(true)) return;
  std::thread([] {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kBeaconPort);
    auto start = std::chrono::steady_clock::now();
    for (uint32_t ip : {uint32_t(INADDR_BROADCAST), uint32_t(INADDR_LOOPBACK)}) {
      to.sin_addr.s_addr = htonl(ip);
      sendto(s, "BF43?", 5, 0, (sockaddr*)&to, sizeof(to));
    }
    std::vector<Server> found;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1200)) {
      char buf[256];
      sockaddr_in from{};
      int len = sizeof(from);
      int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &len);
      if (n < 5 || memcmp(buf, "BF43!", 5)) continue;
      buf[n] = 0;
      std::string text(buf + 5);
      size_t a = text.find('|'), b = text.find('|', a + 1);
      if (a == std::string::npos || b == std::string::npos) continue;
      char ip[32];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      Server sv{text.substr(0, a), text.substr(a + 1, b - a - 1), ip, uint16_t(atoi(text.c_str() + b + 1)),
                int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count())};
      bool dup = false;
      for (const Server& o : found) dup |= o.address == sv.address && o.port == sv.port;
      // This PC answers on loopback and its LAN address: one entry.
      for (Server& o : found)
        if (o.port == sv.port && o.name == sv.name && o.map == sv.map) dup = true;
      if (!dup) found.push_back(sv);
    }
    closesocket(s);
    {
      std::lock_guard lock(g_mutex);
      g_found = found;
    }
    g_browsing = false;
  }).detach();
}

bool Browsing() { return g_browsing; }

std::vector<Server> Found() {
  std::lock_guard lock(g_mutex);
  return g_found;
}

// HOST GAME: the map hosted as a LAN server, with bots joining it.
void Host(const std::string& map, int bots) {
  g_host_map = map;
  g_lan_host = true;
  g_bot_count = bots;
  if (!g_beacon.exchange(true)) std::thread(Beacon).detach();
  RT_INFO("lan: hosting %s with %d bots", map.c_str(), bots);
  RequestHost();
}

void StartBeacon() {
  static WSADATA wsa;
  static bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  (void)started;
  if (!g_beacon.exchange(true)) std::thread(Beacon).detach();
}

// JOIN GAME: the host's map loaded here, the client sent to the host.
bool PrepareJoin(const std::string& address, const std::string& map) {
  in_addr ip{};
  if (inet_pton(AF_INET, address.c_str(), &ip) != 1) return false;
  g_host_map = map;
  g_lan_host = true;
  g_bot_count = 0;
  // On this PC: our own server and ports must not clash with the host's.
  bool local = ip.s_addr == htonl(INADDR_LOOPBACK) || ip.s_addr == g_lan_ip;
  if (local && !g_port_offset) g_port_offset = 10;
  g_join_address = local ? htonl(INADDR_LOOPBACK) : ip.s_addr;
  RT_INFO("lan: joining %s (%s)%s", address.c_str(), map.c_str(), local ? " on this PC" : "");
  return true;
}
void Join(const std::string& address, const std::string& map) {
  if (PrepareJoin(address, map)) RequestHost();
}

}  // namespace rt::lan
