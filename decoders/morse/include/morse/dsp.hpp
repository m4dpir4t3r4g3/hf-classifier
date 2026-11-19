// Small DSP building blocks used by the decoder. All of them allocate only in
// their configure/constructor calls, never while processing samples.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace morse {

constexpr double kPi = 3.14159265358979323846;

// Second-order IIR section (RBJ cookbook), transposed direct form II.
class Biquad {
 public:
  void setLowpass(double sampleRate, double cutoffHz, double q);
  void setHighpass(double sampleRate, double cutoffHz, double q);
  void reset() { z1_ = z2_ = 0.0; }
  double process(double x) {
    double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }

 private:
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  double z1_ = 0, z2_ = 0;
};

// Fourth-order Butterworth low-pass or high-pass built from two biquads.
class Butterworth4 {
 public:
  void setLowpass(double sampleRate, double cutoffHz);
  void setHighpass(double sampleRate, double cutoffHz);
  void reset() {
    s1_.reset();
    s2_.reset();
  }
  double process(double x) { return s2_.process(s1_.process(x)); }

 private:
  Biquad s1_, s2_;
};

// In-place iterative radix-2 complex FFT of a fixed power-of-two size.
class Fft {
 public:
  explicit Fft(std::size_t n = 0) { resize(n); }
  void resize(std::size_t n);
  std::size_t size() const { return n_; }
  void forward(std::vector<std::complex<double>>& data) const;

 private:
  std::size_t n_ = 0;
  std::vector<std::complex<double>> twiddle_;
  std::vector<std::size_t> bitrev_;
};

// Running mean over the last `length` samples.
class MovingAverage {
 public:
  void setMaxLength(std::size_t maxLength);
  void setLength(std::size_t length);  // clamped to [1, maxLength]
  std::size_t length() const { return len_; }
  void reset();
  double process(double x);

 private:
  std::vector<double> buf_;
  std::size_t len_ = 1, pos_ = 0, filled_ = 0;
  double sum_ = 0.0;
};

std::size_t nextPowerOfTwo(std::size_t n);

}  // namespace morse
