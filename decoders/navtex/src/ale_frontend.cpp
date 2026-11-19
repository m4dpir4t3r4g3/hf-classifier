#include "navtex/ale_frontend.hpp"

#include <algorithm>
#include <cmath>

namespace navtex::ale {

// ------------------------------------------------------------- AleFrontEnd

void AleFrontEnd::configure(double inputRate) {
  inRate_ = inputRate;
  step_ = inputRate / kBasebandRate;
  rot_ = std::polar(1.0, -2.0 * kPi * kCarrierHz / inputRate);
  // Low-pass at 3.8 kHz (the PSK signal reaches +-1.62 kHz, the 2G tones
  // -1.05 .. +0.7 kHz), Blackman-windowed over +-1.25 ms.
  const double fc = std::min(3800.0, 0.45 * inputRate);
  half_ = static_cast<int>(std::ceil(0.00125 * inputRate)) + 1;
  const int points = 2 * half_ * kOversample + 2;
  kernel_.assign(static_cast<std::size_t>(points), 0.0f);
  for (int i = 0; i < points; ++i) {
    double tau = static_cast<double>(i) / kOversample - half_;  // input samples
    if (std::fabs(tau) > half_) continue;
    double x = 2.0 * fc * tau / inputRate;
    double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    double w = tau / half_;  // -1 .. 1
    double win = 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2.0 * kPi * w);
    kernel_[static_cast<std::size_t>(i)] = static_cast<float>(2.0 * fc / inputRate * sinc * win);
  }
  std::size_t size = 1;
  while (size < static_cast<std::size_t>(2 * half_ + 8)) size <<= 1;
  ring_.assign(size, cf());
  mask_ = size - 1;
  reset();
}

void AleFrontEnd::reset() {
  std::fill(ring_.begin(), ring_.end(), cf());
  phasor_ = {1.0, 0.0};
  renorm_ = 0;
  n_ = -1;
  tOut_ = 0.0;
}

int AleFrontEnd::push(float x, cf out[4]) {
  ++n_;
  std::complex<double> z = 2.0 * static_cast<double>(x) * phasor_;
  ring_[static_cast<std::size_t>(n_) & mask_] = cf(static_cast<float>(z.real()), static_cast<float>(z.imag()));
  phasor_ *= rot_;
  if (++renorm_ >= 1024) {
    renorm_ = 0;
    phasor_ /= std::abs(phasor_);
  }
  int count = 0;
  while (count < 4 && static_cast<double>(n_) >= tOut_ + half_) {
    long long first = static_cast<long long>(std::ceil(tOut_ - half_));
    long long last = static_cast<long long>(std::floor(tOut_ + half_));
    float re = 0.0f, im = 0.0f;
    for (long long n = std::max(first, 0LL); n <= last; ++n) {
      double pos = (tOut_ - static_cast<double>(n) + half_) * kOversample;
      int i = static_cast<int>(pos);
      float frac = static_cast<float>(pos - i);
      float h = kernel_[static_cast<std::size_t>(i)] * (1.0f - frac) + kernel_[static_cast<std::size_t>(i) + 1] * frac;
      const cf& s = ring_[static_cast<std::size_t>(n) & mask_];
      re += h * s.real();
      im += h * s.imag();
    }
    out[count++] = cf(re, im);
    tOut_ += step_;
  }
  return count;
}

// ------------------------------------------------------------- RrcFilter

double RrcFilter::tap(int i) {
  const double beta = 0.35;
  double t = (i - (kTaps - 1) / 2) / static_cast<double>(kSps);  // symbols
  if (std::fabs(t) < 1e-9) return 1.0 - beta + 4.0 * beta / kPi;
  if (std::fabs(std::fabs(t) - 1.0 / (4.0 * beta)) < 1e-9)
    return beta / std::sqrt(2.0) *
           ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
  return (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) /
         (kPi * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
}

RrcFilter::RrcFilter() {
  double energy = 0.0;
  for (int i = 0; i < kTaps; ++i) energy += tap(i) * tap(i);
  for (int i = 0; i < kTaps; ++i) h_[i] = static_cast<float>(tap(i) / std::sqrt(energy));
}

void RrcFilter::reset() {
  std::fill(std::begin(buf_), std::end(buf_), cf());
  pos_ = 0;
}

cf RrcFilter::process(cf x) {
  buf_[pos_] = buf_[pos_ + kTaps] = x;
  float re = 0.0f, im = 0.0f;
  const cf* b = buf_ + pos_;
  for (int i = 0; i < kTaps; ++i) {
    re += h_[i] * b[i].real();
    im += h_[i] * b[i].imag();
  }
  pos_ = (pos_ + kTaps - 1) % kTaps;
  return cf(re, im);
}

// ------------------------------------------------------------- AleSpectrum

AleSpectrum::AleSpectrum() : fft_(kN), work_(kN), buf_(kN), avg_(kN, 0.0) {}

void AleSpectrum::reset() {
  fill_ = 0;
  first_ = true;
  std::fill(avg_.begin(), avg_.end(), 0.0);
}

bool AleSpectrum::push(cf x, SpectrumFrame& frame) {
  buf_[fill_++] = x;
  if (fill_ < kN) return false;
  fill_ = 0;
  for (std::size_t i = 0; i < kN; ++i) {
    double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / kN);
    work_[i] = std::complex<double>(buf_[i].real(), buf_[i].imag()) * w;
  }
  fft_.forward(work_);
  for (std::size_t i = 0; i < kN; ++i) {
    double p = std::norm(work_[i]);
    avg_[i] = first_ ? p : 0.6 * avg_[i] + 0.4 * p;
  }
  first_ = false;

  frame.lowHz = 300.0f;
  frame.highHz = 3300.0f;
  frame.centreHz = static_cast<float>(kCarrierHz);
  frame.shiftHz = 0.0f;
  frame.locked = false;
  double power[SpectrumFrame::kBins];
  const double binHz = kBasebandRate / kN;
  const double width = (frame.highHz - frame.lowHz) / SpectrumFrame::kBins;
  for (int b = 0; b < SpectrumFrame::kBins; ++b) {
    double f0 = frame.lowHz + b * width - kCarrierHz, f1 = f0 + width;
    int i0 = static_cast<int>(std::floor(f0 / binHz)), i1 = static_cast<int>(std::ceil(f1 / binHz));
    double sum = 0.0;
    int count = 0;
    for (int i = i0; i < std::max(i1, i0 + 1); ++i) {
      sum += avg_[static_cast<std::size_t>((i % static_cast<int>(kN) + static_cast<int>(kN)) % static_cast<int>(kN))];
      ++count;
    }
    power[b] = sum / count + 1e-30;
  }
  double sorted[SpectrumFrame::kBins];
  std::copy(power, power + SpectrumFrame::kBins, sorted);
  std::nth_element(sorted, sorted + SpectrumFrame::kBins / 4, sorted + SpectrumFrame::kBins);
  double floor = sorted[SpectrumFrame::kBins / 4];
  double total = 0.0;
  for (int b = 0; b < SpectrumFrame::kBins; ++b) {
    frame.db[b] = static_cast<float>(10.0 * std::log10(power[b] / floor));
    total += power[b];
  }
  // Signal over noise in the 3 kHz shown: total power less the noise floor's share.
  double noise = floor * SpectrumFrame::kBins;
  snrDb_ = total > noise * 1.0001 ? 10.0 * std::log10((total - noise) / noise) : -30.0;
  return true;
}

}  // namespace navtex::ale
