#include "navtex/fsk_demodulator.hpp"

#include <algorithm>
#include <cmath>

namespace navtex {

namespace {
constexpr double kDemodRateHz = 2000.0;    // at least; and at least kSamplesPerBit per bit
constexpr double kSamplesPerBit = 20.0;
constexpr double kClockGain = 0.08;        // fraction of the timing error corrected per transition
constexpr double kConfidentLevel = 0.3;    // soft value that counts as a definite tone
constexpr double kAtcBalance = 0.3;        // weaker/stronger tone level below which only ATC is used
constexpr double kAtcBlend = 0.4;          // ... blending into the plain ratio over this range
}  // namespace

void FskDemodulator::configure(double sampleRate, double baud, double shiftHz) {
  fs_ = sampleRate;
  baud_ = baud;
  shift_ = shiftHz;
  const double rate = std::max(kDemodRateHz, kSamplesPerBit * baud_);
  decim_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(fs_ / rate)));
  fsd_ = fs_ / static_cast<double>(decim_);
  double cutoff = std::min(shift_ / 2.0 + 1.2 * baud_, 0.4 * fsd_);
  lpI_.setLowpass(fs_, cutoff);
  lpQ_.setLowpass(fs_, cutoff);
  hiStep_ = std::polar(1.0, -2.0 * kPi * (shift_ / 2.0) / fsd_);
  auto samplesPerBit = static_cast<std::size_t>(std::lround(fsd_ / baud_));
  hiSum_.setLength(samplesPerBit);
  loSum_.setLength(samplesPerBit);
  double spb = fsd_ / baud_;
  envAttack_ = 1.0 - std::exp(-1.0 / 2.0);
  envDecay_ = 1.0 - std::exp(-1.0 / (16.0 * spb));
  noiseAttack_ = envAttack_;
  noiseDecay_ = 1.0 - std::exp(-1.0 / (32.0 * spb));
  clockStep_ = baud_ / fsd_;
  setCentre(centre_);
  reset();
}

void FskDemodulator::reset() {
  phase_ = hiPhase_ = {1.0, 0.0};
  renorm_ = 0;
  lpI_.reset();
  lpQ_.reset();
  decimCount_ = 0;
  hiSum_.reset();
  loSum_.reset();
  hiEnv_ = loEnv_ = hiNoise_ = loNoise_ = 0.0;
  levelsInit_ = false;
  disc_ = prevDisc_ = 0.0;
  clock_ = 0.0;
  crossing_ = -1.0;
  confidentSign_ = 0;
}

void FskDemodulator::setCentre(double hz) {
  centre_ = hz;
  step_ = std::polar(1.0, -2.0 * kPi * hz / fs_);
}

bool FskDemodulator::process(double x, float& soft) {
  std::complex<double> z = x * phase_;
  phase_ *= step_;
  if (++renorm_ >= 1024) {
    renorm_ = 0;
    phase_ /= std::abs(phase_);
    hiPhase_ /= std::abs(hiPhase_);
  }
  double i = lpI_.process(z.real()), q = lpQ_.process(z.imag());
  newSample_ = false;
  if (++decimCount_ < decim_) return false;
  decimCount_ = 0;
  newSample_ = true;
  bool bit = false;
  baseband({i, q}, soft, bit);
  return bit;
}

void FskDemodulator::baseband(std::complex<double> z, float& soft, bool& bit) {
  // Matched filters: each tone mixed down to 0 Hz and integrated over one bit.
  std::complex<double> rot = hiPhase_;
  hiPhase_ *= hiStep_;
  double hi = std::abs(hiSum_.process(z * rot));
  double lo = std::abs(loSum_.process(z * std::conj(rot)));

  if (!levelsInit_) {
    hiEnv_ = loEnv_ = std::max({hi, lo, 1e-12});
    levelsInit_ = true;
  }
  hiEnv_ += (hi > hiEnv_ ? envAttack_ : envDecay_) * (hi - hiEnv_);
  loEnv_ += (lo > loEnv_ ? envAttack_ : envDecay_) * (lo - loEnv_);
  hiNoise_ += (hi < hiNoise_ ? noiseAttack_ : noiseDecay_) * (hi - hiNoise_);
  loNoise_ += (lo < loNoise_ ? noiseAttack_ : noiseDecay_) * (lo - loNoise_);

  // Automatic threshold correction: each tone relative to its own floor and
  // envelope; the decision threshold sits midway between the two.
  double eh = std::max(hiEnv_ - hiNoise_, 0.0), el = std::max(loEnv_ - loNoise_, 0.0);
  double mh = std::clamp(hi, hiNoise_, std::max(hiEnv_, hiNoise_)) - hiNoise_;
  double ml = std::clamp(lo, loNoise_, std::max(loEnv_, loNoise_)) - loNoise_;
  double norm = 0.5 * (eh * eh + el * el);
  double atc = norm > 1e-30 ? (mh * eh - ml * el - 0.5 * (eh * eh - el * el)) / norm : 0.0;
  // With both tones equally strong the plain ratio is the better detector
  // (the envelope and floor estimates cost about 1 dB in noise); blend
  // towards ATC as the tones become unbalanced.
  double ratio = (hi - lo) / (hi + lo + 1e-30);
  double balance = std::min(eh, el) / std::max({eh, el, 1e-30});
  double w = std::clamp((balance - kAtcBalance) / kAtcBlend, 0.0, 1.0);
  disc_ = w * ratio + (1.0 - w) * atc;

  // Bit clock: transitions of the integrated signal cross zero half a bit
  // after the bit boundary, so steer the crossings to phase 0.5.
  clock_ += clockStep_;
  if ((prevDisc_ > 0.0) != (disc_ > 0.0)) {
    double frac = prevDisc_ / (prevDisc_ - disc_);
    crossing_ = clock_ - clockStep_ * (1.0 - frac);
    crossing_ -= std::floor(crossing_);
  }
  int sign = disc_ > kConfidentLevel ? 1 : disc_ < -kConfidentLevel ? -1 : 0;
  if (sign != 0 && sign != confidentSign_) {
    if (confidentSign_ != 0 && crossing_ >= 0.0) {
      double err = crossing_ - 0.5;
      clock_ -= kClockGain * err;
    }
    confidentSign_ = sign;
    crossing_ = -1.0;
  }
  prevDisc_ = disc_;

  if (clock_ >= 1.0) {
    clock_ -= 1.0;
    soft = static_cast<float>(std::clamp(disc_, -2.0, 2.0));
    bit = true;
  }
}

}  // namespace navtex
