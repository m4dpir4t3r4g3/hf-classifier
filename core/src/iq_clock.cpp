#include "hfc/iq_clock.hpp"

#include <algorithm>

namespace hfc {

double IqClock::rateForTiming() const {
  if (nominal_ > 0) return nominal_;
  if (rateWin_.size() >= 2) {
    const double dt = rateWin_.back().first - rateWin_.front().first;
    if (dt > 0) return static_cast<double>(rateWin_.back().second - rateWin_.front().second) / dt;
  }
  return 0.0;
}

void IqClock::anchor(double steady, double utc, std::size_t samples) {
  // The block's last sample was received at `steady`, so its first sample
  // was taken samples/rate earlier.
  const double rate = rateForTiming();
  const double span = rate > 0 ? static_cast<double>(samples) / rate : 0.0;
  anchorIndex_ = next_;
  anchorSteady_ = steady - span;
  anchorUtc_ = utc - span;
  anchored_ = rate > 0;
  baselineSet_ = false;
  lateWin_.clear();
}

IqClock::Stamp IqClock::onBlock(std::size_t samples, double steady, double utc) {
  Stamp s;
  s.index = next_;
  ++blocks_;
  if (!anchored_) anchor(steady, utc, samples);
  s.discontinuity = pendingDiscontinuity_;
  pendingDiscontinuity_ = false;
  s.retuned = pendingRetune_;
  pendingRetune_ = false;
  s.settling = steady < settleUntil_;

  next_ += samples;
  rateWin_.emplace_back(steady, next_);
  trimRateWindow(steady);

  const double rate = rateForTiming();
  if (anchored_ && rate > 0) {
    const double predicted = anchorSteady_ + static_cast<double>(next_ - anchorIndex_) / rate;
    const double late = steady - predicted;
    lateWin_.push_back({steady, late, s.index});
    while (!lateWin_.empty() && lateWin_.front().steady < steady - cfg_.gapWindowSeconds) lateWin_.pop_front();
    double minLate = late, maxLate = late;
    for (const auto& p : lateWin_) {
      minLate = std::min(minLate, p.lateness);
      maxLate = std::max(maxLate, p.lateness);
    }
    // Lost samples leave every later block late by the same amount; a
    // reader that stalled gets a burst of blocks whose lateness falls as it
    // catches up. So a gap needs a window that spans gapWindowSeconds of
    // arrivals with the lateness steady across it (not just high: after a
    // stall longer than the window, the window holds only the burst).
    const bool windowFull = steady - anchorSteady_ >= cfg_.gapWindowSeconds &&
                            steady - lateWin_.front().steady >= 0.8 * cfg_.gapWindowSeconds;
    const bool steadyLate = maxLate - minLate < cfg_.gapThresholdSeconds;
    if (!baselineSet_) {
      baseline_ = minLate;
      baselineSet_ = true;
    } else if (minLate < baseline_) {
      baseline_ = minLate;
    } else if (windowFull) {
      const double excess = minLate - baseline_;
      lastExcess_ = excess;
      if (excess > cfg_.gapThresholdSeconds && steadyLate) {
        // Samples were lost: the stream is now `excess` seconds behind the
        // sample count. Re-anchor from this block on.
        ++gaps_;
        lost_ += excess;
        // The samples went missing just before the first block of the
        // window that arrived late by (nearly) the whole excess.
        s.gap = true;
        s.gapSeconds = excess;
        s.gapIndex = s.index;
        for (const auto& p : lateWin_)
          if (p.lateness - baseline_ > excess - cfg_.gapThresholdSeconds / 2) {
            s.gapIndex = p.index;
            break;
          }
        anchorIndex_ = s.index;
        anchorSteady_ = steady - static_cast<double>(samples) / rate;
        anchorUtc_ = utc - static_cast<double>(samples) / rate;
        lateWin_.clear();
        baselineSet_ = false;
        s.discontinuity = true;
        lastExcess_ = 0.0;
      } else if (excess <= cfg_.gapThresholdSeconds) {
        // Follow slow drift between the host clock and the sample clock.
        const double dt = lateWin_.size() >= 2 ? steady - lateWin_[lateWin_.size() - 2].steady : 0.05;
        baseline_ += (minLate - baseline_) * std::min(1.0, dt / cfg_.baselineTimeConstant);
      }
    }
  }
  s.utc = anchored_ && rate > 0 ? anchorUtc_ + static_cast<double>(s.index - anchorIndex_) / rate
                                : utc - (rate > 0 ? static_cast<double>(samples) / rate : 0.0);
  return s;
}

void IqClock::trimRateWindow(double now) {
  while (rateWin_.size() > 2 && rateWin_.front().first < now - cfg_.rateWindowSeconds) rateWin_.pop_front();
}

void IqClock::setNominalRate(double hz, double steady) {
  if (hz == nominal_) return;
  const bool hadRate = nominal_ > 0 || blocks_ > 0;
  nominal_ = hz;
  rateWin_.clear();
  anchored_ = false;  // re-anchor on the next block
  if (hadRate) {
    pendingDiscontinuity_ = true;
    settleUntil_ = steady + cfg_.guardSeconds;
  }
}

void IqClock::markRetune(double steady) {
  pendingRetune_ = true;
  settleUntil_ = steady + cfg_.guardSeconds;
}

void IqClock::holdSettling(double steady, double seconds) { settleUntil_ = std::max(settleUntil_, steady + seconds); }

void IqClock::reset() {
  const double nominal = nominal_;
  const Config cfg = cfg_;
  *this = IqClock(cfg);
  nominal_ = nominal;
}

IqClock::Stats IqClock::stats() const {
  Stats st;
  st.samples = next_;
  st.blocks = blocks_;
  st.gaps = gaps_;
  st.lostSeconds = lost_;
  st.nominalRate = nominal_;
  if (rateWin_.size() >= 2) {
    const double dt = rateWin_.back().first - rateWin_.front().first;
    if (dt > 0) st.measuredRate = static_cast<double>(rateWin_.back().second - rateWin_.front().second) / dt;
  }
  st.lateness = lastExcess_;
  return st;
}

}  // namespace hfc
