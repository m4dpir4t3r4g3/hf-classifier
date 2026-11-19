#include "hfc/fax.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace hfc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRates[4] = {240, 120, 90, 60};  // lines per minute, the fastest first
constexpr double kMaxSeconds = 30 * 60;

int widthOf(int ioc) { return static_cast<int>(std::lround(kPi * ioc)); }

// --- PNG ---

uint32_t crc32(const uint8_t* p, std::size_t n, uint32_t crc = 0) {
  static const struct Table {
    uint32_t t[256];
    Table() {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
    }
  } tab;
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = tab.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// Deflate with the fixed Huffman codes; the only matches are runs of one
// byte (distance 1).
class Deflater {
 public:
  explicit Deflater(std::string& out) : out_(out) {
    bits(1, 1);  // the last block
    bits(1, 2);  // fixed Huffman codes
  }
  void data(const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n;) {
      std::size_t run = 1;
      while (i + run < n && p[i + run] == p[i] && run < 259) ++run;
      if (run >= 5) {
        literal(p[i]);
        length(static_cast<int>(run - 1));
        i += run;
      } else {
        literal(p[i]);
        ++i;
      }
    }
  }
  void finish() {
    code(0, 7);  // end of block (symbol 256)
    if (count_) out_.push_back(static_cast<char>(acc_));
  }

 private:
  // Extra bits and headers go in lowest bit first; Huffman codes highest first.
  void bits(uint32_t v, int n) {
    for (int i = 0; i < n; ++i) {
      acc_ |= ((v >> i) & 1) << count_;
      if (++count_ == 8) {
        out_.push_back(static_cast<char>(acc_));
        acc_ = 0;
        count_ = 0;
      }
    }
  }
  void code(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i) bits((v >> i) & 1, 1);
  }
  void symbol(int s) {
    if (s < 144) code(0x30 + static_cast<uint32_t>(s), 8);
    else if (s < 256) code(0x190 + static_cast<uint32_t>(s - 144), 9);
    else if (s < 280) code(static_cast<uint32_t>(s - 256), 7);
    else code(0xC0 + static_cast<uint32_t>(s - 280), 8);
  }
  void literal(uint8_t b) { symbol(b); }
  // A run of `len` (3..258) more copies of the previous byte.
  void length(int len) {
    static const int base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int k = 28;
    while (base[k] > len) --k;
    symbol(257 + k);
    bits(static_cast<uint32_t>(len - base[k]), extra[k]);
    code(0, 5);  // distance 1
  }
  std::string& out_;
  uint32_t acc_ = 0;
  int count_ = 0;
};

void be32(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>(v >> (8 * i)));
}

void chunk(std::string& png, const char* type, const std::string& body) {
  be32(png, static_cast<uint32_t>(body.size()));
  std::string t(type, 4);
  t += body;
  png += t;
  be32(png, crc32(reinterpret_cast<const uint8_t*>(t.data()), t.size()));
}

}  // namespace

std::string faxPng(const FaxImage& im) {
  std::string png("\x89PNG\r\n\x1a\n", 8), head, idat;
  be32(head, static_cast<uint32_t>(im.width));
  be32(head, static_cast<uint32_t>(im.height));
  head += std::string(im.colour ? "\x08\x02\x00\x00\x00" : "\x08\x00\x00\x00\x00", 5);  // 8 bits, RGB or grey, deflate, no interlace
  chunk(png, "IHDR", head);
  // Every row as its difference from the row above (filter 2): a chart's
  // rows are mostly equal to it, so the runs are long.
  idat += "\x78\x01";
  Deflater z(idat);
  const std::size_t stride = static_cast<std::size_t>(im.width) * (im.colour ? 3 : 1);
  std::vector<uint8_t> row(stride + 1);
  uint32_t a = 1, b = 0;  // Adler-32 of the uncompressed bytes
  for (int y = 0; y < im.height; ++y) {
    row[0] = 2;
    const uint8_t* cur = im.grey.data() + static_cast<std::size_t>(y) * stride;
    for (std::size_t x = 0; x < stride; ++x) row[x + 1] = static_cast<uint8_t>(cur[x] - (y ? *(cur + x - stride) : 0));
    for (uint8_t v : row) {
      a = (a + v) % 65521;
      b = (b + a) % 65521;
    }
    z.data(row.data(), row.size());
  }
  z.finish();
  be32(idat, b << 16 | a);
  chunk(png, "IDAT", idat);
  chunk(png, "IEND", "");
  return png;
}

// ---------------------------------------------------------------- decoder --

struct FaxDecoder::Impl {
  double rate, centre;
  int decim;          // input samples per stored frequency sample
  double fs;          // stored samples per second
  std::vector<float> fir;
  std::vector<std::complex<float>> hist;  // the mixer's output, for the filter
  std::size_t histAt = 0;
  double phase = 0;
  std::complex<float> prev{1, 0};
  double accF = 0;
  int accN = 0;
  // The level: per second, how much it varies within the second (standard
  // deviation over mean), for the last 20 s. A fax is a steady carrier.
  double ampSum = 0, ampSq = 0;
  int ampN = 0;
  std::vector<float> ripple;
  // The frequency, in Hz, from `base` stored samples after the start.
  std::vector<int16_t> f;
  uint64_t base = 0;
  uint64_t analysed = 0;     // stored samples up to which the analysis ran
  bool recognised = false;
  int lpm = 0, ioc = 576;
  double black = 0, white = 0;
  uint64_t pictureStart = 0;  // absolute stored-sample index
  // Tones: seconds in a row, and where the run began.
  int toneKind = 0, toneRun = 0;
  uint64_t toneStart = 0;
  bool toneActed = false;
  std::vector<std::shared_ptr<FaxImage>> done;

  Impl(double r, double c) : rate(r), centre(c) {
    decim = std::max(1, static_cast<int>(std::lround(r / 4000.0)));
    fs = r / decim;
    const int half = std::max(8, static_cast<int>(r / 300.0));
    for (int k = -half; k <= half; ++k) {
      const double x = 2.0 * 900.0 / r * k;
      const double sinc = k == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      fir.push_back(static_cast<float>(sinc * (0.54 + 0.46 * std::cos(kPi * k / half))));
    }
    float sum = 0;
    for (float v : fir) sum += v;
    for (float& v : fir) v /= sum;
    hist.assign(fir.size(), {0, 0});
  }

  uint64_t end() const { return base + f.size(); }
  int16_t at(uint64_t i) const { return f[static_cast<std::size_t>(i - base)]; }

  // Black and white: the 3rd and 97th percentile of [from, to).
  bool levels(uint64_t from, uint64_t to, double& lo, double& hi, double* span98 = nullptr) const {
    const double origin = centre - 1500.0;
    std::vector<int> h(150, 0);  // 20 Hz bins
    int n = 0;
    for (uint64_t i = from; i < to; i += 4) {
      const int bin = static_cast<int>((at(i) - origin) / 20.0);
      if (bin >= 0 && bin < 150) ++h[static_cast<std::size_t>(bin)], ++n;
    }
    if (n < 100) return false;
    if (span98) {
      // From the 1st to the 99th percentile: SSTV's sync pulses, 300 Hz
      // below its black, are 1 to 6 % of it.
      int acc1 = 0, b1 = -1, b99 = 149;
      for (int b = 0; b < 150; ++b) {
        acc1 += h[static_cast<std::size_t>(b)];
        if (b1 < 0 && acc1 >= 0.01 * n) b1 = b;
        if (acc1 >= 0.99 * n) {
          b99 = b;
          break;
        }
      }
      *span98 = 20.0 * (b99 - b1);
    }
    int acc = 0;
    lo = hi = 0;
    bool gotLo = false;
    for (int b = 0; b < 150; ++b) {
      acc += h[static_cast<std::size_t>(b)];
      if (!gotLo && acc >= 0.03 * n) lo = origin + 20.0 * b + 10, gotLo = true;
      if (acc >= 0.97 * n) {
        hi = origin + 20.0 * b + 10;
        break;
      }
    }
    return true;
  }

  // The normalised autocorrelation of the frequency over [from, to) at `lag`.
  double repeat(uint64_t from, uint64_t to, double lagSamples) const {
    const uint64_t lag = static_cast<uint64_t>(std::llround(lagSamples));
    if (to <= from + lag + 16) return 0;
    double mean = 0;
    int n = 0;
    for (uint64_t i = from; i < to; i += 4) mean += at(i), ++n;
    mean /= n;
    double c = 0, v = 0;
    for (uint64_t i = from; i + lag < to; i += 4) {
      const double a = at(i) - mean, b = at(i + lag) - mean;
      c += a * b;
      v += 0.5 * (a * a + b * b);
    }
    return v > 0 ? c / v : 0;
  }

  // How much of the last second is a tone of black and white alternating
  // at `hz` (the fundamental of a square wave holds 81 % of its power).
  double tone(uint64_t from, uint64_t to, double hz) const {
    double mean = 0;
    const double n = static_cast<double>(to - from);
    for (uint64_t i = from; i < to; ++i) mean += at(i);
    mean /= n;
    double re = 0, im = 0, var = 0;
    for (uint64_t i = from; i < to; ++i) {
      const double v = at(i) - mean, ph = 2.0 * kPi * hz * static_cast<double>(i - from) / fs;
      re += v * std::cos(ph);
      im += v * std::sin(ph);
      var += v * v;
    }
    if (var / n < 200.0 * 200.0) return 0;  // not swinging between black and white
    return 2.0 * (re * re + im * im) / (n * var);
  }

  double lineSamples() const { return 60.0 / lpm * fs; }

  // The line's true length in stored samples: the transmitter's clock and
  // the receiver's (or a recording's) differ, and one part in a thousand
  // slants a chart by a line's width in 500 lines. Found from how the
  // signal repeats 1, then 10, then 100 lines later, each within what the
  // step before leaves open (up to 1 % in all).
  double trueLine(uint64_t from, uint64_t to) const {
    double L = lineSamples();
    for (int lines : {1, 10, 100}) {
      const double nominal = L * lines;
      if (static_cast<double>(to - from) < 2.2 * nominal) break;
      const int reach = lines == 1 ? static_cast<int>(0.01 * L) + 1 : 12;
      double best = -1, second = -1;
      int bestD = 0;
      const uint64_t lag0 = static_cast<uint64_t>(std::llround(nominal));
      double mean = 0;
      int n = 0;
      for (uint64_t i = from; i < to; i += 8) mean += at(i), ++n;
      mean /= n;
      std::vector<double> c(static_cast<std::size_t>(2 * reach + 1));
      for (int d = -reach; d <= reach; ++d) {
        const uint64_t lag = lag0 + static_cast<uint64_t>(d + reach) - static_cast<uint64_t>(reach);
        double acc = 0, var = 1e-9;
        for (uint64_t i = from; i + lag < to; i += 8) {
          const double a = at(i) - mean, b = at(i + lag) - mean;
          acc += a * b;
          var += 0.5 * (a * a + b * b);
        }
        c[static_cast<std::size_t>(d + reach)] = acc / var;
        if (acc / var > best) second = best, best = acc / var, bestD = d;
      }
      (void)second;
      if (best < 0.25) break;  // too little repeats that far apart: keep what the shorter step gave
      // The peak between the samples (a parabola through its neighbours).
      double frac = 0;
      const std::size_t k = static_cast<std::size_t>(bestD + reach);
      if (k > 0 && k + 1 < c.size()) {
        const double den = c[k - 1] - 2 * c[k] + c[k + 1];
        if (den < 0) frac = std::clamp(0.5 * (c[k - 1] - c[k + 1]) / den, -0.5, 0.5);
      }
      L = (static_cast<double>(lag0) + bestD + frac) / lines;
    }
    return L;
  }

  std::shared_ptr<FaxImage> picture(uint64_t from, uint64_t to) const {
    if (!recognised || to <= from) return nullptr;
    const double L = trueLine(from, to);
    const int rows = static_cast<int>(static_cast<double>(to - from) / L);
    if (rows < 20) return nullptr;
    auto im = std::make_shared<FaxImage>();
    im->width = widthOf(ioc);
    im->height = rows;
    im->linesPerMinute = lpm;
    im->ioc = ioc;
    double lo = black, hi = white;
    if (!(hi - lo >= 250 && hi - lo <= 1050)) lo = centre - 400, hi = centre + 400;
    // Where a line begins: from phasing lines among the first 90. A phasing
    // line is nearly all one shade with one short pulse of the other.
    double offset = 0;
    {
      std::vector<double> at01;
      const int bins = 1000;
      for (int r = 0; r < std::min(rows, 90); ++r) {
        std::vector<uint8_t> w(bins);
        int white01 = 0;
        for (int b = 0; b < bins; ++b) {
          const uint64_t i = from + static_cast<uint64_t>(r * L + (b + 0.5) * L / bins);
          w[static_cast<std::size_t>(b)] = at(i) > 0.5 * (lo + hi);
          white01 += w[static_cast<std::size_t>(b)];
        }
        const int minority = std::min(white01, bins - white01);
        if (minority < 0.02 * bins || minority > 0.12 * bins) continue;
        const uint8_t pulse = white01 < bins / 2 ? 1 : 0;
        // The pulse's bins must lie together (around the circle of the line).
        double sx = 0, sy = 0;
        for (int b = 0; b < bins; ++b)
          if (w[static_cast<std::size_t>(b)] == pulse) sx += std::cos(2 * kPi * b / bins), sy += std::sin(2 * kPi * b / bins);
        if (std::hypot(sx, sy) / minority < 0.9) continue;
        double a = std::atan2(sy, sx) / (2 * kPi);
        if (a < 0) a += 1;
        at01.push_back(a);
      }
      if (at01.size() >= 5) {
        double sx = 0, sy = 0;
        for (double a : at01) sx += std::cos(2 * kPi * a), sy += std::sin(2 * kPi * a);
        if (std::hypot(sx, sy) / static_cast<double>(at01.size()) > 0.9) {
          double a = std::atan2(sy, sx) / (2 * kPi);
          if (a < 0) a += 1;
          offset = a * L;  // the pulse's middle is where a line begins
          im->phased = true;
        }
      }
    }
    const int usable = static_cast<int>((static_cast<double>(to - from) - offset) / L);
    im->height = usable;
    im->grey.resize(static_cast<std::size_t>(im->width) * static_cast<std::size_t>(usable));
    for (int r = 0; r < usable; ++r)
      for (int x = 0; x < im->width; ++x) {
        const double pos = static_cast<double>(from) + offset + r * L + (x + 0.5) * L / im->width;
        const uint64_t i = std::min<uint64_t>(static_cast<uint64_t>(pos), to - 1);
        const double g = (at(i) - lo) / (hi - lo);
        im->grey[static_cast<std::size_t>(r) * static_cast<std::size_t>(im->width) + static_cast<std::size_t>(x)] =
            static_cast<uint8_t>(std::clamp(g, 0.0, 1.0) * 255.0 + 0.5);
      }
    return usable >= 20 ? im : nullptr;
  }

  // Ends the picture at `to`; the next begins at `next`.
  void complete(uint64_t to, uint64_t next) {
    if (auto im = picture(pictureStart, to)) done.push_back(im);
    pictureStart = next;
    if (next > base) {
      const uint64_t drop = std::min<uint64_t>(next - base, f.size());
      f.erase(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(drop));
      base += drop;
    }
  }

  void analyse() {
    const uint64_t second = static_cast<uint64_t>(fs);
    while (analysed + second <= end()) {
      const uint64_t from = analysed, to = analysed + second;
      analysed = to;
      // Tones, second by second.
      int kind = 0;
      if (tone(from, to, 300) > 0.55) kind = 300;
      else if (tone(from, to, 675) > 0.55) kind = 675;
      else if (tone(from, to, 450) > 0.55) kind = 450;
      if (kind && kind == toneKind) {
        ++toneRun;
      } else {
        if (toneKind && toneActed && toneKind != 450) {
          // A start tone has ended: the picture (its phasing lines first)
          // begins here.
          if (recognised && from > pictureStart + static_cast<uint64_t>(30 * fs)) complete(toneStart, from);
          else pictureStart = from;
        }
        toneKind = kind;
        toneRun = kind ? 1 : 0;
        toneStart = from;
        toneActed = false;
      }
      if (toneRun >= 2 && !toneActed) {
        toneActed = true;
        if (toneKind == 450) complete(toneStart, to);
        else ioc = toneKind == 675 ? 288 : 576;
      }
      // The line rate, every 2 s over the last 20 s.
      const uint64_t window = static_cast<uint64_t>(20 * fs);
      if (to % (2 * second) == 0 && to >= base + window && to >= pictureStart + window) {
        const uint64_t a = to - window;
        double lo = 0, hi = 0;
        // Not a fax: a level that goes up and down within the second (a
        // keyed carrier, a time signal's pulses), or a frequency that
        // goes well below black (the sync pulses of SSTV).
        double rip = 0;
        for (float v : ripple) rip += v;
        rip /= std::max<std::size_t>(1, ripple.size());
        int below = 0, count = 0;
        double span = 0;
        const bool lev = levels(a, to, lo, hi, &span) && span <= 1050;
        if (lev)
          for (uint64_t i = a; i < to; i += 4, ++count) below += at(i) < lo - 200;
        const bool steady = ripple.size() >= 15 && rip < 0.35 && lev && below < 0.006 * count;
        if (steady && hi - lo >= 250 && hi - lo <= 1050) {
          double c[4], best = 0;
          for (int k = 0; k < 4; ++k) best = std::max(best, c[k] = repeat(a, to, 60.0 / kRates[k] * fs));
          for (int k = 0; k < 4; ++k) {
            if (c[k] < 0.4 || c[k] < 0.85 * best) continue;
            // Between the lines it must not repeat as well.
            // (4/3 of a line; and a third and a fifth of one, where a
            // signal with shorter lines, as SSTV's, repeats too.)
            double off = repeat(a, to, 60.0 / kRates[k] * fs * 4.0 / 3.0);
            off = std::max(off, repeat(a, to, 60.0 / kRates[k] * fs / 3.0));
            off = std::max(off, repeat(a, to, 60.0 / kRates[k] * fs / 5.0));
            if (c[k] - off >= 0.15 && (!recognised || kRates[k] == lpm)) {
              if (!recognised) recognised = true, lpm = kRates[k];
              black = lo;
              white = hi;
            }
            break;
          }
        }
      }
      if (static_cast<double>(end() - pictureStart) > kMaxSeconds * fs) complete(end(), end());
    }
  }
};

FaxDecoder::FaxDecoder(double rate, double centreHz) : impl_(std::make_unique<Impl>(rate, centreHz)) {}
FaxDecoder::~FaxDecoder() = default;

void FaxDecoder::process(const float* audio, std::size_t n) {
  Impl& d = *impl_;
  const double step = 2.0 * kPi * d.centre / d.rate;
  const std::size_t taps = d.fir.size();
  for (std::size_t i = 0; i < n; ++i) {
    d.hist[d.histAt] = {static_cast<float>(audio[i] * std::cos(d.phase)), static_cast<float>(-audio[i] * std::sin(d.phase))};
    d.histAt = (d.histAt + 1) % taps;
    d.phase += step;
    if (d.phase > 2.0 * kPi) d.phase -= 2.0 * kPi;
    std::complex<float> y = 0;
    std::size_t k = d.histAt;
    for (std::size_t t = 0; t < taps; ++t) {
      y += d.hist[k] * d.fir[t];
      if (++k == taps) k = 0;
    }
    {
      const double m = std::abs(y);
      d.ampSum += m;
      d.ampSq += m * m;
      if (++d.ampN >= static_cast<int>(d.rate)) {
        const double mean = d.ampSum / d.ampN, var = std::max(0.0, d.ampSq / d.ampN - mean * mean);
        d.ripple.push_back(static_cast<float>(mean > 0 ? std::sqrt(var) / mean : 1.0));
        if (d.ripple.size() > 20) d.ripple.erase(d.ripple.begin());
        d.ampSum = d.ampSq = 0;
        d.ampN = 0;
      }
    }
    const std::complex<float> diff = y * std::conj(d.prev);
    d.prev = y;
    d.accF += std::arg(diff) * d.rate / (2.0 * kPi) + d.centre;
    if (++d.accN == d.decim) {
      d.f.push_back(static_cast<int16_t>(std::clamp(d.accF / d.decim, -30000.0, 30000.0)));
      d.accF = 0;
      d.accN = 0;
    }
  }
  d.analyse();
}

bool FaxDecoder::recognised() const { return impl_->recognised; }
int FaxDecoder::linesPerMinute() const { return impl_->lpm; }
int FaxDecoder::lines() const {
  const Impl& d = *impl_;
  return d.recognised ? static_cast<int>(static_cast<double>(d.end() - d.pictureStart) / d.lineSamples()) : 0;
}

std::vector<std::shared_ptr<FaxImage>> FaxDecoder::take() {
  std::vector<std::shared_ptr<FaxImage>> o;
  o.swap(impl_->done);
  return o;
}

std::shared_ptr<FaxImage> FaxDecoder::finish() {
  Impl& d = *impl_;
  auto im = d.picture(d.pictureStart, d.end());
  d.pictureStart = d.end();
  return im;
}

// -------------------------------------------------------------- modulator --

FaxImage faxTestChart(int lines) {
  FaxImage im;
  im.width = widthOf(576);
  im.height = lines;
  im.linesPerMinute = 120;
  im.ioc = 576;
  im.grey.assign(static_cast<std::size_t>(im.width) * static_cast<std::size_t>(lines), 255);
  auto px = [&](int x, int y) -> uint8_t& { return im.grey[static_cast<std::size_t>(y) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(x)]; };
  for (int y = 0; y < lines; ++y)
    for (int x = 0; x < im.width; ++x) {
      const bool frame = x < 30 || x >= im.width - 30 || y < 4 || y >= lines - 4;
      const bool bars = x >= 200 && x < 600 && (x / 50) % 2 == 0;           // vertical bars
      const bool diagonal = std::abs(x - (700 + 6 * y)) < 12;               // slants if the line rate is wrong
      const bool grey = x >= 1300 && x < 1600;                              // a grey field
      px(x, y) = frame || bars || diagonal ? 0 : grey ? 128 : 255;
    }
  return im;
}

std::vector<float> faxModulate(const FaxImage& im, double rate, bool withTones, double shiftHz) {
  std::vector<float> out;
  double ph = 0;
  auto emit = [&](double grey01) {
    ph += 2.0 * kPi * (1500.0 + 800.0 * grey01 + shiftHz) / rate;
    out.push_back(static_cast<float>(0.5 * std::sin(ph)));
  };
  auto tone = [&](double hz, double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * rate); ++i) emit(std::fmod(i * hz / rate, 1.0) < 0.5 ? 1.0 : 0.0);
  };
  const double line = 60.0 / (im.linesPerMinute ? im.linesPerMinute : 120) * rate;
  if (withTones) {
    tone(im.ioc == 288 ? 675 : 300, 5);
    // Phasing: 20 white lines, each beginning with a black pulse of 5 %
    // (2.5 % at the end of a line, 2.5 % at the start of the next).
    for (int i = 0; i < static_cast<int>(20 * line); ++i) {
      const double x = std::fmod(i / line, 1.0);
      emit(x < 0.025 || x >= 0.975 ? 0.0 : 1.0);
    }
  }
  const long total = static_cast<long>(im.height * line);
  for (long i = 0; i < total; ++i) {
    const int y = static_cast<int>(i / line);
    const int x = std::min(im.width - 1, static_cast<int>(std::fmod(i / line, 1.0) * im.width));
    emit(im.grey[static_cast<std::size_t>(y) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(x)] / 255.0);
  }
  if (withTones) {
    tone(450, 5);
    for (int i = 0; i < static_cast<int>(2 * rate); ++i) emit(0.0);
  }
  return out;
}

}  // namespace hfc
