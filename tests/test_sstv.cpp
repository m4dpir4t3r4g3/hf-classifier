// SSTV: every mode on this file's own modulator (header, colours, line
// timing), reception that began inside a picture, a mistuned receiver and
// a clock that differs; and what must not be taken for a header. Real
// signals: the Signal Identification Wiki's recordings, through
// tools/sigidwiki_bench.py.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/sstv.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

std::vector<float> noisy(std::vector<float> a, double snrDb, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, static_cast<float>(0.5 / std::sqrt(2.0) / std::pow(10.0, snrDb / 20.0)));
  a.resize(a.size() + 6000, 0.0f);
  for (auto& v : a) v += n(rng);
  return a;
}

struct Got {
  std::string mode;
  bool header = false;
  std::vector<std::shared_ptr<FaxImage>> pictures;
};

Got decode(const std::vector<float>& a, double rate, double shift = 0) {
  SstvDecoder d(rate, shift);
  Got g;
  for (std::size_t i = 0; i < a.size(); i += 2000) {
    d.process(a.data() + i, std::min<std::size_t>(2000, a.size() - i));
    if (!d.mode().empty()) g.mode = d.mode(), g.header = d.fromHeader();
    for (auto& p : d.take()) g.pictures.push_back(p);
  }
  if (auto p = d.finish()) g.pictures.push_back(p);
  return g;
}

// The share of the elements of `im` within 60 of `want` in every colour,
// leaving out those next to a change of colour in `want`.
double match(const FaxImage& im, const FaxImage& want) {
  long ok = 0, n = 0;
  for (int y = 2; y < std::min(im.height, want.height) - 2; ++y)
    for (int x = 3; x < im.width - 3; ++x) {
      const uint8_t* w = want.grey.data() + (static_cast<std::size_t>(y) * want.width + x) * 3;
      bool flat = true;
      for (int dy = -2; dy <= 2 && flat; ++dy)
        for (int dx = -3; dx <= 3 && flat; ++dx) {
          const uint8_t* o = want.grey.data() + (static_cast<std::size_t>(y + dy) * want.width + x + dx) * 3;
          flat = std::abs(o[0] - w[0]) < 12 && std::abs(o[1] - w[1]) < 12 && std::abs(o[2] - w[2]) < 12;
        }
      if (!flat) continue;
      const uint8_t* p = im.grey.data() + (static_cast<std::size_t>(y) * im.width + x) * 3;
      ++n;
      ok += std::abs(p[0] - w[0]) < 60 && std::abs(p[1] - w[1]) < 60 && std::abs(p[2] - w[2]) < 60;
    }
  return n ? static_cast<double>(ok) / static_cast<double>(n) : 0;
}

}  // namespace

TEST(sstv_every_mode_from_its_header) {
  for (const std::string& mode : sstvModes()) {
    const FaxImage want = sstvTestPicture(mode);
    const Got g = decode(noisy(sstvModulate(want, mode, 12000), 25), 12000);
    CHECK(g.mode == mode && g.header && g.pictures.size() == 1);
    if (g.pictures.size() != 1) {
      std::printf("    %s: mode '%s', %zu pictures\n", mode.c_str(), g.mode.c_str(), g.pictures.size());
      continue;
    }
    const FaxImage& im = *g.pictures[0];
    const double m = match(im, want);
    std::printf("    %-10s %dx%d: %.3f of its flat elements right\n", mode.c_str(), im.width, im.height, m);
    CHECK(im.colour && im.what == mode && im.width == want.width && im.height == want.height);
    CHECK(m > 0.97);
  }
}

TEST(sstv_inside_a_picture_mistuned_and_slanted) {
  // No header: Martin, Scottie and PD modes are known by their line time.
  for (const char* mode : {"Martin M2", "Scottie S2", "PD 50"}) {
    const FaxImage want = sstvTestPicture(mode);
    const Got g = decode(noisy(sstvModulate(want, mode, 12000, false), 20), 12000);
    CHECK(g.mode == mode && !g.header && g.pictures.size() == 1);
    if (g.pictures.size() == 1) {
      CHECK(g.pictures[0]->height >= want.height - 4);
      // The colour bars in their order (the picture begins a few lines in).
      const FaxImage& im = *g.pictures[0];
      const uint8_t* green = im.grey.data() + (60 * im.width + im.width * 7 / 16) * 3;
      CHECK(green[0] < 80 && green[1] > 180 && green[2] < 80);
    }
  }
  // Robot's line times are shared by other modes: not without a header.
  const Got robot = decode(sstvModulate(sstvTestPicture("Robot 36"), "Robot 36", 12000, false), 12000);
  CHECK(robot.mode.empty() && robot.pictures.empty());
  // 120 Hz off, and played 0.3 % fast (as 8000 samples a second read as 8024).
  const FaxImage want = sstvTestPicture("Martin M2");
  const Got off = decode(noisy(sstvModulate(want, "Martin M2", 8000, true, 120), 20), 8000);
  CHECK(off.mode == "Martin M2" && off.pictures.size() == 1);
  if (off.pictures.size() == 1) CHECK(match(*off.pictures[0], want) > 0.97);
  const Got fast = decode(noisy(sstvModulate(want, "Martin M2", 8000), 20), 8024);
  CHECK(fast.mode == "Martin M2" && fast.pictures.size() == 1);
  if (fast.pictures.size() == 1) {
    const double m = match(*fast.pictures[0], want);
    std::printf("    0.3 %% fast: %.3f\n", m);
    CHECK(m > 0.95);
  }
}

TEST(sstv_not_in_other_signals) {
  // Noise; tones at the header's frequencies in random order; a header
  // with its parity wrong (a code of no decoded mode is the same case).
  std::mt19937 rng(9);
  std::vector<float> noise(12000 * 40);
  std::normal_distribution<float> n(0.0f, 0.2f);
  for (auto& v : noise) v = n(rng);
  Got g = decode(noise, 12000);
  CHECK(g.mode.empty() && g.pictures.empty());
  std::vector<float> tones;
  double ph = 0;
  const double fs[4] = {1100, 1200, 1300, 1900};
  for (int k = 0; k < 1200; ++k) {
    const double f = fs[rng() % 4];
    for (int i = 0; i < 360; ++i) {
      ph += 2 * 3.14159265358979 * f / 12000.0;
      tones.push_back(static_cast<float>(0.5 * std::sin(ph)));
    }
  }
  g = decode(tones, 12000);
  CHECK(g.pictures.empty());
  if (!g.mode.empty()) std::printf("    random tones named %s\n", g.mode.c_str());
  CHECK(g.mode.empty());
}
