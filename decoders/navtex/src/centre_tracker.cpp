#include "navtex/centre_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace navtex {

namespace {
constexpr double kToneBandHz = 35.0;     // half-width of the band measured around each tone
constexpr double kLockRatio = 2.5;       // weaker tone band over noise needed to lock (4 dB)
constexpr double kPresentRatio = 1.8;    // ... to count the signal as present (2.6 dB)
constexpr double kExcludeHz = 60.0;      // a rejected centre excludes this much either side
constexpr double kExcludeSeconds = 30.0; // ... for this long
constexpr int kLockFrames = 3;           // consecutive frames that must agree
constexpr double kLocalSearchHz = 45.0;  // once locked, search only this far from the centre
constexpr double kUnlockSeconds = 5.0;   // unlock after this long without the signal
constexpr double kAfcGain = 0.25;        // fraction of the measured error corrected per frame
constexpr double kDisplayHalfSpanHz = 400.0;
}  // namespace

void CentreTracker::configure(const SitorBConfig& config) {
  cfg_ = config;
  n_ = std::max<std::size_t>(256, nextPowerOfTwo(static_cast<std::size_t>(config.sampleRate / 12.0)));
  hop_ = n_ / 2;
  fft_.resize(n_);
  ring_.assign(n_, 0.0);
  window_.resize(n_);
  // 4-term Blackman-Harris: sidelobes at -92 dB, so even a very strong
  // carrier stays within a few bins and cannot fill a neighbouring tone band.
  for (std::size_t i = 0; i < n_; ++i) {
    double x = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_);
    window_[i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x);
  }
  work_.resize(n_);
  power_.assign(n_ / 2, 0.0);
  prefix_.assign(n_ / 2 + 1, 0.0);
  binHz_ = config.sampleRate / static_cast<double>(n_);
  alpha_ = static_cast<double>(hop_) / config.sampleRate;  // one-second average
  double margin = config.shiftHz / 2.0 + 150.0;
  loBin_ = static_cast<std::size_t>(std::max(1.0, std::floor((config.minCentreHz - margin) / binHz_)));
  hiBin_ = static_cast<std::size_t>(
      std::min(static_cast<double>(n_ / 2 - 2), std::ceil((config.maxCentreHz + margin) / binHz_)));
  if (hiBin_ <= loBin_ + 4) hiBin_ = std::min(n_ / 2 - 2, loBin_ + 5);
  scratch_.reserve(n_ / 2);
  reset();
}

void CentreTracker::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.0);
  std::fill(power_.begin(), power_.end(), 0.0);
  ringPos_ = fill_ = sinceFrame_ = frames_ = 0;
  noiseBand_ = noiseBin_ = 0.0;
  centre_ = cfg_.fixedCentreHz;
  locked_ = !cfg_.afc;
  present_ = false;
  snrDb_ = 0.0;
  measuredShift_ = 0.0;
  candidate_ = -1.0;
  candidateFrames_ = absentFrames_ = 0;
  time_ = 0.0;
  for (auto& e : excluded_) e = Excluded();
}

bool CentreTracker::process(double x) {
  ring_[ringPos_] = x;
  ringPos_ = (ringPos_ + 1) % n_;
  if (fill_ < n_) ++fill_;
  if (++sinceFrame_ < hop_ || fill_ < n_) return false;
  sinceFrame_ = 0;
  analyse();
  return true;
}

double CentreTracker::cumulative(double x) const {
  // Integral of the power from loBin_ to x (in bins), with a fractional end.
  x = std::clamp(x, static_cast<double>(loBin_), static_cast<double>(hiBin_));
  auto k = static_cast<std::size_t>(x);
  return prefix_[k] + (x - static_cast<double>(k)) * power_[k];
}

double CentreTracker::band(double bin) const {
  double w = kToneBandHz / binHz_;
  return cumulative(bin + w + 0.5) - cumulative(bin - w - 0.5);
}

void CentreTracker::reject() {
  if (!cfg_.afc) return;
  double until = time_ + kExcludeSeconds;
  bool stored = false;
  for (auto& e : excluded_) {
    if (e.until <= time_) {
      e = {centre_, until};
      stored = true;
      break;
    }
  }
  if (!stored) excluded_[0] = {centre_, until};  // all in use: replace the oldest
  std::sort(std::begin(excluded_), std::end(excluded_), [](const Excluded& a, const Excluded& b) { return a.until < b.until; });
  locked_ = present_ = false;
  candidate_ = -1.0;
  candidateFrames_ = 0;
}

bool CentreTracker::excluded(double hz) const {
  for (const auto& e : excluded_)
    if (e.until > time_ && std::fabs(hz - e.hz) < kExcludeHz) return true;
  return false;
}

double CentreTracker::score(double centreBin) const {
  double h = cfg_.shiftHz / 2.0 / binHz_;
  return std::min(band(centreBin - h), band(centreBin + h)) / std::max(noiseBand_, 1e-30);
}

double CentreTracker::peakCentroid(double bin) const {
  auto k0 = static_cast<long>(std::lround(bin));
  auto w = static_cast<long>(std::lround(kToneBandHz / binHz_));
  double num = 0.0, den = 0.0;
  for (long k = k0 - w; k <= k0 + w; ++k) {
    if (k < static_cast<long>(loBin_) || k > static_cast<long>(hiBin_)) continue;
    double q = std::max(0.0, power_[static_cast<std::size_t>(k)] - noiseBin_);
    num += q * static_cast<double>(k);
    den += q;
  }
  return den > 0.0 ? num / den : -1.0;
}

double CentreTracker::refine(double centreBin) const {
  double h = cfg_.shiftHz / 2.0 / binHz_;
  double hi = peakCentroid(centreBin + h), lo = peakCentroid(centreBin - h);
  if (hi < 0.0 || lo < 0.0) return centreBin * binHz_;
  // Keep the refinement close to the grid search in case a neighbour
  // pulls one centroid away.
  double c = (hi + lo) / 2.0;
  return std::clamp(c, centreBin - 1.5, centreBin + 1.5) * binHz_;
}

void CentreTracker::analyse() {
  for (std::size_t i = 0; i < n_; ++i) work_[i] = ring_[(ringPos_ + i) % n_] * window_[i];
  fft_.forward(work_);
  ++frames_;
  time_ += static_cast<double>(hop_) / cfg_.sampleRate;
  // Plain average while filling up, then a one-second exponential average.
  double a = std::max(alpha_, 1.0 / static_cast<double>(frames_));
  prefix_[loBin_] = 0.0;
  for (std::size_t k = loBin_; k <= hiBin_; ++k) {
    power_[k] += a * (std::norm(work_[k]) - power_[k]);
    prefix_[k + 1] = prefix_[k] + power_[k];
  }
  scratch_.assign(power_.begin() + static_cast<long>(loBin_), power_.begin() + static_cast<long>(hiBin_) + 1);
  std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<long>(scratch_.size() / 2), scratch_.end());
  noiseBin_ = std::max(scratch_[scratch_.size() / 2], 1e-30);
  noiseBand_ = noiseBin_ * (2.0 * kToneBandHz / binHz_ + 1.0);

  // Best candidate centre, in half-bin steps.
  auto best = [&](double loHz, double hiHz, double& bestScore) {
    double bestBin = centre_ / binHz_;
    bestScore = 0.0;
    for (double b = loHz / binHz_; b <= hiHz / binHz_; b += 0.5) {
      if (!locked_ && excluded(b * binHz_)) continue;
      double s = score(b);
      if (s > bestScore) {
        bestScore = s;
        bestBin = b;
      }
    }
    return bestBin;
  };

  double s = 0.0, bin = centre_ / binHz_;
  if (!cfg_.afc) {
    centre_ = cfg_.fixedCentreHz;
    bin = centre_ / binHz_;
    s = score(bin);
    present_ = s > kPresentRatio;
  } else if (!locked_) {
    bin = best(cfg_.minCentreHz, cfg_.maxCentreHz, s);
    present_ = false;
    if (s > kLockRatio && frames_ >= 6) {
      if (candidate_ >= 0.0 && std::fabs(bin - candidate_) <= 2.0) ++candidateFrames_;
      else candidateFrames_ = 1;
      candidate_ = bin;
      if (candidateFrames_ >= kLockFrames) {
        centre_ = refine(bin);
        locked_ = present_ = true;
        absentFrames_ = 0;
      }
    } else {
      candidateFrames_ = 0;
      candidate_ = -1.0;
    }
  } else {
    double lo = std::max(cfg_.minCentreHz, centre_ - kLocalSearchHz);
    double hi = std::min(cfg_.maxCentreHz, centre_ + kLocalSearchHz);
    bin = best(lo, hi, s);
    present_ = s > kPresentRatio;
    if (present_) {
      centre_ += kAfcGain * (refine(bin) - centre_);
      absentFrames_ = 0;
    } else if (++absentFrames_ * static_cast<double>(hop_) / cfg_.sampleRate > kUnlockSeconds) {
      locked_ = false;
      candidate_ = -1.0;
      candidateFrames_ = 0;
    }
  }

  // Shift: the distance between the two tones' centroids, averaged.
  if (present_) {
    double h = cfg_.shiftHz / 2.0 / binHz_, c = centre_ / binHz_;
    double hi = peakCentroid(c + h), lo = peakCentroid(c - h);
    if (hi >= 0.0 && lo >= 0.0) {
      double shift = (hi - lo) * binHz_;
      measuredShift_ = measuredShift_ > 0.0 ? measuredShift_ + 0.2 * (shift - measuredShift_) : shift;
    }
  }

  // SNR: power around both tones above the noise floor, over the noise in
  // 2500 Hz. The keying spreads the signal about a baud rate beyond the tones.
  double h = cfg_.shiftHz / 2.0 + 1.5 * cfg_.baud;
  auto k0 = static_cast<std::size_t>(std::clamp((centre_ - h) / binHz_, double(loBin_), double(hiBin_)));
  auto k1 = static_cast<std::size_t>(std::clamp((centre_ + h) / binHz_, double(loBin_), double(hiBin_)));
  double sig = 0.0;
  for (std::size_t k = k0; k <= k1; ++k) sig += power_[k] - noiseBin_;
  snrDb_ = present_ ? 10.0 * std::log10(std::max(sig, 1e-3 * noiseBin_) / (noiseBin_ * 2500.0 / binHz_)) : 0.0;
}

void CentreTracker::fillSpectrum(SpectrumFrame& f) const {
  // Whole search range while searching (or always, if asked); otherwise a
  // close-up of the signal, wide enough for its keying sidebands.
  if (cfg_.wideSpectrum || (cfg_.afc && !locked_)) {
    f.lowHz = static_cast<float>(std::max(0.0, cfg_.minCentreHz - cfg_.shiftHz));
    f.highHz = static_cast<float>(cfg_.maxCentreHz + cfg_.shiftHz);
  } else {
    double half = std::max(kDisplayHalfSpanHz, cfg_.shiftHz / 2.0 + 2.5 * cfg_.baud + 50.0);
    f.lowHz = static_cast<float>(std::max(0.0, centre_ - half));
    f.highHz = static_cast<float>(centre_ + half);
  }
  f.centreHz = static_cast<float>(centre_);
  f.shiftHz = static_cast<float>(cfg_.shiftHz);
  f.locked = locked_;
  for (int i = 0; i < SpectrumFrame::kBins; ++i) {
    double hz = f.lowHz + (f.highHz - f.lowHz) * (i + 0.5) / SpectrumFrame::kBins;
    double b = std::clamp(hz / binHz_, static_cast<double>(loBin_), static_cast<double>(hiBin_) - 1.0);
    auto k = static_cast<std::size_t>(b);
    double p = power_[k] + (b - static_cast<double>(k)) * (power_[k + 1] - power_[k]);
    f.db[i] = frames_ ? static_cast<float>(10.0 * std::log10(std::max(p, 1e-30) / noiseBin_)) : 0.0f;
  }
}

}  // namespace navtex
