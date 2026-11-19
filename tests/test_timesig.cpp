// Time signals: DCF77 and MSF minutes on this file's own keying (every
// check), the seconds of other stations, and what must not be taken for
// either. Real signals: the Signal Identification Wiki's recordings,
// through tools/sigidwiki_bench.py (DCF77 and MSF decode there).
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/timesig.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

std::vector<std::string> decode(const std::vector<float>& a, double rate, double tone) {
  TimeSignalDecoder d(rate, tone);
  std::vector<std::string> out;
  for (std::size_t i = 0; i < a.size(); i += 1000) {
    d.process(a.data() + i, std::min<std::size_t>(1000, a.size() - i));
    for (auto& s : d.take()) out.push_back(s);
  }
  return out;
}

std::vector<float> noisy(std::vector<float> a, double snrDb, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, static_cast<float>(0.5 / std::sqrt(2.0) / std::pow(10.0, snrDb / 20.0)));
  for (auto& v : a) v += n(rng);
  return a;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
  for (const auto& x : v)
    if (x == s) return true;
  return false;
}

}  // namespace

TEST(time_codes_dcf77_and_msf) {
  // Two minutes each, starting 3.5 s into a minute (the first is partial).
  for (const char* st : {"DCF77", "MSF"}) {
    auto lv = timeCodeLevels(st, 2026, 11, 15, 23, 59, false, 3);
    lv.erase(lv.begin(), lv.begin() + 350);
    const auto got = decode(noisy(timeSignalAudio(lv, 12000, 1000), 15), 12000, 1000);
    for (const auto& g : got) std::printf("    %s\n", g.c_str());
    CHECK(has(got, "time signal: second pulses (exactly 1 s apart)"));
    // 23:59 is partial; midnight: the next day, and its weekday.
    const std::string zone = std::string(st) == "DCF77" ? "CET" : "UTC";
    CHECK(has(got, std::string(st) + " time code: 2026-11-16 (Monday) 00:00 " + zone));
    CHECK(!has(got, std::string(st) + " time code: 2026-11-15 (Sunday) 23:59 " + zone));
  }
  // Summer time, a leap day (the first minute is never decoded: no mark before it).
  const auto got = decode(timeSignalAudio(timeCodeLevels("DCF77", 2028, 2, 29, 12, 30, true, 2), 12000, 1000), 12000, 1000);
  CHECK(has(got, "DCF77 time code: 2028-02-29 (Tuesday) 12:31 CEST") && !has(got, "DCF77 time code: 2028-02-29 (Tuesday) 12:30 CEST"));
  // Any one bit wrong (a second's pulse long instead of short): that
  // minute is not reported.
  for (int sec : {22, 30, 40, 52}) {
    auto lv = timeCodeLevels("DCF77", 2026, 11, 15, 9, 59, false, 3);
    const std::size_t at = static_cast<std::size_t>(60 + sec) * 100;
    const bool one = lv[at + 15] < 0.5f;
    for (int k = 10; k < 20; ++k) lv[at + static_cast<std::size_t>(k)] = one ? 1.0f : 0.15f;
    const auto bad = decode(timeSignalAudio(lv, 12000, 1000), 12000, 1000);
    CHECK(!has(bad, "DCF77 time code: 2026-11-15 (Sunday) 10:00 CET"));
    CHECK(has(bad, "DCF77 time code: 2026-11-15 (Sunday) 10:01 CET"));
  }
}

TEST(time_seconds_and_not_other_signals) {
  // A station that only ticks: 5 ms pulses each second over 0.5 s of 1 kHz tone.
  std::vector<float> lv;
  for (int s = 0; s < 30; ++s)
    for (int k = 0; k < 100; ++k) lv.push_back(k < 3 ? 1.0f : 0.2f);
  auto got = decode(noisy(timeSignalAudio(lv, 8000, 800), 10), 8000, 800);
  CHECK(got.size() == 1 && has(got, "time signal: second pulses (exactly 1 s apart)"));
  // Morse at random, a steady carrier, noise: not time signals.
  std::mt19937 rng(4);
  lv.clear();
  for (int i = 0; i < 4000; ++i) {
    const int len = (rng() % 3 == 0) ? 18 : 6;
    for (int k = 0; k < len; ++k) lv.push_back(1.0f);
    for (int k = 0; k < 6 + static_cast<int>(rng() % 3) * 6; ++k) lv.push_back(0.0f);
  }
  lv.resize(6000);
  CHECK(decode(timeSignalAudio(lv, 8000, 800), 8000, 800).empty());
  CHECK(decode(timeSignalAudio(std::vector<float>(6000, 1.0f), 8000, 800), 8000, 800).empty());
  std::vector<float> noise(8000 * 60);
  std::normal_distribution<float> n(0.0f, 0.2f);
  for (auto& v : noise) v = n(rng);
  CHECK(decode(noise, 8000, 800).empty());
}
