#include "morse/level_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace morse {

namespace {
constexpr double kStoreRate = 500.0;       // envelope values kept per second
constexpr int kUpdateEvery = 6;            // recompute every 6 stored values (12 ms)
constexpr double kMinRatio = 1.25;         // high/low level ratio to count as keyed
constexpr double kMinSeparation = 3.3;     // class separation (log domain, pooled std-devs)
constexpr double kKeepSeparation = 2.8;    // ... to stay valid once valid
constexpr double kMinClassWeight = 0.08;   // each class holds at least this share
constexpr std::size_t kMinValues = 250;    // 0.5 s of data before the first estimate
}  // namespace

void LevelEstimator::configure(double inputRate, double maxWindowSeconds) {
  decim_ = std::max(1, static_cast<int>(std::lround(inputRate / kStoreRate)));
  ring_.assign(static_cast<std::size_t>(maxWindowSeconds * inputRate / decim_) + 1, 0.0f);
  hist_.assign(kBins, 0);
  window_ = ring_.size();
  reset();
}

void LevelEstimator::reset() {
  pos_ = count_ = 0;
  decimCount_ = sinceUpdate_ = 0;
  low_ = high_ = separation_ = 0;
  valid_ = false;
}

void LevelEstimator::setWindowSeconds(double seconds) {
  auto w = static_cast<std::size_t>(seconds * kStoreRate);
  window_ = std::clamp<std::size_t>(w, 16, ring_.size());
}

void LevelEstimator::push(double value) {
  if (++decimCount_ < decim_) return;
  decimCount_ = 0;
  // Stored as log levels: fading is multiplicative, so in the log domain a
  // fade shifts the mark class instead of smearing it into the noise class.
  ring_[pos_] = static_cast<float>(std::log(std::max(value, 1e-12)));
  pos_ = (pos_ + 1) % ring_.size();
  count_ = std::min(count_ + 1, ring_.size());
  if (++sinceUpdate_ >= kUpdateEvery) {
    sinceUpdate_ = 0;
    update();
  }
}

void LevelEstimator::update() {
  std::size_t n = std::min(count_, window_);
  if (n < std::min(window_, kMinValues)) {
    valid_ = false;
    return;
  }
  auto at = [&](std::size_t i) { return ring_[(pos_ + ring_.size() - 1 - i) % ring_.size()]; };

  float mn = at(0), mx = at(0);
  for (std::size_t i = 1; i < n; ++i) {
    mn = std::min(mn, at(i));
    mx = std::max(mx, at(i));
  }
  if (!(mx > mn)) {
    valid_ = false;
    return;
  }
  double scale = kBins / (static_cast<double>(mx) - mn);
  std::fill(hist_.begin(), hist_.end(), 0);
  for (std::size_t i = 0; i < n; ++i)
    ++hist_[std::min(kBins - 1, static_cast<int>((at(i) - mn) * scale))];

  auto center = [&](int b) { return mn + (b + 0.5) / scale; };
  double total = 0, totalSum = 0;
  for (int b = 0; b < kBins; ++b) {
    total += hist_[b];
    totalSum += hist_[b] * center(b);
  }

  // Otsu: pick the split maximising w0 * w1 * (mu0 - mu1)^2.
  double w0 = 0, sum0 = 0, best = -1;
  int split = 0;
  for (int b = 0; b < kBins - 1; ++b) {
    w0 += hist_[b];
    sum0 += hist_[b] * center(b);
    double w1 = total - w0;
    if (w0 == 0 || w1 == 0) continue;
    double m0 = sum0 / w0, m1 = (totalSum - sum0) / w1;
    double between = w0 * w1 * (m0 - m1) * (m0 - m1);
    if (between > best) {
      best = between;
      split = b;
    }
  }

  double c0 = 0, s0 = 0, q0 = 0, c1 = 0, s1 = 0, q1 = 0;
  for (int b = 0; b < kBins; ++b) {
    double x = center(b), h = hist_[b];
    if (b <= split) {
      c0 += h;
      s0 += h * x;
      q0 += h * x * x;
    } else {
      c1 += h;
      s1 += h * x;
      q1 += h * x * x;
    }
  }
  if (c0 == 0 || c1 == 0) {
    valid_ = false;
    return;
  }
  double m0 = s0 / c0, m1 = s1 / c1;
  double v0 = std::max(0.0, q0 / c0 - m0 * m0), v1 = std::max(0.0, q1 / c1 - m1 * m1);
  separation_ = (m1 - m0) / std::sqrt(0.5 * (v0 + v1) + 1e-30);
  // Report arithmetic (linear) class means: the threshold is applied to the
  // linear envelope, and the geometric mean would understate the noise.
  double splitLog = mn + (split + 1) / scale, l0 = 0, l1 = 0;
  for (std::size_t i = 0; i < n; ++i) {
    double x = std::exp(static_cast<double>(at(i)));
    (at(i) < splitLog ? l0 : l1) += x;
  }
  low_ = l0 / c0;
  high_ = l1 / c1;
  // Hysteresis: once valid, stay valid down to a lower separation, so a fade
  // does not throw the levels away in the middle of a word.
  double need = valid_ ? kKeepSeparation : kMinSeparation;
  valid_ = high_ > kMinRatio * low_ && separation_ >= need && c0 / total >= kMinClassWeight &&
           c1 / total >= kMinClassWeight;
}

}  // namespace morse
