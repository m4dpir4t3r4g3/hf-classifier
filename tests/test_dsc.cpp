// GMDSS digital selective calling: the ten-unit code, a call's two copies
// and its check, the words made of it, and the decoder on audio.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/dsc.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

// A test call as coast stations answer them: selective, to 230145000, safety,
// from 002191000, test, no information, no frequencies, acknowledgement.
const std::vector<int> kTestCall = {120, 23, 1, 45, 0, 0, 108, 0, 21, 91, 0, 0, 118, 126, 126, 126, 126, 126, 126, 126, 122};

std::vector<std::string> parse(const std::vector<bool>& bits, bool invert = false) {
  DscParser p;
  std::vector<std::string> out;
  for (bool b : bits)
    if (p.bit(b != invert)) out.push_back(dscDescribe(p.call()));
  return out;
}

// 100 Bd, 170 Hz continuous-phase FSK on `centre` in 12 kHz audio; 1 is the
// higher tone unless `lowIsOne`.
std::vector<float> fsk(const std::vector<bool>& bits, double centre, bool lowIsOne, double lead = 1.0) {
  std::vector<float> a(static_cast<std::size_t>(lead * 12000), 0.0f);
  double ph = 0;
  for (bool b : bits)
    for (int i = 0; i < 120; ++i) {
      ph += 2 * 3.14159265358979 * (centre + ((b != lowIsOne) ? 85.0 : -85.0)) / 12000.0;
      a.push_back(static_cast<float>(0.3 * std::sin(ph)));
    }
  a.resize(a.size() + 6000, 0.0f);
  return a;
}

}  // namespace

TEST(dsc_ten_unit_code) {
  // Value 0 is seven B and the count 7 (111); value 1 has six B (110).
  CHECK(dscEncode(0) == 0x380 && dscEncode(1) == (0x001 | 0x180) && dscEncode(127) == 0x07F);
  for (int v = 0; v < 128; ++v) {
    CHECK(dscDecode(dscEncode(v)) == v);
    for (int b = 0; b < 10; ++b) CHECK(dscDecode(static_cast<uint16_t>(dscEncode(v) ^ (1 << b))) == -1);  // any one bit wrong
  }
}

TEST(dsc_calls_in_words) {
  CHECK(dscDescribe(kTestCall) == "selective call to 230145000, safety from 002191000, test, no information; acknowledgement");
  // With working frequencies (8291.0 kHz both ways), acknowledgement requested.
  CHECK(dscDescribe({120, 0, 23, 20, 0, 10, 100, 0, 50, 30, 0, 10, 109, 126, 8, 29, 10, 8, 29, 10, 117}) ==
        "selective call to 002320001, routine from 005030001, J3E telephony, no information, receive 8291.0 kHz, "
        "transmit 8291.0 kHz; acknowledgement requested");
  // A distress alert: sinking at 54 30 N 010 15 E, 12:34 UTC, telephony to follow.
  CHECK(dscDescribe({112, 21, 90, 15, 59, 10, 105, 5, 43, 0, 10, 15, 12, 34, 109, 127}) ==
        "distress alert from 219015591: sinking, position 54 30 N 010 15 E at 1234 UTC, then 109");
  CHECK(dscDescribe({116, 108, 0, 21, 91, 0, 0, 109, 126, 127}).rfind("all ships call, safety from 002191000, J3E telephony", 0) == 0);
}

TEST(dsc_parser_needs_phasing_both_copies_and_the_check) {
  const std::vector<bool> clean = dscSequence(kTestCall);
  const std::size_t dots = 200;
  auto flip = [&](std::vector<bool> bits, std::initializer_list<std::size_t> slots) {
    for (std::size_t s : slots) bits[dots + 10 * s + 3] = !bits[dots + 10 * s + 3];
    return bits;
  };
  CHECK(parse(clean).size() == 1 && parse(clean)[0] == dscDescribe(kTestCall));
  // Noise before and after changes nothing; nor the other polarity, given
  // the bits the other way round.
  std::mt19937 rng(3);
  std::vector<bool> noisy;
  for (int i = 0; i < 5000; ++i) noisy.push_back(rng() % 2);
  noisy.insert(noisy.end(), clean.begin(), clean.end());
  for (int i = 0; i < 2000; ++i) noisy.push_back(rng() % 2);
  CHECK(parse(noisy).size() == 1);
  CHECK(parse(clean, true).empty());
  // Message symbol n is in slot 12 + 2 n and again five slots on. One copy
  // of several symbols damaged: still read. Half the phasing gone too.
  CHECK(parse(flip(clean, {12 + 2 * 3, 12 + 2 * 7 + 5, 12 + 2 * 12, 0, 2, 4, 1, 3})).size() == 1);
  // Both copies of one symbol: no call. A wrong check character (both
  // copies of the symbol after the end of sequence): no call.
  CHECK(parse(flip(clean, {12 + 2 * 5, 12 + 2 * 5 + 5})).empty());
  const std::size_t ecc = kTestCall.size() + 1;  // after the two format specifiers and the rest
  std::vector<bool> wrong = clean;
  for (std::size_t s : {12 + 2 * ecc, 12 + 2 * ecc + 5}) {  // another valid symbol in its place
    const uint16_t w = dscEncode(77);
    for (int b = 0; b < 10; ++b) wrong[dots + 10 * s + static_cast<std::size_t>(b)] = (w >> b) & 1;
  }
  CHECK(parse(wrong).empty());
  // Random bits alone: nothing.
  std::vector<bool> random;
  for (int i = 0; i < 400000; ++i) random.push_back(rng() % 2);
  CHECK(parse(random).empty());
}

TEST(dsc_decoder_on_audio) {
  for (bool lowIsOne : {false, true}) {
    std::vector<float> a = fsk(dscSequence(kTestCall), 1700, lowIsOne);
    std::mt19937 rng(5);
    std::normal_distribution<float> n(0, 0.1f);  // about 10 dB in 3 kHz
    for (float& v : a) v += n(rng);
    DscDecoder d(12000, 1650);
    d.process(a.data(), a.size());
    const auto calls = d.take();
    CHECK(calls.size() == 1 && calls[0] == dscDescribe(kTestCall));
    CHECK(d.take().empty());
  }
  // Noise only: nothing.
  std::vector<float> noise(12000 * 30);
  std::mt19937 rng(6);
  std::normal_distribution<float> n(0, 0.1f);
  for (float& v : noise) v = n(rng);
  DscDecoder d(12000, 1700);
  d.process(noise.data(), noise.size());
  CHECK(d.take().empty());
}
