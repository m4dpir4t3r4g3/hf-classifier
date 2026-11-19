#include "hfc/hfdl.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>

namespace hfc {

namespace {

using cd = std::complex<double>;
using cf = std::complex<float>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kBaud = 1800.0;
constexpr int kSps = 8;                       // samples per symbol inside the decoder
constexpr double kFs = kBaud * kSps;          // 14400
constexpr int kALen = 127, kM1Len = 127, kM2Len = 15, kTLen = 15, kDataLen = 30;
constexpr int kPrekey = 448;
constexpr int kPreamble = 2 * kALen + kM1Len + kM2Len + 9 * kTLen;  // symbols before the first data frame
constexpr int kRows = 40;

// The preamble's first sequence, sent twice (bit 0 is the symbol +1).
const uint8_t kAOctets[16] = {0x5B, 0xBC, 0x74, 0x57, 0x03, 0xD9, 0x89, 0x39, 0xF2, 0x08, 0xD5, 0x36, 0x94, 0x2C, 0x32, 0xFE};
// The m-sequence of which M1 is one of eight shifts.
const uint8_t kM[kM1Len] = {0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 1, 0, 0,
                            0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 1,
                            1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 0, 1,
                            0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1};
const int kShifts[8] = {72, 82, 113, 123, 61, 103, 93, 9};
const double kT[kTLen] = {1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1};

struct Mode {
  int arity;     // bits per data symbol
  int frames;    // data frames
  int rate;      // 2: every coded bit once; 4: twice
  int shift;     // the interleaver's column step
  int bitRate;
};
const Mode kModes[8] = {{1, 72, 4, 17, 300},  {1, 72, 2, 17, 600},  {2, 72, 2, 17, 1200},  {3, 72, 2, 17, 1800},
                        {1, 168, 4, 23, 300}, {1, 168, 2, 23, 600}, {2, 168, 2, 23, 1200}, {3, 168, 2, 23, 1800}};

int parity(unsigned v) {
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  return (0x6996 >> (v & 0xF)) & 1;
}

double aSymbol(int i) { return (kAOctets[i / 8] >> (7 - i % 8)) & 1 ? -1.0 : 1.0; }

// The scrambler: 120 bits, then again. Register of 15 stages, feedback
// from stages 15 and 1, starting at 0x4D4B; the new bit is the output.
const std::vector<uint8_t>& scrambler() {
  static const std::vector<uint8_t> seq = [] {
    std::vector<uint8_t> s;
    unsigned v = 0x4D4B;
    for (int i = 0; i < 120; ++i) {
      const int b = parity(v & 0x4001);
      v = ((v << 1) | static_cast<unsigned>(b)) & 0x7FFF;
      s.push_back(static_cast<uint8_t>(b));
    }
    return s;
  }();
  return seq;
}

// The order in which the receiver reads the transmitted coded bits: bit i
// of the decoder's input is transmitted bit order[i]. (Written into a
// 40-row table down the columns, the column stepping back by `shift` after
// every bit; read with the row stepping by 9.)
std::vector<int> interleaverOrder(int bits, int shift) {
  const int cols = bits / kRows;
  std::vector<int> table(static_cast<std::size_t>(bits));
  int row = 0, col = 0;
  for (int p = 0; p < bits; ++p) {
    table[static_cast<std::size_t>(row * cols + col)] = p;
    if (++row == kRows) row = 0, ++col;
    col -= shift;
    if (col < 0) col += cols;
  }
  std::vector<int> order(static_cast<std::size_t>(bits));
  row = col = 0;
  for (int i = 0; i < bits; ++i) {
    order[static_cast<std::size_t>(i)] = table[static_cast<std::size_t>(row * cols + col)];
    row = (row + 9) % kRows;
    if (row == 0) ++col;
  }
  return order;
}

// The convolutional code: K = 7, generators 0x6D and 0x4F over the last 7
// input bits (the newest in bit 0), starting from zeros.
void encode(const std::vector<uint8_t>& bits, std::vector<uint8_t>& coded) {
  unsigned sr = 0;
  for (uint8_t b : bits) {
    sr = ((sr << 1) | b) & 0x7F;
    coded.push_back(static_cast<uint8_t>(parity(sr & 0x6D)));
    coded.push_back(static_cast<uint8_t>(parity(sr & 0x4F)));
  }
}

// Viterbi decoding of soft values (positive: a 0), from state 0 to the
// best end state.
std::vector<uint8_t> viterbi(const std::vector<float>& soft) {
  const std::size_t n = soft.size() / 2;
  static const struct Tables {
    float o0[64][2], o1[64][2];
    Tables() {
      for (unsigned s = 0; s < 64; ++s)
        for (unsigned b = 0; b < 2; ++b) {
          const unsigned sr = ((s << 1) | b) & 0x7F;
          o0[s][b] = parity(sr & 0x6D) ? -1.f : 1.f;
          o1[s][b] = parity(sr & 0x4F) ? -1.f : 1.f;
        }
    }
  } tab;
  std::vector<float> metric(64, -1e30f), next(64);
  metric[0] = 0;
  std::vector<uint8_t> from(n * 64);
  for (std::size_t i = 0; i < n; ++i) {
    const float x0 = soft[2 * i], x1 = soft[2 * i + 1];
    for (unsigned t = 0; t < 64; ++t) {
      const unsigned b = t & 1, s0 = t >> 1, s1 = (t >> 1) | 32;
      const float m0 = metric[s0] + tab.o0[s0][b] * x0 + tab.o1[s0][b] * x1;
      const float m1 = metric[s1] + tab.o0[s1][b] * x0 + tab.o1[s1][b] * x1;
      next[t] = std::max(m0, m1);
      from[i * 64 + t] = m1 > m0;
    }
    metric.swap(next);
  }
  unsigned st = static_cast<unsigned>(std::max_element(metric.begin(), metric.end()) - metric.begin());
  std::vector<uint8_t> bits(n);
  for (std::size_t i = n; i-- > 0;) {
    bits[i] = st & 1;
    st = (st >> 1) | (from[i * 64 + st] ? 32u : 0u);
  }
  return bits;
}

unsigned grayToIndex(unsigned g) {
  unsigned k = 0;
  for (; g; g >>= 1) k ^= g;
  return k;
}

uint8_t reverseBits(uint8_t v) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) r = static_cast<uint8_t>((r << 1) | ((v >> i) & 1));
  return r;
}

uint32_t icaoAt(const uint8_t* p) {
  return static_cast<uint32_t>(reverseBits(p[0])) << 16 | static_cast<uint32_t>(reverseBits(p[1])) << 8 | reverseBits(p[2]);
}

bool fcsRight(const uint8_t* buf, std::size_t n) { return hfdlFcs(buf, n) == (buf[n] | buf[n + 1] << 8); }

std::string station(int id) {
  const char* name = hfdlGroundStation(id);
  char b[96];
  std::snprintf(b, sizeof(b), *name ? "ground station %d (%s)" : "ground station %d", id, name);
  return b;
}

const char* lpduKind(int type) {
  switch (type) {
    case 0x0D: return "data";
    case 0x1D: return "acknowledged data";
    case 0x2F: return "log-on denied";
    case 0x3F: return "log-off request";
    case 0x4F: return "log-on resume";
    case 0x5F: return "log-on resume confirmed";
    case 0x8F: return "log-on request";
    case 0x9F: return "log-on confirmed";
    case 0xBF: return "log-on request (DLS)";
    default: return "";
  }
}

// One LPDU (with its check at the end): its kind and the aircraft address
// it names; "" if its check fails.
std::string describeLpdu(const uint8_t* p, std::size_t len) {
  if (len < 3 || !fcsRight(p, len - 2)) return "";
  const int type = p[0];
  std::string s = lpduKind(type);
  char b[64];
  if (s.empty()) {
    std::snprintf(b, sizeof(b), "LPDU type 0x%02X", type);
    s = b;
  }
  const bool addressed = type == 0x2F || type == 0x3F || type == 0x4F || type == 0x5F || type == 0x8F || type == 0x9F || type == 0xBF;
  if (addressed && len >= 4 + 2) {
    std::snprintf(b, sizeof(b), ", aircraft address %06X", static_cast<unsigned>(icaoAt(p + 1)));
    s += b;
    if ((type == 0x9F || type == 0x5F) && len >= 5 + 2) {
      std::snprintf(b, sizeof(b), " given ID %d", p[4]);
      s += b;
    }
  }
  return s;
}

// The LPDUs listed by `count` size octets at `sizes`, their data from
// `data` on; returns the octets used, or -1 if they do not fit.
int describeLpdus(const uint8_t* sizes, int count, const uint8_t* data, const uint8_t* end, std::string& out) {
  int used = 0;
  for (int j = 0; j < count; ++j) {
    const int len = sizes[j] + 1;
    if (data + used + len > end) return -1;
    const std::string d = describeLpdu(data + used, static_cast<std::size_t>(len));
    out += (out.empty() ? "" : "; ") + (d.empty() ? std::string("an LPDU that fails its check") : d);
    used += len;
  }
  return used;
}

}  // namespace

uint16_t hfdlFcs(const uint8_t* data, std::size_t n) {
  unsigned crc = 0xFFFF;
  for (std::size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return static_cast<uint16_t>(crc ^ 0xFFFF);
}

const char* hfdlGroundStation(int id) {
  switch (id) {
    case 1: return "San Francisco, California";
    case 2: return "Molokai, Hawaii";
    case 3: return "Reykjavik, Iceland";
    case 4: return "Riverhead, New York";
    case 5: return "Auckland, New Zealand";
    case 6: return "Hat Yai, Thailand";
    case 7: return "Shannon, Ireland";
    case 8: return "Johannesburg, South Africa";
    case 9: return "Barrow, Alaska";
    case 10: return "Muan, South Korea";
    case 11: return "Albrook, Panama";
    case 13: return "Santa Cruz, Bolivia";
    case 14: return "Krasnoyarsk, Russia";
    case 15: return "Al Muharraq, Bahrain";
    case 16: return "Agana, Guam";
    case 17: return "Canarias, Spain";
    default: return "";
  }
}

std::string hfdlDescribe(const std::vector<uint8_t>& pdu, int bitRate) {
  const uint8_t* buf = pdu.data();
  const std::size_t len = pdu.size();
  char b[160];
  std::string s;
  if (len < 8) return "";
  if (!(buf[0] & 1)) {
    // Squitter: 64 octets and their check.
    if (len < 66 || !fcsRight(buf, 64)) return "";
    std::snprintf(b, sizeof(b), "squitter of %s, %s, frame %d slot %d", station(buf[1] & 0x7F).c_str(),
                  buf[1] & 0x80 ? "UTC synchronised" : "not UTC synchronised", buf[2] | (buf[3] & 0xF) << 8, buf[3] >> 4);
    s = b;
  } else if (buf[0] & 2) {
    // From an aircraft: the header is 6 octets and one size octet per LPDU.
    const int lpdus = (buf[0] >> 2) & 0xF;
    const std::size_t header = 6 + static_cast<std::size_t>(lpdus);
    if (len < header + 2 || !fcsRight(buf, header)) return "";
    std::snprintf(b, sizeof(b), "aircraft ID %d to %s", buf[2], station(buf[1] & 0x7F).c_str());
    s = b;
    std::string l;
    if (describeLpdus(buf + 6, lpdus, buf + header + 2, buf + len, l) >= 0 && !l.empty()) s += ": " + l;
  } else {
    // From a ground station to up to 8 aircraft, each with its LPDUs.
    const int aircraft = ((buf[0] >> 4) & 7) + 1;
    std::size_t header = 2;
    for (int i = 0; i < aircraft; ++i) {
      if (len < header + 2) return "";
      header += 2 + (buf[header + 1] >> 4);
    }
    if (len < header + 2 || !fcsRight(buf, header)) return "";
    s = station(buf[1] & 0x7F) + " to ";
    const uint8_t* h = buf + 2;
    const uint8_t* data = buf + header + 2;
    for (int i = 0; i < aircraft; ++i) {
      const int id = h[0], lpdus = h[1] >> 4;
      std::string l;
      const int used = describeLpdus(h + 2, lpdus, data, buf + len, l);
      std::snprintf(b, sizeof(b), "%saircraft ID %d", i ? "; " : "", id);
      s += b;
      if (used < 0) break;
      if (!l.empty()) s += ": " + l;
      data += used;
      h += 2 + lpdus;
    }
  }
  std::snprintf(b, sizeof(b), " (%d bit/s)", bitRate);
  return s + b;
}

// ---------------------------------------------------------------- decoder --

struct HfdlDecoder::Impl {
  double rate, carrier;
  // Input: mixed to baseband, kept for the resampler's kernel.
  std::vector<cf> in;
  double inBase = 0;       // index of in[0] among all input samples
  cd rot{1.0, 0.0};        // the mixer's phasor, exp(-i phase)
  int rotCount = 0;
  uint64_t inCount = 0;
  uint64_t outCount = 0;   // baseband samples made (at kFs)
  // Baseband at kFs; bb[0] is sample bbBase.
  std::vector<cf> bb;
  uint64_t bbBase = 0;
  uint64_t search = 0;     // next position to look for a preamble at
  std::vector<std::string> out;
  int preambles = 0, decoded = 0;
  float aRef[kALen];

  Impl(double r, double c) : rate(r), carrier(c) {
    for (int i = 0; i < kALen; ++i) aRef[i] = static_cast<float>(aSymbol(i));
  }

  cf at(uint64_t i) const { return bb[static_cast<std::size_t>(i - bbBase)]; }
  uint64_t have() const { return bbBase + bb.size(); }

  void resample() {
    // Windowed-sinc low-pass at 1150 Hz, evaluated at the output instants.
    const int half = 24;
    const double fc = 1150.0 / rate, step = rate / kFs;
    for (;;) {
      const double pos = static_cast<double>(outCount) * step;  // in input samples
      const long centre = static_cast<long>(std::floor(pos));
      if (static_cast<double>(centre + half + 1) >= static_cast<double>(inCount)) break;
      cd acc = 0;
      for (long k = centre - half + 1; k <= centre + half; ++k) {
        if (k < static_cast<long>(inBase)) continue;
        const double t = pos - static_cast<double>(k);
        const double x = 2.0 * kPi * fc * t;
        const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(x) / x;
        const double w = 0.5 + 0.5 * std::cos(kPi * t / half);
        acc += cd(in[static_cast<std::size_t>(k - static_cast<long>(inBase))]) * (2.0 * fc * sinc * w);
      }
      bb.push_back(cf(acc));
      ++outCount;
    }
    // Input no longer under any kernel goes.
    const long keepFrom = static_cast<long>(std::floor(static_cast<double>(outCount) * step)) - half - 2;
    if (keepFrom > static_cast<long>(inBase) + 4096) {
      in.erase(in.begin(), in.begin() + (keepFrom - static_cast<long>(inBase)));
      inBase = static_cast<double>(keepFrom);
    }
  }

  // How well the A sequence starts at sample `s` (one sample per symbol):
  // the sum of the magnitudes of its correlation over 16 pieces (so that
  // a frequency error of 100 Hz does not hide it), over what a clean one
  // would give.
  float aMetric(uint64_t s) const {
    float sum = 0, energy = 0;
    const cf* x = bb.data() + (s - bbBase);
    for (int p = 0; p < 16; ++p) {
      cf c = 0;
      for (int i = p * kALen / 16; i < (p + 1) * kALen / 16; ++i) {
        const cf v = x[i * kSps];
        c += v * aRef[i];
        energy += std::norm(v);
      }
      sum += std::abs(c);
    }
    return sum / (std::sqrt(energy * static_cast<float>(kALen)) + 1e-20f);
  }

  // Symbols from sample `s0` on, with the frequency error removed.
  bool decodeAt(uint64_t s0, bool& wait) {
    wait = false;
    const uint64_t aSamples = static_cast<uint64_t>(kALen) * kSps;
    // The instant: where the symbols of both A sequences correlate best.
    auto single = [&](uint64_t s) {
      float v = 0;
      for (int blk = 0; blk < 2; ++blk)
        for (int p = 0; p < 16; ++p) {
          cf c = 0;
          for (int i = p * kALen / 16; i < (p + 1) * kALen / 16; ++i)
            c += at(s + static_cast<uint64_t>(blk) * aSamples + static_cast<uint64_t>(i) * kSps) * aRef[i];
          v += std::abs(c);
        }
      return v;
    };
    if (s0 < bbBase + 8) return false;
    float bestV = -1;
    uint64_t best = s0;
    for (uint64_t s = s0 - 6; s <= s0 + 6; ++s) {
      const float v = single(s);
      if (v > bestV) bestV = v, best = s;
    }
    s0 = best;
    // The frequency error: the phase's slope over the 32 pieces of A A.
    cf pieces[32];
    for (int blk = 0; blk < 2; ++blk)
      for (int p = 0; p < 16; ++p) {
        cf c = 0;
        for (int i = p * kALen / 16; i < (p + 1) * kALen / 16; ++i)
          c += at(s0 + static_cast<uint64_t>(blk) * aSamples + static_cast<uint64_t>(i) * kSps) * aRef[i];
        pieces[blk * 16 + p] = c;
      }
    cf slope = 0;
    for (int p = 0; p + 1 < 32; ++p) slope += pieces[p + 1] * std::conj(pieces[p]);
    const double perSymbol = std::arg(cd(slope)) / (2.0 * kALen / 32.0);  // radians per symbol
    auto symbol = [&](int i) {
      const double ph = -perSymbol * i;
      return cd(at(s0 + static_cast<uint64_t>(i) * kSps)) * cd(std::cos(ph), std::sin(ph));
    };
    // The reference phase from A A; then M1: which shift, hence the mode.
    cd ref = 0;
    for (int i = 0; i < 2 * kALen; ++i) ref += symbol(i) * static_cast<double>(aRef[i % kALen]);
    ref /= 2.0 * kALen;
    double m1[kM1Len], total = 1e-20;
    for (int i = 0; i < kM1Len; ++i) {
      m1[i] = (symbol(2 * kALen + i) * std::conj(ref)).real();
      total += std::fabs(m1[i]);
    }
    int mode = 0;
    double bestC = 0;
    for (int k = 0; k < 8; ++k) {
      double c = 0;
      for (int i = 0; i < kM1Len; ++i) c += m1[i] * (kM[(kShifts[k] + i) % kM1Len] ? -1.0 : 1.0);
      c = std::fabs(c) / total;
      if (c > bestC) bestC = c, mode = k;
    }
    if (bestC < 0.5) return false;
    const Mode& m = kModes[mode];
    const int symbols = kPreamble + m.frames * (kDataLen + kTLen);
    if (s0 + static_cast<uint64_t>(symbols + 1) * kSps >= have()) {
      wait = true;
      return false;
    }
    // The channel (phase and level) at every block of known symbols T, its
    // sign from all of them; between the blocks it is interpolated.
    const int tBlocks = 9 + m.frames;
    std::vector<cd> est(static_cast<std::size_t>(tBlocks));
    std::vector<double> estAt(static_cast<std::size_t>(tBlocks));
    auto tStart = [&](int b) { return b < 9 ? 2 * kALen + kM1Len + kM2Len + b * kTLen : kPreamble + (b - 9) * (kDataLen + kTLen) + kDataLen; };
    double sign = 0;
    for (int b = 0; b < tBlocks; ++b) {
      cd e = 0;
      for (int i = 0; i < kTLen; ++i) e += symbol(tStart(b) + i) * kT[i];
      est[static_cast<std::size_t>(b)] = e / static_cast<double>(kTLen);
      estAt[static_cast<std::size_t>(b)] = tStart(b) + 7;
      if (b < 9) sign += (e * std::conj(ref)).real();
    }
    if (sign < 0)
      for (auto& e : est) e = -e;
    // The data symbols: channel removed, unscrambled, to soft bits.
    const auto& pn = scrambler();
    const int nData = m.frames * kDataLen, M = 1 << m.arity;
    std::vector<float> soft;
    soft.reserve(static_cast<std::size_t>(nData * m.arity));
    int block = 8, d = 0;
    for (int f = 0; f < m.frames; ++f) {
      for (int i = 0; i < kDataLen; ++i, ++d) {
        const int pos = kPreamble + f * (kDataLen + kTLen) + i;
        while (block + 1 < tBlocks - 1 && estAt[static_cast<std::size_t>(block + 1)] < pos) ++block;
        const double a = estAt[static_cast<std::size_t>(block)], bpos = estAt[static_cast<std::size_t>(block + 1)];
        const double w = std::clamp((pos - a) / (bpos - a), 0.0, 1.0);
        const cd h = est[static_cast<std::size_t>(block)] * (1.0 - w) + est[static_cast<std::size_t>(block + 1)] * w;
        cd y = symbol(pos) * std::conj(h) / (std::norm(h) + 1e-12);
        if (pn[static_cast<std::size_t>(d % 120)]) y = -y;
        if (m.arity == 1) {
          soft.push_back(static_cast<float>(y.real()));
          continue;
        }
        // Point k at 2 pi k / M carries the Gray code of k, its highest
        // bit first.
        double dist[8];
        for (int k = 0; k < M; ++k) dist[k] = -std::norm(y - std::polar(1.0, 2.0 * kPi * k / M));
        for (int j = m.arity - 1; j >= 0; --j) {
          double zero = -1e30, one = -1e30;
          for (int k = 0; k < M; ++k) {
            const int label = k ^ (k >> 1);
            ((label >> j) & 1 ? one : zero) = std::max((label >> j) & 1 ? one : zero, dist[k]);
          }
          soft.push_back(static_cast<float>(zero - one));
        }
      }
    }
    const auto order = interleaverOrder(static_cast<int>(soft.size()), m.shift);
    std::vector<float> coded;
    coded.reserve(soft.size());
    if (m.rate == 4) {
      for (std::size_t i = 0; i + 1 < order.size(); i += 2)
        coded.push_back(0.5f * (soft[static_cast<std::size_t>(order[i])] + soft[static_cast<std::size_t>(order[i + 1])]));
    } else {
      for (int o : order) coded.push_back(soft[static_cast<std::size_t>(o)]);
    }
    const auto bits = viterbi(coded);
    std::vector<uint8_t> pdu(bits.size() / 8);
    for (std::size_t i = 0; i < pdu.size() * 8; ++i) pdu[i / 8] = static_cast<uint8_t>(pdu[i / 8] | bits[i] << (i % 8));
    const std::string text = hfdlDescribe(pdu, m.bitRate);
    search = s0 + static_cast<uint64_t>(symbols) * kSps;
    if (text.empty()) return false;
    ++decoded;
    out.push_back(text);
    return true;
  }

  void run() {
    const uint64_t aSamples = static_cast<uint64_t>(kALen) * kSps;
    // Enough after a position to judge both A sequences and M1, and to
    // look a little ahead for the best instant.
    const uint64_t ahead = 3 * aSamples + 64;
    if (search < bbBase + 16) search = bbBase + 16;
    while (search + ahead < have()) {
      if (aMetric(search) < 0.5f) {
        search += 2;
        continue;
      }
      // The best start within the next few samples; then the second A.
      uint64_t best = search;
      float bestM = 0;
      for (uint64_t s = search; s < search + 24; ++s) {
        const float v = aMetric(s);
        if (v > bestM) bestM = v, best = s;
      }
      float second = 0;
      for (uint64_t s = best + aSamples - 3; s <= best + aSamples + 3; ++s) second = std::max(second, aMetric(s));
      if (second < 0.4f) {
        search = best + 24;
        continue;
      }
      bool wait = false;
      const uint64_t before = search;
      const bool ok = decodeAt(best, wait);
      if (wait) break;  // the burst is not all here yet
      ++preambles;
      (void)ok;
      if (search == before) search = best + 2 * aSamples;  // no mode recognised: past these two sequences
    }
    // Keep what a burst found late could still need (a double slot: 4.7 s).
    const uint64_t keep = static_cast<uint64_t>(kFs * 0.02);
    if (search > bbBase + keep + 8192) {
      const uint64_t drop = search - keep - bbBase;
      bb.erase(bb.begin(), bb.begin() + static_cast<std::ptrdiff_t>(drop));
      bbBase += drop;
    }
  }
};

HfdlDecoder::HfdlDecoder(double rate, double carrierHz) : impl_(std::make_unique<Impl>(rate, carrierHz)) {}
HfdlDecoder::~HfdlDecoder() = default;

void HfdlDecoder::process(const float* audio, std::size_t n) {
  Impl& d = *impl_;
  // Mixed down by a rotating phasor (exp(-i phase)), renormalised now and
  // then: a sine and a cosine per sample cost more than the rest of the
  // decoder (measured 2026-11-16).
  const cd turn = std::polar(1.0, -2.0 * kPi * d.carrier / d.rate);
  for (std::size_t i = 0; i < n; ++i) {
    d.in.push_back(cf(static_cast<float>(audio[i] * d.rot.real()), static_cast<float>(audio[i] * d.rot.imag())));
    d.rot *= turn;
    if (++d.rotCount == 1024) {
      d.rotCount = 0;
      d.rot /= std::abs(d.rot);
    }
  }
  d.inCount += n;
  d.resample();
  d.run();
}

std::vector<std::string> HfdlDecoder::take() {
  std::vector<std::string> o;
  o.swap(impl_->out);
  return o;
}
int HfdlDecoder::preambles() const { return impl_->preambles; }
int HfdlDecoder::decoded() const { return impl_->decoded; }

// -------------------------------------------------------------- modulator --

std::vector<uint8_t> hfdlSquitter(int gs, int frameIndex, int slot) {
  std::vector<uint8_t> p(66, 0);
  p[0] = 0x04;  // an SPDU, version 1
  p[1] = static_cast<uint8_t>(0x80 | (gs & 0x7F));
  p[2] = static_cast<uint8_t>(frameIndex & 0xFF);
  p[3] = static_cast<uint8_t>(((frameIndex >> 8) & 0xF) | (slot & 0xF) << 4);
  const uint16_t fcs = hfdlFcs(p.data(), 64);
  p[64] = static_cast<uint8_t>(fcs & 0xFF);
  p[65] = static_cast<uint8_t>(fcs >> 8);
  return p;
}

std::vector<uint8_t> hfdlLogon(int gs, int aircraftId, uint32_t icao) {
  // LPDU: type, the address (each octet bit-reversed), its check.
  std::vector<uint8_t> lpdu = {0x8F, reverseBits(static_cast<uint8_t>(icao >> 16)), reverseBits(static_cast<uint8_t>(icao >> 8)),
                               reverseBits(static_cast<uint8_t>(icao))};
  const uint16_t lf = hfdlFcs(lpdu.data(), lpdu.size());
  lpdu.push_back(static_cast<uint8_t>(lf & 0xFF));
  lpdu.push_back(static_cast<uint8_t>(lf >> 8));
  // MPDU from an aircraft with one LPDU: 6 header octets, one size octet.
  std::vector<uint8_t> p = {static_cast<uint8_t>(0x03 | 1 << 2), static_cast<uint8_t>(gs & 0x7F), static_cast<uint8_t>(aircraftId), 0, 0, 0,
                            static_cast<uint8_t>(lpdu.size() - 1)};
  const uint16_t fcs = hfdlFcs(p.data(), p.size());
  p.push_back(static_cast<uint8_t>(fcs & 0xFF));
  p.push_back(static_cast<uint8_t>(fcs >> 8));
  p.insert(p.end(), lpdu.begin(), lpdu.end());
  return p;
}

std::vector<float> hfdlModulate(const std::vector<uint8_t>& pdu, int mode, double rate, double carrierHz) {
  if (mode < 0 || mode > 7) return {};
  const Mode& m = kModes[mode];
  const int nData = m.frames * kDataLen;
  const std::size_t userBits = static_cast<std::size_t>(nData * m.arity / m.rate);
  if (pdu.size() * 8 > userBits) return {};
  std::vector<uint8_t> bits(userBits, 0), half, coded;
  for (std::size_t i = 0; i < pdu.size() * 8; ++i) bits[i] = (pdu[i / 8] >> (i % 8)) & 1;
  encode(bits, half);
  if (m.rate == 4)
    for (uint8_t b : half) coded.push_back(b), coded.push_back(b);
  else
    coded = half;
  const auto order = interleaverOrder(static_cast<int>(coded.size()), m.shift);
  std::vector<uint8_t> tx(coded.size());
  for (std::size_t i = 0; i < coded.size(); ++i) tx[static_cast<std::size_t>(order[i])] = coded[i];
  // The symbols.
  std::vector<cd> sym(static_cast<std::size_t>(kPrekey), cd(1, 0));
  for (int r = 0; r < 2; ++r)
    for (int i = 0; i < kALen; ++i) sym.emplace_back(aSymbol(i), 0);
  for (int i = 0; i < kM1Len + kM2Len; ++i) sym.emplace_back(kM[(kShifts[mode] + i) % kM1Len] ? -1.0 : 1.0, 0);
  for (int r = 0; r < 9; ++r)
    for (double t : kT) sym.emplace_back(t, 0);
  const auto& pn = scrambler();
  const int M = 1 << m.arity;
  for (int d = 0; d < nData; ++d) {
    unsigned label = 0;
    for (int j = 0; j < m.arity; ++j) label = (label << 1) | tx[static_cast<std::size_t>(d * m.arity + j)];
    cd s = std::polar(1.0, 2.0 * kPi * grayToIndex(label) / M);
    if (pn[static_cast<std::size_t>(d % 120)]) s = -s;
    sym.push_back(s);
    if (d % kDataLen == kDataLen - 1)
      for (double t : kT) sym.emplace_back(t, 0);
  }
  // Raised-cosine pulses (roll-off 0.35, 4 symbols each side) on the
  // subcarrier.
  const double perSymbol = rate / kBaud, beta = 0.35;
  const std::size_t n = static_cast<std::size_t>(std::ceil((static_cast<double>(sym.size()) + 8) * perSymbol));
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / perSymbol - 4.0;  // in symbols
    const long k0 = static_cast<long>(std::floor(t));
    cd v = 0;
    for (long k = k0 - 3; k <= k0 + 4; ++k) {
      if (k < 0 || k >= static_cast<long>(sym.size())) continue;
      const double x = t - static_cast<double>(k);
      const double den = 1.0 - 4.0 * beta * beta * x * x;
      const double sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      const double p = std::fabs(den) < 1e-6 ? kPi / 4.0 * sinc : sinc * std::cos(kPi * beta * x) / den;
      v += sym[static_cast<std::size_t>(k)] * p;
    }
    const double ph = 2.0 * kPi * carrierHz * static_cast<double>(i) / rate;
    out[i] = static_cast<float>(0.7 * (v.real() * std::cos(ph) - v.imag() * std::sin(ph)));
  }
  return out;
}

}  // namespace hfc
