#include "hfc/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "hfc/codec.hpp"

namespace hfc {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  std::size_t a = s.find_first_not_of(" \t");
  std::size_t b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The value of header `name` (case-insensitive) in an HTTP header block.
std::string headerValue(const std::string& header, const std::string& name) {
  const std::string want = lower(name);
  std::size_t pos = 0;
  while (pos < header.size()) {
    std::size_t end = header.find("\r\n", pos);
    if (end == std::string::npos) end = header.size();
    const std::string line = header.substr(pos, end - pos);
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == want) return trim(line.substr(colon + 1));
    pos = end + 2;
  }
  return std::string();
}

bool headerHasToken(const std::string& header, const std::string& name, const std::string& token) {
  const std::string v = lower(headerValue(header, name));
  std::size_t pos = 0;
  while (pos <= v.size()) {
    std::size_t end = v.find(',', pos);
    if (end == std::string::npos) end = v.size();
    if (trim(v.substr(pos, end - pos)) == token) return true;
    pos = end + 1;
  }
  return false;
}

bool validUtf8(const std::vector<uint8_t>& v) {
  std::size_t i = 0;
  const std::size_t n = v.size();
  while (i < n) {
    const uint8_t c = v[i];
    std::size_t len;
    uint32_t cp, min;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2, cp = c & 0x1F, min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3, cp = c & 0x0F, min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4, cp = c & 0x07, min = 0x10000;
    } else {
      return false;
    }
    if (n - i < len) return false;
    for (std::size_t k = 1; k < len; ++k) {
      if ((v[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (v[i + k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

bool controlOpcode(WsOpcode op) { return (static_cast<uint8_t>(op) & 0x8) != 0; }

bool knownOpcode(uint8_t op) { return op <= 2 || (op >= 8 && op <= 10); }

}  // namespace

std::vector<uint8_t> encodeWsFrame(WsOpcode opcode, const void* payload, std::size_t n, bool fin,
                                   const uint8_t* mask) {
  std::vector<uint8_t> out;
  out.reserve(n + 14);
  out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));
  const uint8_t maskBit = mask ? 0x80 : 0x00;
  if (n < 126) {
    out.push_back(static_cast<uint8_t>(maskBit | n));
  } else if (n <= 0xFFFF) {
    out.push_back(maskBit | 126);
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
  } else {
    out.push_back(maskBit | 127);
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (8 * i)));
  }
  const uint8_t* p = static_cast<const uint8_t*>(payload);
  if (mask) {
    out.insert(out.end(), mask, mask + 4);
    for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(p[i] ^ mask[i & 3]));
  } else if (n) {
    out.insert(out.end(), p, p + n);
  }
  return out;
}

void WsFrameDecoder::feed(const uint8_t* data, std::size_t n) {
  // Drop what has been consumed before growing the buffer.
  if (pos_ > 0 && pos_ >= buf_.size() / 2) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
  }
  buf_.insert(buf_.end(), data, data + n);
}

WsFrameDecoder::Result WsFrameDecoder::next(WsFrame& out, std::string* error) {
  const std::size_t avail = buf_.size() - pos_;
  if (avail < 2) return Result::NeedMore;
  const uint8_t* p = buf_.data() + pos_;
  if (p[0] & 0x70) {
    if (error) *error = "reserved bits set (no extension was negotiated)";
    return Result::Error;
  }
  const uint8_t op = p[0] & 0x0F;
  if (!knownOpcode(op)) {
    if (error) *error = "unknown opcode " + std::to_string(op);
    return Result::Error;
  }
  const bool fin = (p[0] & 0x80) != 0;
  const bool masked = (p[1] & 0x80) != 0;
  uint64_t len = p[1] & 0x7F;
  std::size_t hdr = 2;
  if (len == 126) {
    if (avail < 4) return Result::NeedMore;
    len = (uint64_t(p[2]) << 8) | p[3];
    hdr = 4;
  } else if (len == 127) {
    if (avail < 10) return Result::NeedMore;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
    hdr = 10;
  }
  if (controlOpcode(static_cast<WsOpcode>(op)) && (len > 125 || !fin)) {
    if (error) *error = "malformed control frame";
    return Result::Error;
  }
  if (len > maxPayload_) {
    if (error) *error = "frame of " + std::to_string(len) + " bytes exceeds the limit";
    return Result::Error;
  }
  const std::size_t total = hdr + (masked ? 4 : 0) + static_cast<std::size_t>(len);
  if (avail < total) return Result::NeedMore;
  out.fin = fin;
  out.opcode = static_cast<WsOpcode>(op);
  out.masked = masked;
  const uint8_t* key = masked ? p + hdr : nullptr;
  const uint8_t* body = p + hdr + (masked ? 4 : 0);
  out.payload.resize(static_cast<std::size_t>(len));
  if (key)
    for (std::size_t i = 0; i < out.payload.size(); ++i) out.payload[i] = body[i] ^ key[i & 3];  // unmask while copying
  else if (len)
    std::memcpy(out.payload.data(), body, static_cast<std::size_t>(len));
  pos_ += total;
  return Result::Frame;
}

WebSocket::WebSocket() : rng_(static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())) {}

WebSocket::~WebSocket() { shutdown(); }

bool WebSocket::readHttpHeader(std::string& header, int timeoutMs, std::string* error) {
  header.clear();
  sock_.setRecvTimeout(timeoutMs);
  char buf[1024];
  while (header.find("\r\n\r\n") == std::string::npos) {
    if (header.size() > 16384) {
      if (error) *error = "HTTP header too long";
      return false;
    }
    const long r = sock_.recvSome(buf, sizeof(buf));
    if (r <= 0) {
      if (error) *error = r == -2 ? "no handshake answer (timeout)" : "connection closed during the handshake";
      return false;
    }
    header.append(buf, static_cast<std::size_t>(r));
  }
  // Bytes after the header already belong to the frame stream.
  const std::size_t end = header.find("\r\n\r\n") + 4;
  if (end < header.size())
    decoder_.feed(reinterpret_cast<const uint8_t*>(header.data()) + end, header.size() - end);
  header.resize(end);
  sock_.setRecvTimeout(0);
  return true;
}

bool WebSocket::connect(const std::string& host, uint16_t port, const std::string& path, int timeoutMs,
                        std::string* error) {
  shutdown();
  resetStream();
  client_ = true;
  if (!sock_.connect(host, port, timeoutMs, error)) return false;
  sock_.setNoDelay(true);
  uint8_t nonce[16];
  for (auto& b : nonce) b = static_cast<uint8_t>(rng_());
  const std::string key = base64Encode(nonce, sizeof(nonce));
  const std::string req = "GET " + (path.empty() ? std::string("/") : path) + " HTTP/1.1\r\n" + "Host: " + host + ":" +
                          std::to_string(port) + "\r\n" +
                          "Upgrade: websocket\r\nConnection: Upgrade\r\n" + "Sec-WebSocket-Key: " + key + "\r\n" +
                          "Sec-WebSocket-Version: 13\r\n\r\n";
  if (!sock_.sendAll(req.data(), req.size())) {
    if (error) *error = "cannot send the handshake";
    sock_.close();
    return false;
  }
  std::string resp;
  if (!readHttpHeader(resp, timeoutMs, error)) {
    sock_.close();
    return false;
  }
  const std::string status = resp.substr(0, resp.find("\r\n"));
  if (status.find(" 101") == std::string::npos) {
    if (error) *error = "server refused the WebSocket upgrade: " + status;
    sock_.close();
    return false;
  }
  if (headerValue(resp, "Sec-WebSocket-Accept") != websocketAccept(key)) {
    if (error) *error = "bad Sec-WebSocket-Accept in the handshake answer";
    sock_.close();
    return false;
  }
  open_ = true;
  return true;
}

bool WebSocket::accept(TcpSocket socket, int timeoutMs, std::string* error) {
  shutdown();
  resetStream();
  client_ = false;
  sock_ = std::move(socket);
  if (!sock_.valid()) {
    if (error) *error = "no connection";
    return false;
  }
  std::string req;
  if (!readHttpHeader(req, timeoutMs, error)) {
    sock_.close();
    return false;
  }
  return answerUpgrade(req, error);
}

bool WebSocket::acceptUpgrade(TcpSocket socket, const std::string& header, std::string* error,
                              const std::string& early) {
  shutdown();
  resetStream();
  if (!early.empty()) decoder_.feed(reinterpret_cast<const uint8_t*>(early.data()), early.size());
  client_ = false;
  sock_ = std::move(socket);
  if (!sock_.valid()) {
    if (error) *error = "no connection";
    return false;
  }
  return answerUpgrade(header, error);
}

bool WebSocket::answerUpgrade(const std::string& req, std::string* error) {
  const std::string key = headerValue(req, "Sec-WebSocket-Key");
  const std::string version = headerValue(req, "Sec-WebSocket-Version");
  if (!version.empty() && version != "13") {
    const std::string bad =
        "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sock_.sendAll(bad.data(), bad.size());
    if (error) *error = "unsupported WebSocket version " + version;
    sock_.close();
    return false;
  }
  if (req.compare(0, 4, "GET ") != 0 || key.empty() || !headerHasToken(req, "Upgrade", "websocket")) {
    const std::string bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    sock_.sendAll(bad.data(), bad.size());
    if (error) *error = "not a WebSocket upgrade request";
    sock_.close();
    return false;
  }
  const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           websocketAccept(key) + "\r\n\r\n";
  if (!sock_.sendAll(resp.data(), resp.size())) {
    if (error) *error = "cannot answer the handshake";
    sock_.close();
    return false;
  }
  sock_.setNoDelay(true);
  open_ = true;
  return true;
}

bool WebSocket::sendFrame(WsOpcode op, const void* data, std::size_t n) {
  std::lock_guard<std::mutex> lock(sendMutex_);
  if (!open_) return false;
  uint8_t key[4];
  if (client_) {
    const uint32_t r = static_cast<uint32_t>(rng_());
    std::memcpy(key, &r, 4);
  }
  const auto frame = encodeWsFrame(op, data, n, true, client_ ? key : nullptr);
  return sock_.sendAll(frame.data(), frame.size());
}

bool WebSocket::sendText(const std::string& text) { return sendFrame(WsOpcode::Text, text.data(), text.size()); }

bool WebSocket::sendBinary(const void* data, std::size_t n) { return sendFrame(WsOpcode::Binary, data, n); }

bool WebSocket::sendClose(uint16_t code) {
  if (closeSent_.exchange(true)) return true;
  const uint8_t body[2] = {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code)};
  return sendFrame(WsOpcode::Close, body, sizeof(body));
}

bool WebSocket::fail(uint16_t code, const std::string& why, std::string* error) {
  if (error) *error = why;
  sendClose(code);
  open_ = false;
  return false;
}

bool WebSocket::receive(WsMessage& msg, std::string* error) {
  std::vector<uint8_t> buf(1 << 16);
  for (;;) {
    WsFrame f;
    std::string err;
    const auto r = decoder_.next(f, &err);
    if (r == WsFrameDecoder::Result::Error) return fail(1002, "protocol error: " + err, error);
    if (r == WsFrameDecoder::Result::NeedMore) {
      if (!open_) {
        if (error) *error = "connection closed";
        return false;
      }
      const long n = sock_.recvSome(buf.data(), buf.size());
      if (n <= 0) {
        if (error) *error = n == 0 ? "connection closed by the peer" : "connection failed";
        open_ = false;
        return false;
      }
      decoder_.feed(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    // Clients mask every frame, servers none (RFC 6455 section 5.1).
    if (f.masked == client_)
      return fail(1002, client_ ? "protocol error: masked frame from the server"
                                : "protocol error: unmasked frame from the client",
                  error);
    switch (f.opcode) {
      case WsOpcode::Ping:
        sendFrame(WsOpcode::Pong, f.payload.data(), f.payload.size());
        break;
      case WsOpcode::Pong:
        break;
      case WsOpcode::Close: {
        if (f.payload.size() == 1) return fail(1002, "protocol error: close frame of 1 byte", error);
        sendClose(1000);
        std::lock_guard<std::mutex> lock(sendMutex_);
        open_ = false;
        if (error) {
          *error = "closed by the peer";
          if (f.payload.size() >= 2) *error += " (code " + std::to_string((f.payload[0] << 8) | f.payload[1]) + ")";
        }
        return false;
      }
      case WsOpcode::Text:
      case WsOpcode::Binary:
        if (inFragment_) return fail(1002, "protocol error: new message inside a fragmented one", error);
        if (f.fin) {
          msg.binary = f.opcode == WsOpcode::Binary;
          // Checked where this end is a server (the UI's browsers); as a
          // client of SDRconnect, a text that is not UTF-8 (a name in a
          // Windows code page) must not cost the receiver its connection.
          if (!client_ && !msg.binary && !validUtf8(f.payload)) return fail(1007, "text message is not UTF-8", error);
          msg.data = std::move(f.payload);
          return true;
        }
        inFragment_ = true;
        fragmentBinary_ = f.opcode == WsOpcode::Binary;
        fragments_ = std::move(f.payload);
        break;
      case WsOpcode::Continuation:
        if (!inFragment_) return fail(1002, "protocol error: continuation without a message", error);
        // The whole message, not only each frame, is limited.
        if (fragments_.size() + f.payload.size() > decoder_.maxPayload())
          return fail(1009, "message too big", error);
        fragments_.insert(fragments_.end(), f.payload.begin(), f.payload.end());
        if (f.fin) {
          inFragment_ = false;
          msg.binary = fragmentBinary_;
          if (!client_ && !msg.binary && !validUtf8(fragments_)) {
            fragments_.clear();
            return fail(1007, "text message is not UTF-8", error);
          }
          msg.data = std::move(fragments_);
          fragments_.clear();
          return true;
        }
        break;
    }
  }
}

void WebSocket::resetStream() {
  decoder_ = WsFrameDecoder();
  fragments_.clear();
  inFragment_ = false;
  closeSent_ = false;
}

void WebSocket::shutdown() {
  // The socket first: that also ends a send blocked on a peer that does not
  // read, which holds sendMutex_.
  open_ = false;
  sock_.shutdown();
  std::lock_guard<std::mutex> lock(sendMutex_);
}

}  // namespace hfc
