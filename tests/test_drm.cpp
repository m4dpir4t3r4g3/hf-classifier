// DRM: each robustness mode on this file's own OFDM, in a slice of its
// carriers, shifted by an arbitrary frequency, in noise; and what must not
// be taken for it. Real signals: the Signal Identification Wiki's
// recordings, through tools/sigidwiki_bench.py.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/drm.hpp"
#include "hfc/sstv.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

char detect(const std::vector<float>& a, double rate, double centre) {
  DrmDetector d(rate, centre);
  for (std::size_t i = 0; i < a.size(); i += 1000) d.process(a.data() + i, std::min<std::size_t>(1000, a.size() - i));
  return d.mode();
}

void addNoise(std::vector<float>& a, double sigma, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, static_cast<float>(sigma));
  for (auto& v : a) v += n(rng);
}

}  // namespace

TEST(drm_robustness_modes) {
  for (char m : {'A', 'B', 'C', 'D'}) {
    // 40 carriers from 400 Hz (a slice of a broadcast's 100 to 400), at
    // 0 dB SNR in the band.
    auto a = drmTestAudio(m, 12000, 437, 40, 10, static_cast<unsigned>(m));
    addNoise(a, 0.3 / std::sqrt(2.0), 2);
    const char got = detect(a, 12000, 1300);
    std::printf("    mode %c: %c\n", m, got ? got : '-');
    CHECK(got == m);
  }
  // 8000 samples a second, carriers not on whole multiples of the rate's
  // grid (the cut-out channel's arbitrary shift).
  const auto a = drmTestAudio('B', 8000, 913, 60, 10, 9);
  CHECK(detect(a, 8000, 1600) == 'B');
}

TEST(drm_not_in_other_signals) {
  // Noise; an SSTV picture; single-carrier PSK at 2400 Bd.
  std::vector<float> noise(12000 * 12, 0.0f);
  addNoise(noise, 0.2, 3);
  CHECK(detect(noise, 12000, 1500) == 0);
  CHECK(detect(sstvModulate(sstvTestPicture("Martin M2"), "Martin M2", 12000), 12000, 1750) == 0);
  std::mt19937 rng(4);
  std::vector<float> psk;
  double ph = 0;
  for (int s = 0; s < 2400 * 12; ++s) {
    const double sym = 3.14159265358979 / 4 * static_cast<double>(rng() % 8);
    for (int k = 0; k < 5; ++k) {
      ph += 2 * 3.14159265358979 * 1800.0 / 12000.0;
      psk.push_back(static_cast<float>(0.4 * std::cos(ph + sym)));
    }
  }
  CHECK(detect(psk, 12000, 1800) == 0);
}
