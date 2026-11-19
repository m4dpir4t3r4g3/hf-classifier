// WSPR: messages through this file's own modulator (weak, off frequency,
// late, two at once), what cannot be packed, and noise. Real signals: the
// Signal Identification Wiki's two recordings, through
// tools/sigidwiki_bench.py (both decode).
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/wspr.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

// White noise for an SNR (in 2500 Hz) for a signal of amplitude 0.5.
void addNoise(std::vector<float>& a, double snrDb, unsigned seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, 1.0f);
  const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) / 2500.0 * 6000.0);
  for (auto& v : a) v += static_cast<float>(sigma) * n(rng);
}

bool has(const std::vector<WsprMessage>& v, const std::string& text) {
  for (const auto& m : v)
    if (m.text() == text) return true;
  return false;
}

}  // namespace

TEST(wspr_packing) {
  CHECK(wsprTones("K1ABC", "FN42", 37).size() == 162);
  // The sync vector is on the low bit of every tone; tones are 0 to 3.
  const auto t = wsprTones("K1ABC", "FN42", 37);
  CHECK(t[0] % 2 == 1 && t[1] % 2 == 1 && t[2] % 2 == 0);
  for (int v : t) CHECK(v >= 0 && v <= 3);
  // Not packable: a power that is not 0/3/7 in its last digit, a locator
  // off the grid, a callsign without its digit in place, too long.
  CHECK(wsprTones("K1ABC", "FN42", 36).empty());
  CHECK(wsprTones("K1ABC", "ZZ42", 37).empty());
  CHECK(wsprTones("KABCD", "FN42", 37).empty());
  CHECK(wsprTones("K1ABCDE", "FN42", 37).empty());
  // One-letter prefixes are moved right ("K1ABC" is " K1ABC").
  CHECK(wsprTones("G4JNT", "IO90", 30) != wsprTones("K1ABC", "FN42", 37));
}

TEST(wspr_decodes_weak_offset_and_two_at_once) {
  for (double snr : {-10.0, -22.0, -25.0}) {
    auto a = wsprAudio(wsprTones("K1ABC", "FN42", 37), 1532.0);
    addNoise(a, snr, 3);
    const auto got = wsprDecode(a);
    std::printf("    %.0f dB: %zu decodes%s\n", snr, got.size(), got.empty() ? "" : (", " + got[0].text()).c_str());
    CHECK(got.size() == 1 && has(got, "K1ABC FN42 37 dBm"));
    if (!got.empty()) CHECK(std::fabs(got[0].audioHz - 1532.0) < 1.0 && std::fabs(got[0].dtSeconds - 1.0) < 0.2);
  }
  // Two stations 60 Hz apart, the second 2 s late and 10 dB weaker.
  auto a = wsprAudio(wsprTones("DL1XYZ", "JO62", 23), 1440.0);
  auto b = wsprAudio(wsprTones("VK2ABC", "QF56", 60), 1500.0, 12000, 122);
  for (std::size_t i = 24000; i < a.size(); ++i) a[i] += 0.3f * b[i - 24000];
  addNoise(a, -15, 5);
  const auto got = wsprDecode(a);
  for (const auto& m : got) std::printf("    %s at %.1f Hz, %.1f s\n", m.text().c_str(), m.audioHz, m.dtSeconds);
  CHECK(has(got, "DL1XYZ JO62 23 dBm") && has(got, "VK2ABC QF56 60 dBm") && got.size() == 2);
}

TEST(wspr_nothing_in_noise_or_a_carrier) {
  std::vector<float> a(12000 * 120, 0.0f);
  addNoise(a, -10, 7);
  CHECK(wsprDecode(a).empty());
  for (std::size_t i = 0; i < a.size(); ++i) a[i] += static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 1500.0 * i / 12000.0));
  CHECK(wsprDecode(a).empty());
  // Too short a slot: nothing, and no reading past its end.
  std::vector<float> shortSlot(12000 * 60, 0.0f);
  CHECK(wsprDecode(shortSlot).empty());
}
