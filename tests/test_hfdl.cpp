// HFDL: the frame check, PDUs in words, and the decoder on this file's own
// modulator (every mode, with noise and a frequency error). Real signals:
// the Signal Identification Wiki's recordings, through tools/sigidwiki_bench.py.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/hfdl.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

// The burst with a second of silence around it, in white noise `snrDb`
// below the signal's power (over the whole 6 kHz of 12 kHz audio).
std::vector<float> burst(const std::vector<uint8_t>& pdu, int mode, double carrier, double snrDb, unsigned seed = 1) {
  const auto s = hfdlModulate(pdu, mode, 12000, carrier);
  std::vector<float> a(12000, 0.0f);
  a.insert(a.end(), s.begin(), s.end());
  a.resize(a.size() + 12000, 0.0f);
  double p = 0;
  for (float v : s) p += v * v;
  p /= static_cast<double>(s.size());
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, static_cast<float>(std::sqrt(p / std::pow(10.0, snrDb / 10.0))));
  for (auto& v : a) v += n(rng);
  return a;
}

std::vector<std::string> decode(const std::vector<float>& a, double carrier) {
  HfdlDecoder d(12000, carrier);
  std::vector<std::string> out;
  for (std::size_t i = 0; i < a.size(); i += 1000) {
    d.process(a.data() + i, std::min<std::size_t>(1000, a.size() - i));
    for (auto& s : d.take()) out.push_back(s);
  }
  return out;
}

}  // namespace

TEST(hfdl_frame_check_and_pdus_in_words) {
  // The X.25 frame check sequence: "123456789" gives 0x906E.
  const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(hfdlFcs(digits, 9) == 0x906E);
  auto sq = hfdlSquitter(7, 902, 10);
  CHECK(sq.size() == 66);
  CHECK(hfdlDescribe(sq, 300) == "squitter of ground station 7 (Shannon, Ireland), UTC synchronised, frame 902 slot 10 (300 bit/s)");
  // Any wrong bit of the header: nothing is reported.
  for (std::size_t i = 0; i < 66; ++i) {
    auto bad = sq;
    bad[i] ^= 0x10;
    CHECK(hfdlDescribe(bad, 300).empty());
  }
  CHECK(hfdlDescribe({}, 300).empty() && hfdlDescribe(std::vector<uint8_t>(66, 0xFF), 300).empty());
  // An unknown station is named by its number alone.
  CHECK(hfdlDescribe(hfdlSquitter(12, 1, 0), 300).find("ground station 12, UTC") != std::string::npos);
  const auto logon = hfdlLogon(3, 82, 0x4CA1F3);
  CHECK(hfdlDescribe(logon, 600) == "aircraft ID 82 to ground station 3 (Reykjavik, Iceland): log-on request, aircraft address 4CA1F3 (600 bit/s)");
  // The LPDU damaged, the header intact: the PDU is reported, the LPDU as failing.
  auto bad = logon;
  bad[bad.size() - 3] ^= 1;
  CHECK(hfdlDescribe(bad, 600) == "aircraft ID 82 to ground station 3 (Reykjavik, Iceland): an LPDU that fails its check (600 bit/s)");
  // Padding after the PDU (as received: the burst is longer) changes nothing.
  auto padded = sq;
  padded.resize(67, 0);
  CHECK(hfdlDescribe(padded, 300) == hfdlDescribe(sq, 300));
}

TEST(hfdl_decodes_every_mode) {
  const auto sq = hfdlSquitter(5, 823, 4);
  const auto logon = hfdlLogon(7, 98, 0xABCDEF);
  const int rates[4] = {300, 600, 1200, 1800};
  for (int mode = 0; mode < 8; ++mode) {
    // 300 bit/s holds 67 octets in a single slot: the squitter just fits.
    const auto& pdu = mode % 2 ? logon : sq;
    const auto got = decode(burst(pdu, mode, 1800, 20, 10 + static_cast<unsigned>(mode)), 1800);
    CHECK(got.size() == 1);
    if (got.size() == 1) CHECK(got[0] == hfdlDescribe(pdu, rates[mode % 4]));
    if (got.size() != 1) std::printf("    mode %d: %zu PDUs\n", mode, got.size());
  }
  // A PDU that does not fit is not modulated.
  CHECK(hfdlModulate(std::vector<uint8_t>(68, 0), 0, 12000, 1800).empty());
  CHECK(!hfdlModulate(std::vector<uint8_t>(68, 0), 1, 12000, 1800).empty());
}

TEST(hfdl_decodes_weak_and_mistuned) {
  const auto sq = hfdlSquitter(7, 902, 10);
  const std::string want = hfdlDescribe(sq, 300);
  // The squitters' mode (300 bit/s, BPSK, every coded bit twice): at 3 dB
  // in 6 kHz (about 7 dB in the signal's 2.4 kHz), and 80 Hz off either way.
  int weak = 0;
  for (unsigned seed = 1; seed <= 5; ++seed) {
    const auto got = decode(burst(sq, 0, 1800, 3, seed), 1800);
    weak += got.size() == 1 && got[0] == want;
  }
  std::printf("    300 bit/s at 3 dB in 6 kHz: %d of 5\n", weak);
  CHECK(weak >= 4);
  for (double off : {-80.0, 80.0}) {
    const auto got = decode(burst(sq, 0, 1800 + off, 15), 1800);
    CHECK(got.size() == 1 && got[0] == want);
  }
  // Two bursts in a row, at another audio rate; noise alone gives nothing.
  {
    const auto one = hfdlModulate(sq, 0, 8000, 1500), two = hfdlModulate(hfdlLogon(7, 61, 0x123456), 2, 8000, 1500);
    std::vector<float> a(4000, 0.0f);
    a.insert(a.end(), one.begin(), one.end());
    a.resize(a.size() + 3000, 0.0f);
    a.insert(a.end(), two.begin(), two.end());
    a.resize(a.size() + 8000, 0.0f);
    HfdlDecoder d(8000, 1500);
    d.process(a.data(), a.size());
    const auto got = d.take();
    CHECK(got.size() == 2 && d.decoded() == 2);
    if (got.size() == 2) CHECK(got[0] == want && got[1].find("aircraft address 123456 (1200 bit/s)") != std::string::npos);
  }
  std::mt19937 rng(3);
  std::normal_distribution<float> n(0.0f, 0.1f);
  std::vector<float> noise(12000 * 30);
  for (auto& v : noise) v = n(rng);
  CHECK(decode(noise, 1800).empty());
}
