#include "navtex/ale_codes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace navtex::ale {

namespace {

// Parity matrix P of figure A-6: the check bits added by data bit W1 (first
// row) .. W12.
constexpr std::uint32_t kGolayP[12] = {0xAE3, 0xF92, 0xD2B, 0xC76, 0xCD9, 0x66D,
                                       0x337, 0xB78, 0x5BC, 0x2DE, 0xB8D, 0x5C7};

std::uint32_t golayCheck(std::uint32_t data12) {
  std::uint32_t c = 0;
  for (int i = 0; i < 12; ++i)
    if ((data12 >> (11 - i)) & 1) c ^= kGolayP[i];
  return c;
}

int popcount(std::uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

// Syndrome -> error pattern of weight <= 3 (0xffffffff: four errors).
struct GolayTable {
  std::array<std::uint32_t, 4096> pattern;
  GolayTable() {
    pattern.fill(0xffffffffu);
    auto add = [&](std::uint32_t e) {
      std::uint32_t s = golayCheck(e >> 12) ^ (e & 0xfff);
      pattern[s] = e;
    };
    add(0);
    for (int a = 0; a < 24; ++a) {
      add(1u << a);
      for (int b = a + 1; b < 24; ++b) {
        add((1u << a) | (1u << b));
        for (int c = b + 1; c < 24; ++c) add((1u << a) | (1u << b) | (1u << c));
      }
    }
  }
};

const GolayTable& golayTable() {
  static const GolayTable t;
  return t;
}

constexpr int kToneTribit[8] = {0, 1, 3, 2, 6, 7, 5, 4};

// Taps of one output from the exponents of its polynomial (x^(k-1): newest bit).
constexpr unsigned taps(int k, std::initializer_list<int> exponents) {
  unsigned m = 0;
  for (int e : exponents) m |= 1u << (k - 1 - e);
  return m;
}

}  // namespace

const char* preambleName(int p) {
  static const char* const kNames[8] = {"DATA", "THRU", "TO", "TWAS", "FROM", "TIS", "CMD", "REP"};
  return kNames[p & 7];
}

std::uint32_t golayEncode(std::uint32_t data12) {
  data12 &= 0xfff;
  return (data12 << 12) | golayCheck(data12);
}

int golayDecode(std::uint32_t r, int& errors) {
  r &= 0xffffff;
  std::uint32_t s = golayCheck(r >> 12) ^ (r & 0xfff);
  std::uint32_t e = golayTable().pattern[s];
  if (e == 0xffffffffu) {
    errors = 4;
    return -1;
  }
  errors = popcount(e);
  return static_cast<int>(((r ^ e) >> 12) & 0xfff);
}

void interleaveWord(std::uint32_t word, std::uint8_t bits[49]) {
  std::uint32_t a = golayEncode(word >> 12), b = golayEncode(word & 0xfff) ^ 0xfff;
  for (int i = 0; i < 24; ++i) {
    bits[2 * i] = static_cast<std::uint8_t>((a >> (23 - i)) & 1);
    bits[2 * i + 1] = static_cast<std::uint8_t>((b >> (23 - i)) & 1);
  }
  bits[48] = 0;
}

void deinterleaveWord(const std::uint8_t bits[48], std::uint32_t& a, std::uint32_t& b) {
  a = b = 0;
  for (int i = 0; i < 24; ++i) {
    a |= static_cast<std::uint32_t>(bits[2 * i] & 1) << (23 - i);
    b |= static_cast<std::uint32_t>(bits[2 * i + 1] & 1) << (23 - i);
  }
  b ^= 0xfff;
}

int tribitOfTone(int tone) { return kToneTribit[tone & 7]; }

int toneOfTribit(int tribit) {
  for (int t = 0; t < 8; ++t)
    if (kToneTribit[t] == (tribit & 7)) return t;
  return 0;
}

void redundantWordTones(std::uint32_t word, std::uint8_t tones[49]) {
  std::uint8_t bits[49];
  interleaveWord(word, bits);
  for (int k = 0; k < 49; ++k) {
    int t = 0;
    for (int j = 0; j < 3; ++j) t = (t << 1) | bits[(3 * k + j) % 49];
    tones[k] = static_cast<std::uint8_t>(toneOfTribit(t));
  }
}

bool isBasic38(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '?'; }
bool isExpanded64(char c) { return c >= 0x20 && c <= 0x5f; }

// ------------------------------------------------------------- CRCs

std::uint32_t crcAppendixC(const std::uint8_t* bits, int n, std::uint32_t poly, int k) {
  const std::uint32_t mask = (k >= 32) ? 0xffffffffu : ((1u << k) - 1);
  std::uint32_t crc = 0;
  for (int i = 0; i < n; ++i) {
    std::uint32_t fb = ((crc >> (k - 1)) & 1) ^ (bits[i] & 1);
    crc = (crc << 1) & mask;
    if (fb) crc ^= poly;
  }
  return crc;
}

std::uint16_t crc16Wale(const std::uint8_t* octets, std::size_t n) {
  unsigned crc = 0xffff;
  for (std::size_t o = 0; o < n; ++o)
    for (unsigned i = 0x01; i < 0x100; i <<= 1) {
      unsigned bit = (crc & 1) ^ ((octets[o] & i) ? 1 : 0);
      crc >>= 1;
      if (bit) crc ^= 0x9299;
    }
  return static_cast<std::uint16_t>(crc ^ 0xffff);
}

// ------------------------------------------------------------- convolutional codes

const ConvCode kBw0Code = {7, 2, {taps(7, {6, 4, 3, 1, 0}), taps(7, {6, 5, 4, 3, 0}), 0}};
const ConvCode kBw1Code = {
    9, 3, {taps(9, {8, 7, 6, 3, 0}), taps(9, {8, 7, 5, 4, 1, 0}), taps(9, {8, 6, 5, 3, 2, 1, 0})}};
const ConvCode kWaleCode = {9, 2, {taps(9, {8, 6, 5, 4, 0}), taps(9, {8, 7, 6, 5, 3, 1, 0}), 0}};

std::vector<std::uint8_t> convEncode(const ConvCode& code, const std::vector<std::uint8_t>& bits) {
  const int n = static_cast<int>(bits.size());
  std::vector<std::uint8_t> out;
  out.reserve(bits.size() * static_cast<std::size_t>(code.n));
  for (int i = 0; i < n; ++i) {
    unsigned reg = 0;
    for (int d = 0; d < code.k; ++d) reg |= static_cast<unsigned>(bits[static_cast<std::size_t>(((i - d) % n + n) % n)] & 1) << d;
    for (int j = 0; j < code.n; ++j) out.push_back(static_cast<std::uint8_t>(parity(reg & code.taps[j])));
  }
  return out;
}

int codeWordDisagreements(const ConvCode& code, const std::vector<std::uint8_t>& decoded, const std::vector<float>& soft) {
  const std::vector<std::uint8_t> re = convEncode(code, decoded);
  int n = 0;
  for (std::size_t i = 0; i < soft.size() && i < re.size(); ++i)
    if ((soft[i] > 0.0f) != (re[i] != 0)) ++n;
  return n;
}

std::vector<std::uint8_t> TailBitingViterbi::decode(const ConvCode& code, const std::vector<float>& soft,
                                                    double* metric) {
  const int n = static_cast<int>(soft.size()) / code.n;
  std::vector<std::uint8_t> bits(static_cast<std::size_t>(n), 0);
  if (n <= 0) {
    if (metric) *metric = 0.0;
    return bits;
  }
  const int states = 1 << (code.k - 1);
  constexpr int kPasses = 3;  // wrap-around: decisions are taken from the middle pass
  const int steps = kPasses * n;
  pm_.assign(static_cast<std::size_t>(states), 0.0f);
  next_.assign(static_cast<std::size_t>(states), 0.0f);
  decisions_.assign(static_cast<std::size_t>(steps) * static_cast<std::size_t>(states), 0);
  // Expected output bits for each register value.
  std::vector<std::uint8_t> outs(static_cast<std::size_t>(states) * 2);
  for (unsigned reg = 0; reg < static_cast<unsigned>(states) * 2; ++reg) {
    unsigned o = 0;
    for (int j = 0; j < code.n; ++j) o |= static_cast<unsigned>(parity(reg & code.taps[j])) << j;
    outs[reg] = static_cast<std::uint8_t>(o);
  }
  for (int t = 0; t < steps; ++t) {
    const float* sv = &soft[static_cast<std::size_t>((t % n) * code.n)];
    float bm[8];
    for (unsigned o = 0; o < (1u << code.n); ++o) {
      float m = 0.0f;
      for (int j = 0; j < code.n; ++j) m += ((o >> j) & 1) ? sv[j] : -sv[j];
      bm[o] = m;
    }
    std::uint8_t* dec = &decisions_[static_cast<std::size_t>(t) * static_cast<std::size_t>(states)];
    float best = -std::numeric_limits<float>::infinity();
    for (unsigned ns = 0; ns < static_cast<unsigned>(states); ++ns) {
      // Predecessors: the oldest bit x of the previous state is dropped.
      unsigned p0 = ns >> 1, p1 = (ns >> 1) | (1u << (code.k - 2));
      unsigned b = ns & 1;
      unsigned r0 = ((p0 << 1) | b), r1 = ((p1 << 1) | b);
      float m0 = pm_[p0] + bm[outs[r0]], m1 = pm_[p1] + bm[outs[r1]];
      if (m1 > m0) {
        next_[ns] = m1;
        dec[ns] = 1;
      } else {
        next_[ns] = m0;
        dec[ns] = 0;
      }
      best = std::max(best, next_[ns]);
    }
    for (int s = 0; s < states; ++s) pm_[static_cast<std::size_t>(s)] = next_[static_cast<std::size_t>(s)] - best;
  }
  unsigned s = static_cast<unsigned>(std::max_element(pm_.begin(), pm_.end()) - pm_.begin());
  for (int t = steps - 1; t >= 0; --t) {
    if (t >= n && t < 2 * n) bits[static_cast<std::size_t>(t - n)] = static_cast<std::uint8_t>(s & 1);
    unsigned x = decisions_[static_cast<std::size_t>(t) * static_cast<std::size_t>(states) + s];
    s = (s >> 1) | (x << (code.k - 2));
  }
  if (metric) {
    std::vector<std::uint8_t> re = convEncode(code, bits);
    double agree = 0.0, total = 0.0;
    for (std::size_t i = 0; i < soft.size(); ++i) {
      agree += re[i] ? soft[i] : -soft[i];
      total += std::fabs(soft[i]);
    }
    *metric = total > 0.0 ? agree / total : 0.0;
  }
  return bits;
}

// ------------------------------------------------------------- 3G interleavers

std::vector<int> blockInterleaverOrder(int rows, int cols, int irs, int ics, int dirs, int dics, int irf, int icf,
                                       int dirf, int dicf) {
  const int size = rows * cols;
  std::vector<int> matrix(static_cast<std::size_t>(size), -1);
  int rs = 0, cs = 0;
  for (int s = 0; s < size;) {
    matrix[static_cast<std::size_t>(rs * cols + cs)] = s;
    ++s;
    cs = (s % rows == 0) ? (cs + ics + dics) % cols : (cs + ics) % cols;
    rs = (s % cols == 0) ? (rs + irs + dirs) % rows : (rs + irs) % rows;
  }
  std::vector<int> order(static_cast<std::size_t>(size));
  int rf = 0, cf = 0;
  for (int f = 0; f < size;) {
    order[static_cast<std::size_t>(f)] = matrix[static_cast<std::size_t>(rf * cols + cf)];
    ++f;
    cf = (f % rows == 0) ? (cf + icf + dicf) % cols : (cf + icf) % cols;
    rf = (f % cols == 0) ? (rf + irf + dirf) % rows : (rf + irf) % rows;
  }
  return order;
}

const std::vector<int>& bw0InterleaverOrder() {
  static const std::vector<int> o = blockInterleaverOrder(4, 13, 0, 1, 1, 0, 1, 0, 0, 1);
  return o;
}

const std::vector<int>& bw1InterleaverOrder() {
  static const std::vector<int> o = blockInterleaverOrder(16, 9, 0, 1, 1, 0, 1, 0, 0, 1);
  return o;
}

// ------------------------------------------------------------- 4G

int walePreambleChip(int dibit, bool exceptional, int j) {
  return exceptional ? walshChip((dibit & 3) | 4, j & 7) : walshChip(dibit & 3, j & 3);
}

void DeepScrambler::reset() { std::memcpy(reg_, kDeepScramblerInit, sizeof reg_); }

int DeepScrambler::next() {
  for (int j = 0; j < 16; ++j) {
    std::uint8_t out = reg_[158], tap = reg_[31];
    std::memmove(reg_ + 1, reg_, 158);
    reg_[0] = out ^ tap;
  }
  return (reg_[2] << 2) | (reg_[1] << 1) | reg_[0];
}

int FastScrambler::next() {
  int v = static_cast<int>(state_ & 7);
  for (int i = 0; i < 3; ++i) {
    unsigned fb = (state_ ^ (state_ >> 4)) & 1;
    state_ = (state_ >> 1) | (fb << 8);
  }
  return v;
}

// ------------------------------------------------------------- bits

std::vector<std::uint8_t> bitsMsbFirst(std::uint64_t value, int count) {
  std::vector<std::uint8_t> b(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) b[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1);
  return b;
}

std::uint64_t valueMsbFirst(const std::uint8_t* bits, int count) {
  std::uint64_t v = 0;
  for (int i = 0; i < count; ++i) v = (v << 1) | (bits[i] & 1);
  return v;
}

}  // namespace navtex::ale
