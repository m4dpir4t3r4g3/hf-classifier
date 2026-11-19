#include "hfc/drm.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

namespace hfc {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Mode {
  char name;
  double tu, tg;  // seconds
};
const Mode kModes[4] = {{'A', 0.024, 0.024 / 9}, {'B', 0.064 / 3, 0.016 / 3}, {'C', 0.044 / 3, 0.016 / 3}, {'D', 0.028 / 3, 0.022 / 3}};

}  // namespace

struct DrmDetector::Impl {
  double rate, centre;
  double phase = 0;
  std::vector<float> fir;              // low-pass at 1400 Hz
  std::vector<std::complex<float>> hist;
  std::size_t histAt = 0;
  std::vector<std::complex<float>> x;  // baseband: the last Tu of the longest mode, and the block
  uint64_t n = 0;                // samples so far
  struct Fold {
    int tu, tg, ts;              // in samples
    std::vector<std::complex<double>> p;  // per phase of the symbol
    std::vector<double> e;
    int hits = 0;
  };
  Fold folds[4];
  uint64_t windowStart = 0;
  char found = 0;

  Impl(double r, double c) : rate(r), centre(c) {
    const int half = std::max(8, static_cast<int>(r / 800.0));
    for (int k = -half; k <= half; ++k) {
      const double x = 2.0 * 1400.0 / r * k;
      const double sinc = k == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      fir.push_back(static_cast<float>(sinc * (0.54 + 0.46 * std::cos(kPi * k / half))));
    }
    float sum = 0;
    for (float v : fir) sum += v;
    for (float& v : fir) v /= sum;
    hist.assign(fir.size(), {0, 0});
    for (int k = 0; k < 4; ++k) {
      Fold& f = folds[k];
      f.tu = static_cast<int>(std::lround(kModes[k].tu * r));
      f.tg = static_cast<int>(std::lround(kModes[k].tg * r));
      f.ts = f.tu + f.tg;
      f.p.assign(static_cast<std::size_t>(f.ts), {0, 0});
      f.e.assign(static_cast<std::size_t>(f.ts), 0);
    }
  }

  // The window's verdict for one mode: the guard interval's correlation
  // against the rest of the symbol.
  bool judge(Fold& f) {
    const int ts = f.ts, tg = f.tg;
    double bestC = -1;
    int best = 0;
    std::vector<double> c(static_cast<std::size_t>(ts));
    for (int ph = 0; ph < ts; ++ph) {
      std::complex<double> p = 0;
      double e = 1e-30;
      for (int k = 0; k < tg; ++k) {
        const std::size_t i = static_cast<std::size_t>((ph + k) % ts);
        p += f.p[i];
        e += f.e[i];
      }
      const double v = std::abs(p) / e;
      c[static_cast<std::size_t>(ph)] = v;
      if (v > bestC) bestC = v, best = ph;
    }
    // The rest: phases at least Tg away from the peak.
    std::vector<double> rest;
    for (int ph = 0; ph < ts; ++ph) {
      const int d = std::min((ph - best + ts) % ts, (best - ph + ts) % ts);
      if (d >= tg) rest.push_back(c[static_cast<std::size_t>(ph)]);
    }
    std::fill(f.p.begin(), f.p.end(), std::complex<double>(0, 0));
    std::fill(f.e.begin(), f.e.end(), 0);
    if (rest.empty()) return false;
    std::sort(rest.begin(), rest.end());
    const double median = rest[rest.size() / 2], top = rest[rest.size() * 95 / 100];
    return bestC > 0.3 && bestC - median > 0.25 && bestC > top + 0.15;
  }
};

DrmDetector::DrmDetector(double rate, double centreHz) : impl_(std::make_unique<Impl>(rate, centreHz)) {}
DrmDetector::~DrmDetector() = default;

void DrmDetector::process(const float* audio, std::size_t count) {
  Impl& d = *impl_;
  const int keep = d.folds[0].tu;  // the longest Tu (mode A)
  const double step = 2.0 * kPi * d.centre / d.rate;
  const std::size_t taps = d.fir.size();
  for (std::size_t i = 0; i < count; ++i) {
    d.hist[d.histAt] = {static_cast<float>(audio[i] * std::cos(d.phase)), static_cast<float>(-audio[i] * std::sin(d.phase))};
    d.histAt = (d.histAt + 1) % taps;
    d.phase += step;
    if (d.phase > 2.0 * kPi) d.phase -= 2.0 * kPi;
    std::complex<float> z = 0;
    std::size_t k = d.histAt;
    for (std::size_t t = 0; t < taps; ++t) {
      z += d.hist[k] * d.fir[t];
      if (++k == taps) k = 0;
    }
    d.x.push_back(z);
    const std::size_t last = d.x.size() - 1;
    for (auto& f : d.folds) {
      if (static_cast<int>(last) < f.tu) continue;
      // The product of the sample Tu ago and this one, at the phase of the
      // earlier sample within the symbol.
      const std::complex<double> a = d.x[last - static_cast<std::size_t>(f.tu)], b = d.x[last];
      const std::size_t ph = static_cast<std::size_t>((d.n - static_cast<uint64_t>(f.tu)) % static_cast<uint64_t>(f.ts));
      f.p[ph] += a * std::conj(b);
      f.e[ph] += 0.5 * (std::norm(a) + std::norm(b));
    }
    ++d.n;
    if (d.n - d.windowStart >= static_cast<uint64_t>(2 * d.rate)) {
      d.windowStart = d.n;
      for (int k = 0; k < 4; ++k) {
        Impl::Fold& f = d.folds[k];
        f.hits = d.judge(f) ? f.hits + 1 : 0;
        // (B and A share the symbol length; their Tu differ, so only one
        // of them correlates.)
        if (f.hits >= 3 && !d.found) d.found = kModes[k].name;
      }
    }
    if (d.x.size() > static_cast<std::size_t>(keep) * 8) d.x.erase(d.x.begin(), d.x.end() - keep);
  }
}

char DrmDetector::mode() const { return impl_->found; }

std::vector<float> drmTestAudio(char mode, double rate, double loHz, int carriers, double seconds, unsigned seed) {
  const Mode* m = nullptr;
  for (const Mode& k : kModes)
    if (k.name == mode) m = &k;
  if (!m) return {};
  std::mt19937 rng(seed);
  const double spacing = 1.0 / m->tu;
  const int k0 = static_cast<int>(std::lround(loHz / spacing));
  const std::size_t tu = static_cast<std::size_t>(std::lround(m->tu * rate)), tg = static_cast<std::size_t>(std::lround(m->tg * rate));
  std::vector<float> out;
  std::vector<std::complex<double>> sym(static_cast<std::size_t>(carriers));
  while (static_cast<double>(out.size()) < seconds * rate) {
    for (auto& s : sym) s = std::polar(1.0, kPi / 4 + kPi / 2 * static_cast<double>(rng() % 4));
    // The useful part, then the guard interval before it: its end repeated.
    std::vector<float> u(tu);
    for (std::size_t t = 0; t < tu; ++t) {
      double v = 0;
      for (int c = 0; c < carriers; ++c) {
        // Carrier frequency (k0 + c) / Tu: whole cycles in Tu, whatever the rate.
        const double ph = 2.0 * kPi * static_cast<double>(k0 + c) * static_cast<double>(t) / static_cast<double>(tu);
        v += (sym[static_cast<std::size_t>(c)] * std::polar(1.0, ph)).real();
      }
      u[t] = static_cast<float>(v / std::sqrt(static_cast<double>(carriers)) * 0.3);
    }
    out.insert(out.end(), u.end() - static_cast<std::ptrdiff_t>(tg), u.end());
    out.insert(out.end(), u.begin(), u.end());
  }
  return out;
}

}  // namespace hfc
