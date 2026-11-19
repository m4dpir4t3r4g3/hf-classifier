#include "hfc/wspr.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <queue>

namespace hfc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBaud = 12000.0 / 8192.0;  // also the tone spacing
constexpr uint32_t kPoly1 = 0xF2D05351u, kPoly2 = 0xE4613C47u;

const uint8_t kSync[162] = {
    1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0,
    0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0,
    1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0,
    0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0};

int parity(uint32_t v) {
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  return (0x6996 >> (v & 0xF)) & 1;
}

// Where coded bit p goes among the 162 transmitted: the p-th 8-bit index
// whose reversal is below 162.
const std::vector<int>& interleave() {
  static const std::vector<int> pos = [] {
    std::vector<int> v;
    for (int i = 0; i < 256 && v.size() < 162; ++i) {
      int j = 0;
      for (int b = 0; b < 8; ++b) j |= ((i >> b) & 1) << (7 - b);
      if (j < 162) v.push_back(j);
    }
    return v;
  }();
  return pos;
}

int charCode(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 36;  // space
}

bool packCall(const std::string& call, uint32_t& n) {
  if (call.size() < 3 || call.size() > 6) return false;
  std::string c6 = call;
  if (std::isdigit(static_cast<unsigned char>(c6[1])) && !std::isdigit(static_cast<unsigned char>(c6[2]))) c6 = " " + c6;
  if (c6.size() > 6 || !std::isdigit(static_cast<unsigned char>(c6[2]))) return false;
  c6.resize(6, ' ');
  for (int i = 3; i < 6; ++i)
    if (!(std::isupper(static_cast<unsigned char>(c6[static_cast<std::size_t>(i)])) || c6[static_cast<std::size_t>(i)] == ' ')) return false;
  n = static_cast<uint32_t>(charCode(c6[0]));
  n = n * 36 + static_cast<uint32_t>(charCode(c6[1]));
  n = n * 10 + static_cast<uint32_t>(charCode(c6[2]));
  for (int i = 3; i < 6; ++i) n = n * 27 + static_cast<uint32_t>(charCode(c6[static_cast<std::size_t>(i)]) - 10);
  return true;
}

bool validPower(int p) { return p >= 0 && p <= 60 && (p % 10 == 0 || p % 10 == 3 || p % 10 == 7); }

// The 50 message bits back to a message; false if any field is not valid.
bool unpack(const std::vector<uint8_t>& bits, WsprMessage& m) {
  uint32_t n = 0, mm = 0;
  for (int i = 0; i < 28; ++i) n = n << 1 | bits[static_cast<std::size_t>(i)];
  for (int i = 28; i < 50; ++i) mm = mm << 1 | bits[static_cast<std::size_t>(i)];
  static const char kAll[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ ";
  char c[7] = {0};
  for (int i = 5; i >= 3; --i) {
    const int v = static_cast<int>(n % 27);
    n /= 27;
    c[i] = v == 26 ? ' ' : static_cast<char>('A' + v);
  }
  c[2] = static_cast<char>('0' + n % 10);
  n /= 10;
  c[1] = kAll[n % 36];
  n /= 36;
  if (n > 36) return false;
  c[0] = kAll[n];
  if (c[1] == ' ') return false;
  std::string call(c);
  while (!call.empty() && call.front() == ' ') call.erase(0, 1);
  while (!call.empty() && call.back() == ' ') call.pop_back();
  // A callsign: a prefix of 1 or 2 characters with a letter, a digit, a
  // suffix of 1 to 3 letters.
  const std::size_t d = call.find_first_of("0123456789", 1);
  if (call.size() < 3 || call.find(' ') != std::string::npos) return false;
  const std::size_t digitAt = std::isdigit(static_cast<unsigned char>(call[2])) ? 2 : 1;
  (void)d;
  bool letterInPrefix = false;
  for (std::size_t i = 0; i < digitAt; ++i) letterInPrefix = letterInPrefix || std::isalpha(static_cast<unsigned char>(call[i]));
  if (!letterInPrefix || call.size() - digitAt - 1 < 1) return false;
  const int power = static_cast<int>(mm % 128) - 64;
  int g = static_cast<int>(mm / 128);
  if (!validPower(power) || g >= 32400) return false;
  const int l2 = g % 180, l1 = 179 - g / 180;
  if (l1 < 0) return false;
  char loc[5] = {static_cast<char>('A' + l1 / 10), static_cast<char>('A' + l2 / 10), static_cast<char>('0' + l1 % 10),
                 static_cast<char>('0' + l2 % 10), 0};
  if (loc[0] > 'R' || loc[1] > 'R') return false;
  m.callsign = call;
  m.locator = loc;
  m.powerDbm = power;
  return true;
}

void encode(const std::vector<uint8_t>& bits81, std::vector<uint8_t>& coded) {
  uint32_t st = 0;
  for (uint8_t b : bits81) {
    st = st << 1 | b;
    coded.push_back(static_cast<uint8_t>(parity(st & kPoly1)));
    coded.push_back(static_cast<uint8_t>(parity(st & kPoly2)));
  }
}

// Sequential (stack) decoding of 81 bits (the last 31 zero) from 162 soft
// values (positive: a 0, in units where 1 is a clean symbol), with the
// Fano metric. Gives up after `maxNodes`.
bool stackDecode(const std::vector<float>& soft, std::vector<uint8_t>& out, int maxNodes = 60000) {
  struct Node {
    int parent, depth;
    uint32_t state;
    float metric;
    uint8_t bit;
  };
  std::vector<Node> nodes;
  nodes.reserve(static_cast<std::size_t>(maxNodes) + 2);
  auto cmp = [&](int a, int b) { return nodes[static_cast<std::size_t>(a)].metric < nodes[static_cast<std::size_t>(b)].metric; };
  std::priority_queue<int, std::vector<int>, decltype(cmp)> open(cmp);
  // Metric of coded bit c given soft r: log2(P(r|c) / P(r)) - 1/2.
  auto bm = [](float r, int c) {
    const double l = 4.0 * (c ? -r : r);  // log-likelihood ratio for c
    return static_cast<float>(1.0 - std::log2(1.0 + std::exp(-l)) - 0.5);
  };
  nodes.push_back({-1, 0, 0, 0, 0});
  open.push(0);
  while (!open.empty() && static_cast<int>(nodes.size()) < maxNodes) {
    const int k = open.top();
    open.pop();
    const Node n = nodes[static_cast<std::size_t>(k)];
    if (n.depth == 81) {
      out.assign(81, 0);
      for (int i = k; nodes[static_cast<std::size_t>(i)].parent >= 0; i = nodes[static_cast<std::size_t>(i)].parent)
        out[static_cast<std::size_t>(nodes[static_cast<std::size_t>(i)].depth - 1)] = nodes[static_cast<std::size_t>(i)].bit;
      return true;
    }
    for (int b = 0; b < (n.depth >= 50 ? 1 : 2); ++b) {
      const uint32_t st = n.state << 1 | static_cast<uint32_t>(b);
      const float r0 = soft[static_cast<std::size_t>(2 * n.depth)], r1 = soft[static_cast<std::size_t>(2 * n.depth + 1)];
      const float m = n.metric + bm(r0, parity(st & kPoly1)) + bm(r1, parity(st & kPoly2));
      nodes.push_back({k, n.depth + 1, st, m, static_cast<uint8_t>(b)});
      open.push(static_cast<int>(nodes.size()) - 1);
    }
  }
  return false;
}

void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> w = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> u = 1;
      for (std::size_t j = 0; j < len / 2; ++j, u *= w) {
        const auto x = a[i + j], y = a[i + j + len / 2] * u;
        a[i + j] = x + y;
        a[i + j + len / 2] = x - y;
      }
    }
  }
}

}  // namespace

std::string WsprMessage::text() const {
  char b[64];
  std::snprintf(b, sizeof b, "%s %s %d dBm", callsign.c_str(), locator.c_str(), powerDbm);
  return b;
}

const std::vector<double>& wsprDialFrequenciesKhz() {
  static const std::vector<double> f = {136.0, 474.2, 1836.6, 3568.6, 5287.2, 7038.6, 10138.7, 14095.6, 18104.6, 21094.6, 24924.6, 28124.6};
  return f;
}

std::vector<int> wsprTones(const std::string& callsign, const std::string& locator, int powerDbm) {
  uint32_t n;
  if (!packCall(callsign, n) || locator.size() != 4 || !validPower(powerDbm)) return {};
  const int a = locator[0] - 'A', b = locator[1] - 'A', c = locator[2] - '0', d = locator[3] - '0';
  if (a < 0 || a > 17 || b < 0 || b > 17 || c < 0 || c > 9 || d < 0 || d > 9) return {};
  const uint32_t m = static_cast<uint32_t>(((179 - 10 * a - c) * 180 + 10 * b + d) * 128 + powerDbm + 64);
  std::vector<uint8_t> bits(81, 0), coded;
  for (int i = 0; i < 28; ++i) bits[static_cast<std::size_t>(i)] = (n >> (27 - i)) & 1;
  for (int i = 0; i < 22; ++i) bits[static_cast<std::size_t>(28 + i)] = (m >> (21 - i)) & 1;
  encode(bits, coded);
  std::vector<int> tones(162);
  const auto& pos = interleave();
  std::vector<uint8_t> tx(162);
  for (int p = 0; p < 162; ++p) tx[static_cast<std::size_t>(pos[static_cast<std::size_t>(p)])] = coded[static_cast<std::size_t>(p)];
  for (int i = 0; i < 162; ++i) tones[static_cast<std::size_t>(i)] = 2 * tx[static_cast<std::size_t>(i)] + kSync[i];
  return tones;
}

std::vector<float> wsprAudio(const std::vector<int>& tones, double audioHz, double rate, double seconds) {
  std::vector<float> a(static_cast<std::size_t>(seconds * rate), 0.0f);
  const double sym = rate / kBaud;
  double ph = 0;
  for (std::size_t i = 0; i < tones.size(); ++i) {
    const double f = audioHz + (tones[i] - 1.5) * kBaud;
    const std::size_t from = static_cast<std::size_t>(rate * 1.0 + static_cast<double>(i) * sym);
    const std::size_t to = static_cast<std::size_t>(rate * 1.0 + static_cast<double>(i + 1) * sym);
    for (std::size_t k = from; k < to && k < a.size(); ++k) {
      ph += 2.0 * kPi * f / rate;
      a[k] = static_cast<float>(0.5 * std::sin(ph));
    }
  }
  return a;
}

std::vector<WsprMessage> wsprDecode(const std::vector<float>& audio, double rate, double loHz, double hiHz) {
  std::vector<WsprMessage> out;
  // Complex baseband at 375 samples a second around 1500 Hz: mix, then
  // average over blocks (a crude low-pass, enough for 4-FSK this slow).
  const double fsb = 375.0, centre = 1500.0;
  const int dec = static_cast<int>(std::lround(rate / fsb));
  if (dec < 1) return out;
  std::vector<std::complex<double>> bb;
  {
    std::complex<double> acc = 0;
    double ph = 0;
    const double step = 2.0 * kPi * centre / rate;
    int k = 0;
    for (float v : audio) {
      acc += std::complex<double>(v * std::cos(ph), -v * std::sin(ph));
      ph += step;
      if (ph > 2 * kPi) ph -= 2 * kPi;
      if (++k == dec) bb.push_back(acc / static_cast<double>(dec)), acc = 0, k = 0;
    }
  }
  // (A recording cut right at the signal's end: room for its last symbol.)
  bb.resize(bb.size() + 512, 0);
  // Spectra of one symbol (256 values) every quarter symbol, zero-padded
  // to 512: bins of half a tone spacing.
  const int nsym = 256, step = 64, nfft = 512;
  const int frames = static_cast<int>((static_cast<int>(bb.size()) - nsym) / step);
  if (frames < 161 * 4 + 1) return out;
  std::vector<std::vector<float>> P(static_cast<std::size_t>(frames), std::vector<float>(nfft));
  std::vector<std::complex<double>> buf(nfft);
  for (int f = 0; f < frames; ++f) {
    std::fill(buf.begin(), buf.end(), 0);
    for (int i = 0; i < nsym; ++i) buf[static_cast<std::size_t>(i)] = bb[static_cast<std::size_t>(f * step + i)];
    fft(buf);
    for (int b = 0; b < nfft; ++b) P[static_cast<std::size_t>(f)][static_cast<std::size_t>((b + nfft / 2) % nfft)] = static_cast<float>(std::norm(buf[static_cast<std::size_t>(b)]));
  }
  const double binHz = fsb / nfft;  // half a tone spacing
  auto binOf = [&](double hz) { return static_cast<int>(std::lround((hz - centre) / binHz)) + nfft / 2; };
  // Within what the baseband holds (+-187 Hz), with room for the 4 tones.
  loHz = std::max(loHz, centre - 180.0);
  hiHz = std::min(hiHz, centre + 180.0);
  if (hiHz - loHz < 10) return out;
  // The noise: the median power over the band.
  std::vector<float> all;
  for (int f = 0; f < frames; f += 7)
    for (int b = binOf(loHz); b < binOf(hiHz); ++b) all.push_back(P[static_cast<std::size_t>(f)][static_cast<std::size_t>(b)]);
  std::nth_element(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() / 2), all.end());
  const double noise = std::max(1e-20, static_cast<double>(all[all.size() / 2]));
  // Candidates: tone 0 at bin b0, the first symbol at frame t0 (quarter
  // symbols; the signal nominally starts 1 s in, searched over 0 to 4 s
  // and, for a buffer longer than a slot, all of it).
  struct Cand {
    double score;
    int b0, t0;
  };
  std::vector<Cand> cands;
  const int maxT0 = frames - 1 - 161 * 4;  // the last symbol's spectrum must exist
  if (maxT0 < 0) return out;
  for (int b0 = binOf(loHz); b0 + 6 < binOf(hiHz); ++b0) {
    Cand best{-1e30, b0, 0};
    for (int t0 = 0; t0 <= maxT0; ++t0) {
      double s = 0, tot = 0;
      for (int i = 0; i < 162; ++i) {
        const auto& p = P[static_cast<std::size_t>(t0 + 4 * i)];
        const double a0 = p[static_cast<std::size_t>(b0)], a1 = p[static_cast<std::size_t>(b0 + 2)], a2 = p[static_cast<std::size_t>(b0 + 4)],
                     a3 = p[static_cast<std::size_t>(b0 + 6)];
        const double d = (a1 + a3) - (a0 + a2);
        s += kSync[i] ? d : -d;
        tot += a0 + a1 + a2 + a3;
      }
      const double score = s / (tot + 1e-30);
      if (score > best.score) best = {score, b0, t0};
    }
    cands.push_back(best);
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
  std::vector<int> taken;
  for (const Cand& c : cands) {
    if (c.score < 0.12 || out.size() >= 8) break;
    bool near = false;
    for (int b : taken) near = near || std::abs(b - c.b0) < 8;
    if (near) continue;
    // Soft data bits: tone 2 + s against tone s, in amplitude, normalised.
    std::vector<float> data(162);
    double sum = 0;
    for (int i = 0; i < 162; ++i) {
      const auto& p = P[static_cast<std::size_t>(c.t0 + 4 * i)];
      const double lo = std::sqrt(p[static_cast<std::size_t>(c.b0 + 2 * kSync[i])]), hi = std::sqrt(p[static_cast<std::size_t>(c.b0 + 2 * (2 + kSync[i]))]);
      data[static_cast<std::size_t>(i)] = static_cast<float>(lo - hi);
      sum += std::fabs(lo - hi);
    }
    const float scale = static_cast<float>(162.0 / (sum + 1e-30));
    std::vector<float> soft(162);
    const auto& pos = interleave();
    for (int p = 0; p < 162; ++p) soft[static_cast<std::size_t>(p)] = data[static_cast<std::size_t>(pos[static_cast<std::size_t>(p)])] * scale;
    std::vector<uint8_t> bits;
    if (!stackDecode(soft, bits)) continue;
    WsprMessage m;
    if (!unpack(bits, m)) continue;
    // Coded again, it must agree with the tones received.
    const auto tones = wsprTones(m.callsign, m.locator, m.powerDbm);
    if (tones.empty()) continue;
    int agree = 0;
    double sig = 0;
    for (int i = 0; i < 162; ++i) {
      const auto& p = P[static_cast<std::size_t>(c.t0 + 4 * i)];
      int best = 0;
      for (int t = 1; t < 4; ++t)
        if (p[static_cast<std::size_t>(c.b0 + 2 * t)] > p[static_cast<std::size_t>(c.b0 + 2 * best)]) best = t;
      agree += best == tones[static_cast<std::size_t>(i)];
      sig += p[static_cast<std::size_t>(c.b0 + 2 * tones[static_cast<std::size_t>(i)])];
    }
    if (agree < 0.75 * 162) continue;
    taken.push_back(c.b0);
    m.audioHz = centre + (c.b0 - nfft / 2 + 3) * binHz;
    m.dtSeconds = c.t0 * step / fsb;
    // SNR in 2500 Hz: the signal's power per symbol over the noise in a bin
    // (bins of fsb / nfft Hz, but the symbol spectrum has nsym values).
    const double s = sig / 162.0 / noise;
    m.snrDb = 10.0 * std::log10(std::max(1e-6, s - 1.0)) + 10.0 * std::log10(fsb / nsym / 2500.0);
    out.push_back(m);
  }
  return out;
}

}  // namespace hfc
