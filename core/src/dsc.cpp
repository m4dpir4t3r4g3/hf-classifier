#include "hfc/dsc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "navtex/fsk_receiver.hpp"

namespace hfc {

uint16_t dscEncode(int value) {
  int zeros = 0;
  for (int b = 0; b < 7; ++b) zeros += (value >> b) & 1 ? 0 : 1;
  uint16_t w = static_cast<uint16_t>(value & 0x7F);  // bit 0 first
  w = static_cast<uint16_t>(w | ((zeros >> 2) & 1) << 7 | ((zeros >> 1) & 1) << 8 | (zeros & 1) << 9);
  return w;
}

int dscDecode(uint16_t w) {
  const int value = w & 0x7F;
  return (w & 0x3FF) == dscEncode(value) ? value : -1;
}

namespace {

constexpr int kPhaseDx = 125;
constexpr std::size_t kKeepBits = 1400;  // phasing and the longest call, with room
constexpr int kMaxSymbols = 60;

bool endOfSequence(int s) { return s == 117 || s == 122 || s == 127; }

// Digits of `count` symbols from `from` (two decimal digits each); false if
// one is not a number.
bool digits(const std::vector<int>& s, std::size_t from, std::size_t count, std::string& out) {
  out.clear();
  if (from + count > s.size()) return false;
  for (std::size_t i = 0; i < count; ++i) {
    if (s[from + i] > 99) return false;
    char b[16];
    std::snprintf(b, sizeof b, "%02d", s[from + i]);
    out += b;
  }
  return true;
}

const char* categoryName(int c) {
  switch (c) {
    case 100: return "routine";
    case 108: return "safety";
    case 110: return "urgency";
    case 112: return "distress";
  }
  return nullptr;
}

const char* natureName(int c) {
  switch (c) {
    case 100: return "fire or explosion";
    case 101: return "flooding";
    case 102: return "collision";
    case 103: return "grounding";
    case 104: return "listing";
    case 105: return "sinking";
    case 106: return "disabled and adrift";
    case 107: return "undesignated";
    case 108: return "abandoning ship";
    case 109: return "piracy or armed attack";
    case 110: return "man overboard";
    case 112: return "EPIRB emission";
  }
  return nullptr;
}

const char* telecommandName(int c) {
  switch (c) {
    case 100: return "F3E/G3E telephony";
    case 101: return "F3E/G3E duplex telephony";
    case 103: return "polling";
    case 104: return "unable to comply";
    case 105: return "end of call";
    case 106: return "data";
    case 109: return "J3E telephony";
    case 110: return "distress acknowledgement";
    case 112: return "distress relay";
    case 113: return "F1B/J2B FEC teleprinter";
    case 115: return "F1B/J2B ARQ teleprinter";
    case 118: return "test";
    case 121: return "position update";
    case 126: return "no information";
  }
  return nullptr;
}

std::string symbolList(const std::vector<int>& s, std::size_t from, std::size_t to) {
  std::string out;
  for (std::size_t i = from; i < to && i < s.size(); ++i) out += (out.empty() ? "" : " ") + std::to_string(s[i]);
  return out;
}

}  // namespace

std::string dscDescribe(const std::vector<int>& s) {
  if (s.size() < 3) return "";
  const int format = s[0], eos = s.back();
  std::string out, id;
  std::size_t i = 1;
  auto mmsi = [&](std::size_t at) {  // 5 symbols: 9 digits and a tenth
    std::string d;
    return digits(s, at, 5, d) ? d.substr(0, 9) : std::string("?");
  };
  auto telecommands = [&] {
    for (int k = 0; k < 2 && i + 1 < s.size(); ++k, ++i) {
      const char* n = telecommandName(s[i]);
      out += std::string(", ") + (n ? n : ("telecommand " + std::to_string(s[i])).c_str());
    }
  };
  switch (format) {
    case 112: {
      out = "distress alert from " + mmsi(i);
      i += 5;
      if (i < s.size()) {
        const char* n = natureName(s[i]);
        out += std::string(": ") + (n ? n : ("nature " + std::to_string(s[i])).c_str());
        ++i;
      }
      std::string pos, time;
      if (digits(s, i, 5, pos)) {
        if (pos == "9999999999") out += ", no position";
        else
          out += ", position " + pos.substr(1, 2) + " " + pos.substr(3, 2) + (pos[0] == '0' || pos[0] == '1' ? " N " : " S ") +
                 pos.substr(5, 3) + " " + pos.substr(8, 2) + (pos[0] == '0' || pos[0] == '2' ? " E" : " W");
        i += 5;
      }
      if (digits(s, i, 2, time)) {
        if (time != "8888") out += " at " + time + " UTC";
        i += 2;
      }
      break;
    }
    case 116:
      out = "all ships call";
      break;
    case 120:
    case 123:
      out = std::string(format == 123 ? "automatic service call to " : "selective call to ") + mmsi(i);
      i += 5;
      break;
    case 114:
      out = "group call to " + mmsi(i);
      i += 5;
      break;
    case 102:
      out = "geographic area call (area " + symbolList(s, i, i + 5) + ")";
      i += 5;
      break;
    default:
      out = "call, format " + std::to_string(format);
  }
  if (format != 112) {
    if (i < s.size() && categoryName(s[i])) out += std::string(", ") + categoryName(s[i]), ++i;
    if (i + 5 < s.size()) {
      out += " from " + mmsi(i);
      i += 5;
    }
    telecommands();
  }
  // Two frequency fields of three symbols each (receive, transmit), in
  // units of 100 Hz when they start with 0, 1 or 2; 126 three times: none.
  for (const char* name : {"receive", "transmit"}) {
    if (format == 112 || i + 3 >= s.size()) break;
    std::string d;
    if (s[i] == 126 && s[i + 1] == 126 && s[i + 2] == 126) {
      i += 3;
    } else if (digits(s, i, 3, d) && d[0] <= '2') {
      out += std::string(", ") + name + " " + std::to_string(std::atoi(d.substr(0, 5).c_str())) + "." + d.substr(5) + " kHz";
      i += 3;
    } else {
      break;
    }
  }
  if (i + 1 < s.size()) out += ", then " + symbolList(s, i, s.size() - 1);
  out += eos == 117 ? "; acknowledgement requested" : eos == 122 ? "; acknowledgement" : "";
  return out;
}

void DscParser::reset() {
  sym_.clear();
  shift_ = 0;
  bits_ = base_ = 0;
  collecting_ = false;
  call_.clear();
}

int DscParser::at(std::size_t bitIndex) const {
  return bitIndex >= base_ && bitIndex - base_ < sym_.size() ? sym_[bitIndex - base_] : -1;
}

bool DscParser::bit(bool one) {
  shift_ = static_cast<uint16_t>((shift_ >> 1) | (one ? 0x200 : 0));
  sym_.push_back(static_cast<int8_t>(bits_ >= 9 ? dscDecode(shift_) : -1));
  ++bits_;
  if (sym_.size() > 2 * kKeepBits) {
    sym_.erase(sym_.begin(), sym_.begin() + static_cast<std::ptrdiff_t>(kKeepBits));
    base_ += kKeepBits;
  }
  if (!collecting_) {
    if (!tryPhasing()) return false;
    collecting_ = true;
  }
  return collect();
}

// The symbol just completed as an RX phasing symbol (111 ... 104), with two
// more of the phasing sequence where they belong before it.
bool DscParser::tryPhasing() {
  const std::size_t now = bits_ - 1;
  const int r = at(now);
  if (r < 104 || r > 111) return false;
  int confirmed = 0;
  auto slot = [&](std::size_t back) { return now >= 10 * back ? at(now - 10 * back) : -1; };
  confirmed += slot(1) == kPhaseDx ? 1 : 0;
  confirmed += slot(3) == kPhaseDx ? 1 : 0;
  confirmed += r + 1 <= 111 && slot(2) == r + 1 ? 1 : 0;
  confirmed += r + 2 <= 111 && slot(4) == r + 2 ? 1 : 0;
  if (confirmed < 2) return false;
  // RX 106 is followed by the first format specifier.
  const long first = static_cast<long>(now) + 10L * (2 * (r - 106) + 1);
  if (first < static_cast<long>(base_)) return false;
  start_ = static_cast<std::size_t>(first);
  return true;
}

// Message symbol n: its DX copy ends at start_ + 20 n bits, its RX copy five
// symbols later. Returns true once a call is complete and its check agrees.
bool DscParser::collect() {
  const std::size_t now = bits_ - 1;
  std::vector<int> s;
  for (int n = 0; n < kMaxSymbols; ++n) {
    const std::size_t dx = start_ + 20u * static_cast<std::size_t>(n), rx = dx + 50;
    // The error-check character follows the end of sequence.
    if (s.size() >= 3 && endOfSequence(s.back()) && n >= 4) {
      if (now < rx && at(dx) < 0) return false;  // its second copy is still to come
      const int ecc = at(dx) >= 0 ? at(dx) : at(rx);
      collecting_ = false;
      if (ecc < 0) return false;
      int x = 0;
      for (std::size_t k = 1; k < s.size(); ++k) x ^= s[k];
      if (x != ecc || s[0] != s[1]) return false;
      call_.assign(s.begin() + 1, s.end());
      return true;
    }
    const int a = at(dx), b = now >= rx ? at(rx) : -1;
    if (a < 0 && b < 0) {
      if (now >= rx) collecting_ = false;  // lost in both copies
      return false;
    }
    if (a < 0 && now < rx) return false;
    s.push_back(a >= 0 ? a : b);
  }
  collecting_ = false;
  return false;
}

std::vector<bool> dscSequence(const std::vector<int>& symbols, int dotBits) {
  // Message symbols: the format specifier twice, the rest, the check, and
  // the end of sequence twice more.
  std::vector<int> m;
  m.push_back(symbols[0]);
  m.insert(m.end(), symbols.begin(), symbols.end());
  int ecc = 0;
  for (int v : symbols) ecc ^= v;
  m.push_back(ecc);
  m.push_back(symbols.back());
  m.push_back(symbols.back());
  // Slots: DX phasing in 0, 2 ... 10 with RX 111 ... 106 between; message
  // symbol n in slot 12 + 2 n and again in slot 12 + 2 n + 5.
  const std::size_t slots = 12 + 2 * m.size() + 6;
  std::vector<int> slot(slots, -1);
  for (int k = 0; k < 6; ++k) slot[static_cast<std::size_t>(2 * k)] = kPhaseDx;
  for (int k = 0; k < 8; ++k) slot[static_cast<std::size_t>(2 * k + 1)] = 111 - k;
  for (std::size_t n = 0; n < m.size(); ++n) {
    slot[12 + 2 * n] = m[n];
    if (12 + 2 * n + 5 < slots) slot[12 + 2 * n + 5] = m[n];
  }
  std::vector<bool> bits;
  for (int i = 0; i < dotBits; ++i) bits.push_back(i % 2 == 0);
  for (int v : slot) {
    const uint16_t w = dscEncode(v < 0 ? symbols.back() : v);
    for (int b = 0; b < 10; ++b) bits.push_back((w >> b) & 1);
  }
  return bits;
}

struct DscDecoder::Impl : navtex::FskReceiver::Client {
  explicit Impl(const navtex::SitorBConfig& c) : rx(c) {}
  navtex::FskReceiver rx;
  DscParser parser[2];  // as received, and inverted
  std::vector<std::string> calls;
  void onBit(float soft, double) override {
    for (int p = 0; p < 2; ++p)
      if (parser[p].bit((soft > 0) != (p == 1))) {
        const std::string text = dscDescribe(parser[p].call());
        if (!text.empty()) calls.push_back(text);
      }
  }
  bool synced() const override { return true; }
};

DscDecoder::DscDecoder(double rate, double centreHz) {
  navtex::SitorBConfig c;
  c.sampleRate = rate;
  c.baud = 100;
  c.shiftHz = 170;
  c.minCentreHz = centreHz - 250;
  c.maxCentreHz = centreHz + 250;
  c.fixedCentreHz = centreHz;
  impl_ = std::make_unique<Impl>(c);
}

DscDecoder::~DscDecoder() = default;

void DscDecoder::process(const float* audio, std::size_t n) { impl_->rx.process(audio, n, *impl_); }

std::vector<std::string> DscDecoder::take() {
  std::vector<std::string> out;
  out.swap(impl_->calls);
  return out;
}

}  // namespace hfc
