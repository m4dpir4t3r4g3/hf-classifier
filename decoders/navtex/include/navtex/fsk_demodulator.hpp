// Non-coherent FSK demodulator with bit clock recovery.
//
//   audio -> complex mixer at the centre frequency -> 4th-order low-pass
//     -> decimate to ~2 kHz -> for each tone: mix to 0 Hz and integrate over
//     one bit (the matched filter) -> magnitude
//     -> per-tone envelope and noise floor tracking -> soft decision
//     -> sampled once per bit by a clock locked to the zero crossings
//
// The soft decision is the ratio (hi - lo) / (hi + lo) while the two tones
// are about equally strong. When they are not (selective fading, common on
// MF at night) it blends into automatic threshold correction (after W7AY, as
// in fldigi): each tone is measured against its own envelope and noise
// floor, so a signal with one tone faded is still sliced in the right place.
// The output is about +1 for the higher tone and -1 for the lower one.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>

#include "navtex/dsp.hpp"

namespace navtex {

class FskDemodulator {
 public:
  void configure(double sampleRate, double baud, double shiftHz);
  void reset();
  void setCentre(double hz);

  // One mono sample. Returns true when a bit was sampled, with its soft
  // value (> 0: higher tone) in `soft`.
  bool process(double x, float& soft);

  // Soft decision at the demodulator rate: the output of the tone matched
  // filters, which integrate over the last bit. newSample() is true when the
  // last process() call produced a new one (at sampleRate() / decimation).
  double discriminator() const { return disc_; }
  bool newSample() const { return newSample_; }
  double demodRate() const { return fsd_; }
  double bitsPerSecond() const { return baud_; }

 private:
  void baseband(std::complex<double> z, float& soft, bool& bit);

  double fs_ = 48000.0, baud_ = 100.0, shift_ = 170.0, centre_ = 1000.0;
  // Mixer at the input rate.
  std::complex<double> phase_{1.0, 0.0}, step_{1.0, 0.0};
  std::uint32_t renorm_ = 0;
  Butterworth4 lpI_, lpQ_;
  std::size_t decim_ = 24, decimCount_ = 0;
  double fsd_ = 2000.0;
  // Tone correlators at the decimated rate.
  std::complex<double> hiPhase_{1.0, 0.0}, hiStep_{1.0, 0.0};
  SlidingSum hiSum_, loSum_;
  // Envelope and noise tracking.
  double hiEnv_ = 0.0, loEnv_ = 0.0, hiNoise_ = 0.0, loNoise_ = 0.0;
  double envAttack_ = 0.5, envDecay_ = 0.005, noiseAttack_ = 0.5, noiseDecay_ = 0.002;
  bool levelsInit_ = false;
  double disc_ = 0.0, prevDisc_ = 0.0;
  bool newSample_ = false;
  // Bit clock.
  double clock_ = 0.0, clockStep_ = 0.05, crossing_ = -1.0;
  int confidentSign_ = 0;
};

}  // namespace navtex
