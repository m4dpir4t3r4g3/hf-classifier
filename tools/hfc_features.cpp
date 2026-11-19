// Research tool: the feature meter's output for audio files, for training
// and scoring the learned family model (tools/ml/).
//
//   hfc_features FILE.f32 [--rate 12000] [--windows 3] [--seconds 8]
//                [--centre HZ --bw HZ] [--snr DB] [--seed N] [--spectrogram]
//
// FILE is raw mono 32-bit float audio (ffmpeg -f f32le). Without --centre
// and --bw the strongest band of the file's averaged spectrum is taken (the
// band 6 dB above the spectrum's 20th percentile holding the most power,
// then its 99 % power width). One JSON line per window of --seconds, spread
// over the file: the band, the meter's summary and family, the feature
// vector, and with --spectrogram a 64 x 48 picture of the band (time x
// frequency, log power, 0..1). --snr adds white noise for that ratio of the
// file's power to the noise in 3 kHz.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hfc/dsp.hpp"
#include "hfc/features.hpp"
#include "hfc/json.hpp"

namespace {

const double kTwoPi = 6.283185307179586;

std::vector<double> averagedSpectrum(const std::vector<float>& a, int n) {
  hfc::Fft fft(n, false);
  std::vector<hfc::cfloat> in(static_cast<std::size_t>(n)), out(static_cast<std::size_t>(n));
  std::vector<double> p(static_cast<std::size_t>(n / 2), 0.0);
  for (std::size_t s = 0; s + static_cast<std::size_t>(n) <= a.size(); s += static_cast<std::size_t>(n / 2)) {
    for (int i = 0; i < n; ++i)
      in[static_cast<std::size_t>(i)] =
          hfc::cfloat(a[s + static_cast<std::size_t>(i)] * static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / n)), 0);
    fft.run(in.data(), out.data());
    for (std::size_t k = 0; k < p.size(); ++k) p[k] += std::norm(out[k]);
  }
  return p;
}

// The strongest band: centre and width in Hz.
void findBand(const std::vector<float>& a, double rate, double& centre, double& bw) {
  const int n = 4096;
  const std::vector<double> p = averagedSpectrum(a, n);
  const double df = rate / n;
  const int k0 = static_cast<int>(100 / df), k1 = std::min(static_cast<int>(p.size()) - 1, static_cast<int>(0.46 * rate / df));
  std::vector<double> sorted(p.begin() + k0, p.begin() + k1);
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 5), sorted.end());
  const double floor = sorted[sorted.size() / 5];
  const int bridge = static_cast<int>(50 / df);
  int bestA = k0, bestB = k0;
  double bestP = 0;
  for (int k = k0; k < k1;) {
    if (p[static_cast<std::size_t>(k)] < 4 * floor) {
      ++k;
      continue;
    }
    int a0 = k, b0 = k, gap = 0;
    double pw = 0;
    for (; k < k1; ++k) {
      if (p[static_cast<std::size_t>(k)] >= 4 * floor) b0 = k, gap = 0, pw += p[static_cast<std::size_t>(k)];
      else if (++gap > bridge) break;
    }
    if (pw > bestP) bestP = pw, bestA = a0, bestB = b0;
  }
  double total = 0;
  for (int k = bestA; k <= bestB; ++k) total += p[static_cast<std::size_t>(k)] - floor;
  int lo = bestA, hi = bestB;
  double acc = 0;
  for (int k = bestA; k <= bestB; ++k) {
    acc += p[static_cast<std::size_t>(k)] - floor;
    if (acc >= 0.005 * total) {
      lo = k;
      break;
    }
  }
  acc = 0;
  for (int k = bestB; k >= bestA; --k) {
    acc += p[static_cast<std::size_t>(k)] - floor;
    if (acc >= 0.005 * total) {
      hi = k;
      break;
    }
  }
  centre = 0.5 * (lo + hi) * df;
  bw = (hi - lo + 1) * df;
}

// 64 time steps x 48 frequency bins of centre +- 0.75 bw, log power, 0..1
// (60 dB below the picture's peak or more is 0).
std::vector<float> spectrogram(const float* a, std::size_t len, double rate, double centre, double bw) {
  const int n = 1024, steps = 64, bins = 48;
  std::vector<float> out(static_cast<std::size_t>(steps * bins), 0.0f);
  if (len < static_cast<std::size_t>(n)) return out;
  hfc::Fft fft(n, false);
  std::vector<hfc::cfloat> in(static_cast<std::size_t>(n)), spec(static_cast<std::size_t>(n));
  const double df = rate / n, f0 = centre - 0.75 * bw, fstep = 1.5 * bw / bins;
  const std::size_t hop = (len - static_cast<std::size_t>(n)) / steps;
  double top = 1e-30;
  std::vector<double> pw(out.size(), 0.0);
  for (int t = 0; t < steps; ++t) {
    // Several transforms per step, averaged.
    const std::size_t begin = static_cast<std::size_t>(t) * hop;
    const int sub = std::max(1, static_cast<int>(std::min<std::size_t>(4, hop / 256 + 1)));
    for (int s = 0; s < sub; ++s) {
      const std::size_t at = std::min(len - static_cast<std::size_t>(n), begin + static_cast<std::size_t>(s) * hop / static_cast<std::size_t>(sub));
      for (int i = 0; i < n; ++i)
        in[static_cast<std::size_t>(i)] =
            hfc::cfloat(a[at + static_cast<std::size_t>(i)] * static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / n)), 0);
      fft.run(in.data(), spec.data());
      for (int b = 0; b < bins; ++b) {
        const double fa = f0 + fstep * b, fb = fa + fstep;
        const int ka = std::clamp(static_cast<int>(fa / df), 0, n / 2 - 1), kb = std::clamp(static_cast<int>(fb / df), ka, n / 2 - 1);
        double acc = 0;
        for (int k = ka; k <= kb; ++k) acc += std::norm(spec[static_cast<std::size_t>(k)]);
        pw[static_cast<std::size_t>(t * bins + b)] += acc / (kb - ka + 1);
      }
    }
  }
  for (double v : pw) top = std::max(top, v);
  for (std::size_t i = 0; i < pw.size(); ++i)
    out[i] = static_cast<float>(std::clamp(1.0 + 10 * std::log10(std::max(pw[i], 1e-30) / top) / 60.0, 0.0, 1.0));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: hfc_features FILE.f32 [--rate HZ] [--windows N] [--seconds S] [--centre HZ --bw HZ] [--snr DB] "
                         "[--seed N] [--spectrogram]\n");
    return 2;
  }
  double rate = 12000, seconds = 8, centre = 0, bw = 0, snr = 1e9;
  int windows = 3;
  unsigned seed = 1;
  bool picture = false;
  for (int i = 2; i < argc; ++i) {
    const std::string o = argv[i];
    auto value = [&] { return i + 1 < argc ? std::atof(argv[++i]) : 0.0; };
    if (o == "--rate") rate = value();
    else if (o == "--windows") windows = static_cast<int>(value());
    else if (o == "--seconds") seconds = value();
    else if (o == "--centre") centre = value();
    else if (o == "--bw") bw = value();
    else if (o == "--snr") snr = value();
    else if (o == "--seed") seed = static_cast<unsigned>(value());
    else if (o == "--spectrogram") picture = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", o.c_str());
      return 2;
    }
  }
  std::FILE* fp = std::fopen(argv[1], "rb");
  if (!fp) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::vector<float> a;
  float buf[4096];
  std::size_t got;
  while ((got = std::fread(buf, 4, 4096, fp)) > 0) a.insert(a.end(), buf, buf + got);
  std::fclose(fp);
  if (a.size() < static_cast<std::size_t>(3 * rate)) return 0;  // too short to measure
  if (snr < 1e8) {
    double p = 0;
    for (float v : a) p += static_cast<double>(v) * v;
    p /= static_cast<double>(a.size());
    const double sigma = std::sqrt(p / std::pow(10.0, snr / 10) * (rate / 2) / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0, static_cast<float>(sigma));
    for (float& v : a) v += n(rng);
  }
  if (centre <= 0 || bw <= 0) findBand(a, rate, centre, bw);
  const auto len = static_cast<std::size_t>(seconds * rate);
  const double total = static_cast<double>(a.size()) / rate;
  windows = std::max(1, std::min(windows, static_cast<int>(total / seconds) + 1));
  for (int w = 0; w < windows; ++w) {
    // Spread over the file, the first a second in when there is room.
    double start = windows == 1 ? std::min(1.0, std::max(0.0, total - seconds - 0.5))
                                : 1.0 + (total - seconds - 1.5) * w / (windows - 1);
    start = std::clamp(start, 0.0, std::max(0.0, total - seconds));
    const auto s0 = static_cast<std::size_t>(start * rate);
    const std::size_t n = std::min(len, a.size() - s0);
    hfc::FeatureMeter meter(rate, centre, seconds);
    meter.add(a.data() + s0, n);
    const hfc::EmissionFeatures f = meter.measure(bw);
    if (!f.valid || f.vector.size() != hfc::kFeatureVector) continue;
    std::string line = "{\"centre\":" + std::to_string(centre) + ",\"bw\":" + std::to_string(bw) + ",\"start\":" +
                       std::to_string(start) + ",\"modulation\":" + hfc::jsonQuote(f.modulation) + ",\"summary\":" +
                       hfc::jsonQuote(f.summary()) + ",\"vector\":[";
    char num[32];
    for (std::size_t i = 0; i < f.vector.size(); ++i) {
      std::snprintf(num, sizeof num, "%s%.5g", i ? "," : "", static_cast<double>(f.vector[i]));
      line += num;
    }
    line += "]";
    if (picture) {
      line += ",\"spectrogram\":[";
      const std::vector<float> img = spectrogram(a.data() + s0, n, rate, centre, bw);
      for (std::size_t i = 0; i < img.size(); ++i) {
        std::snprintf(num, sizeof num, "%s%.3g", i ? "," : "", static_cast<double>(img[i]));
        line += num;
      }
      line += "]";
    }
    std::printf("%s}\n", line.c_str());
  }
  return 0;
}
