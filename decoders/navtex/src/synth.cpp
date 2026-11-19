#include "navtex/synth.hpp"

#include <cmath>

#include "navtex/ccir476.hpp"
#include "navtex/dsp.hpp"
#include "navtex/ita2.hpp"

namespace navtex {

double Noise::uniform() {
  // 53 random bits.
  const std::uint32_t a = rng_() >> 5, b = rng_() >> 6;
  return (a * 67108864.0 + b) / 9007199254740992.0;
}

double Noise::gauss() {
  if (haveSpare_) {
    haveSpare_ = false;
    return spare_;
  }
  const double r = std::sqrt(-2.0 * std::log(1.0 - uniform())), a = 2.0 * kPi * uniform();
  spare_ = r * std::sin(a);
  haveSpare_ = true;
  return r * std::cos(a);
}

std::string teleprinterText(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\n') out += "\r\n";
    else out += c;
  }
  return out;
}

std::string navtexMessage(const std::string& id, const std::string& body) {
  return "ZCZC " + id + "\r\n" + teleprinterText(body) + "\r\nNNNN\r\n";
}

std::string expectedText(const std::string& text) {
  std::string out;
  for (char c : text) {
    std::uint8_t code;
    bool fig;
    if (c == '\r' || !ccir::fromChar(c, code, fig)) continue;
    if (char d = ccir::toChar(code, fig)) out += d;  // 0: bell
  }
  return out;
}

std::vector<std::uint8_t> fecStream(const std::string& text, int phasingPairs, int idlePairs) {
  std::vector<std::uint8_t> codes = encodeText(text), slots;
  for (int i = 0; i < phasingPairs; ++i) {
    slots.push_back(ccir::kRep);
    slots.push_back(ccir::kAlpha);
  }
  // DX of character i, then RX of character i-2 (sent five slots after its DX).
  const std::size_t n = codes.size();
  for (std::size_t i = 0; i < n + 2; ++i) {
    slots.push_back(i < n ? codes[i] : ccir::kAlpha);
    slots.push_back(i >= 2 ? codes[i - 2] : ccir::kAlpha);
  }
  for (int i = 0; i < idlePairs; ++i) {
    slots.push_back(ccir::kAlpha);
    slots.push_back(ccir::kAlpha);
  }
  return slots;
}

SynthResult synthesize(const std::string& text, const SynthConfig& cfg) {
  const double slotSeconds = 7.0 / cfg.baud;
  auto phasingPairs = static_cast<int>(std::lround(cfg.phasingSeconds / (2.0 * slotSeconds)));
  auto idlePairs = static_cast<int>(std::lround(cfg.idleSeconds / (2.0 * slotSeconds)));
  std::vector<std::uint8_t> slots = fecStream(text, phasingPairs, idlePairs);
  std::vector<bool> bits;
  for (auto c : slots)
    for (int i = 0; i < 7; ++i) bits.push_back((c >> i) & 1);
  return synthesizeBits(bits, cfg);
}

SynthResult synthesizeBits(const std::vector<bool>& bits, const SynthConfig& cfg) {
  std::vector<KeyingElement> elements;
  elements.reserve(bits.size());
  for (bool b : bits) elements.push_back({b, 1.0});
  return modulate(elements, cfg);
}

std::vector<KeyingElement> rttyKeying(const std::string& text, const SynthConfig& cfg, double stopBits) {
  std::vector<KeyingElement> out;
  auto character = [&](std::uint8_t code) {
    out.push_back({false, 1.0});  // start
    for (int i = 0; i < 5; ++i) out.push_back({((code >> i) & 1) != 0, 1.0});
    out.push_back({true, stopBits});
  };
  // Before the text: letters shifts ("diddles"), as RTTY stations send.
  const double charBits = 6.0 + stopBits;
  auto diddles = static_cast<int>(std::lround(cfg.phasingSeconds * cfg.baud / charBits));
  for (int i = 0; i < diddles; ++i) character(ita2::kLetters);
  for (auto code : encodeIta2(text)) character(code);
  // After it: steady mark.
  if (cfg.idleSeconds > 0.0) out.push_back({true, cfg.idleSeconds * cfg.baud});
  return out;
}

SynthResult synthesizeRtty(const std::string& text, const SynthConfig& cfg, double stopBits) {
  return modulate(rttyKeying(text, cfg, stopBits), cfg);
}

SynthResult modulate(const std::vector<KeyingElement>& elements, const SynthConfig& cfg) {
  const double fs = cfg.sampleRate;
  // End of each element, in bits from the start of the emission.
  std::vector<double> ends;
  ends.reserve(elements.size());
  double bitCount = 0.0;
  for (const auto& e : elements) ends.push_back(bitCount += e.bits);
  std::size_t element = 0;

  const double bitSeconds = 1.0 / (cfg.baud * (1.0 + cfg.baudErrorPpm * 1e-6));
  const double emission = bitCount * bitSeconds;
  const double skip = std::min(cfg.startAtSeconds, emission);
  const double total = cfg.leadSeconds + (emission - skip) + cfg.tailSeconds;
  const auto count = static_cast<std::size_t>(total * fs);

  SynthResult r;
  r.samples.assign(count, 0.0f);
  r.emissionStart = cfg.leadSeconds - skip;
  r.emissionEnd = cfg.leadSeconds + emission - skip;

  Noise rng(cfg.seed);
  const double a = cfg.amplitude;
  const double signalPower = a * a / 2.0;
  // Noise power over the full band (fs/2) for the requested power in 2500 Hz.
  const double noiseSigma = std::sqrt(signalPower / std::pow(10.0, cfg.snrDb / 10.0) * (fs / 2.0) / 2500.0);
  const double qsbPhase = rng.uniform() * 2.0 * kPi, markPhase = rng.uniform() * 2.0 * kPi, spacePhase = rng.uniform() * 2.0 * kPi;
  auto fade = [](double depthDb, double rate, double phase, double t) {
    if (depthDb <= 0.0) return 1.0;
    double x = 0.5 - 0.5 * std::cos(2.0 * kPi * rate * t + phase);  // 0..1
    return std::pow(10.0, -depthDb * x / 20.0);
  };

  double phase = 0.0, carrierPhase = 0.0;
  double crash = 0.0, crashDecay = std::exp(-1.0 / (0.004 * fs));  // 4 ms static crashes
  const double crashPeak = a * std::pow(10.0, cfg.impulseDb / 20.0);
  const double crashProb = cfg.impulsesPerSecond / fs;
  const double carrierAmp = a * std::pow(10.0, cfg.carrierDb / 20.0);
  for (std::size_t n = 0; n < count; ++n) {
    double t = static_cast<double>(n) / fs;
    double te = t - cfg.leadSeconds + skip;  // time within the emission
    double v = 0.0;
    if (te >= skip && te < emission) {
      const double at = te / bitSeconds;  // in bits
      while (element + 1 < elements.size() && at >= ends[element]) ++element;
      bool b = elements[element].mark;
      bool high = b != cfg.reversed;
      double f = cfg.centreHz + cfg.driftHz * te / emission + (high ? 0.5 : -0.5) * cfg.shiftHz;
      phase += 2.0 * kPi * f / fs;
      if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
      double g = fade(cfg.qsbDepthDb, cfg.qsbRateHz, qsbPhase, t) *
                 fade(cfg.selectiveDepthDb, cfg.selectiveRateHz, high ? markPhase : spacePhase, t);
      v = a * g * std::sin(phase);
    }
    if (cfg.carrierHz > 0.0) {
      carrierPhase += 2.0 * kPi * cfg.carrierHz / fs;
      if (carrierPhase > 2.0 * kPi) carrierPhase -= 2.0 * kPi;
      v += carrierAmp * std::sin(carrierPhase);
    }
    if (crashProb > 0.0 && rng.uniform() < crashProb) crash = crashPeak;
    v += crash * rng.gauss();
    crash *= crashDecay;
    v += noiseSigma * rng.gauss();
    r.samples[n] = static_cast<float>(v);
  }
  return r;
}

}  // namespace navtex
