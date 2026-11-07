// Linux: the Winsock names net.cpp and lan.cpp use, on BSD sockets. The guest
// speaks Winsock (the 360's is), so error codes and socket option numbers it
// sees or passes are Windows ones: errors are mapped from errno, options to
// Linux's (MapSockOpt).
#pragma once
#ifndef _WIN32

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

using SOCKET = int;
using u_long = unsigned long;
using u_short = unsigned short;
using u_int = unsigned int;
using CHAR = char;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
inline int closesocket(SOCKET s) { return close(s); }

struct WSADATA {
  int unused;
};
#define MAKEWORD(a, b) ((unsigned short)(((a) & 0xFF) | (((b) & 0xFF) << 8)))
inline int WSAStartup(unsigned short, WSADATA*) { return 0; }
inline int WSACleanup() { return 0; }

// Windows error codes, as the guest knows them.
enum : int {
  WSA_INVALID_HANDLE = 6, WSA_IO_INCOMPLETE = 996, WSA_IO_PENDING = 997,
  WSAEINTR = 10004, WSAEBADF = 10009, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022,
  WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035, WSAEINPROGRESS = 10036, WSAEALREADY = 10037,
  WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039, WSAEMSGSIZE = 10040, WSAEPROTOTYPE = 10041,
  WSAENOPROTOOPT = 10042, WSAEPROTONOSUPPORT = 10043, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047,
  WSAEADDRINUSE = 10048, WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051,
  WSAECONNABORTED = 10053, WSAECONNRESET = 10054, WSAENOBUFS = 10055, WSAEISCONN = 10056,
  WSAENOTCONN = 10057, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061, WSAEHOSTUNREACH = 10065,
  WSAHOST_NOT_FOUND = 11001,
};

namespace posixnet {
// WSASetLastError keeps its value in errno, offset past any errno value.
constexpr int kSetFlag = 0x40000000;
inline int FromErrno(int e) {
  switch (e) {
    case 0: return 0;
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
#if EAGAIN != EWOULDBLOCK
    case EAGAIN: return WSAEWOULDBLOCK;
#endif
    case EINPROGRESS: return WSAEWOULDBLOCK;  // a non-blocking connect, as Winsock reports it
    case EINTR: return WSAEINTR;
    case EBADF: return WSAENOTSOCK;
    case EACCES: return WSAEACCES;
    case EFAULT: return WSAEFAULT;
    case EINVAL: return WSAEINVAL;
    case EMFILE: return WSAEMFILE;
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EDESTADDRREQ: return WSAEDESTADDRREQ;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EPROTOTYPE: return WSAEPROTOTYPE;
    case ENOPROTOOPT: return WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    default: return WSAEINVAL;
  }
}

// Windows socket option (level, name) -> Linux. False: no such option here.
inline bool MapSockOpt(int& level, int& name) {
  if (level == 0xFFFF) {  // SOL_SOCKET
    level = SOL_SOCKET;
    switch (name) {
      case 0x0004: name = SO_REUSEADDR; return true;
      case 0x0008: name = SO_KEEPALIVE; return true;
      case 0x0020: name = SO_BROADCAST; return true;
      case 0x0080: name = SO_LINGER; return true;
      case 0x1001: name = SO_SNDBUF; return true;
      case 0x1002: name = SO_RCVBUF; return true;
      case 0x1005: name = SO_SNDTIMEO; return true;
      case 0x1006: name = SO_RCVTIMEO; return true;
      case 0x1007: name = SO_ERROR; return true;
      case 0x1008: name = SO_TYPE; return true;
      default: return false;
    }
  }
  if (level == 6 && name == 1) return true;  // IPPROTO_TCP, TCP_NODELAY: the same numbers
  if (level == 0) {                          // IPPROTO_IP
    switch (name) {
      case 2: name = IP_HDRINCL; return true;
      case 4: name = IP_TTL; return true;
      case 9: name = IP_MULTICAST_IF; return true;
      case 10: name = IP_MULTICAST_TTL; return true;
      case 11: name = IP_MULTICAST_LOOP; return true;
      case 12: name = IP_ADD_MEMBERSHIP; return true;
      case 13: name = IP_DROP_MEMBERSHIP; return true;
      default: return false;
    }
  }
  return false;
}
}  // namespace posixnet

inline int WSAGetLastError() {
  int e = errno;
  return (e & posixnet::kSetFlag) ? (e & ~posixnet::kSetFlag) : posixnet::FromErrno(e);
}
inline void WSASetLastError(int e) { errno = e ? (e | posixnet::kSetFlag) : 0; }

// ioctlsocket with the Windows command numbers.
inline int ioctlsocket(SOCKET s, long cmd, u_long* v) {
  if (uint32_t(cmd) == 0x8004667Eu) {  // FIONBIO
    int fl = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, *v ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) < 0 ? SOCKET_ERROR : 0;
  }
  if (uint32_t(cmd) == 0x4004667Fu) {  // FIONREAD
    int n = 0;
    int r = ioctl(s, FIONREAD, &n);
    *v = u_long(n);
    return r < 0 ? SOCKET_ERROR : 0;
  }
  errno = EINVAL;
  return SOCKET_ERROR;
}

// The Winsock calls that take an int length.
inline int getsockname(SOCKET s, sockaddr* a, int* len) {
  socklen_t l = socklen_t(*len);
  int r = ::getsockname(s, a, &l);
  *len = int(l);
  return r;
}
inline int getpeername(SOCKET s, sockaddr* a, int* len) {
  socklen_t l = socklen_t(*len);
  int r = ::getpeername(s, a, &l);
  *len = int(l);
  return r;
}
inline SOCKET accept(SOCKET s, sockaddr* a, int* len) {
  socklen_t l = len ? socklen_t(*len) : 0;
  SOCKET r = ::accept(s, a, len ? &l : nullptr);
  if (len) *len = int(l);
  return r;
}
inline int recvfrom(SOCKET s, char* buf, int n, int flags, sockaddr* a, int* len) {
  socklen_t l = len ? socklen_t(*len) : 0;
  int r = int(::recvfrom(s, buf, size_t(n), flags, a, len ? &l : nullptr));
  if (len) *len = int(l);
  return r;
}
inline int getsockopt(SOCKET s, int level, int name, char* buf, int* len) {
  socklen_t l = socklen_t(*len);
  int r = ::getsockopt(s, level, name, buf, &l);
  *len = int(l);
  return r;
}

struct WSABUF {
  u_long len;
  char* buf;
};
struct WSAOVERLAPPED;
inline int RecvBufs(SOCKET s, WSABUF* bufs, DWORD count, DWORD* got, DWORD* flags, sockaddr* from, int* from_len) {
  iovec iov[16];
  DWORD n = count < 16 ? count : 16;
  for (DWORD i = 0; i < n; ++i) iov[i] = {bufs[i].buf, size_t(bufs[i].len)};
  msghdr m{};
  m.msg_iov = iov;
  m.msg_iovlen = n;
  m.msg_name = from;
  m.msg_namelen = from_len ? socklen_t(*from_len) : 0;
  ssize_t r = recvmsg(s, &m, flags ? int(*flags) : 0);
  if (r < 0) return SOCKET_ERROR;
  if (got) *got = DWORD(r);
  if (from_len) *from_len = int(m.msg_namelen);
  if (flags) *flags = 0;
  return 0;
}
inline int WSARecv(SOCKET s, WSABUF* bufs, DWORD count, DWORD* got, DWORD* flags, WSAOVERLAPPED*, void*) {
  return RecvBufs(s, bufs, count, got, flags, nullptr, nullptr);
}
inline int WSARecvFrom(SOCKET s, WSABUF* bufs, DWORD count, DWORD* got, DWORD* flags, sockaddr* from, int* from_len,
                       WSAOVERLAPPED*, void*) {
  return RecvBufs(s, bufs, count, got, flags, from, from_len);
}

#endif  // !_WIN32
