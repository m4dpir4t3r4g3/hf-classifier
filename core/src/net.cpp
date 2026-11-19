#include "hfc/net.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace hfc {

namespace {

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kRawInvalid = INVALID_SOCKET;

// Winsock must be started once per process before any socket call.
struct WinsockInit {
  WinsockInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
  ~WinsockInit() { WSACleanup(); }
};
void ensureNet() { static WinsockInit init; }
int lastError() { return WSAGetLastError(); }
void closeRaw(RawSocket s) { closesocket(s); }
bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
bool timedOut(int e) { return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
void setNonBlocking(RawSocket s, bool on) {
  u_long v = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &v);
}
int pollOne(RawSocket s, short events, int timeoutMs) {
  WSAPOLLFD p{};
  p.fd = s;
  p.events = events;
  return WSAPoll(&p, 1, timeoutMs);
}
// A non-blocking connect finished (or failed). select(), not WSAPoll(): on
// Windows 10 before version 2004, WSAPoll does not report a failed connect,
// so a refused connection would wait the whole timeout.
int waitConnected(RawSocket s, int timeoutMs) {
  fd_set w, e;
  FD_ZERO(&w);
  FD_ZERO(&e);
  FD_SET(s, &w);
  FD_SET(s, &e);
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  return select(0, nullptr, &w, &e, &tv);
}
#else
using RawSocket = int;
constexpr RawSocket kRawInvalid = -1;
void ensureNet() {}
int lastError() { return errno; }
void closeRaw(RawSocket s) { ::close(s); }
bool wouldBlock(int e) { return e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN; }
bool timedOut(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
void setNonBlocking(RawSocket s, bool on) {
  int f = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
int pollOne(RawSocket s, short events, int timeoutMs) {
  pollfd p{};
  p.fd = s;
  p.events = events;
  return ::poll(&p, 1, timeoutMs);
}
int waitConnected(RawSocket s, int timeoutMs) { return pollOne(s, POLLOUT, timeoutMs); }
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // e.g. macOS (not a supported platform): SIGPIPE is not suppressed there
#endif
#endif

RawSocket raw(std::intptr_t h) { return static_cast<RawSocket>(h); }

std::string errorText(const char* what) { return std::string(what) + " (error " + std::to_string(lastError()) + ")"; }

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalid; }

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    o.fd_ = kInvalid;
  }
  return *this;
}

bool TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs, std::string* error) {
  ensureNet();
  close();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* list = nullptr;
  const std::string service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &list) != 0 || !list) {
    if (error) *error = "cannot resolve " + host;
    return false;
  }
  std::string lastErr = "no address for " + host;
  for (addrinfo* a = list; a; a = a->ai_next) {
    RawSocket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s == kRawInvalid) continue;
    // Non-blocking connect, so that an unreachable host times out quickly.
    setNonBlocking(s, true);
    int r = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
    bool ok = r == 0;
    if (!ok && wouldBlock(lastError())) {
      if (waitConnected(s, timeoutMs) > 0) {
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
        ok = soErr == 0;
        if (!ok) lastErr = "connection to " + host + ":" + service + " refused or failed (error " + std::to_string(soErr) + ")";
      } else {
        lastErr = "connection to " + host + ":" + service + " timed out";
      }
    } else if (!ok) {
      lastErr = errorText(("connection to " + host + ":" + service + " failed").c_str());
    }
    if (ok) {
      setNonBlocking(s, false);
      fd_ = static_cast<Handle>(s);
      freeaddrinfo(list);
      return true;
    }
    closeRaw(s);
  }
  freeaddrinfo(list);
  if (error) *error = lastErr;
  return false;
}

bool TcpSocket::sendAll(const void* data, std::size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n > 0 && valid()) {
    const int chunk = n > (1u << 30) ? (1 << 30) : static_cast<int>(n);
#ifdef _WIN32
    int r = ::send(raw(fd_), p, chunk, 0);
#else
    long r = ::send(raw(fd_), p, static_cast<std::size_t>(chunk), MSG_NOSIGNAL);
#endif
    if (r <= 0) return false;
    p += r;
    n -= static_cast<std::size_t>(r);
  }
  return n == 0;
}

long TcpSocket::recvSome(void* data, std::size_t n) {
  if (!valid()) return -1;
  const int chunk = n > (1u << 30) ? (1 << 30) : static_cast<int>(n);
#ifdef _WIN32
  int r = ::recv(raw(fd_), static_cast<char*>(data), chunk, 0);
#else
  long r = ::recv(raw(fd_), data, static_cast<std::size_t>(chunk), 0);
#endif
  if (r < 0) return timedOut(lastError()) ? -2 : -1;
  return static_cast<long>(r);
}

void TcpSocket::setRecvTimeout(int ms) {
  if (!valid()) return;
#ifdef _WIN32
  DWORD v = static_cast<DWORD>(ms);
  setsockopt(raw(fd_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
#else
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(raw(fd_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void TcpSocket::setNoDelay(bool on) {
  if (!valid()) return;
  int v = on ? 1 : 0;
  setsockopt(raw(fd_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof(v));
}

void TcpSocket::shutdown() {
  if (!valid()) return;
#ifdef _WIN32
  ::shutdown(raw(fd_), SD_BOTH);
#else
  ::shutdown(raw(fd_), SHUT_RDWR);
#endif
}

void TcpSocket::close() {
  if (!valid()) return;
  closeRaw(raw(fd_));
  fd_ = kInvalid;
}

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(const std::string& address, uint16_t port, std::string* error) {
  ensureNet();
  close();
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &a.sin_addr) != 1) {
    if (error) *error = "bad listen address " + address;
    return false;
  }
  RawSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kRawInvalid) {
    if (error) *error = errorText("socket");
    return false;
  }
#ifndef _WIN32
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
  if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(s, 4) != 0) {
    if (error) *error = errorText(("cannot listen on " + address + ":" + std::to_string(port)).c_str());
    closeRaw(s);
    return false;
  }
  socklen_t len = sizeof(a);
  getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  fd_ = static_cast<std::intptr_t>(s);
  return true;
}

TcpSocket TcpListener::accept(int timeoutMs) {
  if (fd_ == -1) return TcpSocket();
  if (pollOne(raw(fd_), POLLIN, timeoutMs) <= 0) return TcpSocket();
  RawSocket c = ::accept(raw(fd_), nullptr, nullptr);
  if (c == kRawInvalid) return TcpSocket();
  return TcpSocket(static_cast<std::intptr_t>(c));
}

void TcpListener::close() {
  if (fd_ == -1) return;
  closeRaw(raw(fd_));
  fd_ = -1;
}

}  // namespace hfc
