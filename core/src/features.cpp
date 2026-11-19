#include "hfc/features.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <tuple>

#include "hfc/dsp.hpp"
#include "hfc/json.hpp"

namespace hfc {

namespace {

double median(std::vector<double> v) {
  if (v.empty()) return 0;
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  const auto k = static_cast<std::size_t>(std::clamp(p, 0.0, 1.0) * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return v[k];
}

std::size_t nextFftSize(std::size_t n) {
  std::size_t m = 1;
  while (m < n) m <<= 1;
  return m;
}

// Averaged power spectrum (Hann, 50 % overlap) of real audio.
std::vector<double> welch(const std::vector<float>& a, int n) {
  std::vector<double> p(static_cast<std::size_t>(n / 2 + 1), 0.0);
  if (a.size() < static_cast<std::size_t>(n)) return p;
  const auto plan = sharedFft(n, false);
  const Fft& fft = *plan;
  std::vector<cfloat> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
  std::vector<float> w(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) w[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / n));
  int segs = 0;
  for (std::size_t s = 0; s + static_cast<std::size_t>(n) <= a.size(); s += static_cast<std::size_t>(n / 2), ++segs) {
    for (int i = 0; i < n; ++i) in[static_cast<std::size_t>(i)] = cfloat(a[s + static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0);
    fft.run(in.data(), out.data());
    for (std::size_t k = 0; k < p.size(); ++k) p[k] += std::norm(out[k]);
  }
  for (double& v : p) v /= std::max(1, segs);
  return p;
}

// The strongest line in the spectrum of `x` between fLo and fHi (Hz, at
// `rate`), as {frequency, dB above the median of the spectrum around it}.
// Prefers the fundamental: when half the frequency also has a clear line,
// that is taken. `combHz`: when the signal repeats itself every 1 / combHz
// (a modem's frame), its spectrum has a line at every multiple of that; a
// line's strength is then also measured against the six on either side,
// 2 dB more leniently (so 15 dB means 13 dB above them: a symbol rate's
// line stood 13 to 22 dB above them on the wiki's serial-modem samples, the
// frame's own lines mostly less, at most 12.8 dB).
std::pair<double, double> strongestLine(const std::vector<double>& x, double rate, double fLo, double fHi,
                                        double combHz = 0) {
  if (x.size() < 64) return {0, 0};
  const std::size_t n = nextFftSize(std::max<std::size_t>(x.size(), 1 << 15));
  const auto plan = sharedFft(static_cast<int>(n), false);
  const Fft& fft = *plan;
  std::vector<cfloat> in(n, cfloat(0, 0)), out(n);
  const double mean = std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(x.size() - 1));
    in[i] = cfloat(static_cast<float>((x[i] - mean) * w), 0);
  }
  fft.run(in.data(), out.data());
  const double df = rate / static_cast<double>(n);
  const auto k0 = static_cast<std::size_t>(std::max(1.0, fLo / df));
  const auto k1 = std::min(n / 2 - 1, static_cast<std::size_t>(fHi / df));
  if (k1 <= k0 + 4) return {0, 0};
  std::vector<double> p(n / 2);
  for (std::size_t k = 0; k < p.size(); ++k) p[k] = std::norm(out[k]);
  // The level around a frequency: the median over +-5 Hz (a line is about
  // 1 / duration wide).
  const auto half = static_cast<std::size_t>(std::max(8.0, 5.0 / df));
  const double comb = combHz / df;  // in bins
  // A line's strength: its power over that level, or over the median of
  // the frame's lines on either side (six, from 5 Hz away: a real line is
  // not quite sharp, with clock drift or a resampled recording) less 2 dB,
  // whichever is the higher.
  auto ratio = [&](std::size_t k) {
    const std::size_t a = k > half ? k - half : 1, b = std::min(p.size() - 1, k + half);
    std::vector<double> v;
    v.reserve(b - a);
    for (std::size_t i = a; i < b; i += std::max<std::size_t>(1, (b - a) / 64)) v.push_back(p[i]);
    double level = median(v);
    if (comb >= 4) {
      std::vector<double> teeth;
      const int first = static_cast<int>(std::ceil(std::max(1.0, static_cast<double>(half) / comb)));
      for (int side : {-1, 1})
        for (int j = first; j < first + 6; ++j) {
          const double at = static_cast<double>(k) + side * j * comb;
          if (at < 2 || at + 2 >= static_cast<double>(p.size())) continue;
          const auto c = static_cast<std::size_t>(std::lround(at));
          teeth.push_back(std::max({p[c - 1], p[c], p[c + 1]}));
        }
      if (teeth.size() >= 6) level = std::max(level, median(teeth) / 1.585);
    }
    return p[k] / std::max(1e-30, level);
  };
  // The strongest bin, on a grid coarse enough to be quick.
  std::size_t best = 0;
  double bestRatio = 0;
  for (std::size_t k = k0; k <= k1; ++k) {
    if (p[k] < p[k - 1] || p[k] < p[k + 1]) continue;
    const double r = ratio(k);
    if (r > bestRatio) bestRatio = r, best = k;
  }
  if (!best) return {0, 0};
  auto lineAt = [&](double f) {  // the strongest ratio within +-1.5 % of f
    const auto a = static_cast<std::size_t>(std::max(1.0, f * 0.985 / df));
    const auto b = std::min(p.size() - 2, static_cast<std::size_t>(f * 1.015 / df) + 1);
    std::size_t at = a;
    double r = 0;
    for (std::size_t k = a; k <= b; ++k) {
      const double q = ratio(k);
      if (q > r) r = q, at = k;
    }
    return std::make_pair(at, r);
  };
  // The fundamental: the lowest of f/4, f/3, f/2 with a clear line (the
  // change signal of FSK often has a stronger third harmonic).
  const double strongest = bestRatio;
  for (bool lower = true; lower;) {
    lower = false;
    const double f = static_cast<double>(best) * df;
    for (int div = 4; div >= 2; --div) {
      if (f / div < fLo) continue;
      const auto [at, r] = lineAt(f / div);
      // A clear line itself (15 dB, which noise alone hardly reaches in so
      // small a window), within 15 dB of the strongest.
      if (r >= 31.6 && r >= strongest / 31.6) {
        best = at;
        bestRatio = r;
        lower = true;
        break;
      }
    }
  }
  // Parabolic interpolation on the log power.
  double f = static_cast<double>(best) * df;
  if (best > 0 && best + 1 < p.size()) {
    const double l = std::log(p[best - 1] + 1e-30), c = std::log(p[best] + 1e-30), r = std::log(p[best + 1] + 1e-30);
    const double d = l - 2 * c + r;
    if (d < 0) f += 0.5 * (l - r) / d * df;
  }
  return {f, 10 * std::log10(bestRatio)};
}

// The period at which a complex baseband signal (at `rate`, about
// `bandwidth` wide) repeats itself, between 10 and 650 ms: {period in ms,
// |R(T)| / R(0) there}, or {0, 0}. R is the autocorrelation (by FFT), each
// lag normalised by the energy of the two stretches it compares, so that
// gaps in the signal do not count against it.
std::pair<double, double> repetition(const std::vector<std::complex<double>>& z, double rate, double bandwidth) {
  const std::size_t n = z.size();
  const double secs = static_cast<double>(n) / rate;
  // The repeat at twice the period is looked for too, over at least 1.5 s.
  const double maxPeriod = std::min(0.650, (secs - 1.5) / 2);
  if (maxPeriod < 0.020) return {0, 0};
  const auto lag0 = static_cast<std::size_t>(std::ceil(0.010 * rate));
  const auto lag1 = static_cast<std::size_t>(maxPeriod * rate);
  const std::size_t maxLag = 2 * lag1 + lag1 / 100 + 4;
  const std::size_t nfft = nextFftSize(n + maxLag + 1);
  const auto fwdPlan = sharedFft(static_cast<int>(nfft), false), invPlan = sharedFft(static_cast<int>(nfft), true);
  const Fft &fwd = *fwdPlan, &inv = *invPlan;
  std::vector<cfloat> a(nfft, cfloat(0, 0)), b(nfft);
  for (std::size_t i = 0; i < n; ++i) a[i] = cfloat(static_cast<float>(z[i].real()), static_cast<float>(z[i].imag()));
  fwd.run(a.data(), b.data());
  for (auto& v : b) v = cfloat(std::norm(v), 0);
  inv.run(b.data(), a.data());
  std::vector<double> energy(n + 1, 0.0);  // energy[i]: of z[0..i)
  for (std::size_t i = 0; i < n; ++i) energy[i + 1] = energy[i] + std::norm(z[i]);
  if (energy[n] <= 0) return {0, 0};
  std::vector<double> rho(maxLag + 1, 0.0);
  for (std::size_t L = 1; L <= maxLag; ++L) {
    const double e = energy[n - L] * (energy[n] - energy[L]);
    rho[L] = e > 0 ? std::abs(a[L]) / static_cast<double>(nfft) / std::sqrt(e) : 0;
  }
  // Thresholds. Random data: |R| / R(0) about 1 / sqrt(bandwidth x
  // duration), its largest over the lags searched about 3 / sqrt(...).
  std::vector<double> range(rho.begin() + static_cast<std::ptrdiff_t>(lag0), rho.begin() + static_cast<std::ptrdiff_t>(lag1) + 1);
  const double background = median(range);
  const double floorAbs = std::max(0.05, 8.0 / std::sqrt(std::max(1.0, bandwidth * secs)));
  auto localMedian = [&](std::size_t L) {  // over 0.75 L .. 1.25 L
    const std::size_t a0 = std::max<std::size_t>(1, L - L / 4), a1 = std::min(maxLag, L + L / 4);
    return median(std::vector<double>(rho.begin() + static_cast<std::ptrdiff_t>(a0), rho.begin() + static_cast<std::ptrdiff_t>(a1) + 1));
  };
  auto bestNear = [&](double at, std::size_t& where) {  // the largest within +-0.4 % (at least 2 lags)
    const auto w = static_cast<std::size_t>(std::max(2.0, 0.004 * at));
    const auto c = static_cast<std::size_t>(std::lround(at));
    double r = 0;
    for (std::size_t L = c > w ? c - w : 1; L <= std::min(maxLag, c + w); ++L)
      if (rho[L] > r) r = rho[L], where = L;
    return r;
  };
  // Peaks: local maxima above the thresholds, 8 times the median of all the
  // lags searched and 4 times that of the lags around them (a carrier, two
  // FSK tones' beat and MFSK's symbols correlate broadly, not at one lag).
  std::vector<std::size_t> peaks;
  for (std::size_t L = lag0; L <= lag1; ++L)
    if (rho[L] > rho[L - 1] && rho[L] >= rho[L + 1] && rho[L] >= floorAbs && rho[L] >= 8 * background) peaks.push_back(L);
  std::sort(peaks.begin(), peaks.end(), [&](std::size_t x, std::size_t y) { return rho[x] > rho[y]; });
  if (peaks.size() > 32) peaks.resize(32);
  std::vector<std::size_t> kept;
  for (std::size_t L : peaks)
    if (rho[L] >= 4 * localMedian(L)) kept.push_back(L);
  if (kept.empty()) return {0, 0};
  double top = 0;
  for (std::size_t L : kept) top = std::max(top, rho[L]);
  std::sort(kept.begin(), kept.end());
  // The fundamental: the shortest period within 6 dB of the strongest that
  // repeats at twice itself (a frame structure repeats every frame; a single
  // echo, such as a cyclic prefix, does not).
  for (std::size_t L : kept) {
    if (rho[L] < 0.5 * top) continue;
    std::size_t at2 = 0;
    if (bestNear(2.0 * static_cast<double>(L), at2) < 0.5 * rho[L]) continue;
    // A period under the range shows at half this one: not ours to name.
    std::size_t half = 0;
    if (L < 2 * lag0 && bestNear(0.5 * static_cast<double>(L), half) >= 0.5 * rho[L] && rho[half] >= 4 * localMedian(half))
      return {0, 0};
    const double l = rho[L - 1], c = rho[L], r = rho[L + 1], d = l - 2 * c + r;
    const double frac = d < 0 ? 0.5 * (l - r) / d : 0;
    return {1000.0 * (static_cast<double>(L) + frac) / rate, c};
  }
  return {0, 0};
}

}  // namespace

std::string EmissionFeatures::summary() const {
  if (!valid) return "";
  char b[160];
  auto baud = [&] {
    if (symbolRate <= 0) return std::string();
    char s[32];
    std::snprintf(s, sizeof s, symbolRate < 100 ? ", %.1f Bd" : ", %.0f Bd", symbolRate);
    return std::string(s);
  };
  if (modulation == "FSK") std::snprintf(b, sizeof b, "2-FSK, shift %.0f Hz%s", toneSpacingHz, baud().c_str());
  else if (modulation == "MFSK") std::snprintf(b, sizeof b, "MFSK, %d tones %.0f Hz apart%s", tones, toneSpacingHz, baud().c_str());
  else if (modulation == "multi-carrier") std::snprintf(b, sizeof b, "multi-carrier, %d tones %.0f Hz apart%s", tones, toneSpacingHz, baud().c_str());
  else if (modulation == "PSK") std::snprintf(b, sizeof b, "PSK%s", baud().c_str());
  else if (modulation == "OOK") std::snprintf(b, sizeof b, "OOK (keyed carrier)");
  else if (modulation == "carrier") std::snprintf(b, sizeof b, "carrier");
  else if (tones > 0) std::snprintf(b, sizeof b, "%d tones%s", tones, baud().c_str());
  else std::snprintf(b, sizeof b, "%s", symbolRate > 0 ? baud().c_str() + 2 : "");
  std::string out = b;
  if (frameMs > 0) {
    std::snprintf(b, sizeof b, "%sframe %.1f ms", out.empty() ? "" : ", ", frameMs);
    out += b;
  }
  return out;
}

std::string EmissionFeatures::json() const {
  // One line: it goes into WebSocket messages and a database column.
  char b[320];
  std::snprintf(b, sizeof b,
                "{\"modulation\":%s,\"tones\":%d,\"spacing_hz\":%.1f,\"symbol_rate\":%.2f,\"symbol_rate_db\":%.1f,"
                "\"envelope_cv\":%.2f,\"frame_ms\":%.2f,\"frame_strength\":%.3f,\"seconds\":%.1f",
                jsonQuote(modulation).c_str(), tones, toneSpacingHz, symbolRate, symbolRateDb, envelopeCv, frameMs,
                frameStrength, seconds);
  std::string out = b;
  out += ",\"summary\":" + jsonQuote(summary());
  out += ",\"consistent\":[";
  for (std::size_t i = 0; i < consistent.size(); ++i) {
    if (i) out += ",";
    out += "{\"mode\":" + jsonQuote(consistent[i].mode) + ",\"provenance\":" + jsonQuote(consistent[i].provenance) +
           ",\"partial\":" + (consistent[i].partial ? "true" : "false") +
           ",\"frame\":" + (consistent[i].frame ? "true" : "false") +
           ",\"listed\":" + (consistent[i].listed ? "true" : "false") + "}";
  }
  out += "]";
  if (!predicted.empty()) {
    char c[32];
    std::snprintf(c, sizeof c, "%.2f", predictedConfidence);
    out += ",\"predicted\":{\"family\":" + jsonQuote(predicted) + ",\"confidence\":" + c + ",\"sure\":" +
           (predictedSure ? "true" : "false") + ",\"model\":" + jsonQuote(modelVersion) + "}";
  }
  out += ",\"listed\":[";
  for (std::size_t i = 0; i < listed.size(); ++i) {
    char khz[32];
    std::snprintf(khz, sizeof khz, "%.3f", listed[i].hz / 1000.0);
    out += std::string(i ? "," : "") + "{\"khz\":" + khz + ",\"label\":" + jsonQuote(listed[i].label) + ",\"mode\":" +
           jsonQuote(listed[i].mode) + ",\"source\":" + jsonQuote(listed[i].source) + "}";
  }
  return out + "]}";
}

FeatureMeter::FeatureMeter(double rate, double audioCentre, double maxSeconds)
    : rate_(rate), centre_(audioCentre), max_(static_cast<std::size_t>(maxSeconds * rate)) {
  audio_.reserve(max_);
}

void FeatureMeter::add(const float* a, std::size_t n) {
  const std::size_t take = std::min(n, max_ - audio_.size());
  audio_.insert(audio_.end(), a, a + take);
}

double FeatureMeter::seconds() const { return static_cast<double>(audio_.size()) / rate_; }
bool FeatureMeter::full() const { return audio_.size() >= max_; }

EmissionFeatures FeatureMeter::measure(double bandwidthHz) const {
  EmissionFeatures f;
  f.seconds = seconds();
  if (f.seconds < 1.0) return f;
  const double bw = std::max(20.0, bandwidthHz);
  const double lo = std::max(30.0, centre_ - bw / 2 - 30), hi = std::min(rate_ / 2 - 30, centre_ + bw / 2 + 30);

  // --- Tones: lines in the averaged spectrum, each standing 3 dB above the
  // dip between it and its neighbours, within 12 dB of the strongest, and
  // 10 dB above the noise around the emission.
  const int n = 4096;
  const std::vector<double> p = welch(audio_, n);
  const double df = rate_ / n;
  auto bin = [&](double hz) { return static_cast<std::size_t>(std::clamp(hz / df, 0.0, static_cast<double>(p.size() - 1))); };
  std::vector<double> sm(p.size());
  for (std::size_t k = 1; k + 1 < p.size(); ++k) sm[k] = (p[k - 1] + 2 * p[k] + p[k + 1]) / 4;
  std::vector<double> noiseBins;
  for (std::size_t k = bin(lo - 300); k <= bin(hi + 300); ++k)
    if (k < bin(lo) || k > bin(hi)) noiseBins.push_back(sm[k]);
  std::vector<double> inBand(sm.begin() + static_cast<std::ptrdiff_t>(bin(lo)), sm.begin() + static_cast<std::ptrdiff_t>(bin(hi)) + 1);
  const double noise = noiseBins.size() >= 20 ? median(noiseBins) : percentile(inBand, 0.2);
  const double top = inBand.empty() ? 0 : *std::max_element(inBand.begin(), inBand.end());
  std::vector<std::size_t> maxima;
  const auto reach = static_cast<std::size_t>(std::max(2.0, 6.0 / df));
  for (std::size_t k = bin(lo) + reach; k + reach <= bin(hi); ++k) {
    bool isMax = true;
    for (std::size_t j = k - reach; j <= k + reach && isMax; ++j) isMax = j == k || sm[j] <= sm[k];
    if (isMax && sm[k] >= 10 * noise && sm[k] >= top / 16) maxima.push_back(k);
  }
  std::sort(maxima.begin(), maxima.end(), [&](std::size_t a, std::size_t b) { return sm[a] > sm[b]; });
  std::vector<std::size_t> lines;
  for (std::size_t k : maxima) {
    bool ok = true;
    for (std::size_t q : lines) {
      const std::size_t a = std::min(k, q), b = std::max(k, q);
      if (static_cast<double>(b - a) * df < 10) {
        ok = false;
        break;
      }
      const double dip = *std::min_element(sm.begin() + static_cast<std::ptrdiff_t>(a), sm.begin() + static_cast<std::ptrdiff_t>(b) + 1);
      if (dip > 0.5 * std::min(sm[k], sm[q])) {
        ok = false;
        break;
      }
    }
    if (ok) lines.push_back(k);
  }
  std::sort(lines.begin(), lines.end());
  f.tones = static_cast<int>(lines.size());
  // Each line's frequency: the power-weighted centre of the bins above half
  // its peak (a keyed tone is a hump as wide as the keying rate).
  std::vector<double> at;
  for (std::size_t k : lines) {
    std::size_t a = k, b = k;
    while (a > 1 && sm[a - 1] >= 0.5 * sm[k] && sm[a - 1] <= sm[a]) --a;
    while (b + 2 < sm.size() && sm[b + 1] >= 0.5 * sm[k] && sm[b + 1] <= sm[b]) ++b;
    double w = 0, c = 0;
    for (std::size_t j = a; j <= b; ++j) w += sm[j], c += sm[j] * static_cast<double>(j);
    at.push_back((w > 0 ? c / w : static_cast<double>(k)) * df);
    f.toneHz.push_back(at.back() - centre_);
  }
  bool evenlySpaced = false;
  if (lines.size() == 2) {
    f.toneSpacingHz = at[1] - at[0];
  } else if (lines.size() >= 3) {
    // The spacing that puts the lines best on a grid: the strongest
    // |mean exp(j 2 pi f_i / s)| for s around the median gap.
    std::vector<double> gaps;
    for (std::size_t i = 1; i < at.size(); ++i) gaps.push_back(at[i] - at[i - 1]);
    const double g = median(gaps);
    double bestS = g, bestR = 0;
    for (double sp = 0.7 * g; sp <= 1.3 * g; sp += 0.002 * g) {
      double re = 0, im = 0;
      for (double x : at) re += std::cos(2 * kPi * x / sp), im += std::sin(2 * kPi * x / sp);
      const double r = std::hypot(re, im) / static_cast<double>(at.size());
      if (r > bestR) bestR = r, bestS = sp;
    }
    f.toneSpacingHz = bestS;
    // Random positions reach about 0.5 by this search; keyed tones' humps
    // put real ones off their grid by up to a seventh of the spacing.
    evenlySpaced = bestR >= 0.7;
  }

  // --- Baseband: mixed down, low-passed to the band (windowed sinc).
  const double cutoff = bw / 2 + 50;
  const int taps = 129;
  std::vector<double> h(taps);
  for (int i = 0; i < taps; ++i) {
    const double t = i - (taps - 1) / 2.0;
    const double s = t == 0 ? 2 * cutoff / rate_ : std::sin(2 * kPi * cutoff / rate_ * t) / (kPi * t);
    h[static_cast<std::size_t>(i)] = s * (0.54 - 0.46 * std::cos(2 * kPi * i / (taps - 1)));
  }
  const std::size_t len = audio_.size();
  std::vector<std::complex<double>> mixed(len), z(len > static_cast<std::size_t>(taps) ? len - taps : 0);
  for (std::size_t i = 0; i < len; ++i) {
    const double ph = -2 * kPi * centre_ * static_cast<double>(i) / rate_;
    mixed[i] = std::complex<double>(audio_[i] * std::cos(ph), audio_[i] * std::sin(ph));
  }
  for (std::size_t i = 0; i < z.size(); ++i) {
    std::complex<double> acc = 0;
    for (int j = 0; j < taps; ++j) acc += mixed[i + static_cast<std::size_t>(j)] * h[static_cast<std::size_t>(j)];
    z[i] = acc;
  }
  if (z.size() < static_cast<std::size_t>(rate_ / 2)) return f;
  // Decimated to a rate of at least 8x the cut-off: fast enough, and fine
  // enough for timing the transitions of FSK.
  const auto dec = static_cast<std::size_t>(std::max(1.0, std::floor(rate_ / (8 * cutoff))));
  const double r2 = rate_ / static_cast<double>(dec);
  std::vector<std::complex<double>> zd;
  for (std::size_t i = 0; i < z.size(); i += dec) zd.push_back(z[i]);

  // --- Frame: the period at which the baseband repeats itself. It also
  // puts lines in the spectrum, at every multiple of 1 / frame: close lines
  // (under 60 Hz apart) spaced by a small multiple of that are the frame's,
  // not tones (the averaged spectrum resolves only some of them).
  std::tie(f.frameMs, f.frameStrength) = repetition(zd, r2, bw);
  const double frameHz = f.frameMs > 0 ? 1000.0 / f.frameMs : 0;
  auto frameLines = [&](double spacing) {
    if (frameHz <= 0 || spacing <= 0 || spacing >= 60) return false;
    const double k = std::round(spacing / frameHz);
    return k >= 1 && k <= 12 && std::fabs(spacing - k * frameHz) <= std::max(0.1 * frameHz, 0.02 * spacing);
  };
  if (f.tones >= 3 && frameLines(f.toneSpacingHz)) {
    f.tones = 0;
    f.toneHz.clear();
    f.toneSpacingHz = 0;
    evenlySpaced = false;
  }

  // --- Envelope.
  std::vector<double> env(zd.size());
  for (std::size_t i = 0; i < zd.size(); ++i) env[i] = std::abs(zd[i]);
  const double mean = std::accumulate(env.begin(), env.end(), 0.0) / static_cast<double>(env.size());
  double var = 0;
  for (double e : env) var += (e - mean) * (e - mean);
  f.envelopeCv = mean > 0 ? std::sqrt(var / static_cast<double>(env.size())) / mean : 0;
  const double on = percentile(env, 0.9);
  std::size_t off = 0;
  for (double e : env) off += e < 0.2 * on ? 1 : 0;
  f.offFraction = static_cast<double>(off) / static_cast<double>(env.size());
  // The same while the signal is on (above half its usual level): gaps
  // between transmissions or frames (ALE, ARQ) do not make an FSK signal's
  // envelope vary.
  double onSum = 0, onSq = 0;
  std::size_t onN = 0;
  for (double e : env)
    if (e >= 0.5 * on) onSum += e, onSq += e * e, ++onN;
  const double onMean = onN ? onSum / static_cast<double>(onN) : 0;
  const double cvOn = onN && onMean > 0 ? std::sqrt(std::max(0.0, onSq / static_cast<double>(onN) - onMean * onMean)) / onMean : 0;

  // --- The frequency discriminator (Hz), lightly smoothed (timing), and
  // its changes weighted by the amplitude (the discriminator is noise where
  // the signal is weak or absent).
  std::vector<double> disc(zd.size(), 0.0);
  for (std::size_t i = 1; i < zd.size(); ++i) disc[i] = std::arg(zd[i] * std::conj(zd[i - 1]));
  auto movingAverage = [](const std::vector<double>& x, std::size_t w) {
    std::vector<double> y(x.size(), 0.0);
    double run = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
      run += x[i] - (i >= w ? x[i - w] : 0);
      y[i] = run / static_cast<double>(std::min(i + 1, w));
    }
    return y;
  };
  const auto m = static_cast<std::size_t>(std::max(1.0, std::round(r2 / (2 * cutoff))));
  std::vector<double> hz = movingAverage(disc, m);
  for (double& v : hz) v *= r2 / (2 * kPi);
  std::vector<double> change(hz.size(), 0.0), env2(env.size());
  for (std::size_t i = m; i < hz.size(); ++i) change[i] = std::abs(hz[i] - hz[i - m]) * std::min(1.0, env[i] / (mean + 1e-30));
  for (std::size_t i = 0; i < env.size(); ++i) env2[i] = env[i] * env[i];

  // --- Spectral symbol-rate lines, in two ranges. A single carrier's rate
  // explains its band (PSK31: 31 Bd in about 60 Hz; 2400 Bd serial modems in
  // 2.4-3 kHz): a line in the power envelope between 0.3 and 1.25 times the
  // occupied band. Below that: keying rates (FSK, MFSK) and the per-tone rate
  // of multi-carrier modems, in the discriminator's changes or the envelope.
  // 15 dB: in noise alone, the strongest of the thousands of bins searched
  // reaches about 10 dB.
  const double split = 0.3 * bw;
  const auto high = strongestLine(env2, r2, split, std::min(1.25 * bw + 20, r2 / 2.2), frameHz);
  const auto lowA = strongestLine(change, r2, 5.0, std::max(6.0, split), frameHz);
  const auto lowB = strongestLine(env2, r2, 5.0, std::max(6.0, split), frameHz);
  const auto low = lowA.second >= lowB.second ? lowA : lowB;
  const bool longEnough = f.seconds >= 1.5;
  const bool singleCarrier = longEnough && high.second >= 15.0;
  // A fast single carrier (a serial modem: its rate fills most of its band)
  // swings the discriminator across the band and can look like FSK levels;
  // its varying envelope tells it apart (decided below, once the levels are
  // known: two FSK tones heard together beat at their spacing).
  // So does a wide signal that repeats itself with a varying envelope: a
  // modem's frame (it has a line at its symbol rate only if its roll-off
  // leaves one).
  const bool framedWide = f.frameMs > 0 && bw >= 300 && f.envelopeCv >= 0.25 && f.offFraction < 0.15;
  bool fastPsk = ((singleCarrier && high.first >= 0.6 * bw) || framedWide) && f.envelopeCv >= 0.25 && f.offFraction < 0.15;

  // --- Frequency levels: peaks of the histogram of the discriminator,
  // smoothed over about a third of a symbol (if a keying rate was found) for
  // slow MFSK's close tones, weighted by the amplitude, where the signal is
  // on. For a constant envelope this finds the tones of FSK and MFSK even
  // when fast keying smears the spectrum into humps (RTTY at 100 Bd, 850 Hz).
  const double onLevel = 0.5 * percentile(env, 0.9);
  std::size_t mw = m;
  if (longEnough && low.second >= 15.0)
    mw = std::clamp<std::size_t>(static_cast<std::size_t>(r2 / (3 * low.first)), m, static_cast<std::size_t>(0.010 * r2) + 1);
  // Tried first on the discriminator as it is, and only if that shows no
  // levels on the longer average: averaging over a third of a symbol of a
  // rate read off a weak line smears fast keying into a false middle level
  // (200 Bd FSK averaged over 10 ms sat 45 % of the time between its tones).
  std::vector<double> levels;
  double levelShare = 0, levelDip = 1, levelTight = 0;  // of two levels: weight near them, the dip between, weight within a tenth of the gap
  std::vector<double> hzLevels;
  auto findLevels = [&](const std::vector<double>& series, std::size_t skip) {
    levels.clear();
    levelShare = 0, levelDip = 1, levelTight = 0;
    hzLevels = series;
    const double span = cutoff, binHz = 1.0;
    const auto nb = static_cast<std::size_t>(2 * span / binHz) + 1;
    std::vector<double> hist(nb, 0.0);
    for (std::size_t i = skip; i < hzLevels.size(); ++i) {
      if (env[i] < onLevel || std::fabs(hzLevels[i]) >= span) continue;
      hist[static_cast<std::size_t>((hzLevels[i] + span) / binHz)] += env[i] * env[i];
    }
    // Smoothed over about 1 % of the band (at least 2 Hz).
    const auto w = static_cast<std::size_t>(std::max(2.0, 0.01 * bw / binHz));
    std::vector<double> hs(nb, 0.0);
    for (std::size_t i = 0; i < nb; ++i) {
      double acc = 0, wsum = 0;
      for (std::size_t j = i >= 3 * w ? i - 3 * w : 0; j <= std::min(nb - 1, i + 3 * w); ++j) {
        const double d = (static_cast<double>(j) - static_cast<double>(i)) / static_cast<double>(w);
        const double g = std::exp(-0.5 * d * d);
        acc += g * hist[j], wsum += g;
      }
      hs[i] = acc / wsum;
    }
    const double peakMax = *std::max_element(hs.begin(), hs.end());
    std::vector<std::size_t> cand;
    for (std::size_t i = 1; i + 1 < nb; ++i)
      if (hs[i] >= hs[i - 1] && hs[i] > hs[i + 1] && hs[i] >= 0.12 * peakMax) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [&](std::size_t a, std::size_t b) { return hs[a] > hs[b]; });
    std::vector<std::size_t> kept;
    for (std::size_t c : cand) {
      bool ok = true;
      for (std::size_t k : kept) {
        const std::size_t a = std::min(c, k), b = std::max(c, k);
        if (static_cast<double>(b - a) * binHz < 6) { ok = false; break; }
        const double dip = *std::min_element(hs.begin() + static_cast<std::ptrdiff_t>(a), hs.begin() + static_cast<std::ptrdiff_t>(b) + 1);
        if (dip > 0.6 * std::min(hs[c], hs[k])) { ok = false; break; }
      }
      if (ok) kept.push_back(c);
    }
    std::sort(kept.begin(), kept.end());
    for (std::size_t k : kept) levels.push_back(static_cast<double>(k) * binHz - span);
    // FSK spends its time on its levels, none of them most of it: at least
    // 60 % of the weight within a third of the smallest gap of a level, at
    // most 80 % on one, and each at least a quarter of an equal share. A PSK
    // signal sits on its carrier, with brief excursions at its phase changes.
    if (levels.size() >= 2) {
      double gap = 1e9, total = 0;
      for (std::size_t i = 1; i < levels.size(); ++i) gap = std::min(gap, levels[i] - levels[i - 1]);
      std::vector<double> wl(levels.size(), 0.0);
      for (std::size_t i = 0; i < nb; ++i) {
        total += hist[i];
        const double fq = static_cast<double>(i) * binHz - span;
        for (std::size_t k = 0; k < levels.size(); ++k)
          if (std::fabs(fq - levels[k]) <= gap / 3) wl[k] += hist[i];
      }
      double sum = 0, mx = 0, mn = 1e300;
      for (double v : wl) sum += v, mx = std::max(mx, v), mn = std::min(mn, v);
      // Three evenly spaced levels with the middle one the weakest: two
      // tones and the passage between them (fast keying spends a good part
      // of each bit there). The middle one goes.
      if (levels.size() == 3 && std::fabs((levels[2] - levels[1]) - (levels[1] - levels[0])) <= 0.15 * (levels[2] - levels[0]) / 2 &&
          wl[1] < std::min(wl[0], wl[2])) {
        sum -= wl[1];
        levels.erase(levels.begin() + 1);
        wl.erase(wl.begin() + 1);
        mx = std::max(wl[0], wl[1]), mn = std::min(wl[0], wl[1]);
        gap = levels[1] - levels[0];
      }
      const double share = 1.0 / static_cast<double>(levels.size());
      if (levels.size() == 2 && total > 0) {
        levelShare = sum / total;
        const auto a = static_cast<std::size_t>((levels[0] + span) / binHz), b = static_cast<std::size_t>((levels[1] + span) / binHz);
        levelDip = *std::min_element(hs.begin() + static_cast<std::ptrdiff_t>(a), hs.begin() + static_cast<std::ptrdiff_t>(b) + 1) /
                   std::max(1e-30, std::min(hs[a], hs[b]));
        double tight = 0;
        for (std::size_t i = 0; i < nb; ++i) {
          const double fq = static_cast<double>(i) * binHz - span;
          if (std::fabs(fq - levels[0]) <= gap / 10 || std::fabs(fq - levels[1]) <= gap / 10) tight += hist[i];
        }
        levelTight = tight / total;
      }
      if (total <= 0 || sum < 0.6 * total || mx > 0.8 * total || mn < 0.25 * share * total) levels.clear();
    }
  };
  findLevels(hz, m);
  if (levels.size() < 2 && mw > m) findLevels(movingAverage(hz, mw - m + 1), mw);
  if (fastPsk && levels.size() == 2 && std::fabs(high.first - (levels[1] - levels[0])) <= 0.05 * high.first) fastPsk = false;
  // FSK and MFSK: a constant envelope while on (about 0.03 at good SNR, 0.2
  // at 10 dB) and two or more distinct frequency levels (PSK has one).
  const bool constantEnvelope = cvOn < 0.25;
  // Two levels that hold the signal tightly (over half its weight within a
  // tenth of their gap of them, a deep dip between) are FSK even when the
  // envelope varies (a receiver's filter, selective fading: real recordings
  // of FSK read 0.25 to 0.35) or a line in the envelope looks like a fast
  // single carrier's rate. FSK keyed through a filter has such lines at
  // multiples of its keying rate; a PSK signal idling on a pattern can sit
  // on two frequencies as tightly. The edge clock tells them apart (below):
  // the envelope's line is the clock itself for PSK, a multiple for FSK.
  const bool tightTwo = levels.size() == 2 && levelTight >= 0.55 && levelDip <= 0.2 && cvOn < 0.35;
  const bool onTrial = tightTwo && !(constantEnvelope && !fastPsk);
  bool fskLevels = (constantEnvelope && levels.size() >= 2 && !fastPsk) || tightTwo;
  const EmissionFeatures beforeFsk = f;
  const bool evenBeforeFsk = evenlySpaced;
  if (fskLevels) {
    f.tones = static_cast<int>(levels.size());
    f.toneHz = levels;
    if (levels.size() == 2) {
      f.toneSpacingHz = levels[1] - levels[0];
      evenlySpaced = true;
    } else {
      std::vector<double> gaps;
      for (std::size_t i = 1; i < levels.size(); ++i) gaps.push_back(levels[i] - levels[i - 1]);
      const double g = median(gaps);
      double bestS = g, bestR = 0;
      for (double sp = 0.7 * g; sp <= 1.3 * g; sp += 0.002 * g) {
        double re = 0, im = 0;
        for (double x : levels) re += std::cos(2 * kPi * x / sp), im += std::sin(2 * kPi * x / sp);
        const double r = std::hypot(re, im) / static_cast<double>(levels.size());
        if (r > bestR) bestR = r, bestS = sp;
      }
      f.toneSpacingHz = bestS;
      evenlySpaced = bestR >= 0.7;
    }
  }

  // --- FSK and MFSK: the symbol clock from the times the discriminator
  // crosses between levels (interpolated between samples; a run shorter
  // than 1 ms is a glitch and both its edges go). For each candidate unit u,
  // how well the edges of each second line up on a grid of u (|mean of
  // exp(j 2 pi t / u)|, combined over the seconds, so that asynchronous
  // characters may start anywhere). The best u; then the symbol is u, or 2u
  // when no interval is one u long: RTTY's 1.5-bit stop bits put its edges
  // on a grid of half a bit, and make its spectral line weak. (MFSK, whose
  // jumps between distant tones cross the levels in between, uses the
  // spectral line when there is one.)
  if (fskLevels && longEnough) {
    f.symbolRate = 0;
    auto nearest = [&](double v) {
      std::size_t b = 0;
      for (std::size_t k = 1; k < levels.size(); ++k)
        if (std::fabs(v - levels[k]) < std::fabs(v - levels[b])) b = k;
      return b;
    };
    std::vector<double> edges;  // seconds
    std::vector<bool> joined;   // edge i is in the same stretch of signal as edge i-1
    std::size_t state = SIZE_MAX;
    long stretch = 0, lastStretch = -1;
    for (std::size_t i = m + 1; i < hz.size(); ++i) {
      if (env[i] < onLevel) {  // the signal is off: a new stretch begins after it
        if (state != SIZE_MAX) ++stretch;
        state = SIZE_MAX;
        continue;
      }
      const std::size_t lv = nearest(hz[i]);
      if (state == SIZE_MAX) {
        state = lv;
        continue;
      }
      if (lv == state) continue;
      double t = static_cast<double>(i);
      const double mid = 0.5 * (levels[lv] + levels[state]);
      const double a = hz[i - 1], b = hz[i];
      if ((a - mid) * (b - mid) <= 0 && a != b) t = static_cast<double>(i - 1) + (mid - a) / (b - a);
      t /= r2;
      if (!edges.empty() && lastStretch == stretch && t - edges.back() < 0.001) {
        edges.pop_back();  // a glitch: both of its edges go
        joined.pop_back();
      } else {
        joined.push_back(!edges.empty() && lastStretch == stretch);
        edges.push_back(t);
        lastStretch = stretch;
      }
      state = lv;
    }
    // Two levels: a trigger with hysteresis (a fifth of the shift either
    // side of the middle; noise on a narrow shift otherwise makes edges),
    // each edge timed where the discriminator crossed the middle. The clock
    // comes from intervals between edges of the same direction: a threshold
    // that is not quite in the middle (unequal tone levels, a filter's
    // slope) makes every mark longer and every space shorter by the same
    // amount, which those intervals do not see.
    std::vector<double> iv;   // intervals that are multiples of the clock unit
    std::vector<double> dur;  // more than two levels: the same intervals
    if (levels.size() == 2) {
      struct Edge { double t; bool up; long stretch; };
      std::vector<Edge> ed;
      const double mid = 0.5 * (levels[0] + levels[1]), hyst = 0.2 * (levels[1] - levels[0]);
      int st = -1;
      long str = 0;
      std::size_t from = m + 1;
      // The signal is off after 5 ms below half its level: tones that are
      // cross-faded (AFSK with shaped keying) dip at every transition.
      const auto offRun = static_cast<std::size_t>(0.005 * r2) + 1;
      std::size_t below = 0;
      for (std::size_t i = m + 1; i < hz.size(); ++i) {
        if (env[i] < onLevel) {
          if (++below == offRun) {
            if (st >= 0) ++str;
            st = -1;
          }
          if (st < 0) from = i + 1;
          continue;
        }
        below = 0;
        if (st < 0) {
          st = hz[i] > mid ? 1 : 0;
          continue;
        }
        const bool up = st == 0 && hz[i] > mid + hyst, down = st == 1 && hz[i] < mid - hyst;
        if (!up && !down) continue;
        std::size_t j = i;
        while (j > from && (up ? hz[j - 1] > mid : hz[j - 1] < mid)) --j;
        double t = static_cast<double>(j);
        if (j > from && hz[j] != hz[j - 1]) t = static_cast<double>(j - 1) + (mid - hz[j - 1]) / (hz[j] - hz[j - 1]);
        ed.push_back({t / r2, up, str});
        st = up ? 1 : 0;
        from = i;
      }
      for (int dir = 0; dir < 2; ++dir) {
        const Edge* prev = nullptr;
        for (const Edge& e : ed) {
          if (e.up != (dir == 1)) continue;
          if (prev && prev->stretch == e.stretch) iv.push_back(e.t - prev->t);
          prev = &e;
        }
      }
    } else {
      for (std::size_t i = 1; i < edges.size(); ++i)
        if (joined[i]) iv.push_back(edges[i] - edges[i - 1]);
      dur = iv;
    }
    const bool two = levels.size() == 2;
    const bool mfskWithLine = levels.size() > 2 && low.second >= 15.0;
    if (mfskWithLine) {
      f.symbolRate = low.first;
      f.symbolRateDb = low.second;
    } else if (iv.size() >= 16) {
      auto coherence = [&](double u) {
        double re = 0, im = 0;
        for (double d : iv) re += std::cos(2 * kPi * d / u), im += std::sin(2 * kPi * d / u);
        return iv.empty() ? 0.0 : std::hypot(re, im) / static_cast<double>(iv.size());
      };
      // The coherence on a coarse grid. The unit is the largest at which it
      // peaks at 0.5 or more and within 0.6 of its highest: a unit's halves
      // and thirds fit as well, and fit better when shaped keying moves the
      // edges of short elements (real RTTY at 45.45 Bd: 0.70 at half a bit,
      // 0.94 at a sixth of one); twice the unit does not fit at all.
      std::vector<std::pair<double, double>> grid;  // unit, coherence
      for (double u = 0.040; u >= std::max(0.0015, 4.0 / r2); u *= 0.995) grid.emplace_back(u, coherence(u));
      double unit = 0, bestS = 0;
      for (const auto& g : grid) bestS = std::max(bestS, g.second);
      for (std::size_t i = 1; i + 1 < grid.size() && unit == 0; ++i)
        if (grid[i].second > grid[i - 1].second && grid[i].second >= grid[i + 1].second && grid[i].second >= 0.5 &&
            grid[i].second >= 0.6 * bestS)
          unit = grid[i].first;
      if (bestS < 0.5) unit = 0;
      if (unit > 0) {
        // Least squares on the intervals that are clean multiples of it.
        for (int pass = 0; pass < 2; ++pass) {
          double num = 0, den = 0;
          for (double d : iv) {
            const double k = std::round(d / unit);
            if (k >= 1 && std::fabs(d / unit - k) <= 0.2) num += d * k, den += k * k;
          }
          if (den > 0) unit = num / den;
        }
        if (two) {
          // The bit: half the shortest interval between like edges that is
          // common (a tenth of them or more). That is a mark and a space of
          // one bit each, whatever fraction of a bit the unit turned out to
          // be: RTTY's 1.5-bit stop bits put its edges on a half-bit grid,
          // shaped keying sometimes on a finer one. Glitches are rarer.
          std::vector<int> count(64, 0);
          for (double d : iv) {
            const long k = std::lround(d / unit);
            if (k >= 1 && k < 64) ++count[static_cast<std::size_t>(k)];
          }
          int shortest = 0;
          for (int k = 1; k < 64 && !shortest; ++k)
            if (count[static_cast<std::size_t>(k)] * 10 >= static_cast<int>(iv.size())) shortest = k;
          f.symbolRate = shortest ? 2.0 / (shortest * unit) : 0;
        } else {
          // Intervals one unit long: random data makes many, noise a few.
          // None: the unit is half a symbol.
          int ones = 0;
          for (double d : dur) ones += std::round(std::fabs(d) / unit) == 1 ? 1 : 0;
          const bool halfBits = ones * 100 < static_cast<int>(dur.size()) * 20;
          f.symbolRate = 1.0 / (halfBits ? 2 * unit : unit);
        }
        f.symbolRateDb = 20 * std::log10(coherence(unit) * std::sqrt(static_cast<double>(iv.size())));
      }
    }
  }
  if (onTrial && singleCarrier && !(f.symbolRate > 0 && f.symbolRate <= high.first / 1.8)) {
    f = beforeFsk;  // a single carrier after all
    evenlySpaced = evenBeforeFsk;
    fskLevels = false;
  }
  if (fskLevels && longEnough) {
  } else if (singleCarrier) {
    f.symbolRate = high.first;
    f.symbolRateDb = high.second;
  } else if (longEnough && low.second >= 15.0) {
    f.symbolRate = low.first;
    f.symbolRateDb = low.second;
  }

  // --- Multi-carrier: tones evenly spaced. As lines with dips between them,
  // or, when each subcarrier's keying makes its hump overlap the next (CIS-12:
  // 120 Bd on 200 Hz spacing), as a periodicity of the spectrum across the
  // band (the strongest autocorrelation of its log, a slow trend removed).
  double combSpacing = 0;
  if (!fskLevels && !singleCarrier && bw >= 300) {
    const std::size_t k0 = bin(centre_ - bw / 2 + 20), k1 = bin(centre_ + bw / 2 - 20);
    double inBandMean = 0;
    for (std::size_t k = k0; k <= k1; ++k) inBandMean += sm[k];
    inBandMean /= static_cast<double>(k1 - k0 + 1);
    // Only what stands out: the band at least 6 dB above the noise around it.
    if (k1 > k0 + 40 && inBandMean >= 4 * noise) {
      std::vector<double> x;
      for (std::size_t k = k0; k <= k1; ++k) x.push_back(std::log(sm[k] + 1e-30));
      // Trend: a moving average over an eighth of the band.
      const std::size_t tw = std::max<std::size_t>(5, x.size() / 8);
      std::vector<double> y(x.size());
      for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t a = i >= tw ? i - tw : 0, b = std::min(x.size() - 1, i + tw);
        double acc = 0;
        for (std::size_t j = a; j <= b; ++j) acc += x[j];
        y[i] = x[i] - acc / static_cast<double>(b - a + 1);
      }
      double e0 = 0;
      for (double v : y) e0 += v * v;
      const auto lag0 = static_cast<std::size_t>(std::ceil(15.0 / df));
      const std::size_t lag1 = y.size() / 3;
      std::vector<double> ac(2 * lag1 + 2, 0.0);
      for (std::size_t L = lag0; L < ac.size() && L < y.size(); ++L) {
        double acc = 0;
        for (std::size_t i = 0; i + L < y.size(); ++i) acc += y[i] * y[i + L];
        ac[L] = e0 > 0 ? acc / e0 * static_cast<double>(y.size()) / static_cast<double>(y.size() - L) : 0;
      }
      double acTop = 0;
      for (std::size_t L = lag0 + 1; L <= lag1; ++L)
        if (ac[L] > ac[L - 1] && ac[L] >= ac[L + 1]) acTop = std::max(acTop, ac[L]);
      // The smallest lag with a peak near the strongest (the fundamental),
      // repeated at twice the lag (a comb, not one chance bump).
      for (std::size_t L = lag0 + 1; L <= lag1 && acTop >= 0.4; ++L) {
        const bool twice = 2 * L + 1 < ac.size() ? std::max({ac[2 * L - 1], ac[2 * L], ac[2 * L + 1]}) >= 0.5 * ac[L] : true;
        if (ac[L] > ac[L - 1] && ac[L] >= ac[L + 1] && ac[L] >= 0.8 * acTop && twice) {
          const double d = ac[L - 1] - 2 * ac[L] + ac[L + 1];
          const double frac = d < 0 ? 0.5 * (ac[L - 1] - ac[L + 1]) / d : 0;
          combSpacing = (static_cast<double>(L) + frac) * df;
          break;
        }
      }
    }
  }
  if (frameLines(combSpacing)) combSpacing = 0;

  // --- A cautious summary.
  if (f.tones == 1 && f.offFraction >= 0.15 && f.envelopeCv >= 0.4) f.modulation = "OOK";
  else if (f.tones == 1 && f.envelopeCv < 0.15 && bandwidthHz < 60) f.modulation = "carrier";
  else if (fskLevels && f.tones == 2) f.modulation = "FSK";
  else if (fskLevels && f.tones >= 3 && f.tones <= 64 && evenlySpaced) f.modulation = "MFSK";
  else if (singleCarrier && f.envelopeCv >= 0.2 && f.offFraction < 0.15) {
    f.modulation = "PSK";
    f.tones = 1;  // one carrier (lines from its frame structure are not tones)
    f.toneHz.clear();
    f.toneSpacingHz = 0;
  } else if (!fskLevels && f.envelopeCv >= 0.3 && ((f.tones >= 6 && evenlySpaced) || combSpacing > 0)) {
    f.modulation = "multi-carrier";
    if (!(f.tones >= 6 && evenlySpaced)) {
      f.toneSpacingHz = combSpacing;
      f.tones = static_cast<int>(std::lround(bandwidthHz / combSpacing));
      f.toneHz.clear();
    }
  } else if (singleCarrier && f.frameMs > 0 && f.envelopeCv >= 0.2 && f.tones != 1 && high.first >= 0.6 * bw) {
    // A serial modem in bursts or fades (its envelope off now and then): a
    // frame, and a symbol rate that fills most of the band.
    f.modulation = "PSK";
    f.tones = 1;
    f.toneHz.clear();
    f.toneSpacingHz = 0;
  } else if (framedWide && !fskLevels) {
    // A wide signal that repeats itself, with no carriers once its frame's
    // lines are set aside: a single-carrier modem whose symbol rate did not
    // show (a narrow roll-off leaves almost no line at it; a line below 0.3
    // of the band is not its rate).
    f.modulation = "PSK";
    f.tones = 1;
    f.toneHz.clear();
    f.toneSpacingHz = 0;
    f.symbolRate = f.symbolRateDb = 0;
  }
  if (f.modulation == "carrier") f.symbolRate = f.symbolRateDb = 0;

  // --- The vector for the learned family model: shapes and statistics that
  // do not depend on the signal's level, and little on its exact centre.
  {
    std::vector<float>& v = f.vector;
    v.clear();
    auto push = [&](double x) { v.push_back(static_cast<float>(std::isfinite(x) ? std::clamp(x, -8.0, 8.0) : 0.0)); };
    // 32: the averaged spectrum over 1.5 times the band, in dB below its
    // peak, 0 (40 dB down or more) to 1.
    {
      const double a0 = centre_ - 0.75 * bw, step = 1.5 * bw / 32;
      std::vector<double> band(32, 0.0);
      double top = 1e-30;
      for (int i = 0; i < 32; ++i) {
        const std::size_t k0 = bin(a0 + step * i), k1 = std::max(k0, bin(a0 + step * (i + 1)));
        double acc = 0;
        for (std::size_t k = k0; k <= k1; ++k) acc += sm[k];
        band[static_cast<std::size_t>(i)] = acc / static_cast<double>(k1 - k0 + 1);
        top = std::max(top, band[static_cast<std::size_t>(i)]);
      }
      for (double b : band) push(std::clamp(1.0 + 10 * std::log10(std::max(b, 1e-30) / top) / 40.0, 0.0, 1.0));
    }
    // 24: where the instantaneous frequency spends its time (weighted by
    // power), over 1.5 times the band.
    {
      std::vector<double> hist(24, 0.0);
      double total = 0;
      for (std::size_t i = m; i < hz.size(); ++i) {
        const double x = (hz[i] + 0.75 * bw) / (1.5 * bw) * 24;
        if (x < 0 || x >= 24) continue;
        hist[static_cast<std::size_t>(x)] += env[i] * env[i];
        total += env[i] * env[i];
      }
      for (double h : hist) push(total > 0 ? h / total : 0);
    }
    // 8: the amplitude's distribution, 0 to 2 times its median.
    {
      const double med = std::max(1e-30, median(env));
      std::vector<double> hist(8, 0.0);
      for (double e : env) hist[static_cast<std::size_t>(std::min(7.0, e / med * 4))] += 1;
      for (double h : hist) push(h / static_cast<double>(env.size()));
    }
    // 22 single numbers.
    double p2 = 0, p4 = 0;
    for (const auto& z1 : zd) p2 += std::norm(z1), p4 += std::norm(z1) * std::norm(z1);
    p2 /= static_cast<double>(zd.size()), p4 /= static_cast<double>(zd.size());
    push(p2 > 0 ? p4 / (p2 * p2) - 2.0 : 0);  // C42 of the normalised signal: -1 for PSK and FSK, 0 for noise
    // Lines of the squared and fourth-power signal (a carrier there: BPSK,
    // QPSK, and FSK of whole-number index), dB above the median, / 40.
    auto lineDb = [&](int power) {
      const std::size_t len = std::min<std::size_t>(zd.size(), 32768);
      const std::size_t nf = nextFftSize(len);
      const auto plan = sharedFft(static_cast<int>(nf), false);
      const Fft& fft = *plan;
      std::vector<cfloat> in(nf, cfloat(0, 0)), out(nf);
      const double scale = p2 > 0 ? 1.0 / std::sqrt(p2) : 0;
      for (std::size_t i = 0; i < len; ++i) {
        std::complex<double> x = zd[i] * scale;
        x = power == 2 ? x * x : x * x * x * x;
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(len - 1));
        in[i] = cfloat(static_cast<float>(x.real() * w), static_cast<float>(x.imag() * w));
      }
      fft.run(in.data(), out.data());
      std::vector<double> pw(nf);
      double top = 0;
      for (std::size_t k = 0; k < nf; ++k) pw[k] = std::norm(out[k]), top = std::max(top, pw[k]);
      return 10 * std::log10(top / std::max(1e-30, median(pw)));
    };
    push(lineDb(2) / 40);
    push(lineDb(4) / 40);
    push(high.second / 40);
    push(low.second / 40);
    push(high.second >= 15 ? high.first / bw : 0);
    push(low.second >= 15 ? low.first / bw : 0);
    push(f.envelopeCv);
    push(cvOn);
    push(f.offFraction);
    push(static_cast<double>(levels.size()) / 8);
    push(levelTight);
    push(levelDip);
    push(levelShare);
    push(std::min(f.tones, 32) / 32.0);
    push(f.toneSpacingHz / bw);
    push(f.frameStrength);
    push(f.frameMs > 0 ? 1 : 0);
    push(std::log10(bw) / 4);
    push(combSpacing / bw);
    push(f.symbolRate > 0 ? std::min(4.0, f.symbolRate / bw) : 0);
    // Spectral flatness inside the band (geometric over arithmetic mean): 1
    // for a flat-topped modem, small for a few lines.
    {
      double lg = 0, ar = 0;
      std::size_t cnt = 0;
      for (std::size_t k = bin(centre_ - bw / 2); k <= bin(centre_ + bw / 2); ++k) lg += std::log(sm[k] + 1e-30), ar += sm[k], ++cnt;
      push(cnt && ar > 0 ? std::exp(lg / static_cast<double>(cnt)) / (ar / static_cast<double>(cnt)) : 0);
    }
  }
  f.valid = true;
  return f;
}

}  // namespace hfc
