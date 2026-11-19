#include "navtex/ale_psk.hpp"

#include <algorithm>
#include <cmath>

namespace navtex::ale {

namespace {

constexpr double kSymbolRate = 2400.0;
constexpr int kSegment = 16;     // detection segment, symbols
constexpr int kMinDelay = -3, kMaxDelay = 12;
constexpr int kMaxTaps = 4;
constexpr int kMaxPdus = 32;
constexpr double kReportFailedScore = 0.5;  // failures are reported above this preamble score

cf psk(int symbol) {
  static const cf kTable[8] = {
      {1.0f, 0.0f},  {0.70710678f, 0.70710678f},   {0.0f, 1.0f},  {-0.70710678f, 0.70710678f},
      {-1.0f, 0.0f}, {-0.70710678f, -0.70710678f}, {0.0f, -1.0f}, {0.70710678f, -0.70710678f}};
  return kTable[symbol & 7];
}

void wht16(cf v[16]) {
  for (int len = 1; len < 16; len <<= 1)
    for (int i = 0; i < 16; i += 2 * len)
      for (int j = i; j < i + len; ++j) {
        cf a = v[j], b = v[j + len];
        v[j] = a + b;
        v[j + len] = a - b;
      }
}

int fixedLength(PskWaveform w) {
  switch (w) {
    case PskWaveform::Bw0: return 384;
    case PskWaveform::Bw1: return 576;
    case PskWaveform::DeepWale: return 448;
    case PskWaveform::FastWale: return 160;
  }
  return 0;
}

}  // namespace

const char* waveformName(PskWaveform w) {
  switch (w) {
    case PskWaveform::Bw0: return "BW0";
    case PskWaveform::Bw1: return "BW1";
    case PskWaveform::DeepWale: return "Deep WALE";
    case PskWaveform::FastWale: return "Fast WALE";
  }
  return "?";
}

// One burst being received.
struct AlePskReceiver::Job {
  PskWaveform wf = PskWaveform::Bw0;
  long long tau = 0;  // sample index of preamble symbol 0
  double freq = 0.0;  // carrier offset, Hz
  double score = 0.0;
  double snrDb = 0.0;
  int taps = 0;
  int delay[kMaxTaps] = {};
  cf h[kMaxTaps];
  int maxDelay = 0;

  bool exceptionalDone = false;
  bool confirmed = false;  // WALE: the exceptional symbols fit
  bool preambleMore = false;
  long long dataStart = 0;  // symbol index of the first data symbol
  int pdu = 0;
  int unit = 0;  // Walsh symbols (or Fast WALE blocks) done in this PDU
  std::vector<float> soft;
  DeepScrambler deep;
  std::vector<int> fastSeq;   // Fast WALE scrambling symbols from the start of the data
  int fastHypothesis = -1;    // scrambler re-initialisation found to work
  std::vector<cf> hBefore;    // channel at the probe before the current block
  bool haveBefore = false;
  cf z[192];
  std::string text;
  bool textOpen = false;
  bool failedOnce = false;
  bool done = false;
  float qualitySum = 0;
};

AlePskReceiver::AlePskReceiver() : ring_(kRing) {
  auto addRef = [&](PskWaveform w, const std::vector<int>& symbols) {
    Ref r;
    r.waveform = w;
    r.length = static_cast<int>(symbols.size());
    for (int s : symbols) r.conj.push_back(std::conj(psk(s)));
    // Noise gives 1/16 on average, with a standard deviation of 1/16 over
    // the square root of the number of segments: the threshold is nine
    // standard deviations above that.
    const double segments = std::ceil(r.length / static_cast<double>(kSegment));
    r.threshold = (1.0 + 9.0 / std::sqrt(segments)) / kSegment;
    // Autocorrelation, to tell the channel's paths from the sidelobes of the
    // preamble (the WALE preambles have one of -12 dB at 4 symbols).
    r.autocorr.resize(2 * kLags + 1);
    for (int l = -kLags; l <= kLags; ++l) {
      cf a = 0.0f;
      for (int i = std::max(0, -l); i < r.length && i + l < r.length; ++i)
        a += std::conj(r.conj[static_cast<std::size_t>(i + l)]) * r.conj[static_cast<std::size_t>(i)];
      r.autocorr[static_cast<std::size_t>(l + kLags)] = a / static_cast<float>(r.length);
    }
    refs_.push_back(std::move(r));
  };
  addRef(PskWaveform::Bw0, std::vector<int>(kBw0Preamble, kBw0Preamble + 384));
  addRef(PskWaveform::Bw1, std::vector<int>(kBw1Preamble, kBw1Preamble + 576));
  auto wale = [](const std::uint8_t* dibits, int n) {
    std::vector<int> s;
    for (int d = 0; d < n; ++d)
      for (int j = 0; j < 32; ++j) s.push_back((walePreambleChip(dibits[d], false, j) + kWalePreambleScramble[j]) & 7);
    return s;
  };
  addRef(PskWaveform::DeepWale, wale(kDeepPreambleDibits, 14));
  addRef(PskWaveform::FastWale, wale(kFastPreambleDibits, 5));
  reset();
}

AlePskReceiver::~AlePskReceiver() = default;

void AlePskReceiver::reset() {
  std::fill(ring_.begin(), ring_.end(), cf());
  n_ = -1;
  time_ = 0.0;
  for (Ref& r : refs_) {
    r.peak = 0;
    r.peakAt = -1;
    r.since = 0;
  }
  job3g_.reset();
  job4g_.reset();
  lastDetection_ = -1e9;
}

void AlePskReceiver::enable(bool g3, bool g4) {
  g3_ = g3;
  g4_ = g4;
  if (!g3) job3g_.reset();
  if (!g4) job4g_.reset();
}

void AlePskReceiver::process(cf y, double time) {
  ++n_;
  time_ = time;
  ring_[static_cast<std::size_t>(n_) & kMask] = y;
  if ((n_ & 1) == 0) detect();
  runJob();
}

double AlePskReceiver::correlate(const Ref& r, long long end, int segment, std::vector<cf>* segs) const {
  // Each segment's correlation power over its energy, averaged: a stretch
  // without the signal (silence, or a shorter preamble) counts as noise.
  const long long first = end - static_cast<long long>(kSps) * (r.length - 1);
  if (first < 0 || n_ - first >= static_cast<long long>(kRing)) return 0.0;
  double total = 0.0;
  int count = 0;
  if (segs) segs->clear();
  for (int k = 0; k * segment < r.length; ++k) {
    float re = 0.0f, im = 0.0f, energy = 0.0f;
    const int i0 = k * segment, i1 = std::min(r.length, (k + 1) * segment);
    for (int i = i0; i < i1; ++i) {
      cf v = at(first + kSps * i);
      const cf& c = r.conj[static_cast<std::size_t>(i)];
      re += v.real() * c.real() - v.imag() * c.imag();
      im += v.real() * c.imag() + v.imag() * c.real();
      energy += v.real() * v.real() + v.imag() * v.imag();
    }
    if (energy > 0.0f) total += (static_cast<double>(re) * re + static_cast<double>(im) * im) / ((i1 - i0) * static_cast<double>(energy));
    ++count;
    if (segs) segs->push_back(cf(re, im));
  }
  return count ? total / count : 0.0;
}

void AlePskReceiver::detect() {
  for (Ref& r : refs_) {
    const bool is3g = r.waveform == PskWaveform::Bw0 || r.waveform == PskWaveform::Bw1;
    if (is3g ? !g3_ : !g4_) {
      r.peakAt = -1;
      continue;
    }
    // A burst being received blocks other detections of its generation,
    // unless they are stronger (the burst was a false alarm); once a WALE
    // preamble has been confirmed by its last symbols, much stronger.
    const std::unique_ptr<Job>& busy = is3g ? job3g_ : job4g_;
    const double threshold = busy ? std::max(r.threshold, busy->score * (busy->confirmed ? 2.0 : 1.1)) : r.threshold;
    double rho = correlate(r, n_, kSegment, nullptr);
    if (rho > threshold && rho > r.peak) {
      r.peak = rho;
      r.peakAt = n_;
      r.since = 0;
    } else if (r.peakAt >= 0 && ++r.since > 16) {
      start(r);
      r.peak = 0;
      r.peakAt = -1;
    }
  }
}

void AlePskReceiver::start(Ref& detected) {
  auto job = std::make_unique<Job>();
  Job& j = *job;
  // Timing to a quarter symbol.
  long long bestEnd = detected.peakAt;
  double best = -1.0;
  std::vector<cf> segs, bestSegs;
  for (long long e = detected.peakAt - 2; e <= detected.peakAt + 2; ++e) {
    double rho = correlate(detected, e, kSegment, &segs);
    if (rho > best) {
      best = rho;
      bestEnd = e;
      bestSegs = segs;
    }
  }
  // The end of the Deep WALE preamble is the whole Fast WALE preamble, so
  // both detect either; the longer one fits a Deep WALE preamble about as
  // well as the shorter, and a Fast one much worse.
  const Ref* use = &detected;
  if (detected.waveform == PskWaveform::DeepWale || detected.waveform == PskWaveform::FastWale) {
    const Ref& deep = refs_[2];
    const Ref& fast = refs_[3];
    std::vector<cf> sd, sf;
    double rd = correlate(deep, bestEnd, kSegment, &sd), rf = correlate(fast, bestEnd, kSegment, &sf);
    use = rd >= 0.75 * rf ? &deep : &fast;
    best = use == &deep ? rd : rf;
    bestSegs = use == &deep ? sd : sf;
  }
  const Ref& r = *use;
  j.wf = r.waveform;
  const int len = r.length;
  j.score = best;
  j.tau = bestEnd - static_cast<long long>(kSps) * (len - 1);
  const bool is3g = j.wf == PskWaveform::Bw0 || j.wf == PskWaveform::Bw1;
  const std::unique_ptr<Job>& running = is3g ? job3g_ : job4g_;
  if (running && best < running->score * (running->confirmed ? 2.0 : 1.1)) return;
  lastDetection_ = time_;

  // Carrier offset: phase advance between 16-symbol segments, then between
  // 64-symbol segments after that correction.
  std::complex<double> z = 0.0;
  for (std::size_t k = 1; k < bestSegs.size(); ++k)
    z += std::complex<double>(bestSegs[k].real(), bestSegs[k].imag()) *
         std::conj(std::complex<double>(bestSegs[k - 1].real(), bestSegs[k - 1].imag()));
  double f = std::arg(z) / (2.0 * kPi * kSegment / kSymbolRate);
  auto sampleAt = [&](long long k, double freq) {
    double ph = -2.0 * kPi * freq * static_cast<double>(k) / kSymbolRate;
    return at(j.tau + kSps * k) * cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
  };
  if (len >= 128) {
    std::vector<std::complex<double>> c;
    for (int k = 0; (k + 1) * 64 <= len; ++k) {
      std::complex<double> s = 0.0;
      for (int i = 64 * k; i < 64 * (k + 1); ++i) {
        cf v = sampleAt(i, f) * r.conj[static_cast<std::size_t>(i)];
        s += std::complex<double>(v.real(), v.imag());
      }
      c.push_back(s);
    }
    std::complex<double> z2 = 0.0;
    for (std::size_t k = 1; k < c.size(); ++k) z2 += c[k] * std::conj(c[k - 1]);
    f += std::arg(z2) / (2.0 * kPi * 64.0 / kSymbolRate);
  }
  j.freq = f;

  // Channel paths.
  cf hAll[kMaxDelay - kMinDelay + 1];
  double strongest = 0.0, total = 0.0;
  for (int d = kMinDelay; d <= kMaxDelay; ++d) {
    std::complex<double> s = 0.0;
    for (int i = 0; i < len; ++i) {
      cf v = sampleAt(i + d, f) * r.conj[static_cast<std::size_t>(i)];
      s += std::complex<double>(v.real(), v.imag());
    }
    s /= static_cast<double>(len);
    hAll[d - kMinDelay] = cf(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    strongest = std::max(strongest, std::norm(s));
  }
  double power = 0.0;
  for (int i = 0; i < len; ++i) power += std::norm(at(j.tau + kSps * i));
  power /= len;
  // The strongest paths, each one's sidelobes removed before looking for the next.
  for (int pass = 0; pass < kMaxTaps; ++pass) {
    int best = kMinDelay;
    for (int d = kMinDelay; d <= kMaxDelay; ++d)
      if (std::norm(hAll[d - kMinDelay]) > std::norm(hAll[best - kMinDelay])) best = d;
    const cf hb = hAll[best - kMinDelay];
    if (std::norm(hb) < 0.15 * strongest) break;
    j.delay[j.taps] = best;
    j.h[j.taps] = hb;
    j.maxDelay = std::max(j.maxDelay, best);
    total += std::norm(hb);
    ++j.taps;
    for (int d = kMinDelay; d <= kMaxDelay; ++d) {
      const int lag = d - best;
      if (lag >= -kLags && lag <= kLags) hAll[d - kMinDelay] -= hb * r.autocorr[static_cast<std::size_t>(lag + kLags)];
    }
  }
  if (j.taps == 0 || strongest <= 0.0) return;
  double noise = std::max(power - total, 0.01 * power);
  j.snrDb = 10.0 * std::log10(total / noise * kSymbolRate / 3000.0);

  switch (j.wf) {
    case PskWaveform::Bw0:
    case PskWaveform::Bw1:
      j.dataStart = len;
      j.exceptionalDone = true;
      job3g_ = std::move(job);
      break;
    case PskWaveform::DeepWale:
    case PskWaveform::FastWale:
      job4g_ = std::move(job);
      break;
  }
}

namespace {

// Derotated sample of symbol k (relative to the preamble start) on path t.
struct View {
  const std::vector<cf>& ring;
  std::size_t mask;
  long long tau;
  double freq;
  cf operator()(long long k) const {
    double ph = -2.0 * kPi * freq * static_cast<double>(k) / kSymbolRate;
    return ring[static_cast<std::size_t>(tau + kSps * k) & mask] *
           cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
  }
};

// One 64-chip Walsh symbol starting at symbol k0, scrambled with scr[0..64):
// soft bits b3 b2 b1 b0 appended to `soft`. Each path is correlated with the
// 16 Walsh sequences over the whole symbol (26.7 ms, coherently), and the
// paths' energies are added, weighted by their power: no carrier phase is
// needed, so fading does not derail it. The path powers follow each decision.
float walshSymbol(const View& y, int taps, const int* delay, cf* h, long long k0, const int* scr,
                  std::vector<float>& soft) {
  float metric[16] = {};
  cf corr[kMaxTaps][16];
  float weight = 0.0f;
  for (int t = 0; t < taps; ++t) weight += std::norm(h[t]);
  weight = weight > 0.0f ? 1.0f / weight : 1.0f;
  for (int t = 0; t < taps; ++t) {
    cf* c = corr[t];
    for (int m = 0; m < 16; ++m) c[m] = 0.0f;
    for (int j = 0; j < 64; ++j) c[j & 15] += y(k0 + j + delay[t]) * std::conj(psk(scr[j]));
    wht16(c);
    const float w = std::norm(h[t]) * weight;
    for (int v = 0; v < 16; ++v) metric[v] += w * std::norm(c[v]);
  }
  float total = 0.0f, best = -1.0f;
  int vhat = 0;
  for (int v = 0; v < 16; ++v) {
    total += metric[v];
    if (metric[v] > best) {
      best = metric[v];
      vhat = v;
    }
  }
  const float noise = (total - best) / 15.0f + 1e-20f;
  for (int b = 3; b >= 0; --b) {
    float m1 = 0.0f, m0 = 0.0f;
    for (int v = 0; v < 16; ++v) {
      if ((v >> b) & 1) m1 = std::max(m1, metric[v]);
      else m0 = std::max(m0, metric[v]);
    }
    soft.push_back((m1 - m0) / noise);
  }
  // Path gains from the decided symbol.
  for (int t = 0; t < taps; ++t) h[t] = 0.7f * h[t] + 0.3f * (corr[t][vhat] / 64.0f);
  return best / (best + 15.0f * noise);
}

// Channel on a Fast WALE probe block at symbol k0.
void probeChannel(const View& y, int taps, const int* delay, long long k0, cf* out) {
  for (int t = 0; t < taps; ++t) {
    cf s = 0.0f;
    for (int j = 0; j < 32; ++j) s += y(k0 + j + delay[t]) * std::conj(psk(kFastProbe[j & 15]));
    out[t] = s / 32.0f;
  }
}

}  // namespace

void AlePskReceiver::runJob() {
  for (int g = 0; g < 2; ++g) {
    std::unique_ptr<Job>& slot = g == 0 ? job3g_ : job4g_;
    while (slot && !slot->done) {
      Job& j = *slot;
      const View y{ring_, kMask, j.tau, j.freq};
      auto available = [&](long long lastSymbol) { return j.tau + kSps * (lastSymbol + j.maxDelay) <= n_; };
      auto burst = [&](bool ok) {
        PskBurst b;
        b.waveform = j.wf;
        b.time = time_;
        b.ok = ok;
        b.snrDb = j.snrDb;
        b.offsetHz = j.freq;
        b.preambleScore = j.score;
        b.index = j.pdu;
        return b;
      };
      auto emit = [&](const PskBurst& b) {
        if (listener_ && (b.ok || b.preambleScore >= kReportFailedScore)) listener_->onBurst(b);
      };

      // WALE: the four exceptional preamble symbols.
      if (!j.exceptionalDone) {
        const int fixed = fixedLength(j.wf);
        if (!available(fixed + 128)) break;
        int dibit[4];
        for (int e = 0; e < 4; ++e) {
          cf r[32];
          const long long k0 = fixed + 32 * e;
          for (int c = 0; c < 32; ++c) {
            cf acc = 0.0f;
            for (int t = 0; t < j.taps; ++t) acc += std::conj(j.h[t]) * y(k0 + c + j.delay[t]);
            r[c] = acc;
          }
          float best = -1.0f;
          for (int d = 0; d < 4; ++d) {
            cf s = 0.0f;
            for (int c = 0; c < 32; ++c)
              s += r[c] * std::conj(psk(walePreambleChip(d, true, c) + kWalePreambleScramble[c]));
            if (std::norm(s) > best) {
              best = std::norm(s);
              dibit[e] = d;
            }
          }
        }
        // The first exceptional di-bit tells Deep from Fast WALE (the end of
        // the Deep WALE preamble is the whole Fast WALE preamble).
        if (dibit[0] > 1 || dibit[1] > 1) {  // not a WALE preamble after all
          slot.reset();
          break;
        }
        const bool deep = dibit[0] == 0;
        j.wf = deep ? PskWaveform::DeepWale : PskWaveform::FastWale;
        j.confirmed = true;
        j.preambleMore = dibit[1] == 1;
        const int counter = deep ? (dibit[2] << 2) | dibit[3] : 0;
        // Deep: the data after `counter` more preambles; Fast: the first probe.
        j.dataStart = fixed + 128 + 576LL * counter;
        j.exceptionalDone = true;
        if (!deep) {
          FastScrambler s;
          j.fastSeq.resize(192 * kMaxPdus);
          for (int& v : j.fastSeq) v = s.next();
        }
        continue;
      }

      if (j.wf == PskWaveform::Bw0 || j.wf == PskWaveform::Bw1) {
        const int symbols = j.wf == PskWaveform::Bw0 ? 13 : 36;
        while (j.unit < symbols) {
          const long long k0 = j.dataStart + 64LL * j.unit;
          if (!available(k0 + 63)) break;
          int scr[64];
          for (int c = 0; c < 64; ++c) scr[c] = k3gPn[(64 * j.unit + c) % 256];
          j.qualitySum += walshSymbol(y, j.taps, j.delay, j.h, k0, scr, j.soft);
          ++j.unit;
        }
        if (j.unit < symbols) break;
        const std::vector<int>& order = j.wf == PskWaveform::Bw0 ? bw0InterleaverOrder() : bw1InterleaverOrder();
        std::vector<float> coded(order.size());
        for (std::size_t f = 0; f < order.size(); ++f) coded[static_cast<std::size_t>(order[f])] = j.soft[f];
        double metric = 0.0;
        const ConvCode& code = j.wf == PskWaveform::Bw0 ? kBw0Code : kBw1Code;
        Bits bits = viterbi_.decode(code, coded, &metric);
        PskBurst b = burst(false);
        b.bits = bits;
        b.quality = static_cast<float>(std::clamp(metric, 0.0, 1.0));
        const bool intact = codeWordDisagreements(code, bits, coded) == 0;
        if (j.wf == PskWaveform::Bw0) {
          b.ok = leCrcOk(bits) && metric > 0.9;  // only a 4- or 8-bit CRC
          b.description = b.ok       ? describeLe(bits)
                          : intact   ? "3G-ALE PDU received intact but fails its CRC (linking protection, or not to the standard)"
                                     : "3G-ALE PDU not decoded (too weak or corrupted)";
        } else {
          b.ok = tmCrcOk(bits) && metric > 0.9;
          b.description = b.ok     ? describeTm(bits)
                          : intact ? "traffic management PDU received intact but fails its CRC (linking protection, HDL/LDL, or not to the standard)"
                                   : "traffic management PDU not decoded (too weak or corrupted, or HDL/LDL)";
        }
        b.intact = !b.ok && intact;
        emit(b);
        j.done = true;
        break;
      }

      // WALE data, PDU by PDU.
      bool pduReady = false;
      if (j.wf == PskWaveform::DeepWale) {
        while (j.unit < 48) {
          const long long k0 = j.dataStart + 3072LL * j.pdu + 64LL * j.unit;
          if (!available(k0 + 63)) break;
          int scr[64];
          for (int& v : scr) v = j.deep.next();
          j.qualitySum += walshSymbol(y, j.taps, j.delay, j.h, k0, scr, j.soft);
          ++j.unit;
        }
        pduReady = j.unit == 48;
      } else {
        while (j.unit < 2) {
          const long long block = 2LL * j.pdu + j.unit;
          const long long kBefore = j.dataStart + 128 * block, kAfter = kBefore + 128, d0 = kBefore + 32;
          if (!available(kAfter + 31)) break;
          if (!j.haveBefore) {
            j.hBefore.assign(static_cast<std::size_t>(j.taps), cf());
            probeChannel(y, j.taps, j.delay, kBefore, j.hBefore.data());
            j.haveBefore = true;
          }
          std::vector<cf> hAfter(static_cast<std::size_t>(j.taps));
          probeChannel(y, j.taps, j.delay, kAfter, hAfter.data());
          for (int i = 0; i < 96; ++i) {
            float w = (static_cast<float>(i) + 16.0f) / 128.0f;  // from probe centre to probe centre
            cf acc = 0.0f;
            for (int t = 0; t < j.taps; ++t) {
              cf hc = (1.0f - w) * j.hBefore[static_cast<std::size_t>(t)] + w * hAfter[static_cast<std::size_t>(t)];
              acc += std::conj(hc) * y(d0 + i + j.delay[t]);
            }
            j.z[96 * j.unit + i] = acc;
          }
          j.hBefore = hAfter;
          ++j.unit;
        }
        pduReady = j.unit == 2;
      }
      if (!pduReady) break;

      // Deinterleave, decode, check; `disagreements`: received coded bits
      // off the decoded code word.
      auto decodeWale = [&](const std::vector<float>& sent, WalePdu& pdu, double& metric, int& disagreements) {
        std::vector<float> coded(192);
        for (int n = 0; n < 192; ++n) coded[static_cast<std::size_t>(n)] = sent[static_cast<std::size_t>(waleInterleavePos(n))];
        Bits q = viterbi_.decode(kWaleCode, coded, &metric);
        disagreements = codeWordDisagreements(kWaleCode, q, coded);
        pdu.fill(0);
        for (int i = 0; i < 96; ++i)
          if (q[static_cast<std::size_t>(i)]) {
            int p = (i + 8) % 96;
            pdu[static_cast<std::size_t>(p / 8)] |= static_cast<std::uint8_t>(1u << (p % 8));
          }
        return waleCrcOk(pdu) && metric > 0.2;
      };
      WalePdu pdu{};
      double metric = 0.0;
      int disagreements = 192;
      bool ok = false;
      if (j.wf == PskWaveform::DeepWale) {
        ok = decodeWale(j.soft, pdu, metric, disagreements);
      } else {
        // When no hypothesis gives a valid CRC, the one closest to a code
        // word is reported.
        WalePdu best{};
        double bestMetric = 0.0;
        int bestDisagreements = 193;
        // The scrambler restarts with every data block, every PDU, or once
        // per transmission (G.5.1.8.3.2 says "each data frame"): try each.
        for (int hyp = 0; hyp < 3 && !ok; ++hyp) {
          if (j.fastHypothesis >= 0 && hyp != j.fastHypothesis) continue;
          std::vector<float> sent(192);
          for (int i = 0; i < 192; ++i) {
            int s = hyp == 0 ? j.fastSeq[static_cast<std::size_t>(i % 96)]
                    : hyp == 1 ? j.fastSeq[static_cast<std::size_t>(i)]
                               : j.fastSeq[static_cast<std::size_t>(192 * j.pdu + i)];
            sent[static_cast<std::size_t>(i)] = -(j.z[i] * std::conj(psk(s))).real();
          }
          float scale = 0.0f;
          for (float v : sent) scale += std::fabs(v);
          scale = scale > 0.0f ? 192.0f / scale : 1.0f;
          for (float& v : sent) v *= 2.0f * scale;
          ok = decodeWale(sent, pdu, metric, disagreements);
          if (ok) j.fastHypothesis = hyp;
          if (!ok && disagreements < bestDisagreements) {
            best = pdu;
            bestMetric = metric;
            bestDisagreements = disagreements;
          }
        }
        if (!ok) {
          pdu = best;
          metric = bestMetric;
          disagreements = bestDisagreements;
        }
      }
      PskBurst b = burst(ok);
      b.pdu = pdu;
      b.quality = static_cast<float>(std::clamp(metric, 0.0, 1.0));
      bool more;
      if (ok) {
        more = waleMoreFollow(pdu);
        if (waleIsText(pdu)) {
          bool som, eom;
          std::string t = waleTextOf(pdu, som, eom);
          if (som) j.text.clear();
          j.text += t;
          j.textOpen = !eom;
          if (eom) {
            b.text = true;
            b.description = "Text message \"" + j.text + "\"";
            emit(b);
            j.text.clear();
          }
        } else {
          b.description = describeWale(pdu);
          emit(b);
        }
      } else {
        b.intact = disagreements == 0;
        b.description = b.intact ? "WALE PDU received intact but fails its CRC (encrypted: linking protection, or not to the standard)"
                                 : "WALE PDU not decoded (too weak or corrupted)";
        emit(b);
        more = !j.failedOnce && (j.pdu == 0 ? j.preambleMore : true);
        j.failedOnce = true;
      }
      ++j.pdu;
      j.unit = 0;
      j.soft.clear();
      if (!more || j.pdu >= kMaxPdus) {
        if (j.textOpen && !j.text.empty()) {
          PskBurst t = burst(true);
          t.text = true;
          t.description = "Text message \"" + j.text + "\" (incomplete)";
          emit(t);
        }
        j.done = true;
      }
    }
    if (slot && slot->done) slot.reset();
  }
}

}  // namespace navtex::ale
