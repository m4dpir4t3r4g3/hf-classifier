// The feature meter on synthetic audio: the emission centred at 1500 Hz in
// 12 kHz audio, with white noise, as the decoder host gives it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "hfc/features.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

constexpr double kRate = 12000, kCentre = 1500, kPi2 = 6.283185307179586;

// Continuous-phase FSK with random symbols: `tones` frequencies `spacing`
// apart, centred on kCentre.
std::vector<float> mfsk(int tones, double spacing, double baud, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  double ph = 0;
  int sym = 0;
  long index = -1;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto k = static_cast<long>(std::floor(static_cast<double>(i) * baud / kRate));
    if (k != index) index = k, sym = static_cast<int>(rng() % static_cast<unsigned>(tones));
    const double f = kCentre + (sym - (tones - 1) / 2.0) * spacing;
    ph += kPi2 * f / kRate;
    a[i] = static_cast<float>(0.5 * std::sin(ph));
  }
  return a;
}

// BPSK with raised-cosine amplitude at the reversals (as PSK31), random bits.
std::vector<float> bpsk(double baud, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  int prev = 1, cur = 1;
  long index = -1;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double s = static_cast<double>(i) * baud / kRate;
    const double pos = s - std::floor(s);
    if (static_cast<long>(std::floor(s)) != index) {
      index = static_cast<long>(std::floor(s));
      prev = cur;
      cur = rng() % 2 ? -cur : cur;
    }
    // Amplitude goes through zero mid-transition when the phase reverses.
    const double amp = prev == cur ? 1.0 : std::cos(kPi2 / 2 * pos);
    const double sign = prev == cur ? cur : (pos < 0.5 ? prev : cur);
    a[i] = static_cast<float>(0.5 * sign * std::fabs(amp) * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate));
  }
  return a;
}

// Morse-like on-off keying of a tone at 20 WPM (60 ms dots), random elements.
std::vector<float> ook(double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  const auto dot = static_cast<std::size_t>(0.06 * kRate);
  std::size_t i = 0;
  while (i < a.size()) {
    const std::size_t on = (rng() % 2 ? 3 : 1) * dot;
    for (std::size_t k = 0; k < on && i < a.size(); ++k, ++i)
      a[i] = static_cast<float>(0.5 * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate));
    i += (rng() % 3 == 0 ? 3 : 1) * dot;
  }
  return a;
}

// Many carriers, each with random phase changes at `baud` (a parallel-tone modem).
std::vector<float> multiCarrier(int carriers, double spacing, double baud, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate), 0.0f);
  std::vector<double> ph(static_cast<std::size_t>(carriers));
  const auto sym = static_cast<std::size_t>(kRate / baud);
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (i % sym == 0)
      for (auto& p : ph) p = (rng() % 4) * kPi2 / 4;
    double v = 0;
    for (int c = 0; c < carriers; ++c) {
      const double f = kCentre + (c - (carriers - 1) / 2.0) * spacing;
      v += std::sin(kPi2 * f * static_cast<double>(i) / kRate + ph[static_cast<std::size_t>(c)]);
    }
    a[i] = static_cast<float>(0.5 * v / std::sqrt(static_cast<double>(carriers)));
  }
  return a;
}

// Asynchronous RTTY: start bit (space), 5 random data bits, 1.5 stop bits
// (mark), and now and then an idle pause of random length, so that
// characters start off any common bit clock.
std::vector<float> asyncRtty(double baud, double shift, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  std::vector<int> bits;  // in half bits: +1 mark, -1 space
  while (bits.size() < static_cast<std::size_t>(seconds * baud * 2) + 20) {
    bits.push_back(-1), bits.push_back(-1);
    for (int b = 0; b < 5; ++b) {
      const int v = rng() % 2 ? 1 : -1;
      bits.push_back(v), bits.push_back(v);
    }
    for (int k = 0; k < 3; ++k) bits.push_back(1);
    if (rng() % 3 == 0)
      for (unsigned k = 0, n = rng() % 9; k < n; ++k) bits.push_back(1);  // pause of 0..4 bits
  }
  const double half = kRate / baud / 2;
  double ph = 0, jitter = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    // A pause also shifts the clock by a random fraction of a half bit.
    const auto k = static_cast<std::size_t>((static_cast<double>(i) + jitter) / half);
    if (k < bits.size() && k > 0 && bits[k - 1] == 1 && bits[k] == 1 && rng() % 4000 == 0)
      jitter += half * static_cast<double>(rng() % 1000) / 1000.0;
    const int v = bits[std::min(k, bits.size() - 1)];
    ph += kPi2 * (kCentre + v * shift / 2) / kRate;
    a[i] = static_cast<float>(0.5 * std::sin(ph));
  }
  return a;
}

// Single-carrier BPSK at `baud` with rectangular pulses (the meter's filter
// band-limits it), as a serial modem.
std::vector<float> serialPsk(double baud, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  double sign = 1;
  long index = -1;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto k = static_cast<long>(std::floor(static_cast<double>(i) * baud / kRate));
    if (k != index) index = k, sign = rng() % 2 ? 1.0 : -1.0;
    a[i] = static_cast<float>(0.5 * sign * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate));
  }
  return a;
}

// A serial modem with a frame: 8PSK at `baud` (rectangular pulses), every
// frame of `frame` symbols starting with the same `known` symbols (a
// preamble or probe sent in every frame), the rest random.
std::vector<float> framedPsk(double baud, int frame, int known, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<int> preamble(static_cast<std::size_t>(known));
  for (int& v : preamble) v = static_cast<int>(rng() % 8);
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  int sym = 0;
  long index = -1;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto k = static_cast<long>(std::floor(static_cast<double>(i) * baud / kRate));
    if (k != index) {
      index = k;
      const auto pos = static_cast<int>(k % frame);
      sym = pos < known ? preamble[static_cast<std::size_t>(pos)] : static_cast<int>(rng() % 8);
    }
    a[i] = static_cast<float>(0.5 * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate + kPi2 / 8 * sym));
  }
  return a;
}

// RTTY as sound-card programs send it: two tones cross-faded over `fade`
// seconds at each transition (the envelope dips there), a start bit, 5 data
// bits and 1.5 stop bits, no pauses.
std::vector<float> crossFadedRtty(double baud, double shift, double fade, double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<int> half;  // half bits: 1 mark, 0 space
  while (half.size() < static_cast<std::size_t>(seconds * baud * 2) + 20) {
    half.push_back(0), half.push_back(0);
    for (int b = 0; b < 5; ++b) {
      const int v = static_cast<int>(rng() % 2);
      half.push_back(v), half.push_back(v);
    }
    for (int k = 0; k < 3; ++k) half.push_back(1);
  }
  std::vector<float> a(static_cast<std::size_t>(seconds * kRate));
  const double per = kRate / baud / 2, ramp = fade * kRate;
  double mark = 1;  // the mark tone's share, slewed towards the bit sent
  for (std::size_t i = 0; i < a.size(); ++i) {
    const int v = half[std::min(half.size() - 1, static_cast<std::size_t>(static_cast<double>(i) / per))];
    mark = std::clamp(mark + (v ? 1.0 : -1.0) / ramp, 0.0, 1.0);
    const double w = 0.5 - 0.5 * std::cos(kPi2 / 2 * mark);
    const double t = static_cast<double>(i) / kRate;
    a[i] = static_cast<float>(0.5 * (w * std::sin(kPi2 * (kCentre + shift / 2) * t) + (1 - w) * std::sin(kPi2 * (kCentre - shift / 2) * t + 1.0)));
  }
  return a;
}

void addNoise(std::vector<float>& a, double snrDbIn3k, unsigned seed) {
  double p = 0;
  for (float v : a) p += v * v;
  p /= static_cast<double>(a.size());
  // Noise power in 3 kHz = p / snr; white over kRate / 2.
  const double sigma = std::sqrt(p / std::pow(10, snrDbIn3k / 10) * (kRate / 2) / 3000);
  std::mt19937 rng(seed);
  std::normal_distribution<double> n(0, sigma);
  for (float& v : a) v += static_cast<float>(n(rng));
}

// The noise seed: 7, or HFC_NOISE_SEED (to check that no result hangs on
// one noise sequence; the standard libraries' normal distributions differ).
unsigned noiseSeed() {
  const char* e = std::getenv("HFC_NOISE_SEED");
  return e && *e ? static_cast<unsigned>(std::atoi(e)) : 7u;
}

EmissionFeatures measure(std::vector<float> a, double bw, double snr = 20) {
  addNoise(a, snr, noiseSeed());
  FeatureMeter m(kRate, kCentre);
  m.add(a.data(), a.size());
  EmissionFeatures f = m.measure(bw);
  if (std::getenv("HFC_FEATURES")) {
    std::printf("    bw %.0f: %s | tones %d spacing %.1f rate %.2f (%.1f dB) cv %.2f off %.2f frame %.2f (%.3f) [", bw,
                f.summary().c_str(), f.tones, f.toneSpacingHz, f.symbolRate, f.symbolRateDb, f.envelopeCv, f.offFraction,
                f.frameMs, f.frameStrength);
    for (double t : f.toneHz) std::printf(" %.0f", t);
    std::printf(" ]\n");
  }
  return f;
}

bool near(double v, double want, double tol) { return std::fabs(v - want) <= tol; }

}  // namespace

TEST(features_of_2fsk) {
  // RTTY 45.45 Bd / 170 Hz, RTTY 75 Bd / 850 Hz, SITOR-B 100 Bd / 170 Hz.
  struct Case { double shift, baud, bw; } cases[] = {{170, 45.45, 260}, {850, 75, 1000}, {170, 100, 360}, {450, 50, 560}};
  for (const auto& c : cases) {
    const EmissionFeatures f = measure(mfsk(2, c.shift, c.baud, 8, 1), c.bw);
    CHECK(f.valid && f.modulation == "FSK" && f.tones == 2);
    CHECK(near(f.toneSpacingHz, c.shift, std::max(10.0, 0.05 * c.shift)));
    CHECK(near(f.symbolRate, c.baud, 0.5));
  }
}

TEST(features_of_mfsk) {
  // 8 tones 250 Hz apart at 125 Bd (2G ALE's tones and rate).
  const EmissionFeatures f = measure(mfsk(8, 250, 125, 8, 2), 2000);
  CHECK(f.modulation == "MFSK" && f.tones == 8);
  CHECK(near(f.toneSpacingHz, 250, 6) && near(f.symbolRate, 125, 1));
}

TEST(features_of_bpsk_ook_carrier_and_multicarrier) {
  const EmissionFeatures p = measure(bpsk(31.25, 8, 3), 60);
  CHECK(p.modulation == "PSK" && near(p.symbolRate, 31.25, 0.5));
  const EmissionFeatures o = measure(ook(8, 4), 60);
  CHECK(o.modulation == "OOK");
  std::vector<float> c(static_cast<std::size_t>(8 * kRate));
  for (std::size_t i = 0; i < c.size(); ++i) c[i] = static_cast<float>(0.5 * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate));
  const EmissionFeatures k = measure(c, 10);
  CHECK(k.modulation == "carrier" && k.tones == 1);
  // 16 carriers 112.5 Hz apart, 75 Bd (a parallel-tone HF modem's layout).
  const EmissionFeatures m = measure(multiCarrier(16, 112.5, 75, 8, 5), 1850);
  CHECK(m.modulation == "multi-carrier" && near(m.toneSpacingHz, 112.5, 6));
}

TEST(features_of_noise_are_nothing) {
  std::vector<float> a(static_cast<std::size_t>(8 * kRate), 0.0f);
  addNoise(a, 0, 9);  // p = 0: unit-less, sigma 0; add noise directly
  std::mt19937 rng(11);
  std::normal_distribution<double> n(0, 0.1);
  for (float& v : a) v = static_cast<float>(n(rng));
  FeatureMeter m(kRate, kCentre);
  m.add(a.data(), a.size());
  const EmissionFeatures f = m.measure(500);
  if (std::getenv("HFC_FEATURES")) std::printf("    noise: %s tones %d rate %.1f cv %.2f\n", f.summary().c_str(), f.tones, f.symbolRate, f.envelopeCv);
  CHECK(f.valid && f.modulation.empty() && f.symbolRate == 0 && f.tones == 0);
}

TEST(features_of_async_rtty_serial_psk_overlapping_carriers_and_mfsk16) {
  // RTTY 45.45 Bd / 170 Hz and 75 Bd / 850 Hz, characters starting off the
  // bit clock after pauses.
  EmissionFeatures r = measure(asyncRtty(45.45, 170, 8, 21), 260);
  CHECK(r.modulation == "FSK" && near(r.symbolRate, 45.45, 0.6));
  r = measure(asyncRtty(75, 850, 8, 22), 1000);
  CHECK(r.modulation == "FSK" && near(r.symbolRate, 75, 1.0));
  // A 2400 Bd serial modem: single-carrier PSK, its rate explaining its band.
  const EmissionFeatures p = measure(serialPsk(2400, 8, 23), 2600);
  CHECK(p.modulation == "PSK" && near(p.symbolRate, 2400, 30));
  // 12 DQPSK subcarriers 200 Hz apart at 120 Bd (CIS-12's layout): their
  // humps overlap.
  const EmissionFeatures c = measure(multiCarrier(12, 200, 120, 8, 24), 2400);
  CHECK(c.modulation == "multi-carrier" && near(c.toneSpacingHz, 200, 12) && c.tones >= 11 && c.tones <= 13);
  // MFSK16: 16 tones 15.625 Hz apart at 15.625 Bd.
  const EmissionFeatures m = measure(mfsk(16, 15.625, 15.625, 8, 25), 260);
  CHECK(m.modulation == "MFSK" && near(m.toneSpacingHz, 15.625, 1.0) && m.tones >= 12);
}

TEST(features_find_a_serial_modems_frame) {
  // STANAG 4285's frame: 2400 Bd, 256 symbols, the same 80-symbol preamble
  // in each, 106.67 ms. About 0.31 of the signal repeats.
  EmissionFeatures f = measure(framedPsk(2400, 256, 80, 8, 31), 2600);
  CHECK(near(f.frameMs, 106.667, 0.3) && f.frameStrength > 0.2 && f.frameStrength < 0.4);
  CHECK(f.summary().find("frame 106.7 ms") != std::string::npos);
  CHECK(f.json().find("\"frame_ms\":106.6") != std::string::npos);
  // ... a few dB above the noise in 3 kHz.
  f = measure(framedPsk(2400, 256, 80, 8, 32), 2600, 3);
  CHECK(near(f.frameMs, 106.667, 0.3) && f.frameStrength > 0.1);
  // STANAG 4529's: 1200 Bd, the same structure, 213.33 ms (not 106.67).
  f = measure(framedPsk(1200, 256, 80, 8, 33), 1400);
  CHECK(near(f.frameMs, 213.333, 0.6));
  // STANAG 4539's: 31 known symbols in 287, 119.58 ms, about 0.11 repeating.
  f = measure(framedPsk(2400, 287, 31, 8, 34), 2600);
  CHECK(near(f.frameMs, 119.583, 0.3));
  // CIS BPSK 1200's 426.67 ms period, here as 64 known symbols in 512.
  f = measure(framedPsk(1200, 512, 64, 8, 35), 1400);
  CHECK(near(f.frameMs, 426.667, 1.0));
  // No frame: random 8PSK, and signals that correlate at many lags (a
  // carrier, FSK and MFSK tones, a modem's symbols, keying).
  CHECK(measure(framedPsk(2400, 256, 0, 8, 36), 2600).frameMs == 0);
  CHECK(measure(serialPsk(1200, 8, 37), 1400).frameMs == 0);
  CHECK(measure(bpsk(31.25, 8, 38), 60).frameMs == 0);
  CHECK(measure(mfsk(2, 170, 45.45, 8, 39), 260).frameMs == 0);
  CHECK(measure(mfsk(2, 850, 75, 8, 40), 1000).frameMs == 0);
  CHECK(measure(mfsk(8, 250, 125, 8, 41), 2000).frameMs == 0);
  CHECK(measure(mfsk(16, 15.625, 15.625, 8, 42), 260).frameMs == 0);
  CHECK(measure(multiCarrier(12, 200, 120, 8, 43), 2400).frameMs == 0);
  CHECK(measure(ook(8, 44), 60).frameMs == 0);
  std::vector<float> c(static_cast<std::size_t>(8 * kRate));
  for (std::size_t i = 0; i < c.size(); ++i) c[i] = static_cast<float>(0.5 * std::sin(kPi2 * kCentre * static_cast<double>(i) / kRate));
  CHECK(measure(c, 10).frameMs == 0);
  // Too short to look for the repeat at twice the period: none.
  CHECK(measure(framedPsk(2400, 256, 80, 1.6, 45), 2600).frameMs == 0);
}

TEST(features_of_fsk_as_it_is_really_sent) {
  // Cross-faded tones: the envelope dips at every transition, which used to
  // end the stretch of signal the clock is taken from (no rate at all on
  // the wiki's RTTY samples).
  EmissionFeatures f = measure(crossFadedRtty(45.45, 170, 0.004, 8, 51), 260);
  CHECK(f.modulation == "FSK" && near(f.toneSpacingHz, 170, 10) && near(f.symbolRate, 45.45, 1.0));
  f = measure(crossFadedRtty(50, 450, 0.004, 8, 52), 560);
  CHECK(f.modulation == "FSK" && near(f.toneSpacingHz, 450, 25) && near(f.symbolRate, 50, 1.0));
  f = measure(crossFadedRtty(75, 850, 0.003, 8, 53), 1000);
  CHECK(f.modulation == "FSK" && near(f.symbolRate, 75, 1.5));
  // A fading envelope (standard deviation about 0.3 of the mean while on):
  // still FSK, by the time it spends on its two tones.
  std::vector<float> a = mfsk(2, 400, 200, 8, 54);
  for (std::size_t i = 0; i < a.size(); ++i) a[i] *= static_cast<float>(1.0 + 0.42 * std::sin(kPi2 * 2.3 * static_cast<double>(i) / kRate));
  f = measure(a, 700);
  CHECK(f.modulation == "FSK" && near(f.toneSpacingHz, 400, 25) && near(f.symbolRate, 200, 4));
  // Fast keying on a slow false line: 200 Bd is not averaged into a third
  // level between its tones.
  f = measure(mfsk(2, 400, 200, 8, 55), 700);
  CHECK(f.modulation == "FSK" && f.tones == 2 && near(f.symbolRate, 200, 4));
}
