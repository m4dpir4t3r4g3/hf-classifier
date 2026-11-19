#include "navtex/ale_synth.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "navtex/ale_codes.hpp"
#include "navtex/ale_frontend.hpp"
#include "navtex/dsp.hpp"
#include "navtex/synth.hpp"

namespace navtex::ale {

namespace {

using cd = std::complex<double>;

// Scales a baseband burst to mean power amplitude^2 (a real signal of power
// amplitude^2 / 2, as a sine of that amplitude).
void normalise(std::vector<cd>& b, double amplitude) {
  double p = 0.0;
  for (const cd& v : b) p += std::norm(v);
  p /= std::max<std::size_t>(1, b.size());
  if (p <= 0.0) return;
  const double k = amplitude / std::sqrt(p);
  for (cd& v : b) v *= k;
}

std::vector<cd> fskBaseband(const std::vector<int>& tones, const AleChannel& ch) {
  const double fs = ch.sampleRate, ts = (1.0 + ch.clockPpm * 1e-6) / kBaud2g;
  const auto n = static_cast<std::size_t>(std::llround(tones.size() * ts * fs));
  std::vector<cd> b(n);
  double phase = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t sym = std::min(tones.size() - 1, static_cast<std::size_t>(static_cast<double>(i) / fs / ts));
    double f = kToneHz0 + kToneStepHz * tones[sym] - kCarrierHz;
    b[i] = std::polar(1.0, phase);
    phase += 2.0 * kPi * f / fs;
    if (phase > kPi) phase -= 2.0 * kPi;
    if (phase < -kPi) phase += 2.0 * kPi;
  }
  normalise(b, ch.amplitude);
  return b;
}

double rrc(double t) {
  // RrcFilter::tap at any time t (symbols).
  const double beta = 0.35;
  if (std::fabs(t) < 1e-9) return 1.0 - beta + 4.0 * beta / kPi;
  if (std::fabs(std::fabs(t) - 1.0 / (4.0 * beta)) < 1e-9)
    return beta / std::sqrt(2.0) *
           ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
  return (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) /
         (kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
}

std::vector<cd> pskBaseband(const std::vector<int>& symbols, const AleChannel& ch) {
  const double fs = ch.sampleRate, ts = (1.0 + ch.clockPpm * 1e-6) / 2400.0;
  const double span = 6.0;
  const auto n = static_cast<std::size_t>(std::llround((symbols.size() + 2 * span) * ts * fs));
  std::vector<cd> b(n);
  for (std::size_t k = 0; k < symbols.size(); ++k) {
    const cd a = std::polar(1.0, kPi / 4.0 * symbols[k]);
    const double centre = (static_cast<double>(k) + span) * ts;
    const auto i0 = static_cast<long long>(std::floor((centre - span * ts) * fs));
    const auto i1 = static_cast<long long>(std::ceil((centre + span * ts) * fs));
    for (long long i = std::max(0LL, i0); i <= i1 && i < static_cast<long long>(n); ++i)
      b[static_cast<std::size_t>(i)] += a * rrc((static_cast<double>(i) / fs - centre) / ts);
  }
  normalise(b, ch.amplitude);
  return b;
}

// Rayleigh fading gain (sum of sinusoids), mean power 1.
class Fader {
 public:
  Fader(Noise& rng, double hz) {
    for (int m = 0; m < kM; ++m) {
      freq_[m] = hz * std::cos(2.0 * kPi * rng.uniform());
      phase_[m] = 2.0 * kPi * rng.uniform();
    }
  }
  cd at(double t) const {
    cd g = 0.0;
    for (int m = 0; m < kM; ++m) g += std::polar(1.0, 2.0 * kPi * freq_[m] * t + phase_[m]);
    return g / std::sqrt(static_cast<double>(kM));
  }

 private:
  static constexpr int kM = 16;
  double freq_[kM], phase_[kM];
};

AleSignal channel(const std::vector<cd>& b, const AleChannel& ch) {
  Noise rng(ch.seed);
  const double fs = ch.sampleRate;
  const auto lead = static_cast<std::size_t>(ch.leadSeconds * fs), tail = static_cast<std::size_t>(ch.tailSeconds * fs);
  const bool twoPath = ch.pathDb > -60.0;
  const auto delay = static_cast<std::size_t>(std::llround(ch.pathDelayMs * 1e-3 * fs));
  const double g1 = std::pow(10.0, ch.pathDb / 20.0);
  const double phase1 = 2.0 * kPi * rng.uniform();
  Fader f0(rng, ch.fadingHz), f1(rng, ch.fadingHz);
  const std::size_t n = lead + b.size() + (twoPath ? delay : 0) + tail;
  AleSignal s;
  s.samples.resize(n);
  s.start = static_cast<double>(lead) / fs;
  s.end = static_cast<double>(lead + b.size() + (twoPath ? delay : 0)) / fs;
  const double sigma = std::sqrt(ch.amplitude * ch.amplitude / 2.0 / std::pow(10.0, ch.snrDb / 10.0) * (fs / 2.0) / 3000.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / fs;
    cd y = 0.0;
    if (i >= lead && i - lead < b.size()) y += b[i - lead] * (ch.fadingHz > 0 ? f0.at(t) : cd(1.0));
    if (twoPath && i >= lead + delay && i - lead - delay < b.size())
      y += b[i - lead - delay] * g1 * (ch.fadingHz > 0 ? f1.at(t) : std::polar(1.0, phase1));
    y *= std::polar(1.0, 2.0 * kPi * (kCarrierHz + ch.offsetHz) * t);
    s.samples[i] = static_cast<float>(y.real() + sigma * rng.gauss());
  }
  return s;
}

void appendWale(std::vector<int>& out, int dibit, bool exceptional) {
  for (int j = 0; j < 32; ++j) out.push_back((walePreambleChip(dibit, exceptional, j) + kWalePreambleScramble[j]) & 7);
}

// The 192 coded, interleaved bits of a WALE PDU, in the order sent.
std::vector<std::uint8_t> waleBlock(const WalePdu& pdu) {
  std::vector<std::uint8_t> p(96);
  for (int i = 0; i < 96; ++i) p[static_cast<std::size_t>(i)] = (pdu[static_cast<std::size_t>(i / 8)] >> (i % 8)) & 1;
  std::vector<std::uint8_t> q(96);
  for (int i = 0; i < 96; ++i) q[static_cast<std::size_t>(i)] = p[static_cast<std::size_t>((i + 8) % 96)];
  std::vector<std::uint8_t> coded = convEncode(kWaleCode, q), sent(192);
  for (int n = 0; n < 192; ++n) sent[static_cast<std::size_t>(waleInterleavePos(n))] = coded[static_cast<std::size_t>(n)];
  return sent;
}

std::vector<int> walshBurst(const Bits& pdu, const ConvCode& code, const std::vector<int>& order, const std::uint8_t* pre,
                            int preLen) {
  std::vector<int> s(k3gTlc, k3gTlc + 256);
  s.insert(s.end(), pre, pre + preLen);
  std::vector<std::uint8_t> coded = convEncode(code, pdu);
  std::vector<std::uint8_t> o(order.size());
  for (std::size_t f = 0; f < order.size(); ++f) o[f] = coded[static_cast<std::size_t>(order[f])];
  for (std::size_t g = 0; g * 4 < o.size(); ++g) {
    int v = (o[4 * g] << 3) | (o[4 * g + 1] << 2) | (o[4 * g + 2] << 1) | o[4 * g + 3];
    for (int j = 0; j < 64; ++j) s.push_back((walshChip(v, j & 15) + k3gPn[(64 * g + static_cast<std::size_t>(j)) % 256]) & 7);
  }
  return s;
}

}  // namespace

std::vector<int> bw0Symbols(const Bits& pdu) { return walshBurst(pdu, kBw0Code, bw0InterleaverOrder(), kBw0Preamble, 384); }
std::vector<int> bw1Symbols(const Bits& pdu) { return walshBurst(pdu, kBw1Code, bw1InterleaverOrder(), kBw1Preamble, 576); }

std::vector<int> waleSymbols(bool deep, const std::vector<WalePdu>& pdus, int preambles) {
  std::vector<int> s;
  const int more = !pdus.empty() && waleMoreFollow(pdus[0]) ? 1 : 0;
  const int count = deep ? std::clamp(preambles, 1, 16) : 1;
  for (int c = count - 1; c >= 0; --c) {
    if (deep)
      for (int d : kDeepPreambleDibits) appendWale(s, d, false);
    else
      for (int d : kFastPreambleDibits) appendWale(s, d, false);
    appendWale(s, deep ? 0 : 1, true);
    appendWale(s, more, true);
    appendWale(s, deep ? (c >> 2) & 3 : 0, true);
    appendWale(s, deep ? c & 3 : 0, true);
  }
  auto probe = [&] {
    for (int r = 0; r < 2; ++r) s.insert(s.end(), kFastProbe, kFastProbe + 16);
  };
  if (deep) {
    DeepScrambler scr;
    for (const WalePdu& pdu : pdus) {
      std::vector<std::uint8_t> a = waleBlock(pdu);
      for (int g = 0; g < 48; ++g) {
        int v = (a[static_cast<std::size_t>(4 * g)] << 3) | (a[static_cast<std::size_t>(4 * g + 1)] << 2) |
                (a[static_cast<std::size_t>(4 * g + 2)] << 1) | a[static_cast<std::size_t>(4 * g + 3)];
        for (int j = 0; j < 64; ++j) s.push_back((walshChip(v, j & 15) + scr.next()) & 7);
      }
    }
  } else {
    probe();
    for (const WalePdu& pdu : pdus) {
      std::vector<std::uint8_t> a = waleBlock(pdu);
      for (int block = 0; block < 2; ++block) {
        FastScrambler scr;  // restarted with every data frame
        for (int i = 0; i < 96; ++i) s.push_back(((a[static_cast<std::size_t>(96 * block + i)] ? 4 : 0) + scr.next()) & 7);
        probe();
      }
    }
  }
  return s;
}

AleSignal synth2g(const std::vector<std::uint32_t>& words, const AleChannel& ch) {
  std::vector<int> tones;
  for (std::uint32_t w : words) {
    std::uint8_t t[49];
    redundantWordTones(w, t);
    tones.insert(tones.end(), t, t + 49);
  }
  return channel(fskBaseband(tones, ch), ch);
}

AleSignal synthBw0(const Bits& pdu, const AleChannel& ch) { return channel(pskBaseband(bw0Symbols(pdu), ch), ch); }
AleSignal synthBw1(const Bits& pdu, const AleChannel& ch) { return channel(pskBaseband(bw1Symbols(pdu), ch), ch); }
AleSignal synthWale(bool deep, const std::vector<WalePdu>& pdus, const AleChannel& ch, int preambles) {
  return channel(pskBaseband(waleSymbols(deep, pdus, preambles), ch), ch);
}

std::vector<std::uint32_t> ale2gAddress(int first, const std::string& address) {
  std::string a = address.substr(0, 15);
  while (a.empty() || a.size() % 3) a += '@';
  std::vector<std::uint32_t> w;
  for (std::size_t i = 0; i < a.size(); i += 3) {
    int p = i == 0 ? first : ((i / 3) % 2 == 1 ? kData : kRep);
    w.push_back(makeWord(p, a[i], a[i + 1], a[i + 2]));
  }
  return w;
}

std::vector<std::uint32_t> ale2gCall(const std::string& to, const std::string& from, int scanWords,
                                     const std::string& amd, bool twas) {
  std::vector<std::uint32_t> w;
  std::vector<std::uint32_t> toWords = ale2gAddress(kTo, to);
  for (int i = 0; i < scanWords; ++i) w.push_back(toWords[0]);
  w.insert(w.end(), toWords.begin(), toWords.end());
  if (!amd.empty()) {
    std::string m;
    for (char c : amd) m += static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    while (m.size() % 3) m += ' ';
    for (std::size_t i = 0; i < m.size(); i += 3) {
      int p = i == 0 ? kCmd : ((i / 3) % 2 == 1 ? kData : kRep);
      w.push_back(makeWord(p, m[i], m[i + 1], m[i + 2]));
    }
  }
  std::vector<std::uint32_t> fromWords = ale2gAddress(twas ? kTwas : kTis, from);
  w.insert(w.end(), fromWords.begin(), fromWords.end());
  return w;
}

std::vector<std::uint32_t> ale2gSound(const std::string& from, int repeats, bool tis) {
  std::vector<std::uint32_t> a = ale2gAddress(tis ? kTis : kTwas, from), w;
  for (int i = 0; i < std::max(1, repeats) - 1; ++i) w.push_back(a[0]);
  w.insert(w.end(), a.begin(), a.end());
  return w;
}

AleSignal concatenate(const std::vector<AleSignal>& parts) {
  AleSignal s;
  for (const AleSignal& p : parts) s.samples.insert(s.samples.end(), p.samples.begin(), p.samples.end());
  if (!parts.empty()) {
    s.start = parts.front().start;
    s.end = s.start;
  }
  return s;
}

}  // namespace navtex::ale
