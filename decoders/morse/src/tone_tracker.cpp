#include "morse/tone_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace morse {

namespace {
constexpr double kAverageSeconds = 1.0;   // spectral averaging time constant
constexpr int kAcquireFrames = 3;         // frames of agreement before first lock
constexpr int kJumpFrames = 12;           // frames a new peak must persist to move a lock
constexpr double kUnlockSeconds = 3.0;    // silence after which the lock is dropped
constexpr double kSmoothing = 0.15;       // per-frame smoothing of a locked estimate
constexpr double kMinDispersion = 0.25;   // sd/mean below which a bin is a carrier
constexpr double kOvertakeRatio = 8.0;    // a peak this much stronger may take over a lock
constexpr double kReferenceHz = 250.0;    // noise reference: bins up to this far away
constexpr std::size_t kRefStride = 4;     // reference computed every 4 bins
constexpr std::size_t kGuardBins = 5;     // ... excluding the tone's own bins
}  // namespace

void ToneTracker::configure(double sampleRate, double minHz, double maxHz) {
  fs_ = sampleRate;
  minHz_ = std::max(50.0, std::min(minHz, maxHz));
  maxHz_ = std::min(sampleRate * 0.45, std::max(minHz, maxHz));
  // ~10 Hz resolution: 4096 points at 44.1/48 kHz.
  n_ = nextPowerOfTwo(static_cast<std::size_t>(sampleRate / 12.0));
  hop_ = n_ / 4;
  binHz_ = fs_ / static_cast<double>(n_);
  kMin_ = static_cast<std::size_t>(std::floor(minHz_ / binHz_));
  kMax_ = std::min(n_ / 2 - 2, static_cast<std::size_t>(std::ceil(maxHz_ / binHz_)));
  kMin_ = std::max<std::size_t>(kMin_, 2);
  // Statistics are kept from 0 Hz up to the search range plus the
  // neighbourhood used as its noise reference, and at least up to
  // kSpectrumMaxHz, so the whole spectrum can be displayed.
  refSpan_ = static_cast<std::size_t>(std::ceil(kReferenceHz / binHz_));
  kLo_ = 1;
  kHi_ = std::min(n_ / 2 - 2, std::max(kMax_ + refSpan_, static_cast<std::size_t>(kSpectrumMaxHz / binHz_)));

  fft_.resize(n_);
  ring_.assign(n_, 0.0);
  window_.resize(n_);
  for (std::size_t i = 0; i < n_; ++i)
    window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n_));
  work_.assign(n_, {});
  meanP_.assign(n_ / 2, 0.0);
  meanP2_.assign(n_ / 2, 0.0);
  score_.assign(n_ / 2, 0.0);
  ratio_.assign(n_ / 2, 0.0);
  scratch_.reserve(2 * refSpan_ + 2);
  alphaFloor_ = (static_cast<double>(hop_) / fs_) / kAverageSeconds;
  reset();
}

void ToneTracker::reset() {
  std::fill(ring_.begin(), ring_.end(), 0.0);
  std::fill(meanP_.begin(), meanP_.end(), 0.0);
  std::fill(meanP2_.begin(), meanP2_.end(), 0.0);
  std::fill(score_.begin(), score_.end(), 0.0);
  std::fill(ratio_.begin(), ratio_.end(), 0.0);
  writePos_ = sinceHop_ = filled_ = 0;
  frames_ = 0;
  locked_ = false;
  peakRatioDb_ = 0.0;
  candidate_ = 0.0;
  candidateCount_ = 0;
  absentFrames_ = 0;
}

bool ToneTracker::process(double x) {
  ring_[writePos_] = x;
  writePos_ = (writePos_ + 1) % n_;
  if (filled_ < n_) ++filled_;
  if (++sinceHop_ < hop_) return false;
  sinceHop_ = 0;
  // A partly filled window would look like a tone switching on.
  if (filled_ < n_) return false;
  analyze();
  return true;
}

bool ToneTracker::signalPresentNear(double hz) const {
  if (frames_ < 2) return false;
  auto k = static_cast<long>(std::lround(hz / binHz_));
  double best = 0.0;
  for (long j = k - 1; j <= k + 1; ++j)
    if (j >= static_cast<long>(kMin_) && j <= static_cast<long>(kMax_)) best = std::max(best, ratio_[j]);
  return best > presenceRatio();
}

void ToneTracker::analyze() {
  for (std::size_t i = 0; i < n_; ++i) work_[i] = ring_[(writePos_ + i) % n_] * window_[i];
  fft_.forward(work_);

  ++frames_;
  double alpha = std::max(alphaFloor_, 1.0 / static_cast<double>(frames_));
  for (std::size_t k = kLo_; k <= kHi_; ++k) {
    double p = std::norm(work_[k]);
    meanP_[k] += alpha * (p - meanP_[k]);
    meanP2_[k] += alpha * (p * p - meanP2_[k]);
    double var = std::max(0.0, meanP2_[k] - meanP_[k] * meanP_[k]);
    // Keyed tones vary as much as they average (dispersion ~1), like noise,
    // but are much stronger; steady carriers barely vary. Scaling the spread
    // by min(1, spread/mean) leaves keyed tones and noise alone and pushes a
    // carrier down to about the noise level.
    // Bins varying far less than keying or noise would (strong carriers) are
    // suppressed further so they can never win the peak search.
    double sd = std::sqrt(var);
    double cv = meanP_[k] > 0.0 ? sd / meanP_[k] : 0.0;
    double gate = cv < kMinDispersion ? (cv / kMinDispersion) * (cv / kMinDispersion) : 1.0;
    score_[k] = sd * std::min(1.0, cv) * gate;
  }

  // Each bin is judged against its own neighbourhood: the median score of
  // the bins 3..refSpan_ bins away on either side. A keyed tone is narrow,
  // while the noise follows the receiver's filter shape, which is locally
  // flat; so the ratio does not depend on how wide the receiver filter is
  // compared with the search range (a 500 Hz CW filter or a 2.7 kHz SSB one).
  // The reference is computed every kRefStride bins and shared by the bins
  // around it (the guard is wide enough to exclude a tone in any of them).
  std::size_t best = kMin_;
  double ref = 0.0;
  std::size_t refCentre = 0;
  for (std::size_t k = kMin_; k <= kMax_; ++k) {
    std::size_t c = (k + kRefStride / 2) / kRefStride * kRefStride;
    if (c != refCentre || ref == 0.0) {
      refCentre = c;
      scratch_.clear();
      std::size_t a = c > kLo_ + refSpan_ ? c - refSpan_ : kLo_;
      std::size_t b = std::min(kHi_, c + refSpan_);
      for (std::size_t j = a; j <= b; ++j)
        if (j + kGuardBins < c || j > c + kGuardBins) scratch_.push_back(score_[j]);
      auto mid = scratch_.begin() + static_cast<long>(scratch_.size() / 2);
      std::nth_element(scratch_.begin(), mid, scratch_.end());
      ref = *mid;
    }
    ratio_[k] = ref > 0.0 ? score_[k] / ref : 0.0;
    if (ratio_[k] > ratio_[best]) best = k;
  }
  double peak = score_[best];
  peakRatioDb_ = ratio_[best] > 0.0 ? 10.0 * std::log10(ratio_[best]) : 0.0;
  const double ratio = presenceRatio();
  const bool valid = frames_ >= 2 && ratio_[best] > ratio;
  const double near = std::max(3.0 * binHz_, 25.0);

  if (!locked_) {
    if (!valid) {
      candidateCount_ = 0;
      return;
    }
    double f = interpolate(best);
    if (candidateCount_ > 0 && std::fabs(f - candidate_) < near) ++candidateCount_;
    else candidateCount_ = 1;
    candidate_ = f;
    if (candidateCount_ >= kAcquireFrames) {
      locked_ = true;
      freq_ = f;
      candidateCount_ = 0;
      absentFrames_ = 0;
    }
    return;
  }

  // Locked: stay on the station while it is present, following it through
  // the strongest bin near the current estimate, even if something else in
  // the range is stronger.
  auto k0 = static_cast<long>(std::lround(freq_ / binHz_));
  auto span = static_cast<long>(std::ceil(near / binHz_));
  std::size_t local = static_cast<std::size_t>(std::clamp(k0, static_cast<long>(kMin_), static_cast<long>(kMax_)));
  for (long k = k0 - span; k <= k0 + span; ++k)
    if (k >= static_cast<long>(kMin_) && k <= static_cast<long>(kMax_) && score_[k] > score_[local])
      local = static_cast<std::size_t>(k);
  bool overtaken = valid && peak > kOvertakeRatio * score_[local];
  if (frames_ >= 2 && ratio_[local] > ratio && !overtaken) {
    freq_ += kSmoothing * (interpolate(local) - freq_);
    candidateCount_ = 0;
    absentFrames_ = 0;
    return;
  }

  // The station has faded or stopped, or a far stronger one (9 dB) has
  // appeared. Move only once the new peak has persisted for a while; forget
  // the lock after a long silence.
  ++absentFrames_;
  if (valid) {
    double f = interpolate(best);
    if (candidateCount_ > 0 && std::fabs(f - candidate_) < near) ++candidateCount_;
    else candidateCount_ = 1;
    candidate_ = f;
    if (candidateCount_ >= kJumpFrames) {
      freq_ = f;
      candidateCount_ = 0;
      absentFrames_ = 0;
      return;
    }
  } else {
    candidateCount_ = 0;
  }
  if (absentFrames_ * static_cast<double>(hop_) / fs_ > kUnlockSeconds) locked_ = false;
}

double ToneTracker::presenceRatio() const {
  // Few averaged frames make noise peaks larger, so be stricter at first.
  return kPresenceRatio + 40.0 / static_cast<double>(std::max<long>(frames_, 1));
}

double ToneTracker::interpolate(std::size_t k) const {
  // Parabolic interpolation on log scores around bin k.
  double l = std::log(score_[k - 1] + 1e-30), c = std::log(score_[k] + 1e-30),
         r = std::log(score_[k + 1] + 1e-30);
  double denom = l - 2.0 * c + r;
  double delta = denom < 0.0 ? std::clamp(0.5 * (l - r) / denom, -0.5, 0.5) : 0.0;
  return (static_cast<double>(k) + delta) * binHz_;
}

}  // namespace morse
