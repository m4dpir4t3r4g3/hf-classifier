#include "hfc/codec.hpp"

#include <cstring>

namespace hfc {

namespace {

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int alphabetIndex(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace

// FIPS 180-4, section 6.1.
std::array<uint8_t, 20> sha1(const void* data, std::size_t n) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<uint8_t> msg(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
  const uint64_t bits = static_cast<uint64_t>(n) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  for (std::size_t off = 0; off < msg.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const uint8_t* p = &msg[off + 4 * static_cast<std::size_t>(i)];
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  std::array<uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(4 * i + j)] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
  return out;
}

std::string base64Encode(const void* data, std::size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
    if (i + 2 < n) v |= p[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out += i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  return out;
}

bool base64Decode(const std::string& text, std::vector<uint8_t>& out) {
  out.clear();
  if (text.size() % 4 != 0) return false;
  for (std::size_t i = 0; i < text.size(); i += 4) {
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = text[i + static_cast<std::size_t>(j)];
      if (c == '=' && i + 4 == text.size() && j >= 2) {
        v[j] = 0;
        ++pad;
      } else {
        if (pad) return false;  // data after padding
        v[j] = alphabetIndex(c);
        if (v[j] < 0) return false;
      }
    }
    const uint32_t w = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) | uint32_t(v[3]);
    out.push_back(static_cast<uint8_t>(w >> 16));
    if (pad < 2) out.push_back(static_cast<uint8_t>(w >> 8));
    if (pad < 1) out.push_back(static_cast<uint8_t>(w));
  }
  return true;
}

std::string websocketAccept(const std::string& key) {
  const std::string s = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto h = sha1(s.data(), s.size());
  return base64Encode(h.data(), h.size());
}

}  // namespace hfc
