// A small WebSocket implementation (RFC 6455): the client side used to talk
// to SDRconnect's WebSocket API, and the server side used by the fake
// SDRconnect in the tests. No extensions, no TLS (SDRconnect's WSS exists
// only in headless 1.0.10 and is not needed on 127.0.0.1).
//
//   WsFrameDecoder: bytes -> frames, incremental, independent of sockets
//   encodeWsFrame:  one frame -> bytes (masked when a key is given)
//   WebSocket:      handshake, message reassembly, ping/pong, close
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "hfc/net.hpp"

namespace hfc {

enum class WsOpcode : uint8_t { Continuation = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10 };

struct WsFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::Binary;
  bool masked = false;
  std::vector<uint8_t> payload;  // unmasked
};

// One frame on the wire. `mask`: the 4-byte masking key (clients must mask
// every frame), or nullptr for an unmasked (server) frame.
std::vector<uint8_t> encodeWsFrame(WsOpcode opcode, const void* payload, std::size_t n, bool fin,
                                   const uint8_t* mask);

class WsFrameDecoder {
 public:
  // Frames whose payload exceeds this are a protocol error.
  explicit WsFrameDecoder(std::size_t maxPayload = std::size_t(64) << 20) : maxPayload_(maxPayload) {}
  std::size_t maxPayload() const { return maxPayload_; }
  void feed(const uint8_t* data, std::size_t n);
  enum class Result { Frame, NeedMore, Error };
  // Takes the next complete frame out of the buffer.
  Result next(WsFrame& out, std::string* error);
  std::size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::vector<uint8_t> buf_;
  std::size_t pos_ = 0;
  std::size_t maxPayload_;
};

struct WsMessage {
  bool binary = false;
  std::vector<uint8_t> data;
  std::string text() const { return std::string(data.begin(), data.end()); }
};

class WebSocket {
 public:
  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // Client: TCP connection and opening handshake to ws://host:port/path.
  bool connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs,
               std::string* error);
  // Server: opening handshake on a socket returned by TcpListener::accept().
  bool accept(TcpSocket socket, int timeoutMs, std::string* error);
  // Server: the same, for a request whose HTTP header (up to and including
  // the blank line) the caller has already read, e.g. an HTTP server that
  // serves pages and WebSockets on one port.
  // `early`: bytes the caller read past the header (frames already sent).
  bool acceptUpgrade(TcpSocket socket, const std::string& header, std::string* error,
                     const std::string& early = std::string());
  bool open() const { return open_; }

  // Thread safe with respect to each other and to receive().
  bool sendText(const std::string& text);
  bool sendBinary(const void* data, std::size_t n);
  bool sendClose(uint16_t code = 1000);

  // Blocks until a whole text or binary message arrives (answering pings,
  // reassembling fragments). False when the connection closed or failed;
  // `error` then says why.
  bool receive(WsMessage& msg, std::string* error);
  // Unblocks receive() in another thread and closes the connection.
  void shutdown();

 private:
  bool sendFrame(WsOpcode op, const void* data, std::size_t n);
  // Protocol failure: sends a Close with `code`, closes, returns false.
  bool fail(uint16_t code, const std::string& why, std::string* error);
  bool readHttpHeader(std::string& header, int timeoutMs, std::string* error);
  bool answerUpgrade(const std::string& header, std::string* error);
  void resetStream();

  TcpSocket sock_;
  bool client_ = true;
  std::atomic<bool> open_{false};  // read by receive() while another thread may shut down
  WsFrameDecoder decoder_;
  std::vector<uint8_t> fragments_;
  bool fragmentBinary_ = false;
  bool inFragment_ = false;
  std::atomic<bool> closeSent_{false};  // a Close is sent once (RFC 6455 section 5.5.1)
  std::mutex sendMutex_;
  std::mt19937 rng_;
};

}  // namespace hfc
