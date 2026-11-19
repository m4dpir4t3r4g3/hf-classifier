// Automatic frequency tracking for the keyed audio tone.
//
// Every hop (a quarter of the FFT length, ~21 ms at 48 kHz) a Hann-windowed
// FFT of the latest ~85 ms of audio is taken. Per bin, exponential averages of
// the power and of the squared power are kept over about one second. A bin is
// scored by   score = sd * min(1, sd / mean)   (sd = std-dev of the power).
// On-off keyed tones vary about as much as they average, so they keep their
// full spread, while steady carriers (birdies, tuning carriers, heterodynes)
// vary little relative to their power and drop to about the noise level.
//
// Each bin's score is compared with the median score of its neighbourhood
// (bins 3 to ~250 Hz away on either side). A keyed tone is narrow while the
// noise follows the receiver filter's shape, which is locally flat, so this
// ratio works the same with a 500 Hz CW filter or a 2.7 kHz SSB filter, and
// with a search range much wider than the filter. The bin with the highest
// ratio, refined by parabolic interpolation, is the tone estimate, accepted
// only when the ratio is high enough; noise alone never moves the tracker.
//
// Once locked the tracker stays with that station while it is present, even
// if a somewhat stronger one appears elsewhere; it moves only after the
// station has gone (or one at least 9 dB stronger appears) and the new peak
// has persisted, and it unlocks completely after a few seconds of silence.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "morse/dsp.hpp"

namespace morse {

class ToneTracker {
 public:
  void configure(double sampleRate, double minHz, double maxHz);
  void reset();

  // Feeds one sample. Returns true when a new analysis frame was completed.
  bool process(double x);

  bool locked() const { return locked_; }
  double frequency() const { return freq_; }
  // Peak-to-median score ratio of the last frame, in dB (a spectral SNR).
  double peakRatioDb() const { return peakRatioDb_; }
  // Whether a keyed tone is currently visible close to `hz`.
  bool signalPresentNear(double hz) const;
  double binHz() const { return binHz_; }

  // Ratio a bin must exceed over its neighbourhood's median score to count
  // as a signal (plus a margin while few frames have been averaged).
  static constexpr double kPresenceRatio = 5.0;
  // Upper end of the averaged power spectrum kept for display.
  static constexpr double kSpectrumMaxHz = 3500.0;

  // Averaged power per FFT bin (bins with statistics only; others are 0).
  const std::vector<double>& power() const { return meanP_; }
  double searchMinHz() const { return kMin_ * binHz_; }
  double searchMaxHz() const { return kMax_ * binHz_; }

 private:
  void analyze();
  double presenceRatio() const;
  double interpolate(std::size_t k) const;

  double fs_ = 48000, minHz_ = 300, maxHz_ = 1200, binHz_ = 1;
  std::size_t n_ = 0, hop_ = 0, writePos_ = 0, sinceHop_ = 0, filled_ = 0;
  std::size_t kMin_ = 0, kMax_ = 0;  // search range
  std::size_t kLo_ = 0, kHi_ = 0;    // bins with statistics
  std::size_t refSpan_ = 1;
  std::vector<double> ring_, window_;
  std::vector<std::complex<double>> work_;
  std::vector<double> meanP_, meanP2_, score_, ratio_, scratch_;
  Fft fft_;
  long frames_ = 0;
  double alphaFloor_ = 0.02;

  bool locked_ = false;
  double freq_ = 700.0, peakRatioDb_ = 0.0;
  double candidate_ = 0.0;
  int candidateCount_ = 0;
  long absentFrames_ = 0;
};

}  // namespace morse
