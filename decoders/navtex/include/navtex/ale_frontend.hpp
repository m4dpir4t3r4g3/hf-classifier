// The front end shared by the 2G, 3G and 4G ALE receivers: audio at any
// rate to complex baseband at 9600 Hz around 1800 Hz (the PSK carrier, and
// the middle of the 2G tones), the root-raised-cosine matched filter for the
// 2400 Bd PSK waveforms, and the spectrum shown in the tuning display.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "navtex/dsp.hpp"
#include "navtex/types.hpp"

namespace navtex::ale {

using cf = std::complex<float>;

constexpr double kBasebandRate = 9600.0;
constexpr double kCarrierHz = 1800.0;
constexpr int kSps = 4;  // baseband samples per 2400 Bd symbol

// Mixer, windowed-sinc low-pass and resampler to kBasebandRate.
class AleFrontEnd {
 public:
  void configure(double inputRate);
  void reset();
  // One input sample; returns the number of output samples (0..2 for input
  // rates down to 8 kHz) written to `out`.
  int push(float x, cf out[4]);

 private:
  double inRate_ = 0.0, step_ = 1.0;  // input samples per output sample
  std::complex<double> phasor_{1.0, 0.0}, rot_{1.0, 0.0};
  int renorm_ = 0;
  std::vector<cf> ring_;
  std::size_t mask_ = 0;
  long long n_ = -1;   // index of the newest input sample
  double tOut_ = 0.0;  // input-sample time of the next output
  int half_ = 0;       // kernel half width in input samples
  std::vector<float> kernel_;  // tabulated at kOversample points per input sample
  static constexpr int kOversample = 32;
};

// Root-raised-cosine filter (35% roll-off, as G.5.1.9 recommends) at 4
// samples per symbol.
class RrcFilter {
 public:
  RrcFilter();
  void reset();
  cf process(cf x);
  static constexpr int kTaps = 49;  // +-6 symbols
  static double tap(int i);         // i in [0, kTaps)

 private:
  float h_[kTaps];
  cf buf_[2 * kTaps] = {};
  int pos_ = 0;
};

// Power spectrum of the baseband for the tuning display, ~9 frames a second,
// covering 300 .. 3300 Hz audio.
class AleSpectrum {
 public:
  AleSpectrum();
  void reset();
  // True when `frame` was filled.
  bool push(cf x, SpectrumFrame& frame);
  // Noise floor and signal level (dB, arbitrary reference) of the last frame.
  double snrDb() const { return snrDb_; }

 private:
  static constexpr std::size_t kN = 1024;
  Fft fft_;
  std::vector<std::complex<double>> work_;
  std::vector<cf> buf_;
  std::vector<double> avg_;
  std::size_t fill_ = 0;
  bool first_ = true;
  double snrDb_ = 0.0;
};

}  // namespace navtex::ale
