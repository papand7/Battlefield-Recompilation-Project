// Network imports (xam NetDll_*): guest Winsock and XNet on top of host Winsock.
//
// Every NetDll_ call takes the XNCALLER type as its first argument (r3), except
// inet_addr, __WSAFDIsSet, WSAGetLastError and WSASetLastError. Guest sockets are
// the low 32 bits of host SOCKETs. Guest sockaddr_in has a big-endian family;
// port and address are in network order, so they copy as bytes.
//
// XNet has no security layer here: an XNADDR carries the peer's plain LAN IP,
// keys are random bytes, and VDP sockets are plain UDP. A VDP packet is a be16
// game-data length, the game data and voice data; on the wire we send just the
// game data, as the console's network stack does for UDP peers (the game's
// server listens on plain UDP), and add the length back on receive.
#include "kernel.h"

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <thread>

using namespace rt;

namespace rt {
std::string g_gamertag = "Player";  // --gamertag
std::string g_online_server;  // --online-server: EA host names resolve here
uint32_t g_lan_ip = 0;        // --lan-ip (network order), else found at startup
uint16_t g_port_offset = 0;   // --port-offset: added to bound ports (two copies on one PC)
std::atomic<bool> g_online_ready{false};  // Theater login answered
}

namespace {
constexpr uint32_t kGuestAfInet = 2;
constexpr int kIpprotoVdp = 254;

std::once_flag g_wsa_once;
void HostStartup() {
  std::call_once(g_wsa_once, [] {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    if (!g_lan_ip) {
      // The address the default route uses; connect() on UDP sends nothing.
      SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      sockaddr_in a{AF_INET, htons(53)};
      inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
      int len = sizeof(a);
      if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0 && getsockname(s, (sockaddr*)&a, &len) == 0)
        g_lan_ip = a.sin_addr.s_addr;
      closesocket(s);
      if (!g_lan_ip) inet_pton(AF_INET, "127.0.0.1", &g_lan_ip);
    }
    char ip[32];
    inet_ntop(AF_INET, &g_lan_ip, ip, sizeof(ip));
    RT_INFO("net: LAN address %s, port offset %u, online server %s", ip, g_port_offset,
            g_online_server.empty() ? "(real DNS)" : g_online_server.c_str());
  });
}

SOCKET Host(uint32_t s) { return SOCKET(s); }

std::mutex g_vdp_mutex;
std::set<uint32_t> g_vdp_sockets;
bool IsVdp(uint32_t s) {
  std::lock_guard l(g_vdp_mutex);
  return g_vdp_sockets.count(s) != 0;
}

std::string HexBytes(const uint8_t* p, uint32_t n) {
  std::string out;
  char b[4];
  for (uint32_t i = 0; i < n; ++i) snprintf(b, sizeof(b), "%02X", p[i]), out += b;
  return out;
}

// Guest sockaddr -> host sockaddr_in. False if not AF_INET.
bool ToHost(uint32_t guest, uint32_t len, sockaddr_in* out) {
  memset(out, 0, sizeof(*out));
  if (!guest || len < 8 || Ld16(guest) != kGuestAfInet) return false;
  out->sin_family = AF_INET;
  memcpy(&out->sin_port, G(guest + 2), 2);
  memcpy(&out->sin_addr, G(guest + 4), 4);
  return true;
}
void ToGuest(const sockaddr_in& a, uint32_t guest, uint32_t len_ptr) {
  if (!guest) return;
  memset(G(guest), 0, 16);
  St16(guest, kGuestAfInet);
  memcpy(G(guest + 2), &a.sin_port, 2);
  memcpy(G(guest + 4), &a.sin_addr, 4);
  Out32(len_ptr, 16);
}
std::string AddrText(const sockaddr_in& a) {
  char ip[32];
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

// Guest fd_set: be32 fd_count, be32 fd_array[64].
void FdsToHost(uint32_t guest, fd_set* out) {
  FD_ZERO(out);
  if (!guest) return;
  uint32_t n = std::min<uint32_t>(Ld32(guest), FD_SETSIZE);
  for (uint32_t i = 0; i < n; ++i) FD_SET(Host(Ld32(guest + 4 + i * 4)), out);
}
void FdsToGuest(const fd_set& in, uint32_t guest) {
  if (!guest) return;
  St32(guest, in.fd_count);
  for (u_int i = 0; i < in.fd_count; ++i) St32(guest + 4 + i * 4, uint32_t(in.fd_array[i]));
}

// The engine sends to its own server as ":port" (0.0.0.0), which Winsock won't
// send to, so those go to loopback; replies must then look like they came from
// 0.0.0.0 again, or the engine drops them as from an unknown peer.
std::mutex g_any_mutex;
std::set<std::pair<SOCKET, uint16_t>> g_any_peers;  // (socket, port in network order)
void FixAnyDestination(SOCKET s, sockaddr_in* a) {
  if (a->sin_addr.s_addr != INADDR_ANY) return;
  // JOIN GAME (lan.cpp): our client's packets to its "own" server go to the
  // host's.
  a->sin_addr.s_addr = rt::g_join_address && a->sin_port == htons(1003) ? rt::g_join_address
                                                                   : htonl(INADDR_LOOPBACK);
  std::lock_guard lock(g_any_mutex);
  g_any_peers.insert({s, a->sin_port});
}
void FixAnySource(SOCKET s, sockaddr_in* a) {
  bool from_host = rt::g_join_address && a->sin_addr.s_addr == rt::g_join_address;
  if (a->sin_addr.s_addr != htonl(INADDR_LOOPBACK) && !from_host) return;
  std::lock_guard lock(g_any_mutex);
  if (g_any_peers.count({s, a->sin_port})) a->sin_addr.s_addr = INADDR_ANY;
}

uint32_t Fail() { return 0xFFFFFFFF; }  // SOCKET_ERROR / INVALID_SOCKET

bool IsEaHost(const std::string& name) {
  auto ends = [&](const char* s) {
    size_t n = strlen(s);
    return name.size() >= n && _stricmp(name.c_str() + name.size() - n, s) == 0;
  };
  return ends(".ea.com") || ends(".arcadia");
}
}  // namespace

// ---- Winsock ----------------------------------------------------------------------
GUEST_IMPORT(NetDll_WSAStartup) {
  HostStartup();
  if (ARG32(5)) {
    memset(G(ARG32(5)), 0, 0x190);
    St16(ARG32(5), 0x0202);
    St16(ARG32(5) + 2, 0x0202);
  }
  RETURN(0);
}
GUEST_IMPORT(NetDll_WSACleanup) { RETURN(0); }
GUEST_IMPORT(NetDll_WSAGetLastError) { RETURN(WSAGetLastError()); }
GUEST_IMPORT(NetDll_WSASetLastError) { WSASetLastError(int(ARG32(3))); RETURN(0); }

// socket(caller, af, type, protocol)
GUEST_IMPORT(NetDll_socket) {
  HostStartup();
  int type = int(ARG32(5)), proto = int(ARG32(6));
  if (proto == kIpprotoVdp) proto = IPPROTO_UDP;
  SOCKET s = ::socket(AF_INET, type, proto);
  if (s != INVALID_SOCKET && int(ARG32(6)) == kIpprotoVdp) {
    std::lock_guard l(g_vdp_mutex);
    g_vdp_sockets.insert(uint32_t(s));
  }
  RT_INFO("net: socket(type %d, protocol %d) -> %llu", type, int(ARG32(6)), (unsigned long long)s);
  RETURN(s == INVALID_SOCKET ? Fail() : uint32_t(s));
}
GUEST_IMPORT(NetDll_closesocket) {
  {
    std::lock_guard l(g_vdp_mutex);
    g_vdp_sockets.erase(ARG32(4));
  }
  RETURN(::closesocket(Host(ARG32(4))));
}
GUEST_IMPORT(NetDll_shutdown) { RETURN(::shutdown(Host(ARG32(4)), int(ARG32(5)))); }

// ioctlsocket(caller, s, cmd, u32* arg)
GUEST_IMPORT(NetDll_ioctlsocket) {
  u_long v = ARG32(6) ? Ld32(ARG32(6)) : 0;
  int r = ::ioctlsocket(Host(ARG32(4)), long(ARG32(5)), &v);
  if (r == 0 && ARG32(6)) St32(ARG32(6), uint32_t(v));
  RETURN(r);
}

// setsockopt(caller, s, level, name, value, length). Values are big-endian ints
// except SO_LINGER (two be16).
GUEST_IMPORT(NetDll_setsockopt) {
  int level = int(ARG32(5)), name = int(ARG32(6)), len = int(ARG32(8));
  uint32_t val = ARG32(7);
  char buf[16] = {};
  if (level == SOL_SOCKET && name == SO_LINGER && len >= 4) {
    u_short l[2] = {Ld16(val), Ld16(val + 2)};
    memcpy(buf, l, 4);
    len = 4;
  } else if (len == 4) {
    uint32_t v = Ld32(val);
    memcpy(buf, &v, 4);
  } else if (len == 1) {
    buf[0] = char(*G(val));
  } else {
    len = std::min(len, int(sizeof(buf)));
    memcpy(buf, G(val), len);
  }
  int r = ::setsockopt(Host(ARG32(4)), level, name, buf, len);
  if (r != 0) {
    // Xbox-only options (e.g. 0x5801 SO_NONBLOCK-style, VDP/security ones): ignore.
    RT_WARN("net: setsockopt(level %X, option %X) failed (%d), ignored", level, name, WSAGetLastError());
    r = 0;
  }
  RETURN(r);
}
// getsockopt(caller, s, level, name, value, length*)
GUEST_IMPORT(NetDll_getsockopt) {
  char buf[16] = {};
  int len = sizeof(buf);
  int r = ::getsockopt(Host(ARG32(4)), int(ARG32(5)), int(ARG32(6)), buf, &len);
  if (r == 0 && ARG32(7) && ARG32(8)) {
    uint32_t room = Ld32(ARG32(8));
    if (len == 4 && room >= 4) {
      uint32_t v;
      memcpy(&v, buf, 4);
      St32(ARG32(7), v);
    } else {
      len = std::min<int>(len, int(room));
      memcpy(G(ARG32(7)), buf, len);
    }
    St32(ARG32(8), uint32_t(len));
  }
  RETURN(r);
}

// bind(caller, s, name, length)
GUEST_IMPORT(NetDll_bind) {
  sockaddr_in a;
  if (!ToHost(ARG32(5), ARG32(6), &a)) { WSASetLastError(WSAEAFNOSUPPORT); RETURN(Fail()); }
  if (a.sin_port && g_port_offset) a.sin_port = htons(uint16_t(ntohs(a.sin_port) + g_port_offset));
  int r = ::bind(Host(ARG32(4)), (sockaddr*)&a, sizeof(a));
  RT_INFO("net: bind(%u, %s) -> %d (%d)", ARG32(4), AddrText(a).c_str(), r, r ? WSAGetLastError() : 0);
  RETURN(r);
}
// connect(caller, s, name, length)
GUEST_IMPORT(NetDll_connect) {
  sockaddr_in a;
  if (!ToHost(ARG32(5), ARG32(6), &a)) { WSASetLastError(WSAEAFNOSUPPORT); RETURN(Fail()); }
  int r = ::connect(Host(ARG32(4)), (sockaddr*)&a, sizeof(a));
  int err = r ? WSAGetLastError() : 0;
  RT_INFO("net: connect(%u, %s) -> %d (%d)", ARG32(4), AddrText(a).c_str(), r, err);
  WSASetLastError(err);
  RETURN(r);
}
GUEST_IMPORT(NetDll_listen) { RETURN(::listen(Host(ARG32(4)), int(ARG32(5)))); }
// accept(caller, s, addr, length*)
GUEST_IMPORT(NetDll_accept) {
  sockaddr_in a{};
  int len = sizeof(a);
  SOCKET s = ::accept(Host(ARG32(4)), (sockaddr*)&a, &len);
  if (s == INVALID_SOCKET) RETURN(Fail());
  ToGuest(a, ARG32(5), ARG32(6));
  RT_INFO("net: accept(%u) <- %s", ARG32(4), AddrText(a).c_str());
  RETURN(uint32_t(s));
}
// getsockname / getpeername(caller, s, name, length*)
GUEST_IMPORT(NetDll_getsockname) {
  sockaddr_in a{};
  int len = sizeof(a);
  int r = ::getsockname(Host(ARG32(4)), (sockaddr*)&a, &len);
  if (r == 0) {
    if (a.sin_port && g_port_offset) a.sin_port = htons(uint16_t(ntohs(a.sin_port) - g_port_offset));
    ToGuest(a, ARG32(5), ARG32(6));
  }
  RETURN(r);
}
GUEST_IMPORT(NetDll_getpeername) {
  sockaddr_in a{};
  int len = sizeof(a);
  int r = ::getpeername(Host(ARG32(4)), (sockaddr*)&a, &len);
  if (r == 0) ToGuest(a, ARG32(5), ARG32(6));
  RETURN(r);
}

// select(caller, nfds, read, write, except, timeval*). timeval: be32 sec, be32 usec.
GUEST_IMPORT(NetDll_select) {
  fd_set r, w, e;
  FdsToHost(ARG32(5), &r);
  FdsToHost(ARG32(6), &w);
  FdsToHost(ARG32(7), &e);
  timeval tv, *ptv = nullptr;
  if (ARG32(8)) {
    tv.tv_sec = long(Ld32(ARG32(8)));
    tv.tv_usec = long(Ld32(ARG32(8) + 4));
    ptv = &tv;
  }
  int n = 0;
  if (r.fd_count + w.fd_count + e.fd_count == 0) {
    // Winsock rejects empty sets; the console just waits.
    if (ptv) std::this_thread::sleep_for(std::chrono::microseconds(tv.tv_sec * 1000000ll + tv.tv_usec));
  } else {
    n = ::select(0, ARG32(5) ? &r : nullptr, ARG32(6) ? &w : nullptr, ARG32(7) ? &e : nullptr, ptv);
    if (n < 0) RETURN(Fail());
  }
  FdsToGuest(r, ARG32(5));
  FdsToGuest(w, ARG32(6));
  FdsToGuest(e, ARG32(7));
  RETURN(n);
}
// __WSAFDIsSet(s, fd_set*)
GUEST_IMPORT(NetDll___WSAFDIsSet) {
  uint32_t set = ARG32(4), n = std::min<uint32_t>(Ld32(set), 64);
  for (uint32_t i = 0; i < n; ++i)
    if (Ld32(set + 4 + i * 4) == ARG32(3)) RETURN(1);
  RETURN(0);
}

// recv(caller, s, buf, len, flags)
GUEST_IMPORT(NetDll_recv) {
  int r = ::recv(Host(ARG32(4)), (char*)G(ARG32(5)), int(ARG32(6)), int(ARG32(7)));
  // The Theater server's USER reply ends the online login (used by --host-map).
  if (r >= 4 && !memcmp(G(ARG32(5)), "USER", 4)) g_online_ready = true;
  RETURN(uint32_t(r));
}
// A Theater EGAM (join request) tells the host which port to reach us on. With
// --port-offset our sockets are bound that much higher than the game thinks,
// so add it to PORT and R-INT-PORT (packet: 4-byte type, 4 flags, be32 length,
// then key=value lines).
static std::string OffsetJoinPorts(const char* p, int n) {
  std::string out(p, n), body;
  size_t pos = 12;
  while (pos < out.size()) {
    size_t end = out.find('\n', pos);
    if (end == std::string::npos) end = out.size();
    std::string line = out.substr(pos, end - pos);
    for (const char* key : {"PORT=", "R-INT-PORT="})
      if (!line.compare(0, strlen(key), key))
        line = key + std::to_string(atoi(line.c_str() + strlen(key)) + g_port_offset);
    body += line;
    if (end < out.size()) body += '\n';
    pos = end + 1;
  }
  out = out.substr(0, 12) + body;
  uint32_t len = _byteswap_ulong(uint32_t(out.size()));
  memcpy(&out[8], &len, 4);
  return out;
}

// send(caller, s, buf, len, flags)
GUEST_IMPORT(NetDll_send) {
  const char* data = (const char*)G(ARG32(5));
  int n = int(ARG32(6));
  if (g_port_offset && n > 12 && !memcmp(data, "EGAM", 4)) {
    std::string fixed = OffsetJoinPorts(data, n);
    int r = ::send(Host(ARG32(4)), fixed.data(), int(fixed.size()), int(ARG32(7)));
    RETURN(r == int(fixed.size()) ? uint32_t(n) : uint32_t(r));
  }
  RETURN(::send(Host(ARG32(4)), data, n, int(ARG32(7))));
}
// recvfrom(caller, s, buf, len, flags, from, fromlen*)
GUEST_IMPORT(NetDll_recvfrom) {
  sockaddr_in a{};
  int len = sizeof(a);
  bool vdp = IsVdp(ARG32(4)) && ARG32(6) > 2;
  uint8_t* buf = G(ARG32(5)) + (vdp ? 2 : 0);
  int r = ::recvfrom(Host(ARG32(4)), (char*)buf, int(ARG32(6)) - (vdp ? 2 : 0), int(ARG32(7)), (sockaddr*)&a, &len);
  if (vdp && r >= 0) {
    St16(ARG32(5), uint16_t(r));  // game data only, no voice
    r += 2;
  }
  if (r > 0) {
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint16_t>> seen;  // log the first packet from each address
    if (std::lock_guard l(m); seen.insert({a.sin_addr.s_addr, a.sin_port}).second)
      RT_INFO("net: first recvfrom(%u, %s, %d bytes) %s", ARG32(4), AddrText(a).c_str(), r,
              HexBytes(G(ARG32(5)), std::min(r, 32)).c_str());
  }
  if (r >= 0) {
    FixAnySource(Host(ARG32(4)), &a);
    ToGuest(a, ARG32(8), ARG32(9));
  }
  RETURN(r);
}
// sendto(caller, s, buf, len, flags, to, tolen)
GUEST_IMPORT(NetDll_sendto) {
  sockaddr_in a;
  if (!ARG32(9)) RETURN(::send(Host(ARG32(4)), (const char*)G(ARG32(5)), int(ARG32(6)), int(ARG32(7))));
  if (!ToHost(ARG32(8), ARG32(9), &a)) { WSASetLastError(WSAEAFNOSUPPORT); RETURN(Fail()); }
  FixAnyDestination(Host(ARG32(4)), &a);
  const uint8_t* data = G(ARG32(5));
  int size = int(ARG32(6));
  bool vdp = IsVdp(ARG32(4)) && size >= 2;
  if (vdp) size = std::min<int>(Ld16(ARG32(5)), size - 2), data += 2;
  int r = ::sendto(Host(ARG32(4)), (const char*)data, size, int(ARG32(7)), (sockaddr*)&a, sizeof(a));
  if (vdp && r >= 0) r = int(ARG32(6));  // the guest sent the whole VDP packet
  static std::mutex m;
  static std::set<std::pair<uint32_t, uint16_t>> seen;  // log the first packet to each address
  if (std::lock_guard l(m); seen.insert({a.sin_addr.s_addr, a.sin_port}).second)
    RT_INFO("net: first sendto(%u, %s, %u bytes) -> %d %s", ARG32(4), AddrText(a).c_str(), ARG32(6), r,
            HexBytes(G(ARG32(5)), std::min(ARG32(6), 32u)).c_str());
  if (r < 0) {
    int err = WSAGetLastError();
    RT_WARN("net: sendto(%u, %s) failed (%d)", ARG32(4), AddrText(a).c_str(), err);
    WSASetLastError(err);
  }
  RETURN(r);
}
// inet_addr(cp): returns the address in network order, i.e. as the guest reads it.
GUEST_IMPORT(NetDll_inet_addr) {
  RETURN(_byteswap_ulong(::inet_addr(ReadCString(ARG32(3)).c_str())));
}

// ---- XNet ---------------------------------------------------------------------------
GUEST_IMPORT(NetDll_XNetStartup) { HostStartup(); RETURN(0); }
GUEST_IMPORT(NetDll_XNetCleanup) { RETURN(0); }
GUEST_IMPORT(NetDll_XNetRandom) {
  static std::mt19937 rng{std::random_device{}()};
  for (uint32_t i = 0; i < ARG32(5); ++i) St8(ARG32(4) + i, uint8_t(rng()));
  RETURN(0);
}
// Link active, 100 Mbps, full duplex.
GUEST_IMPORT(NetDll_XNetGetEthernetLinkStatus) { RETURN(0x01 | 0x02 | 0x08); }

// XNADDR (0x24): in_addr ina, in_addr inaOnline, be16 wPortOnline, u8 abEnet[6], u8 abOnline[20].
static void FillXnAddr(uint32_t xna, uint32_t ina) {
  memset(G(xna), 0, 0x24);
  memcpy(G(xna), &ina, 4);
  memcpy(G(xna + 4), &ina, 4);
  St16(xna + 8, 3074);
  uint8_t mac[6] = {0x00, 0x22, 0x48, 0, 0, 0};
  memcpy(mac + 2, &ina, 4);  // unique per host and stable
  memcpy(G(xna + 10), mac, 6);
  memcpy(G(xna + 16), &ina, 4);
}
// XNetGetTitleXnAddr(caller, xnaddr*)
GUEST_IMPORT(NetDll_XNetGetTitleXnAddr) {
  HostStartup();
  FillXnAddr(ARG32(4), g_lan_ip);
  RETURN(0x02 | 0x08 | 0x20 | 0x40 | 0x80);  // ethernet, DHCP, gateway, DNS, online
}
// XNetXnAddrToInAddr(caller, xnaddr*, xnkid*, in_addr*)
GUEST_IMPORT(NetDll_XNetXnAddrToInAddr) {
  memcpy(G(ARG32(6)), G(ARG32(4)), 4);
  RETURN(0);
}
// XNetInAddrToXnAddr(caller, in_addr, xnaddr*, xnkid*)
GUEST_IMPORT(NetDll_XNetInAddrToXnAddr) {
  uint32_t ina = _byteswap_ulong(ARG32(4));
  if (ARG32(5)) FillXnAddr(ARG32(5), ina);
  if (ARG32(6)) memset(G(ARG32(6)), 0, 8);
  RETURN(0);
}
// XNetServerToInAddr(caller, in_addr, service id, in_addr*)
GUEST_IMPORT(NetDll_XNetServerToInAddr) {
  St32(ARG32(6), ARG32(4));
  RETURN(0);
}
GUEST_IMPORT(NetDll_XNetConnect) { RETURN(0); }
GUEST_IMPORT(NetDll_XNetGetConnectStatus) { RETURN(2); }  // XNET_CONNECT_STATUS_CONNECTED
// XNetCreateKey(caller, xnkid*, xnkey*)
GUEST_IMPORT(NetDll_XNetCreateKey) {
  static std::mt19937 rng{std::random_device{}()};
  for (int i = 0; i < 8; ++i) St8(ARG32(4) + i, uint8_t(rng()));
  for (int i = 0; i < 16; ++i) St8(ARG32(5) + i, uint8_t(rng()));
  St8(ARG32(4), *G(ARG32(4)) & 0x0F);  // XNET_XNKID_SYSTEM_LINK
  RETURN(0);
}
GUEST_IMPORT(NetDll_XNetRegisterKey) { RETURN(0); }
GUEST_IMPORT(NetDll_XNetUnregisterKey) { RETURN(0); }

// XNetDnsLookup(caller, name, event, XNDNS**). XNDNS: be32 status, be32 count,
// in_addr addresses[8]. EA names go to --online-server when it is set.
GUEST_IMPORT(NetDll_XNetDnsLookup) {
  HostStartup();
  std::string name = ReadCString(ARG32(4));
  std::string target = (!g_online_server.empty() && IsEaHost(name)) ? g_online_server : name;
  uint32_t dns = g_system_heap->Alloc(40, 4);
  memset(G(dns), 0, 40);
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  uint32_t count = 0;
  if (getaddrinfo(target.c_str(), nullptr, &hints, &res) == 0) {
    for (addrinfo* p = res; p && count < 8; p = p->ai_next, ++count)
      memcpy(G(dns + 8 + count * 4), &((sockaddr_in*)p->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
  }
  St32(dns, count ? 0 : uint32_t(WSAHOST_NOT_FOUND));
  St32(dns + 4, count);
  RT_INFO("net: DNS %s -> %s (%u addresses)", name.c_str(), target.c_str(), count);
  St32(ARG32(6), dns);
  if (ARG32(5)) {
    if (uint32_t e = HandleToObject(ARG32(5))) SetEvent(e);
  }
  RETURN(0);
}
GUEST_IMPORT(NetDll_XNetDnsRelease) {
  if (ARG32(4)) g_system_heap->Free(ARG32(4));
  RETURN(0);
}

// ---- WSA events and overlapped receives ------------------------------------------------
// WSAEVENT is a kernel event handle (manual reset).
GUEST_IMPORT(NetDll_WSACreateEvent) {
  uint32_t o = g_system_heap->Alloc(0x20, 0x10);
  memset(G(o), 0, 0x20);
  InitDispatcherHeader(o, kNotificationEvent, 0);
  RETURN(CreateHandle(o));
}
GUEST_IMPORT(NetDll_WSACloseEvent) { RETURN(CloseHandle(ARG32(3)) ? 1 : 0); }
GUEST_IMPORT(NetDll_WSASetEvent) {
  uint32_t o = HandleToObject(ARG32(3));
  if (o) SetEvent(o);
  RETURN(o ? 1 : 0);
}
GUEST_IMPORT(NetDll_WSAResetEvent) {
  uint32_t o = HandleToObject(ARG32(3));
  if (o) ResetEvent(o);
  RETURN(o ? 1 : 0);
}
// WSAWaitForMultipleEvents(count, events*, wait_all, timeout ms, alertable).
// Results match the NT ones: WSA_WAIT_EVENT_0 + i, WSA_WAIT_TIMEOUT = 0x102.
GUEST_IMPORT(NetDll_WSAWaitForMultipleEvents) {
  uint32_t n = std::min<uint32_t>(ARG32(3), 64);
  uint32_t objs[64];
  for (uint32_t i = 0; i < n; ++i)
    if (!(objs[i] = HandleToObject(Ld32(ARG32(4) + i * 4)))) {
      WSASetLastError(WSA_INVALID_HANDLE);
      RETURN(0xFFFFFFFF);
    }
  thread_local uint32_t timeout = [] { return g_system_heap->Alloc(8, 8); }();
  uint32_t ptr = 0;
  if (ARG32(6) != 0xFFFFFFFF) {
    St64(timeout, uint64_t(-int64_t(ARG32(6)) * 10000));
    ptr = timeout;
  }
  RETURN(WaitMultiple(objs, n, ARG32(5) != 0, ptr, ARG32(7) != 0));
}

namespace {
// WSAOVERLAPPED: be32 Internal (status), InternalHigh (bytes), Offset, OffsetHigh, hEvent.
// WSABUF: be32 len, be32 buf.
struct PendingRecv {
  SOCKET s;
  uint32_t bufs, count, flags_ptr, from, from_len, overlapped;
};
int RecvInto(SOCKET s, uint32_t bufs, uint32_t count, uint32_t from, uint32_t from_len, int* err) {
  std::vector<WSABUF> host(std::min<uint32_t>(count, 16));
  for (size_t i = 0; i < host.size(); ++i) {
    host[i].len = Ld32(bufs + uint32_t(i) * 8);
    host[i].buf = (CHAR*)G(Ld32(bufs + uint32_t(i) * 8 + 4));
  }
  DWORD got = 0, flags = 0;
  sockaddr_in a{};
  int alen = sizeof(a);
  int r = from ? WSARecvFrom(s, host.data(), DWORD(host.size()), &got, &flags, (sockaddr*)&a, &alen, nullptr, nullptr)
               : WSARecv(s, host.data(), DWORD(host.size()), &got, &flags, nullptr, nullptr);
  if (r != 0) {
    *err = WSAGetLastError();
    return -1;
  }
  if (from) {
    FixAnySource(s, &a);
    ToGuest(a, from, from_len);
  }
  return int(got);
}
void Complete(uint32_t ov, uint32_t status, uint32_t bytes) {
  St32(ov + 4, bytes);
  St32(ov, status);
  if (uint32_t e = HandleToObject(Ld32(ov + 16))) SetEvent(e);
}
// Waits on a host thread until the socket is readable, then receives and completes.
void StartPending(PendingRecv p) {
  St32(p.overlapped, ERROR_IO_PENDING);
  if (uint32_t e = HandleToObject(Ld32(p.overlapped + 16))) ResetEvent(e);
  std::thread([p] {
    for (;;) {
      fd_set r, e;
      FD_ZERO(&r);
      FD_ZERO(&e);
      FD_SET(p.s, &r);
      FD_SET(p.s, &e);
      timeval tv{1, 0};
      int n = ::select(0, &r, nullptr, &e, &tv);
      if (n == 0) continue;
      int err = 0, got = n < 0 ? (err = WSAGetLastError(), -1) : RecvInto(p.s, p.bufs, p.count, p.from, p.from_len, &err);
      if (got < 0 && err == WSAEWOULDBLOCK) continue;
      if (p.flags_ptr) St32(p.flags_ptr, 0);
      Complete(p.overlapped, got < 0 ? uint32_t(err) : 0, got < 0 ? 0 : uint32_t(got));
      return;
    }
  }).detach();
}
uint32_t OverlappedRecv(PPCContext& ctx, PendingRecv p, uint32_t bytes_ptr) {
  int err = 0;
  int got = RecvInto(p.s, p.bufs, p.count, p.from, p.from_len, &err);
  if (got >= 0) {
    Out32(bytes_ptr, uint32_t(got));
    Out32(p.flags_ptr, 0);
    if (p.overlapped) Complete(p.overlapped, 0, uint32_t(got));
    return 0;
  }
  if (err == WSAEWOULDBLOCK && p.overlapped) {
    StartPending(p);
    WSASetLastError(WSA_IO_PENDING);
    return 0xFFFFFFFF;
  }
  WSASetLastError(err);
  return 0xFFFFFFFF;
}
}  // namespace

// WSARecv(caller, s, bufs, count, bytes*, flags*, overlapped, completion)
GUEST_IMPORT(NetDll_WSARecv) {
  if (ARG32(10)) RT_WARN("net: WSARecv completion routines are not supported");
  RETURN(OverlappedRecv(ctx, {Host(ARG32(4)), ARG32(5), ARG32(6), ARG32(8), 0, 0, ARG32(9)}, ARG32(7)));
}
// WSARecvFrom(caller, s, bufs, count, bytes*, flags*, from, fromlen*, overlapped, completion)
GUEST_IMPORT(NetDll_WSARecvFrom) {
  uint32_t ov = Ld32(ctx.r1.u32 + 0x54);
  if (Ld32(ctx.r1.u32 + 0x5C)) RT_WARN("net: WSARecvFrom completion routines are not supported");
  RETURN(OverlappedRecv(ctx, {Host(ARG32(4)), ARG32(5), ARG32(6), ARG32(8), ARG32(9), ARG32(10), ov}, ARG32(7)));
}
// WSAGetOverlappedResult(caller, s, overlapped, bytes*, wait, flags*)
GUEST_IMPORT(NetDll_WSAGetOverlappedResult) {
  uint32_t ov = ARG32(5);
  if (Ld32(ov) == ERROR_IO_PENDING && ARG32(7)) {
    uint32_t e = HandleToObject(Ld32(ov + 16));
    if (e) WaitMultiple(&e, 1, false, 0, false);
  }
  uint32_t status = Ld32(ov);
  if (status == ERROR_IO_PENDING) { WSASetLastError(WSA_IO_INCOMPLETE); RETURN(0); }
  Out32(ARG32(6), Ld32(ov + 4));
  Out32(ARG32(8), 0);
  if (status) { WSASetLastError(int(status)); RETURN(0); }
  RETURN(1);
}

namespace rt {
// Lists the game this copy hosts with Arcadia (--online-server). The 360 game
// never registers its own match with Theater (no CGAM), so Quick Match would
// never find it; Arcadia's external-game port (42071) takes a one-line
// registration and keeps the game listed while this connection stays open.
void RegisterHostedGame(uint16_t port, const std::string& map) {
  if (g_online_server.empty()) return;
  std::thread([port, map] {
    HostStartup();
    char lan[32];
    inet_ntop(AF_INET, &g_lan_ip, lan, sizeof(lan));
    for (int attempt = 0; attempt < 10; ++attempt) {
      SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in a{AF_INET, htons(42071)};
      inet_pton(AF_INET, g_online_server.c_str(), &a.sin_addr);
      if (::connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        char line[256];
        int n = snprintf(line, sizeof(line), "REG user=%s port=%u int-ip=%s map=%s\n", g_gamertag.c_str(), port, lan,
                         map.c_str());
        ::send(s, line, n, 0);
        char reply[64] = {};
        int r = ::recv(s, reply, sizeof(reply) - 1, 0);
        if (r > 0 && !strncmp(reply, "OK", 2)) {
          reply[strcspn(reply, "\r\n")] = 0;
          RT_INFO("net: hosted game listed on Arcadia (%s, port %u)", reply, port);
          while (::recv(s, reply, sizeof(reply), 0) > 0) {}  // stay listed until Arcadia goes away
          closesocket(s);
          RT_INFO("net: Arcadia registration closed");
          return;
        }
        RT_INFO("net: Arcadia refused the hosted game: %s", r > 0 ? reply : "(no answer)");
      }
      closesocket(s);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}
}  // namespace rt
