#include "navtex/dsp.hpp"

#include <algorithm>
#include <cmath>

namespace navtex {

void Biquad::setLowpass(double sampleRate, double cutoffHz, double q) {
  double w0 = 2.0 * kPi * cutoffHz / sampleRate;
  double cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
  double a0 = 1.0 + alpha;
  b0_ = (1.0 - cw) / 2.0 / a0;
  b1_ = (1.0 - cw) / a0;
  b2_ = b0_;
  a1_ = -2.0 * cw / a0;
  a2_ = (1.0 - alpha) / a0;
}

void Butterworth4::setLowpass(double sampleRate, double cutoffHz) {
  // Pole-pair Qs of a 4th-order Butterworth: 1/(2 cos(pi/8)), 1/(2 cos(3pi/8)).
  s1_.setLowpass(sampleRate, cutoffHz, 0.54119610);
  s2_.setLowpass(sampleRate, cutoffHz, 1.30656296);
}

std::size_t nextPowerOfTwo(std::size_t n) {
  std::size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

void Fft::resize(std::size_t n) {
  n_ = n;
  twiddle_.resize(n / 2);
  bitrev_.resize(n);
  for (std::size_t k = 0; k < n / 2; ++k)
    twiddle_[k] = std::polar(1.0, -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n));
  std::size_t bits = 0;
  while ((std::size_t{1} << bits) < n) ++bits;
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t r = 0;
    for (std::size_t b = 0; b < bits; ++b)
      if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
    bitrev_[i] = r;
  }
}

void Fft::forward(std::vector<std::complex<double>>& a) const {
  for (std::size_t i = 0; i < n_; ++i)
    if (i < bitrev_[i]) std::swap(a[i], a[bitrev_[i]]);
  for (std::size_t len = 2; len <= n_; len <<= 1) {
    std::size_t half = len / 2, step = n_ / len;
    for (std::size_t i = 0; i < n_; i += len) {
      for (std::size_t j = 0; j < half; ++j) {
        auto t = a[i + j + half] * twiddle_[j * step];
        a[i + j + half] = a[i + j] - t;
        a[i + j] += t;
      }
    }
  }
}

void SlidingSum::setLength(std::size_t length) {
  buf_.assign(std::max<std::size_t>(length, 1), {});
  reset();
}

void SlidingSum::reset() {
  std::fill(buf_.begin(), buf_.end(), std::complex<double>());
  pos_ = sinceRebuild_ = 0;
  sum_ = {};
}

std::complex<double> SlidingSum::process(std::complex<double> x) {
  sum_ += x - buf_[pos_];
  buf_[pos_] = x;
  pos_ = (pos_ + 1) % buf_.size();
  // Recompute now and then so rounding errors cannot accumulate.
  if (++sinceRebuild_ >= 4096) {
    sinceRebuild_ = 0;
    sum_ = {};
    for (const auto& v : buf_) sum_ += v;
  }
  return sum_;
}

}  // namespace navtex
