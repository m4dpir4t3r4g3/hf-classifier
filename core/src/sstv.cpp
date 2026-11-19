#include "hfc/sstv.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace hfc {

namespace {

constexpr double kPi = 3.14159265358979323846;

enum class Kind { Martin, Scottie, Robot36, Robot72, Pd };

struct Spec {
  const char* name;
  int vis;
  Kind kind;
  int width, lines;
  double scan;  // ms: one colour (Martin, Scottie), the brightness (Robot), one of the four parts (PD)
  double line;  // ms from one sync pulse to the next
};

// (N7CXI's table. A Scottie line: 1.5 + green + 1.5 + blue + 9 sync + 1.5 + red.)
const Spec kSpecs[] = {
    {"Martin M1", 44, Kind::Martin, 320, 256, 146.432, 446.446},
    {"Martin M2", 40, Kind::Martin, 320, 256, 73.216, 226.798},
    {"Scottie S1", 60, Kind::Scottie, 320, 256, 138.240, 428.220},
    {"Scottie S2", 56, Kind::Scottie, 320, 256, 88.064, 277.692},
    {"Scottie DX", 76, Kind::Scottie, 320, 256, 345.600, 1050.300},
    {"Robot 36", 8, Kind::Robot36, 320, 240, 88.0, 150.0},
    {"Robot 72", 12, Kind::Robot72, 320, 240, 138.0, 300.0},
    {"PD 50", 93, Kind::Pd, 320, 256, 91.520, 388.160},
    {"PD 90", 99, Kind::Pd, 320, 256, 170.240, 703.040},
    {"PD 120", 95, Kind::Pd, 640, 496, 121.600, 508.480},
    {"PD 160", 98, Kind::Pd, 512, 400, 195.584, 804.416},
    {"PD 180", 96, Kind::Pd, 640, 496, 183.040, 754.240},
    {"PD 240", 97, Kind::Pd, 640, 496, 244.480, 1000.000},
    {"PD 290", 94, Kind::Pd, 800, 616, 228.800, 937.280},
};

const Spec* specNamed(const std::string& name) {
  for (const Spec& s : kSpecs)
    if (name == s.name) return &s;
  return nullptr;
}

double syncMs(const Spec& s) { return s.kind == Kind::Martin ? 4.862 : s.kind == Kind::Pd ? 20.0 : 9.0; }
int linesPerSync(const Spec& s) { return s.kind == Kind::Pd ? 2 : 1; }

uint8_t clip(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5); }

// Brightness and colour differences (0..255, the differences around 128)
// to red, green, blue.
void yuvToRgb(double y, double ry, double by, uint8_t* rgb) {
  rgb[0] = clip(y + 1.402 * (ry - 128));
  rgb[1] = clip(y - 0.714 * (ry - 128) - 0.344 * (by - 128));
  rgb[2] = clip(y + 1.772 * (by - 128));
}

}  // namespace

struct SstvDecoder::Impl {
  double rate, shift0;
  int decim;
  double fs;
  std::vector<float> fir;
  std::vector<std::complex<float>> hist;
  std::size_t histAt = 0;
  double phase = 0;
  std::complex<float> prev{1, 0};
  double accF = 0;
  int accN = 0;
  std::vector<int16_t> f;   // frequency in Hz, from sample `base`
  uint64_t base = 0;
  uint64_t scanned = 0;     // up to where headers were looked for
  uint64_t syncChecked = 0;
  uint64_t syncFrom = 0;    // sync pulses before this belong to a picture already made
  // The picture being received.
  const Spec* spec = nullptr;
  bool header = false;
  double first = 0;         // the first line's sync pulse (absolute sample)
  double shift = 0;         // tuning error in Hz
  std::vector<std::shared_ptr<FaxImage>> done;

  Impl(double r, double s) : rate(r), shift0(s) {
    decim = std::max(1, static_cast<int>(std::lround(r / 12000.0)));
    fs = r / decim;
    const int half = std::max(8, static_cast<int>(r / 400.0));
    for (int k = -half; k <= half; ++k) {
      const double x = 2.0 * 1000.0 / r * k;
      const double sinc = k == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      fir.push_back(static_cast<float>(sinc * (0.54 + 0.46 * std::cos(kPi * k / half))));
    }
    float sum = 0;
    for (float v : fir) sum += v;
    for (float& v : fir) v /= sum;
    hist.assign(fir.size(), {0, 0});
  }

  uint64_t end() const { return base + f.size(); }
  double at(double i) const {
    const double c = std::clamp(i, static_cast<double>(base), static_cast<double>(end() - 1));
    return f[static_cast<std::size_t>(c - static_cast<double>(base))];
  }
  double ms(double v) const { return v * fs / 1000.0; }  // milliseconds to samples
  // The mean frequency over [from, from + len) samples.
  double mean(double from, double len) const {
    const long a = static_cast<long>(std::floor(from)), n = std::max(1L, static_cast<long>(std::lround(len)));
    double s = 0;
    for (long i = 0; i < n; ++i) s += at(static_cast<double>(a + i));
    return s / static_cast<double>(n);
  }

  // The header ending in data that starts at `cell0` (in 10 ms cells from
  // `from`): the code, or -1.
  int visAt(uint64_t from, double* leader) const {
    const double cell = ms(10);
    auto c = [&](int i) { return mean(static_cast<double>(from) + (i + 0.15) * cell, 0.7 * cell); };
    double lo = 1e9, hi = -1e9, L = 0;
    for (int i = 0; i < 9; ++i) {
      const double v = c(i);
      lo = std::min(lo, v), hi = std::max(hi, v), L += v / 9.0;
    }
    // (Where the tone is says little: a session is centred on the middle
    // of the power, which a dark picture has at its black.)
    if (hi - lo > 60 || std::fabs(L - 1900 - shift0) > 400) return -1;
    // Both ends of the start bit and of the stop bit, so that the header's
    // place is known to a millisecond or two.
    for (int i : {9, 11, 36, 38})
      if (std::fabs(c(i) - (L - 700)) > 60) return -1;
    int code = 0, ones = 0;
    for (int k = 0; k < 8; ++k) {
      const double v = c(13 + 3 * k);
      int bit;
      if (std::fabs(v - (L - 800)) <= 60) bit = 1;
      else if (std::fabs(v - (L - 600)) <= 60) bit = 0;
      else return -1;
      ones += bit;
      if (k < 7) code |= bit << k;
    }
    if (ones % 2) return -1;
    *leader = L;
    return code;
  }

  void lookForHeader() {
    const uint64_t need = static_cast<uint64_t>(ms(400));
    const uint64_t step = static_cast<uint64_t>(ms(2.5));
    if (scanned < base) scanned = base;
    for (; scanned + need < end(); scanned += step) {
      double L = 0;
      const int code = visAt(scanned, &L);
      if (code < 0) continue;
      const Spec* found = nullptr;
      for (const Spec& s : kSpecs)
        if (s.vis == code) found = &s;
      if (!found) continue;
      // A picture in progress ends here (its sender started another).
      const double dataStart = static_cast<double>(scanned) + ms(390);
      if (spec && dataStart > first + ms(2000)) complete(static_cast<double>(scanned) - ms(700));
      if (spec) continue;
      spec = found;
      header = true;
      shift = L - 1900;
      // Scottie: a 9 ms sync, then green and blue of the first line come
      // before its own sync pulse.
      first = dataStart + (found->kind == Kind::Scottie ? ms(9 + 1.5 + found->scan + 1.5 + found->scan) : 0);
      scanned += static_cast<uint64_t>(ms(390));
    }
  }

  // Sync pulses (runs below 1350 Hz of 3 to 30 ms) in [from, to): their starts.
  std::vector<double> pulses(uint64_t from, uint64_t to, double* freq, std::vector<double>* lengthsMs = nullptr) const {
    std::vector<double> out;
    // Where the sync is: the lowest frequency that is there 1 % of the
    // time (within 400 Hz of where it should be).
    double low = 1200 + shift0;
    {
      std::vector<int16_t> v;
      for (uint64_t i = from; i < to; i += 5) v.push_back(f[static_cast<std::size_t>(i - base)]);
      if (v.size() > 200) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 100), v.end());
        low = std::clamp(static_cast<double>(v[v.size() / 100]), 800 + shift0, 1600 + shift0);
      }
    }
    const double limit = low + 150;
    double sum = 0;
    int count = 0;
    uint64_t run = 0;
    double runSum = 0;
    for (uint64_t i = from; i < to; ++i) {
      const double v = at(static_cast<double>(i));
      if (v < limit && v > limit - 400) {
        ++run;
        runSum += v;
      } else {
        if (run >= ms(3) && run <= ms(30)) {
          out.push_back(static_cast<double>(i - run));
          if (lengthsMs) lengthsMs->push_back(static_cast<double>(run) * 1000.0 / fs);
          sum += runSum / static_cast<double>(run);
          ++count;
        }
        run = 0;
        runSum = 0;
      }
    }
    if (freq) *freq = count ? sum / count : 1200 + shift0;
    return out;
  }

  // No header received: the mode from the time between sync pulses.
  void lookForSync() {
    const uint64_t window = static_cast<uint64_t>(ms(9000));
    if (end() < base + window || end() < syncChecked + static_cast<uint64_t>(ms(1000))) return;
    syncChecked = end();
    double freq = 0;
    if (end() - window < syncFrom) return;
    std::vector<double> lengths;
    const auto p = pulses(end() - window, end(), &freq, &lengths);
    if (p.size() < 7) return;
    std::vector<double> gaps;
    for (std::size_t i = 1; i < p.size(); ++i) gaps.push_back((p[i] - p[i - 1]) * 1000.0 / fs);
    std::vector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    int agree = 0;
    for (double g : gaps) agree += std::fabs(g - med) < 0.007 * med;
    if (agree < 6 || agree < 0.7 * static_cast<double>(gaps.size())) return;
    const Spec* found = nullptr;
    for (const Spec& s : kSpecs)
      // (Not the Robot modes: other modes share their line times.)
      if (s.kind != Kind::Robot36 && s.kind != Kind::Robot72 && std::fabs(med - s.line) < 0.007 * s.line && (!found || std::fabs(med - s.line) < std::fabs(med - found->line))) found = &s;
    if (!found) return;
    // They must be sync pulses: of the mode's length (7 in 10 within 35 %),
    // and with the gap between sync and black above them nearly empty. A
    // picture received with the sidebands swapped has its white there
    // instead, in runs of any length, and its shades right above.
    int rightLength = 0;
    for (double l : lengths) rightLength += std::fabs(l - syncMs(*found)) < 0.35 * syncMs(*found);
    if (rightLength < 0.7 * static_cast<double>(lengths.size())) return;
    long inGap = 0, all = 0;
    for (uint64_t i = end() - window; i < end(); i += 3, ++all) {
      const double v = at(static_cast<double>(i));
      inGap += v > freq + 90 && v < freq + 230;
    }
    if (inGap > 0.03 * static_cast<double>(all)) return;
    spec = found;
    header = false;
    shift = freq - 1200;
    // The first pulse that the next follows after a line (a Scottie
    // picture opens with a sync pulse that is not a line's).
    first = p.front();
    for (std::size_t i = 0; i + 1 < p.size(); ++i)
      if (std::fabs(gaps[i] - med) < 0.007 * med) {
        first = p[i];
        break;
      }
  }

  // The sync pulse's start near `guess`: where most of the next `len`
  // samples are below 1350 Hz; negative if there is none.
  double syncNear(double guess, double reach, double len) const {
    const double limit = 1350 + shift;
    double best = -1;
    int bestCount = 0;
    const int n = static_cast<int>(len);
    int count = 0;
    const long from = static_cast<long>(guess - reach), to = static_cast<long>(guess + reach);
    if (from < static_cast<long>(base) || static_cast<uint64_t>(to + n) >= end()) return -1;
    for (int i = 0; i < n; ++i) count += at(static_cast<double>(from + i)) < limit;
    // The middle of the positions that hold the most (a pulse found whole
    // gives one; a longer stretch at 1200 Hz, as the header's stop bit
    // before the first line, gives many and is not taken).
    long firstBest = from, lastBest = from;
    for (long s = from;; ++s) {
      if (count > bestCount) bestCount = count, firstBest = lastBest = s;
      else if (count == bestCount) lastBest = s;
      if (s >= to) break;
      count += (at(static_cast<double>(s + n)) < limit) - (at(static_cast<double>(s)) < limit);
    }
    best = 0.5 * static_cast<double>(firstBest + lastBest);
    if (bestCount < 0.6 * n || static_cast<double>(lastBest - firstBest) > 0.5 * n) return -1;
    return best;
  }

  // Builds the picture from the lines up to sample `upTo` and ends it.
  void complete(double upTo) {
    const Spec& s = *spec;
    const double T = ms(s.line);
    syncFrom = std::max(syncFrom, static_cast<uint64_t>(std::min(upTo, static_cast<double>(end()))));
    int syncs = std::min(s.lines / linesPerSync(s), static_cast<int>((upTo - first) / T) + (s.kind == Kind::Scottie ? 1 : 0));
    // Scottie's sync is in the line's middle: the last line needs its red after it.
    while (syncs > 0 && first + (syncs - 1) * T + (s.kind == Kind::Scottie ? ms(10.5 + s.scan) : T) > static_cast<double>(end())) --syncs;
    spec = nullptr;
    if (syncs * linesPerSync(s) < 16) return;
    // The line length and start fitted to the sync pulses found.
    double a = first, b = T;
    {
      std::vector<std::pair<int, double>> found;
      double drift = 0;
      for (int k = 0; k < syncs; ++k) {
        const double at0 = syncNear(first + k * T + drift, ms(6), ms(syncMs(s)));
        if (at0 < 0) continue;
        drift = at0 - (first + k * T);
        found.emplace_back(k, at0);
      }
      for (int pass = 0; pass < 2 && found.size() >= std::max<std::size_t>(5, static_cast<std::size_t>(syncs) / 10); ++pass) {
        double sk = 0, st = 0, skk = 0, skt = 0;
        const double n = static_cast<double>(found.size());
        for (const auto& [k, t] : found) sk += k, st += t, skk += static_cast<double>(k) * k, skt += k * t;
        const double den = n * skk - sk * sk;
        if (den <= 0) break;
        const double nb = (n * skt - sk * st) / den, na = (st - nb * sk) / n;
        if (std::fabs(nb - T) > 0.01 * T) break;
        a = na, b = nb;
        std::vector<std::pair<int, double>> kept;
        for (const auto& kt : found)
          if (std::fabs(kt.second - (a + b * kt.first)) < ms(1.5)) kept.push_back(kt);
        found.swap(kept);
      }
    }
    auto im = std::make_shared<FaxImage>();
    im->width = s.width;
    im->height = syncs * linesPerSync(s);
    im->colour = true;
    im->what = s.name;
    im->grey.assign(static_cast<std::size_t>(im->width) * static_cast<std::size_t>(im->height) * 3, 0);
    const double scale = b / T;  // the clock's ratio
    auto shade = [&](double from, double len) { return (mean(from, len) - 1500 - shift) / 800.0 * 255.0; };
    // One scan of `width` elements from `start` (samples) lasting `msLen`.
    auto scan = [&](double start, double msLen, std::vector<double>& out) {
      const double px = ms(msLen) * scale / s.width;
      out.resize(static_cast<std::size_t>(s.width));
      for (int x = 0; x < s.width; ++x) out[static_cast<std::size_t>(x)] = shade(start + x * px, std::max(1.0, px));
    };
    std::vector<double> c0, c1, c2, c3;
    std::vector<std::vector<double>> ry(static_cast<std::size_t>(syncs)), by(static_cast<std::size_t>(syncs));
    std::vector<std::vector<double>> ys(static_cast<std::size_t>(syncs));
    for (int k = 0; k < syncs; ++k) {
      const double r = a + b * k;
      uint8_t* row = im->grey.data() + static_cast<std::size_t>(k) * linesPerSync(s) * static_cast<std::size_t>(s.width) * 3;
      auto at0 = [&](double msFromSync) { return r + ms(msFromSync) * scale; };
      if (s.kind == Kind::Martin || s.kind == Kind::Scottie) {
        if (s.kind == Kind::Martin) {
          scan(at0(4.862 + 0.572), s.scan, c0);                           // green
          scan(at0(4.862 + 0.572 + s.scan + 0.572), s.scan, c1);          // blue
          scan(at0(4.862 + 0.572 + 2 * (s.scan + 0.572)), s.scan, c2);    // red
        } else {
          scan(at0(-(2 * s.scan + 1.5)), s.scan, c0);
          scan(at0(-s.scan), s.scan, c1);
          scan(at0(9 + 1.5), s.scan, c2);
        }
        for (int x = 0; x < s.width; ++x) {
          row[3 * x] = clip(c2[static_cast<std::size_t>(x)]);
          row[3 * x + 1] = clip(c0[static_cast<std::size_t>(x)]);
          row[3 * x + 2] = clip(c1[static_cast<std::size_t>(x)]);
        }
      } else if (s.kind == Kind::Pd) {
        scan(at0(22.08), s.scan, c0);
        scan(at0(22.08 + s.scan), s.scan, c1);
        scan(at0(22.08 + 2 * s.scan), s.scan, c2);
        scan(at0(22.08 + 3 * s.scan), s.scan, c3);
        for (int x = 0; x < s.width; ++x) {
          const std::size_t i = static_cast<std::size_t>(x);
          yuvToRgb(c0[i], c1[i], c2[i], row + 3 * x);
          yuvToRgb(c3[i], c1[i], c2[i], row + 3 * (s.width + x));
        }
      } else if (s.kind == Kind::Robot72) {
        scan(at0(12), 138, c0);
        scan(at0(12 + 138 + 6), 69, c1);
        scan(at0(12 + 138 + 6 + 69 + 6), 69, c2);
        for (int x = 0; x < s.width; ++x) {
          const std::size_t i = static_cast<std::size_t>(x);
          yuvToRgb(c0[i], c1[i], c2[i], row + 3 * x);
        }
      } else {
        // Robot 36: one colour difference per line, named by the tone
        // before it (1500 Hz: red, 2300 Hz: blue).
        scan(at0(12), 88, ys[static_cast<std::size_t>(k)]);
        const bool blue = mean(at0(12 + 88 + 0.7), ms(3)) - shift > 1900;
        scan(at0(12 + 88 + 6), 44, blue ? by[static_cast<std::size_t>(k)] : ry[static_cast<std::size_t>(k)]);
      }
    }
    if (s.kind == Kind::Robot36) {
      // The other colour difference from the nearest line that has it.
      auto nearest = [&](const std::vector<std::vector<double>>& v, int k) -> const std::vector<double>* {
        for (int d = 0; d < 3; ++d)
          for (int j : {k + d, k - d})
            if (j >= 0 && j < syncs && !v[static_cast<std::size_t>(j)].empty()) return &v[static_cast<std::size_t>(j)];
        return nullptr;
      };
      for (int k = 0; k < syncs; ++k) {
        const auto* r = nearest(ry, k);
        const auto* bl = nearest(by, k);
        uint8_t* row = im->grey.data() + static_cast<std::size_t>(k) * static_cast<std::size_t>(s.width) * 3;
        for (int x = 0; x < s.width; ++x) {
          const std::size_t i = static_cast<std::size_t>(x);
          yuvToRgb(ys[static_cast<std::size_t>(k)][i], r ? (*r)[i] : 128, bl ? (*bl)[i] : 128, row + 3 * x);
        }
      }
    }
    done.push_back(im);
  }

  void run() {
    lookForHeader();
    if (!spec) lookForSync();
    if (spec) {
      const double T = ms(spec->line);
      const double whole = first + (spec->lines / linesPerSync(*spec)) * T + ms(50);
      if (static_cast<double>(end()) >= whole) {
        complete(whole);
        scanned = std::max<uint64_t>(scanned, static_cast<uint64_t>(whole - ms(100)));
      }
    }
    // What nothing needs any more goes.
    const double keepFrom = spec ? first - ms(spec->line + 1500) : static_cast<double>(end()) - ms(12000);
    if (keepFrom > static_cast<double>(base) + 8192) {
      const uint64_t drop = static_cast<uint64_t>(keepFrom) - base;
      f.erase(f.begin(), f.begin() + static_cast<std::ptrdiff_t>(drop));
      base += drop;
    }
  }
};

SstvDecoder::SstvDecoder(double rate, double shiftHz) : impl_(std::make_unique<Impl>(rate, shiftHz)) {}
SstvDecoder::~SstvDecoder() = default;

void SstvDecoder::process(const float* audio, std::size_t n) {
  Impl& d = *impl_;
  const double centre = 1750.0 + d.shift0;
  const double step = 2.0 * kPi * centre / d.rate;
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
    const std::complex<float> diff = y * std::conj(d.prev);
    d.prev = y;
    d.accF += std::arg(diff) * d.rate / (2.0 * kPi) + centre;
    if (++d.accN == d.decim) {
      d.f.push_back(static_cast<int16_t>(std::clamp(d.accF / d.decim, -30000.0, 30000.0)));
      d.accF = 0;
      d.accN = 0;
    }
  }
  d.run();
}

std::string SstvDecoder::mode() const { return impl_->spec ? impl_->spec->name : ""; }
bool SstvDecoder::fromHeader() const { return impl_->header; }

std::vector<std::shared_ptr<FaxImage>> SstvDecoder::take() {
  std::vector<std::shared_ptr<FaxImage>> o;
  o.swap(impl_->done);
  return o;
}

std::shared_ptr<FaxImage> SstvDecoder::finish() {
  Impl& d = *impl_;
  if (!d.spec) return nullptr;
  d.complete(static_cast<double>(d.end()));
  if (d.done.empty()) return nullptr;
  auto im = d.done.back();
  d.done.pop_back();
  return im;
}

// -------------------------------------------------------------- modulator --

std::vector<std::string> sstvModes() {
  std::vector<std::string> v;
  for (const Spec& s : kSpecs) v.push_back(s.name);
  return v;
}

FaxImage sstvTestPicture(const std::string& mode) {
  FaxImage im;
  const Spec* s = specNamed(mode);
  if (!s) return im;
  im.width = s->width;
  im.height = s->lines;
  im.colour = true;
  im.what = s->name;
  im.grey.resize(static_cast<std::size_t>(im.width) * static_cast<std::size_t>(im.height) * 3);
  // Eight colour bars over the top three quarters, a grey ramp below, and
  // a white diagonal: a wrong line time or colour order shows.
  static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}};
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) {
      uint8_t* p = im.grey.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(x)) * 3;
      if (y < im.height * 3 / 4) std::memcpy(p, bars[x * 8 / im.width], 3);
      else p[0] = p[1] = p[2] = static_cast<uint8_t>(x * 255 / im.width);
      if (std::abs(x - y * im.width / im.height) < im.width / 80) p[0] = p[1] = p[2] = 255;
    }
  return im;
}

std::vector<float> sstvModulate(const FaxImage& im, const std::string& mode, double rate, bool withHeader, double shiftHz) {
  const Spec* sp = specNamed(mode);
  if (!sp || !im.colour || im.width != sp->width || im.height > sp->lines) return {};
  const Spec& s = *sp;
  std::vector<float> out;
  double ph = 0, t = 0;  // t: milliseconds asked for so far
  auto tone = [&](double hz, double msLen) {
    t += msLen;
    const std::size_t until = static_cast<std::size_t>(std::llround(t * rate / 1000.0));
    while (out.size() < until) {
      ph += 2.0 * kPi * (hz + shiftHz) / rate;
      out.push_back(static_cast<float>(0.5 * std::sin(ph)));
    }
  };
  auto shade = [&](double v, double msLen) { tone(1500.0 + 800.0 * std::clamp(v, 0.0, 255.0) / 255.0, msLen); };
  if (withHeader) {
    tone(1900, 300);
    tone(1200, 10);
    tone(1900, 300);
    tone(1200, 30);
    int ones = 0;
    for (int k = 0; k < 7; ++k) {
      const int bit = (s.vis >> k) & 1;
      ones += bit;
      tone(bit ? 1100 : 1300, 30);
    }
    tone(ones % 2 ? 1100 : 1300, 30);
    tone(1200, 30);
  }
  auto px = [&](int x, int y, int c) {
    return static_cast<double>(im.grey[(static_cast<std::size_t>(y) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]);
  };
  auto lum = [&](int x, int y) { return 0.299 * px(x, y, 0) + 0.587 * px(x, y, 1) + 0.114 * px(x, y, 2); };
  auto dRed = [&](int x, int y) { return 128 + 0.713 * (px(x, y, 0) - lum(x, y)); };
  auto dBlue = [&](int x, int y) { return 128 + 0.564 * (px(x, y, 2) - lum(x, y)); };
  const double p = s.scan / s.width;
  if (s.kind == Kind::Scottie) tone(1200, 9);
  for (int y = 0; y < im.height; y += linesPerSync(s)) {
    switch (s.kind) {
      case Kind::Martin:
        tone(1200, 4.862);
        tone(1500, 0.572);
        for (int c : {1, 2, 0}) {
          for (int x = 0; x < s.width; ++x) shade(px(x, y, c), p);
          tone(1500, 0.572);
        }
        break;
      case Kind::Scottie:
        tone(1500, 1.5);
        for (int x = 0; x < s.width; ++x) shade(px(x, y, 1), p);
        tone(1500, 1.5);
        for (int x = 0; x < s.width; ++x) shade(px(x, y, 2), p);
        tone(1200, 9);
        tone(1500, 1.5);
        for (int x = 0; x < s.width; ++x) shade(px(x, y, 0), p);
        break;
      case Kind::Robot36:
        tone(1200, 9);
        tone(1500, 3);
        for (int x = 0; x < s.width; ++x) shade(lum(x, y), 88.0 / s.width);
        tone(y % 2 ? 2300 : 1500, 4.5);
        tone(1900, 1.5);
        for (int x = 0; x < s.width; ++x) shade(y % 2 ? dBlue(x, y) : dRed(x, y), 44.0 / s.width);
        break;
      case Kind::Robot72:
        tone(1200, 9);
        tone(1500, 3);
        for (int x = 0; x < s.width; ++x) shade(lum(x, y), 138.0 / s.width);
        tone(1500, 4.5);
        tone(1900, 1.5);
        for (int x = 0; x < s.width; ++x) shade(dRed(x, y), 69.0 / s.width);
        tone(2300, 4.5);
        tone(1900, 1.5);
        for (int x = 0; x < s.width; ++x) shade(dBlue(x, y), 69.0 / s.width);
        break;
      case Kind::Pd: {
        const int y2 = std::min(y + 1, im.height - 1);
        tone(1200, 20);
        tone(1500, 2.08);
        for (int x = 0; x < s.width; ++x) shade(lum(x, y), p);
        for (int x = 0; x < s.width; ++x) shade(0.5 * (dRed(x, y) + dRed(x, y2)), p);
        for (int x = 0; x < s.width; ++x) shade(0.5 * (dBlue(x, y) + dBlue(x, y2)), p);
        for (int x = 0; x < s.width; ++x) shade(lum(x, y2), p);
        break;
      }
    }
  }
  tone(1900, 100);
  return out;
}

}  // namespace hfc
