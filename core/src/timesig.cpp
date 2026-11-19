#include "hfc/timesig.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace hfc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 100;            // envelope values a second
constexpr std::size_t kKeep = 130 * kRate;

int bcd(const int* bits, std::initializer_list<int> weights) {
  int v = 0, i = 0;
  for (int w : weights) v += bits[i++] ? w : 0;
  return v;
}

// 0 Monday .. 6 Sunday; Zeller's congruence.
int weekday(int y, int m, int d) {
  if (m < 3) m += 12, --y;
  const int k = y % 100, j = y / 100;
  const int h = (d + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;  // 0 Saturday
  return (h + 5) % 7;
}

bool realDate(int y, int m, int d) {
  static const int days[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12 || d < 1 || d > days[m - 1]) return false;
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return !(m == 2 && d == 29 && !leap);
}

const char* kDays[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

}  // namespace

struct TimeSignalDecoder::Impl {
  double rate, tone;
  std::complex<double> rot{1.0, 0.0};  // the mixer's phasor, exp(-i phase)
  int rotCount = 0;
  std::complex<double> acc{0, 0};
  double wide = 0;
  int count = 0, per = 0;
  std::vector<float> narrow, broad;   // envelopes, kRate a second
  uint64_t base = 0;                  // envelope index of narrow[0]
  uint64_t analysed = 0;
  bool seconds = false;
  int phaseBin = 0;                   // where in the second the pulses are
  uint64_t lastMinute = 0;
  std::vector<std::string> out;

  // The first second of audio, to find the carrier in (within 400 Hz of
  // where it was said to be: a session is centred on the middle of the
  // power, which a keyed carrier's sidebands move).
  std::vector<float> probe;
  bool tuned = false;

  Impl(double r, double t) : rate(r), tone(t), per(static_cast<int>(std::lround(r / kRate))) {}

  void tune() {
    double best = 0, bestF = tone;
    const std::size_t n = probe.size();
    for (double f = tone - 400; f <= tone + 400; f += 5) {
      if (f < 50 || f > rate / 2 - 50) continue;
      double re = 0, im = 0;
      const double w = 2.0 * kPi * f / rate;
      for (std::size_t i = 0; i < n; ++i) re += probe[i] * std::cos(w * static_cast<double>(i)), im += probe[i] * std::sin(w * static_cast<double>(i));
      if (re * re + im * im > best) best = re * re + im * im, bestF = f;
    }
    tone = bestF;
    tuned = true;
  }

  uint64_t end() const { return base + narrow.size(); }
  float at(uint64_t i) const { return narrow[static_cast<std::size_t>(i - base)]; }

  // How clearly `env` over [from, end) repeats with `period` (in envelope
  // values, fractional): the folded profile's spread over the spread of
  // its values.
  double fold(const std::vector<float>& env, uint64_t from, double period, int* peakBin = nullptr) const {
    const int bins = 50;
    std::vector<double> sum(bins, 0), n(bins, 0);
    double mean = 0, var = 0;
    const uint64_t to = end();
    for (uint64_t i = from; i < to; ++i) {
      const double v = env[static_cast<std::size_t>(i - base)];
      const double ph = std::fmod(static_cast<double>(i) / period, 1.0);
      const int b = std::min(bins - 1, static_cast<int>(ph * bins));
      sum[static_cast<std::size_t>(b)] += v;
      n[static_cast<std::size_t>(b)] += 1;
      mean += v;
    }
    const double total = static_cast<double>(to - from);
    mean /= total;
    for (uint64_t i = from; i < to; ++i) {
      const double d = env[static_cast<std::size_t>(i - base)] - mean;
      var += d * d;
    }
    var /= total;
    double pv = 0, lo = 1e30;
    int lowBin = 0;
    for (int b = 0; b < bins; ++b) {
      const double m = n[static_cast<std::size_t>(b)] ? sum[static_cast<std::size_t>(b)] / n[static_cast<std::size_t>(b)] : mean;
      pv += (m - mean) * (m - mean);
      if (m < lo) lo = m, lowBin = b;
    }
    pv /= bins;
    if (peakBin) *peakBin = lowBin;
    return var > 0 ? pv / var : 0;
  }

  void recognise() {
    const uint64_t window = 12 * kRate;
    if (end() < base + window) return;
    const uint64_t from = end() - window;
    for (const auto* env : {&narrow, &broad}) {
      const double at1 = fold(*env, from, kRate);
      const double off = std::max(fold(*env, from, kRate * 0.95), fold(*env, from, kRate * 1.05));
      if (at1 > 0.5 && at1 > 3.0 * off) {
        seconds = true;
        out.push_back("time signal: second pulses (exactly 1 s apart)");
        return;
      }
    }
  }

  // The low runs of the carrier in [from, end): (start, length) in
  // envelope values.
  std::vector<std::pair<uint64_t, int>> lows(uint64_t from) const {
    std::vector<float> v;
    for (uint64_t i = from; i < end(); ++i) v.push_back(at(i));
    if (v.size() < 1000) return {};
    std::vector<float> s = v;
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() * 8 / 10), s.end());
    const float hi = s[s.size() * 8 / 10];
    std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() * 3 / 100), s.end());
    const float lo = s[s.size() * 3 / 100];
    if (hi < 2.0f * lo + 1e-12f) return {};
    const float thr = 0.5f * (hi + lo);
    std::vector<std::pair<uint64_t, int>> runs;
    for (std::size_t i = 0; i < v.size();) {
      if (v[i] < thr) {
        std::size_t j = i;
        while (j < v.size() && v[j] < thr) ++j;
        if (j - i >= 4 && j < v.size()) runs.emplace_back(from + i, static_cast<int>(j - i));
        i = j;
      } else {
        ++i;
      }
    }
    return runs;
  }

  // The run starting within 6 values of `t`, or null.
  static const std::pair<uint64_t, int>* runAt(const std::vector<std::pair<uint64_t, int>>& runs, uint64_t t) {
    for (const auto& r : runs)
      if (r.first + 6 >= t && r.first <= t + 6) return &r;
    return nullptr;
  }

  void decodeDcf(const std::vector<std::pair<uint64_t, int>>& runs) {
    // The minute begins at the first pulse after a second without one
    // (heard: the audio reaches back over it).
    for (std::size_t k = 0; k < runs.size(); ++k) {
      const uint64_t t0 = runs[k].first;
      if (t0 < base + 115 || runAt(runs, t0 - kRate)) continue;
      if (t0 <= lastMinute + 100 || t0 + 59 * kRate > end()) continue;
      int bits[59];
      bool ok = true;
      for (int sec = 0; sec < 59 && ok; ++sec) {
        const auto* r = runAt(runs, t0 + static_cast<uint64_t>(sec) * kRate);
        if (!r) ok = false;
        else if (r->second >= 6 && r->second <= 14) bits[sec] = 0;
        else if (r->second >= 16 && r->second <= 25) bits[sec] = 1;
        else ok = false;
      }
      if (!ok) continue;
      auto even = [&](int a, int b) {
        int s = 0;
        for (int i = a; i <= b; ++i) s += bits[i];
        return s % 2 == 0;
      };
      if (bits[0] != 0 || bits[20] != 1 || !even(21, 28) || !even(29, 35) || !even(36, 58) || bits[17] == bits[18]) continue;
      const int mi = bcd(bits + 21, {1, 2, 4, 8, 10, 20, 40}), h = bcd(bits + 29, {1, 2, 4, 8, 10, 20});
      const int d = bcd(bits + 36, {1, 2, 4, 8, 10, 20}), dow = bcd(bits + 42, {1, 2, 4});
      const int mo = bcd(bits + 45, {1, 2, 4, 8, 10}), y = 2000 + bcd(bits + 50, {1, 2, 4, 8, 10, 20, 40, 80});
      if (mi > 59 || h > 23 || !realDate(y, mo, d) || dow < 1 || dow > 7 || weekday(y, mo, d) != dow - 1) continue;
      char b[160];
      std::snprintf(b, sizeof b, "DCF77 time code: %04d-%02d-%02d (%s) %02d:%02d %s", y, mo, d, kDays[dow - 1], h, mi, bits[17] ? "CEST" : "CET");
      out.push_back(b);
      lastMinute = t0;
    }
  }

  void decodeMsf(const std::vector<std::pair<uint64_t, int>>& runs) {
    for (const auto& m : runs) {
      if (m.second < 45 || m.second > 55) continue;  // the minute marker: 0.5 s off
      const uint64_t t0 = m.first;
      if (t0 <= lastMinute + 100 || t0 + 60 * kRate > end()) continue;
      int A[60] = {0}, B[60] = {0};
      bool ok = true;
      for (int sec = 1; sec < 60 && ok; ++sec) {
        const uint64_t s = t0 + static_cast<uint64_t>(sec) * kRate;
        const auto* r = runAt(runs, s);
        if (!r || r->second < 6 || r->second > 32) {
          ok = false;
          break;
        }
        // Off at 0.15 s into the second: bit A; at 0.25 s: bit B (the
        // carrier may come back between 0.1 and 0.2 s when A is 0).
        A[sec] = at(s + 15) < at(s + 50) * 0.5f;
        B[sec] = at(s + 25) < at(s + 50) * 0.5f;
      }
      if (!ok) continue;
      auto odd = [&](int a, int b, int p) {
        int s = B[p];
        for (int i = a; i <= b; ++i) s += A[i];
        return s % 2 == 1;
      };
      static const int fixed[8] = {0, 1, 1, 1, 1, 1, 1, 0};
      bool pattern = true;
      for (int i = 0; i < 8; ++i) pattern = pattern && A[52 + i] == fixed[i];
      if (!pattern || !odd(17, 24, 54) || !odd(25, 35, 55) || !odd(36, 38, 56) || !odd(39, 51, 57)) continue;
      const int y = 2000 + bcd(A + 17, {80, 40, 20, 10, 8, 4, 2, 1}), mo = bcd(A + 25, {10, 8, 4, 2, 1});
      const int d = bcd(A + 30, {20, 10, 8, 4, 2, 1}), dow = bcd(A + 36, {4, 2, 1});
      const int h = bcd(A + 39, {20, 10, 8, 4, 2, 1}), mi = bcd(A + 45, {40, 20, 10, 8, 4, 2, 1});
      if (mi > 59 || h > 23 || !realDate(y, mo, d) || dow > 6 || (weekday(y, mo, d) + 1) % 7 != dow) continue;
      char b[160];
      std::snprintf(b, sizeof b, "MSF time code: %04d-%02d-%02d (%s) %02d:%02d %s", y, mo, d, kDays[(dow + 6) % 7], h, mi, B[58] ? "BST" : "UTC");
      out.push_back(b);
      lastMinute = t0;
    }
  }

  void analyse() {
    while (analysed + kRate <= end()) {
      analysed += kRate;
      if (!seconds) {
        if (analysed % (2 * kRate) == 0) recognise();
        continue;
      }
      if (end() < base + 61 * kRate) continue;
      const auto runs = lows(end() - std::min<uint64_t>(end() - base, 125 * kRate));
      decodeDcf(runs);
      decodeMsf(runs);
    }
    if (narrow.size() > kKeep) {
      const std::size_t drop = narrow.size() - kKeep;
      narrow.erase(narrow.begin(), narrow.begin() + static_cast<std::ptrdiff_t>(drop));
      broad.erase(broad.begin(), broad.begin() + static_cast<std::ptrdiff_t>(drop));
      base += drop;
    }
  }
};

TimeSignalDecoder::TimeSignalDecoder(double rate, double toneHz) : impl_(std::make_unique<Impl>(rate, toneHz)) {}
TimeSignalDecoder::~TimeSignalDecoder() = default;

void TimeSignalDecoder::process(const float* audio, std::size_t n) {
  Impl& d = *impl_;
  if (!d.tuned) {
    const std::size_t want = static_cast<std::size_t>(d.rate);
    const std::size_t take = std::min(n, want - d.probe.size());
    d.probe.insert(d.probe.end(), audio, audio + take);
    if (d.probe.size() < want) return;
    d.tune();
    // The probe first, then the rest of this block.
    std::vector<float> first;
    first.swap(d.probe);
    process(first.data(), first.size());
    if (take < n) process(audio + take, n - take);
    return;
  }
  // A rotating phasor (exp(-i phase)), renormalised now and then, in place
  // of a sine and a cosine per sample (which cost most of this decoder).
  const std::complex<double> turn = std::polar(1.0, -2.0 * kPi * d.tone / d.rate);
  for (std::size_t i = 0; i < n; ++i) {
    // Mixed to 0 Hz and averaged over 10 ms: what is within about 50 Hz of
    // the carrier; and the power of everything.
    d.acc += static_cast<double>(audio[i]) * d.rot;
    d.wide += static_cast<double>(audio[i]) * audio[i];
    d.rot *= turn;
    if (++d.rotCount == 1024) {
      d.rotCount = 0;
      d.rot /= std::abs(d.rot);
    }
    if (++d.count == d.per) {
      d.narrow.push_back(static_cast<float>(std::abs(d.acc) / d.per));
      d.broad.push_back(static_cast<float>(std::sqrt(d.wide / d.per)));
      d.acc = 0;
      d.wide = 0;
      d.count = 0;
    }
  }
  d.analyse();
}

bool TimeSignalDecoder::recognised() const { return impl_->seconds; }

std::vector<std::string> TimeSignalDecoder::take() {
  std::vector<std::string> o;
  o.swap(impl_->out);
  return o;
}

// -------------------------------------------------------------- modulator --

std::vector<float> timeCodeLevels(const std::string& station, int year, int month, int day, int hour, int minute, bool summer,
                                  int minutes) {
  std::vector<float> lv;
  const bool dcf = station == "DCF77";
  for (int m = 0; m < minutes; ++m) {
    // The minute's civil time, the date rolled over at midnight.
    int mi = minute + m, h = hour + mi / 60, d0 = day, mo0 = month, y0 = year;
    mi %= 60;
    while (h >= 24) {
      h -= 24;
      if (!realDate(y0, mo0, ++d0)) {
        d0 = 1;
        if (++mo0 > 12) mo0 = 1, ++y0;
      }
    }
    const int year = y0, month = mo0, day = d0;
    const int y2 = year % 100, dow = weekday(year, month, day);  // 0 Monday
    auto put = [](int* bits, int v, std::initializer_list<int> weights) {
      int i = 0;
      for (int w : weights) {
        bits[i] = v >= w ? 1 : 0;
        if (bits[i]) v -= w;
        ++i;
      }
    };
    if (dcf) {
      int bits[60] = {0};
      bits[17] = summer, bits[18] = !summer, bits[20] = 1;
      int t[8];
      put(t, mi, {40, 20, 10, 8, 4, 2, 1});
      for (int i = 0; i < 7; ++i) bits[21 + i] = t[6 - i];
      put(t, h, {20, 10, 8, 4, 2, 1});
      for (int i = 0; i < 6; ++i) bits[29 + i] = t[5 - i];
      put(t, day, {20, 10, 8, 4, 2, 1});
      for (int i = 0; i < 6; ++i) bits[36 + i] = t[5 - i];
      put(t, dow + 1, {4, 2, 1});
      for (int i = 0; i < 3; ++i) bits[42 + i] = t[2 - i];
      put(t, month, {10, 8, 4, 2, 1});
      for (int i = 0; i < 5; ++i) bits[45 + i] = t[4 - i];
      put(t, y2, {80, 40, 20, 10, 8, 4, 2, 1});
      for (int i = 0; i < 8; ++i) bits[50 + i] = t[7 - i];
      auto par = [&](int a, int b) {
        int s = 0;
        for (int i = a; i <= b; ++i) s += bits[i];
        return s % 2;
      };
      bits[28] = par(21, 27), bits[35] = par(29, 34), bits[58] = par(36, 57);
      for (int sec = 0; sec < 60; ++sec)
        for (int k = 0; k < kRate; ++k) lv.push_back(sec < 59 && k < (bits[sec] ? 20 : 10) ? 0.15f : 1.0f);
    } else {
      int A[60] = {0}, B[60] = {0};
      put(A + 17, y2, {80, 40, 20, 10, 8, 4, 2, 1});
      put(A + 25, month, {10, 8, 4, 2, 1});
      put(A + 30, day, {20, 10, 8, 4, 2, 1});
      put(A + 36, (dow + 1) % 7, {4, 2, 1});
      put(A + 39, h, {20, 10, 8, 4, 2, 1});
      put(A + 45, mi, {40, 20, 10, 8, 4, 2, 1});
      static const int fixed[8] = {0, 1, 1, 1, 1, 1, 1, 0};
      for (int i = 0; i < 8; ++i) A[52 + i] = fixed[i];
      auto oddp = [&](int a, int b) {
        int s = 0;
        for (int i = a; i <= b; ++i) s += A[i];
        return 1 - s % 2;
      };
      B[54] = oddp(17, 24), B[55] = oddp(25, 35), B[56] = oddp(36, 38), B[57] = oddp(39, 51), B[58] = summer;
      for (int sec = 0; sec < 60; ++sec)
        for (int k = 0; k < kRate; ++k) {
          bool off;
          if (sec == 0) off = k < 50;
          else off = k < 10 || (k < 20 && A[sec]) || (k >= 20 && k < 30 && B[sec]);
          lv.push_back(off ? 0.0f : 1.0f);
        }
    }
  }
  return lv;
}

std::vector<float> timeSignalAudio(const std::vector<float>& levels, double rate, double toneHz) {
  std::vector<float> a;
  const double per = rate / kRate;
  const std::size_t n = static_cast<std::size_t>(static_cast<double>(levels.size()) * per);
  a.reserve(n);
  double ph = 0, lv = levels.empty() ? 0 : levels[0];
  for (std::size_t i = 0; i < n; ++i) {
    const float target = levels[std::min(levels.size() - 1, static_cast<std::size_t>(static_cast<double>(i) / per))];
    lv += (target - lv) * 0.05;  // edges of about 2 ms
    ph += 2.0 * kPi * toneHz / rate;
    a.push_back(static_cast<float>(0.5 * lv * std::sin(ph)));
  }
  return a;
}

}  // namespace hfc
