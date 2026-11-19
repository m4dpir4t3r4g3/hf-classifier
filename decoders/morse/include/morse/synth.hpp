// Synthesises Morse audio with controllable impairments. Used by the test
// suite, the benchmark and the example-signal generator.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace morse {

struct SynthConfig {
  double sampleRate = 48000.0;
  double wpm = 20.0;
  double farnsworthWpm = 0.0;  // > 0 and < wpm: stretch character/word gaps
  double toneHz = 700.0;
  double amplitude = 0.5;
  double riseMs = 5.0;         // raised-cosine key shaping
  double dahRatio = 3.0;       // dah length in dits ("weight")
  double jitter = 0.0;         // std-dev of element/gap lengths, fraction of a dit
  double snrDb = 100.0;        // tone power over noise power in a 2500 Hz band
  double qsbDepthDb = 0.0;     // peak-to-trough sinusoidal fading depth
  double qsbHz = 0.2;
  double driftHz = 0.0;        // total linear tone drift over the transmission
  // Receiver audio filter applied to everything (signal, QRM and noise), as
  // SDRuno's demodulator filter would: 0 = none. E.g. 300/2700 for USB.
  double passbandLowHz = 0.0;
  double passbandHighHz = 0.0;
  double leadSeconds = 1.0;    // noise before and after the text
  double tailSeconds = 1.0;
  std::uint32_t seed = 1;
};

struct Interferer {
  std::string text;
  double wpm = 25.0;
  double toneHz = 1000.0;
  double levelDb = -6.0;  // relative to the wanted signal
  bool carrier = false;   // steady unkeyed carrier instead of Morse
};

struct SynthResult {
  std::vector<float> samples;
  // For each non-space character of the text: the end time of its last
  // element, in seconds. Used to measure decode latency.
  std::vector<double> charEndTimes;
};

SynthResult synthesize(const std::string& text, const SynthConfig& config,
                       const std::vector<Interferer>& interferers = {});

}  // namespace morse
