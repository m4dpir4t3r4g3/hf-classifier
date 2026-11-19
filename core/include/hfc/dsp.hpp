// Small DSP building blocks shared by the channelizer, the detector and the
// decoder host: a complex FFT (KISS FFT, any size made of factors 2, 3 and
// 5), and a numerically controlled oscillator.
#pragma once

#include <complex>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hfc {

using cfloat = std::complex<float>;

class Fft {
 public:
  Fft(int n, bool inverse);
  ~Fft();
  Fft(const Fft&) = delete;
  Fft& operator=(const Fft&) = delete;
  int size() const { return n_; }
  // Unnormalised transform, out-of-place.
  void run(const cfloat* in, cfloat* out) const;

 private:
  int n_;
  void* cfg_;
};

// A plan of this size and direction shared by all callers and threads (a
// plan only is read while transforming): making one computes its twiddles,
// which cost more than the transforms where one was made per measurement.
// At most 64 sizes are kept; beyond, a new plan each time.
std::shared_ptr<const Fft> sharedFft(int n, bool inverse);

// True when n's only prime factors are 2, 3 and 5.
bool isSmooth235(long n);
long gcdLong(long a, long b);

// exp(j*phase), advanced by `step` radians per sample; renormalised so that
// the amplitude does not drift over hours.
class Nco {
 public:
  void setStep(double radiansPerSample);
  double step() const { return step_; }
  cfloat next();
  void reset() { re_ = 1.0, im_ = 0.0; }

 private:
  double re_ = 1.0, im_ = 0.0;
  double stepRe_ = 1.0, stepIm_ = 0.0, step_ = 0.0;
  unsigned count_ = 0;
};

constexpr double kPi = 3.14159265358979323846;

}  // namespace hfc
