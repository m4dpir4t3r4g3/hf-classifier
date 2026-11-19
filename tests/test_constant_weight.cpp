// The 7-bit constant-weight code (T-600): found by the weight of its words.
#include <random>
#include <vector>

#include "hfc/constant_weight.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {
// Random 7-bit words with `ones` ones each.
std::vector<bool> words(int n, int ones, std::mt19937& rng) {
  std::vector<bool> bits;
  for (int w = 0; w < n; ++w) {
    std::vector<bool> word(7, false);
    for (int k = 0; k < ones;) {
      const std::size_t at = rng() % 7;
      if (!word[at]) word[at] = true, ++k;
    }
    bits.insert(bits.end(), word.begin(), word.end());
  }
  return bits;
}
int finds(const std::vector<bool>& bits, double* share = nullptr, int* ones = nullptr) {
  ConstantWeightParser p;
  int n = 0;
  for (bool b : bits)
    if (p.bit(b)) {
      ++n;
      if (share) *share = p.share();
      if (ones) *ones = p.ones();
    }
  return n;
}
}  // namespace

TEST(constant_weight_code_is_found_by_its_words) {
  std::mt19937 rng(1);
  double share = 0;
  int ones = 0;
  // Words of four ones, at any alignment, found once; of three ones too.
  std::vector<bool> b = words(200, 4, rng);
  CHECK(finds(b, &share, &ones) == 1 && share >= 0.75 && ones == 4);
  b.insert(b.begin(), {true, false, true});
  CHECK(finds(b) == 1);
  CHECK(finds(words(200, 3, rng), &share, &ones) == 1 && ones == 3);
  // One bit in 30 wrong (a fifth of the words spoiled): still found.
  b = words(300, 4, rng);
  for (std::size_t i = 0; i < b.size(); i += 30) b[i] = !b[i];
  CHECK(finds(b) == 1);
  // Too few words: not yet.
  CHECK(finds(words(50, 4, rng)) == 0);
  // Random bits, and asynchronous 7.5-unit characters (RTTY: a start bit, 5
  // random bits, a stop of 1.5 sampled as one or two bits): never.
  std::vector<bool> random;
  for (int i = 0; i < 2000000; ++i) random.push_back(rng() & 1);
  CHECK(finds(random) == 0);
  std::vector<bool> rtty;
  for (int c = 0; c < 20000; ++c) {
    rtty.push_back(false);
    for (int k = 0; k < 5; ++k) rtty.push_back(rng() & 1);
    rtty.push_back(true);
    if (c % 2) rtty.push_back(true);
  }
  CHECK(finds(rtty) == 0);
}
