#include "hfc/dsp.hpp"

#include <cmath>
#include <map>
#include <mutex>
#include <stdexcept>

#include "fft/kiss_fft.h"

namespace hfc {

static_assert(sizeof(kiss_fft_cpx) == sizeof(cfloat), "KISS FFT must use float");

Fft::Fft(int n, bool inverse) : n_(n), cfg_(kiss_fft_alloc(n, inverse ? 1 : 0, nullptr, nullptr)) {
  if (!cfg_) throw std::runtime_error("FFT allocation failed");
}

Fft::~Fft() { kiss_fft_free(cfg_); }

void Fft::run(const cfloat* in, cfloat* out) const {
  kiss_fft(static_cast<kiss_fft_cfg>(cfg_), reinterpret_cast<const kiss_fft_cpx*>(in),
           reinterpret_cast<kiss_fft_cpx*>(out));
}

std::shared_ptr<const Fft> sharedFft(int n, bool inverse) {
  static std::mutex mu;
  static std::map<std::pair<int, bool>, std::shared_ptr<const Fft>> plans;
  std::lock_guard<std::mutex> l(mu);
  auto it = plans.find({n, inverse});
  if (it != plans.end()) return it->second;
  auto p = std::make_shared<const Fft>(n, inverse);
  if (plans.size() < 64) plans.emplace(std::make_pair(n, inverse), p);
  return p;
}

bool isSmooth235(long n) {
  if (n <= 0) return false;
  for (long p : {2L, 3L, 5L})
    while (n % p == 0) n /= p;
  return n == 1;
}

long gcdLong(long a, long b) {
  while (b) {
    const long t = a % b;
    a = b;
    b = t;
  }
  return a < 0 ? -a : a;
}

void Nco::setStep(double radiansPerSample) {
  step_ = radiansPerSample;
  stepRe_ = std::cos(radiansPerSample);
  stepIm_ = std::sin(radiansPerSample);
}

cfloat Nco::next() {
  const cfloat out(static_cast<float>(re_), static_cast<float>(im_));
  const double r = re_ * stepRe_ - im_ * stepIm_;
  im_ = re_ * stepIm_ + im_ * stepRe_;
  re_ = r;
  if (++count_ == 1024) {
    count_ = 0;
    const double g = 1.0 / std::sqrt(re_ * re_ + im_ * im_);
    re_ *= g;
    im_ *= g;
  }
  return out;
}

}  // namespace hfc
