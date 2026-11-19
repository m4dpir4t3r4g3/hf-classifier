#include "morse/synth.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "morse/dsp.hpp"
#include "morse/morse_table.hpp"

namespace morse {

namespace {

// Box-Muller on top of mt19937 so results match across standard libraries.
class Gaussian {
 public:
  explicit Gaussian(std::uint32_t seed) : rng_(seed) {}
  double operator()() {
    if (haveSpare_) {
      haveSpare_ = false;
      return spare_;
    }
    double u1, u2;
    do {
      u1 = (rng_() + 0.5) / 4294967296.0;
      u2 = (rng_() + 0.5) / 4294967296.0;
    } while (u1 <= 0.0);
    double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2.0 * kPi * u2);
    haveSpare_ = true;
    return r * std::cos(2.0 * kPi * u2);
  }

 private:
  std::mt19937 rng_;
  double spare_ = 0.0;
  bool haveSpare_ = false;
};

struct Mark {
  double start, end;
};

std::vector<Mark> keying(const std::string& text, const SynthConfig& c, Gaussian& g,
                         std::vector<double>* charEnds) {
  double unit = 1.2 / c.wpm;
  double charGap = 3.0 * unit, wordGap = 7.0 * unit;
  if (c.farnsworthWpm > 0.0 && c.farnsworthWpm < c.wpm) {
    // ARRL Farnsworth timing: extra delay spread over character and word gaps.
    double ta = (60.0 * c.wpm - 37.2 * c.farnsworthWpm) / (c.wpm * c.farnsworthWpm);
    charGap = 3.0 * ta / 19.0;
    wordGap = 7.0 * ta / 19.0;
  }
  auto vary = [&](double d) { return std::max(0.3 * unit, d + c.jitter * unit * g()); };

  std::vector<Mark> marks;
  double t = c.leadSeconds;
  for (const auto& tok : tokenize(text)) {
    if (tok.text == " ") {
      t += vary(wordGap) - vary(charGap);  // the char gap was already added
      continue;
    }
    for (std::size_t i = 0; i < tok.pattern.size(); ++i) {
      if (i) t += vary(unit);
      double d = vary(tok.pattern[i] == '-' ? c.dahRatio * unit : unit);
      marks.push_back({t, t + d});
      t += d;
    }
    if (charEnds) charEnds->push_back(t);
    t += vary(charGap);
  }
  return marks;
}

void render(const std::vector<Mark>& marks, double amplitude, double toneHz, double driftHz,
            double riseSec, double fs, std::vector<float>& out, double gainDb = 0.0,
            const SynthConfig* fading = nullptr) {
  double total = static_cast<double>(out.size()) / fs;
  std::vector<float> env(out.size(), 0.0f);
  std::size_t rise = std::max<std::size_t>(1, static_cast<std::size_t>(riseSec * fs));
  for (const auto& m : marks) {
    auto s = static_cast<std::size_t>(m.start * fs);
    auto e = static_cast<std::size_t>(m.end * fs);
    for (std::size_t n = s; n < e + rise && n < out.size(); ++n) {
      double v = 1.0;
      if (n < s + rise) v = 0.5 - 0.5 * std::cos(kPi * static_cast<double>(n - s) / rise);
      if (n >= e) v = std::min(v, 0.5 + 0.5 * std::cos(kPi * static_cast<double>(n - e) / rise));
      env[n] = std::max(env[n], static_cast<float>(v));
    }
  }
  double a = amplitude * std::pow(10.0, gainDb / 20.0), phase = 0.0;
  for (std::size_t n = 0; n < out.size(); ++n) {
    double t = static_cast<double>(n) / fs;
    double f = toneHz + driftHz * t / total;
    phase += 2.0 * kPi * f / fs;
    if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
    if (env[n] == 0.0f) continue;
    double gain = a;
    if (fading && fading->qsbDepthDb > 0.0) {
      double depth = -fading->qsbDepthDb * (0.5 + 0.5 * std::sin(2.0 * kPi * fading->qsbHz * t));
      gain *= std::pow(10.0, depth / 20.0);
    }
    out[n] += static_cast<float>(gain * env[n] * std::sin(phase));
  }
}

}  // namespace

SynthResult synthesize(const std::string& text, const SynthConfig& c,
                       const std::vector<Interferer>& interferers) {
  Gaussian g(c.seed);
  SynthResult r;
  auto marks = keying(text, c, g, &r.charEndTimes);
  double end = marks.empty() ? c.leadSeconds : marks.back().end;
  auto total = static_cast<std::size_t>((end + c.tailSeconds) * c.sampleRate);
  r.samples.assign(total, 0.0f);

  render(marks, c.amplitude, c.toneHz, c.driftHz, c.riseMs / 1000.0, c.sampleRate, r.samples, 0.0, &c);

  for (std::size_t k = 0; k < interferers.size(); ++k) {
    const auto& q = interferers[k];
    if (q.carrier) {
      std::vector<Mark> on{{0.0, static_cast<double>(total) / c.sampleRate}};
      render(on, c.amplitude, q.toneHz, 0.0, 0.005, c.sampleRate, r.samples, q.levelDb);
      continue;
    }
    SynthConfig qc = c;
    qc.wpm = q.wpm;
    // Interfering stations join after the wanted one has started.
    qc.leadSeconds = c.leadSeconds + 2.0 + 0.37 * static_cast<double>(k);
    qc.farnsworthWpm = 0.0;
    Gaussian qg(c.seed * 7919u + static_cast<std::uint32_t>(k));
    // Repeat the interfering text until it covers the whole signal.
    std::string qtext = q.text;
    std::vector<Mark> qm;
    while (true) {
      qm = keying(qtext, qc, qg, nullptr);
      if (qm.empty() || qm.back().end >= static_cast<double>(total) / c.sampleRate) break;
      qtext += " " + q.text;
    }
    render(qm, c.amplitude, q.toneHz, 0.0, c.riseMs / 1000.0, c.sampleRate, r.samples, q.levelDb);
  }

  if (c.snrDb < 99.0) {
    double signalPower = c.amplitude * c.amplitude / 2.0;
    double noiseIn2500 = signalPower / std::pow(10.0, c.snrDb / 10.0);
    double sigma = std::sqrt(noiseIn2500 * (c.sampleRate / 2.0) / 2500.0);
    for (auto& s : r.samples) s += static_cast<float>(sigma * g());
  }
  if (c.passbandLowHz > 0.0 || c.passbandHighHz > 0.0) {
    Butterworth4 hp, lp;
    if (c.passbandLowHz > 0.0) hp.setHighpass(c.sampleRate, c.passbandLowHz);
    if (c.passbandHighHz > 0.0) lp.setLowpass(c.sampleRate, c.passbandHighHz);
    for (auto& s : r.samples) {
      double x = s;
      if (c.passbandLowHz > 0.0) x = hp.process(x);
      if (c.passbandHighHz > 0.0) x = lp.process(x);
      s = static_cast<float>(x);
    }
  }
  return r;
}

}  // namespace morse
