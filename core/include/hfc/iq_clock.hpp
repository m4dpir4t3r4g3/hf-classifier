// Time keeping for an IQ stream that carries no timestamps.
//
// SDRconnect's binary IQ messages hold samples only: no time, no sample
// rate, no counter. This class numbers the samples, estimates when each was
// received (UTC) and detects lost data, from what is known on the host:
//
//   - the sample rate SDRconnect reports (device_sample_rate);
//   - when each message arrived (steady clock for intervals, system clock
//     for UTC).
//
// Arrival times jitter (about 20 messages a second) and include an unknown,
// roughly constant latency. `lateness` = arrival time minus the time the
// sample count predicts. When samples are lost (SDRconnect drops IQ once
// about 2.5 s is queued for a slow reader), the lateness rises for good;
// when the reader merely stalls, it rises and falls back as the queue
// drains. So a gap is declared when the *minimum* lateness over the last
// window stays above its baseline by more than a threshold. The UTC estimate
// is then re-anchored. Absolute UTC is off by the unknown latency
// (tens of ms is a guess, not a measurement).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace hfc {

class IqClock {
 public:
  struct Config {
    double gapThresholdSeconds = 0.2;
    double gapWindowSeconds = 1.0;    // minimum lateness taken over this window
    double rateWindowSeconds = 5.0;   // for the measured rate
    double guardSeconds = 0.1;        // blocks marked `settling` after a retune
    double baselineTimeConstant = 60.0;  // slow tracking of clock drift
  };

  struct Stamp {
    uint64_t index = 0;          // of the block's first sample, counting received samples only
    double utc = 0.0;            // Unix seconds of the block's first sample (estimate)
    bool discontinuity = false;  // samples lost before this block, or the rate changed
    bool settling = false;       // inside the guard time after a retune or rate change
    bool retuned = false;        // first block after a retune
    // When this block reveals a gap: the index at which samples went missing
    // (the start of the first block that arrived late for good), <= index.
    // Detection lags the gap by up to gapWindowSeconds; blocks in between
    // were dated as if nothing was lost.
    bool gap = false;
    uint64_t gapIndex = 0;
    double gapSeconds = 0.0;     // estimated duration lost
  };

  struct Stats {
    uint64_t samples = 0;
    uint64_t blocks = 0;
    uint64_t gaps = 0;
    double lostSeconds = 0.0;     // estimated total duration lost in gaps
    double nominalRate = 0.0;     // as reported by SDRconnect (0: unknown)
    double measuredRate = 0.0;    // samples per second over the rate window
    double lateness = 0.0;        // current minimum lateness above baseline, seconds
  };

  IqClock() = default;
  explicit IqClock(const Config& c) : cfg_(c) {}

  // A block of `samples` IQ pairs arrived at `steady` (seconds, monotonic)
  // and `utc` (Unix seconds).
  Stamp onBlock(std::size_t samples, double steady, double utc);
  // SDRconnect reported a (new) sample rate.
  void setNominalRate(double hz, double steady);
  // SDRconnect reported a new centre frequency.
  void markRetune(double steady);
  // A retune was asked for: IQ may come from the new frequency before
  // SDRconnect reports it, so blocks are flagged settling for up to
  // `seconds` (until markRetune() shortens it to the guard time).
  void holdSettling(double steady, double seconds);
  double nominalRate() const { return nominal_; }
  void reset();

  Stats stats() const;
  double rateForTiming() const;  // nominal if known, else measured

 private:
  void anchor(double steady, double utc, std::size_t samples);
  void trimRateWindow(double now);

  Config cfg_;
  uint64_t next_ = 0;                  // index of the next sample
  bool anchored_ = false;
  uint64_t anchorIndex_ = 0;
  double anchorSteady_ = 0.0, anchorUtc_ = 0.0;
  double baseline_ = 0.0;
  bool baselineSet_ = false;
  struct Late {
    double steady, lateness;
    uint64_t index;  // of the block's first sample
  };
  std::deque<Late> lateWin_;
  std::deque<std::pair<double, uint64_t>> rateWin_;  // (steady, samples after block)
  double nominal_ = 0.0;
  bool pendingDiscontinuity_ = false;
  bool pendingRetune_ = false;
  double settleUntil_ = -1.0;
  uint64_t blocks_ = 0, gaps_ = 0;
  double lost_ = 0.0, lastExcess_ = 0.0;
};

}  // namespace hfc
