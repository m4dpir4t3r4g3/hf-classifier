// Minimal blocking TCP sockets for Windows (Winsock 2) and POSIX, enough for
// a WebSocket client to SDRconnect and for the fake SDRconnect server used in
// the tests. No dependencies beyond the platform's socket library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hfc {

class TcpSocket {
 public:
  TcpSocket() = default;
  ~TcpSocket();
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;
  TcpSocket(TcpSocket&& o) noexcept;
  TcpSocket& operator=(TcpSocket&& o) noexcept;

  // Connects to host:port (name or address), giving up after timeoutMs.
  bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string* error);
  bool valid() const { return fd_ != kInvalid; }

  // Sends all `n` bytes; false if the connection failed.
  bool sendAll(const void* data, std::size_t n);
  // Receives up to `n` bytes. Returns the count, 0 when the peer closed the
  // connection, -1 on error, -2 when the receive timeout expired.
  long recvSome(void* data, std::size_t n);
  // 0 (the default) blocks without limit.
  void setRecvTimeout(int ms);
  void setNoDelay(bool on);
  // Unblocks a recvSome() waiting in another thread; the socket stays open.
  void shutdown();
  void close();

 private:
  friend class TcpListener;
  using Handle = std::intptr_t;
  static constexpr Handle kInvalid = -1;
  explicit TcpSocket(Handle fd) : fd_(fd) {}
  Handle fd_ = kInvalid;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Listens on address:port; port 0 picks a free port (see port()).
  bool listen(const std::string& address, uint16_t port, std::string* error);
  uint16_t port() const { return port_; }
  // Waits up to timeoutMs for a connection; an invalid socket on timeout.
  TcpSocket accept(int timeoutMs);
  void close();

 private:
  std::intptr_t fd_ = -1;
  uint16_t port_ = 0;
};

}  // namespace hfc
