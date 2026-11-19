// Akula's structure: the published groups, bit errors, word alignment.
#include <random>
#include <string>
#include <vector>

#include "hfc/akula.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

std::vector<bool> frame(int words, std::mt19937& rng, bool sync = true) {
  std::vector<bool> bits;
  for (int i = 0; i < 60; ++i) bits.push_back(i % 2 == 0);
  if (sync)
    for (bool b : akulaSync()) bits.push_back(b);
  for (int i = 0; i < 6 * words; ++i) bits.push_back(rng() % 2);
  for (bool b : akulaEnd()) bits.push_back(b);
  return bits;
}

std::vector<AkulaParser::Burst> parse(const std::vector<bool>& bits) {
  AkulaParser p;
  std::vector<AkulaParser::Burst> out;
  for (bool b : bits)
    if (p.bit(b)) out.push_back(p.burst());
  return out;
}

}  // namespace

TEST(akula_groups_and_alignment) {
  CHECK(akulaSync().size() == 36 && akulaEnd().size() == 30);
  std::mt19937 rng(1);
  // Sync, 142 words, end: 147 words from the end of sync to the end of EOT.
  std::vector<bool> f = frame(142, rng);
  auto b = parse(f);
  CHECK(b.size() == 1 && b[0].sync && b[0].words == 147 && b[0].syncErrors == 0 && b[0].endErrors == 0);
  CHECK(AkulaParser::describe(b[0]) ==
        "burst with Akula's sync and end groups, 147 six-bit words between them; content encrypted, not shown");
  // Three bits of the sync and two of the end wrong: still found, and said.
  std::vector<bool> e = f;
  for (std::size_t i : {60u + 2, 60u + 17, 60u + 30}) e[i] = !e[i];
  for (std::size_t i : {e.size() - 3, e.size() - 20}) e[i] = !e[i];
  b = parse(e);
  CHECK(b.size() == 1 && b[0].sync && b[0].syncErrors == 3 && b[0].endErrors == 2);
  CHECK(AkulaParser::describe(b[0]).find("(5 bits of the groups wrong)") != std::string::npos);
  // A bit lost between the groups (not a whole number of words apart), or
  // no sync group at all (a data-only burst): not reported; the end group
  // alone turns up in other systems' data.
  std::vector<bool> slipped = f;
  slipped.erase(slipped.begin() + 300);
  CHECK(parse(slipped).empty());
  CHECK(parse(frame(154, rng, false)).empty());
  // The other polarity: nothing. Random bits (a day of 500 Bd): nothing with a sync.
  std::vector<bool> inv = f;
  inv.flip();
  CHECK(parse(inv).empty());
  AkulaParser p;
  int found = 0;
  for (int i = 0; i < 43200000; ++i) found += p.bit(rng() & 1) ? 1 : 0;
  CHECK(found == 0);
}
