#include "navtex/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace navtex {

namespace {
std::uint32_t u32(const unsigned char* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
void put32(std::ofstream& f, std::uint32_t v) {
  unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                        static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24)};
  f.write(reinterpret_cast<char*>(b), 4);
}
void put16(std::ofstream& f, std::uint16_t v) {
  unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
  f.write(reinterpret_cast<char*>(b), 2);
}
}  // namespace

bool readWav(const std::string& path, WavData& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
    error = "not a RIFF/WAVE file";
    return false;
  }
  int format = 0, bits = 0;
  std::size_t pos = 12;
  const unsigned char* pcm = nullptr;
  std::size_t pcmBytes = 0;
  while (pos + 8 <= d.size()) {
    std::uint32_t size = u32(&d[pos + 4]);
    const unsigned char* body = &d[pos + 8];
    std::size_t avail = std::min<std::size_t>(size, d.size() - pos - 8);
    if (!std::memcmp(&d[pos], "fmt ", 4) && avail >= 16) {
      format = u16(body);
      out.channels = u16(body + 2);
      out.sampleRate = u32(body + 4);
      bits = u16(body + 14);
      if (format == 0xFFFE && avail >= 26) format = u16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
    } else if (!std::memcmp(&d[pos], "data", 4)) {
      pcm = body;
      pcmBytes = avail;
    }
    pos += 8 + size + (size & 1);
  }
  if (!pcm || out.channels <= 0 || out.sampleRate <= 0) {
    error = "missing fmt or data chunk";
    return false;
  }
  std::size_t bytes = static_cast<std::size_t>(bits / 8);
  if (!((format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (format == 3 && bits == 32))) {
    error = "unsupported WAV format (need PCM 8/16/24/32 or float32)";
    return false;
  }
  std::size_t count = pcmBytes / bytes;
  out.samples.resize(count);
  for (std::size_t i = 0; i < count; ++i) {
    const unsigned char* p = pcm + i * bytes;
    float v = 0;
    if (format == 3) std::memcpy(&v, p, 4);
    else if (bits == 8) v = (p[0] - 128) / 128.0f;
    else if (bits == 16) v = static_cast<std::int16_t>(u16(p)) / 32768.0f;
    else if (bits == 24) {
      std::uint32_t w = (static_cast<std::uint32_t>(p[0]) << 8) | (static_cast<std::uint32_t>(p[1]) << 16) |
                        (static_cast<std::uint32_t>(p[2]) << 24);
      v = static_cast<float>(static_cast<std::int32_t>(w) / 2147483648.0);
    }
    else v = static_cast<float>(static_cast<std::int32_t>(u32(p)) / 2147483648.0);
    out.samples[i] = v;
  }
  return true;
}

bool writeWav(const std::string& path, const WavData& data, std::string& error) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    error = "cannot create " + path;
    return false;
  }
  auto bytes = static_cast<std::uint32_t>(data.samples.size() * 2);
  auto ch = static_cast<std::uint16_t>(data.channels);
  auto rate = static_cast<std::uint32_t>(data.sampleRate);
  f.write("RIFF", 4);
  put32(f, 36 + bytes);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 1);
  put16(f, ch);
  put32(f, rate);
  put32(f, rate * ch * 2);
  put16(f, static_cast<std::uint16_t>(ch * 2));
  put16(f, 16);
  f.write("data", 4);
  put32(f, bytes);
  for (float s : data.samples) {
    auto v = static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
    put16(f, static_cast<std::uint16_t>(v));
  }
  if (!f) {
    error = "write failed";
    return false;
  }
  return true;
}

}  // namespace navtex
