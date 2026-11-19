// Radiofax: the decoder on this file's own modulator (line rate, black and
// white, line start, the picture), what it must not take for a fax, and the
// PNG writer (inflated here and compared). Real signals: the Signal
// Identification Wiki's recordings, through tools/sigidwiki_bench.py.
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "hfc/fax.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

std::vector<float> noisy(std::vector<float> a, double snrDb, unsigned seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> n(0.0f, static_cast<float>(0.5 / std::sqrt(2.0) / std::pow(10.0, snrDb / 20.0)));
  for (auto& v : a) v += n(rng);
  return a;
}

struct Got {
  bool recognised = false;
  int lpm = 0;
  std::vector<std::shared_ptr<FaxImage>> pictures;
};

Got decode(const std::vector<float>& a, double rate, double centre) {
  FaxDecoder d(rate, centre);
  Got g;
  for (std::size_t i = 0; i < a.size(); i += 2000) {
    d.process(a.data() + i, std::min<std::size_t>(2000, a.size() - i));
    for (auto& p : d.take()) g.pictures.push_back(p);
  }
  g.recognised = d.recognised();
  g.lpm = d.linesPerMinute();
  if (auto p = d.finish()) g.pictures.push_back(p);
  return g;
}

// The share of row `y` of `im` within 70 grey levels of row `cy` of `chart`.
double rowMatch(const FaxImage& im, int y, const FaxImage& chart, int cy) {
  int ok = 0;
  for (int x = 0; x < im.width; ++x) {
    const int cx = std::min(chart.width - 1, x * chart.width / im.width);
    ok += std::abs(im.grey[static_cast<std::size_t>(y) * im.width + x] - chart.grey[static_cast<std::size_t>(cy) * chart.width + cx]) < 70;
  }
  return static_cast<double>(ok) / im.width;
}

// Inflates a zlib stream of one block with the fixed Huffman codes, as
// faxPng writes it; empty on anything else.
std::vector<uint8_t> inflateFixed(const std::string& z) {
  std::size_t at = 16;  // bits, after the two zlib octets
  auto bit = [&]() -> int {
    if (at / 8 >= z.size()) return -1;
    const int b = (static_cast<uint8_t>(z[at / 8]) >> (at % 8)) & 1;
    ++at;
    return b;
  };
  auto bits = [&](int n) {
    int v = 0;
    for (int i = 0; i < n; ++i) v |= bit() << i;
    return v;
  };
  if (bits(1) != 1 || bits(2) != 1) return {};
  static const int base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const int extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  std::vector<uint8_t> out;
  for (;;) {
    int code = 0;
    for (int i = 0; i < 7; ++i) code = code << 1 | bit();
    int sym;
    if (code <= 0x17) sym = 256 + code;
    else {
      code = code << 1 | bit();
      if (code >= 0x30 && code <= 0xBF) sym = code - 0x30;
      else if (code >= 0xC0 && code <= 0xC7) sym = 280 + code - 0xC0;
      else sym = 144 + ((code << 1 | bit()) - 0x190);
    }
    if (sym < 256) out.push_back(static_cast<uint8_t>(sym));
    else if (sym == 256) break;
    else {
      if (sym > 285 || out.empty()) return {};
      const int len = base[sym - 257] + bits(extra[sym - 257]);
      if (bits(5) != 0) return {};  // only distance 1
      out.insert(out.end(), static_cast<std::size_t>(len), out.back());
    }
    if (at / 8 > z.size()) return {};
  }
  return out;
}

}  // namespace

TEST(fax_picture_from_a_whole_transmission) {
  // Start tone, phasing lines, a chart of 100 lines, stop tone; 15 dB.
  const FaxImage chart = faxTestChart(100);
  const Got g = decode(noisy(faxModulate(chart, 12000, true), 15), 12000, 1900);
  CHECK(g.recognised && g.lpm == 120);
  CHECK(g.pictures.size() == 1);
  if (g.pictures.size() != 1) return;
  const FaxImage& im = *g.pictures[0];
  std::printf("    %dx%d, IOC %d, phased %d\n", im.width, im.height, im.ioc, im.phased);
  // The 20 phasing lines, then the chart: ended by the stop tone.
  CHECK(im.width == 1810 && im.ioc == 576 && im.phased && im.height >= 118 && im.height <= 124);
  // Chart rows where they belong, the line start within a few elements
  // (the diagonal's place differs on every row: a wrong rate or start fails).
  double worst = 1;
  // (The picture begins within a line of the start tone's end.)
  int first = 20;
  for (int d : {19, 21})
    if (rowMatch(im, d + 50, chart, 50) > rowMatch(im, first + 50, chart, 50)) first = d;
  for (int cy = 10; cy < 90; cy += 10) worst = std::min(worst, rowMatch(im, first + cy, chart, cy));
  std::printf("    worst row: %.3f of its elements right\n", worst);
  CHECK(worst > 0.97);
}

TEST(fax_from_the_middle_mistuned_and_at_another_rate) {
  // No tones (reception began inside the chart), the receiver 300 Hz off,
  // 8 kHz audio: recognised, black and white found by themselves.
  const FaxImage chart = faxTestChart(90);
  for (double shift : {-300.0, 300.0}) {
    const Got g = decode(noisy(faxModulate(chart, 8000, false, shift), 20), 8000, 1900);
    CHECK(g.recognised && g.lpm == 120 && g.pictures.size() == 1);
    if (g.pictures.size() != 1) continue;
    const FaxImage& im = *g.pictures[0];
    CHECK(im.height >= 88 && im.height <= 90);
    // The grey field is grey, the white white, whatever the tuning.
    int grey = 0, white = 0;
    for (int y = 20; y < 60; ++y) {
      grey += std::abs(im.grey[static_cast<std::size_t>(y) * im.width + 1450] - 128) < 50;
      white += im.grey[static_cast<std::size_t>(y) * im.width + 1000] > 200;
    }
    CHECK(grey >= 36 && white >= 30);
  }
  // 60 lines a minute.
  FaxImage slow = faxTestChart(40);
  slow.linesPerMinute = 60;
  const Got g = decode(faxModulate(slow, 12000, false), 12000, 1900);
  CHECK(g.recognised && g.lpm == 60 && g.pictures.size() == 1);
}

TEST(fax_is_not_seen_in_other_signals) {
  // Noise; a two-tone FSK with random data 800 Hz apart (the same two
  // frequencies as black and white); a steady tone.
  std::mt19937 rng(5);
  std::vector<float> noise(12000 * 60);
  std::normal_distribution<float> n(0.0f, 0.2f);
  for (auto& v : noise) v = n(rng);
  CHECK(!decode(noise, 12000, 1900).recognised);
  for (double baud : {50.0, 75.0, 100.0, 200.0}) {
    std::vector<float> fsk;
    double ph = 0;
    for (int b = 0; b < static_cast<int>(60 * baud); ++b) {
      const double f = rng() & 1 ? 2300.0 : 1500.0;
      for (int i = 0; i < static_cast<int>(12000 / baud); ++i) {
        ph += 2 * 3.14159265358979 * f / 12000.0;
        fsk.push_back(static_cast<float>(0.5 * std::sin(ph)));
      }
    }
    const Got g = decode(noisy(fsk, 20), 12000, 1900);
    CHECK(!g.recognised && g.pictures.empty());
  }
  std::vector<float> tone(12000 * 40);
  for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * 1900.0 * i / 12000.0));
  CHECK(!decode(noisy(tone, 20), 12000, 1900).recognised);
}

TEST(fax_png) {
  const FaxImage chart = faxTestChart(50);
  const std::string png = faxPng(chart);
  CHECK(png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
  // IHDR: 13 octets, the size, 8 bits grey.
  CHECK(png.compare(12, 4, "IHDR") == 0 && static_cast<uint8_t>(png[18]) == 0x07 && static_cast<uint8_t>(png[19]) == 0x12);  // 1810
  CHECK(static_cast<uint8_t>(png[23]) == 50 && png[24] == 8 && png[25] == 0);
  CHECK(png.compare(png.size() - 8, 4, "IEND") == 0);
  // The data inflates to the rows (each after a filter octet 2: the
  // difference from the row above) of the chart, and is far smaller.
  const std::size_t idat = png.find("IDAT");
  CHECK(idat != std::string::npos);
  const std::size_t len = (static_cast<uint8_t>(png[idat - 4]) << 24) | (static_cast<uint8_t>(png[idat - 3]) << 16) |
                          (static_cast<uint8_t>(png[idat - 2]) << 8) | static_cast<uint8_t>(png[idat - 1]);
  const auto raw = inflateFixed(png.substr(idat + 4, len));
  CHECK(raw.size() == 50u * 1811u);
  bool same = raw.size() == 50u * 1811u;
  for (int y = 0; y < 50 && same; ++y) {
    same = raw[static_cast<std::size_t>(y) * 1811] == 2;
    for (int x = 0; x < 1810 && same; ++x) {
      const uint8_t above = y ? chart.grey[static_cast<std::size_t>(y - 1) * 1810 + x] : 0;
      same = static_cast<uint8_t>(raw[static_cast<std::size_t>(y) * 1811 + 1 + x] + above) == chart.grey[static_cast<std::size_t>(y) * 1810 + x];
    }
  }
  CHECK(same);
  std::printf("    %zu octets for %d\n", png.size(), 50 * 1810);
  CHECK(png.size() < 50u * 1810u / 20);
}
