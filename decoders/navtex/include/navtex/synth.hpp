// FSK test-signal synthesiser: builds a complete SITOR-B emission (phasing,
// text with FEC interleaving, end-of-emission idle), an RTTY emission, or raw
// bits, and modulates it as continuous-phase FSK with controlled
// impairments. SITOR-B text may be NAVTEX messages (navtexMessage) or any
// other text (teleprinterText). Used by the tests, the validation program and
// navtex_gen.
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace navtex {

struct SynthConfig {
  double sampleRate = 48000.0;
  double centreHz = 1000.0;
  double shiftHz = 170.0;
  double baud = 100.0;
  bool reversed = false;         // B bits on the lower tone
  double amplitude = 0.5;        // peak amplitude of the FSK signal
  double snrDb = 100.0;          // signal power over the noise power in 2500 Hz
  std::uint32_t seed = 1;

  double leadSeconds = 1.0;      // noise before the emission
  double tailSeconds = 1.0;      // noise after it
  double phasingSeconds = 10.0;  // phasing signals before the first message
  double idleSeconds = 2.0;      // end-of-emission idle (alpha)
  double startAtSeconds = 0.0;   // skip the first part of the emission (join it late)

  double driftHz = 0.0;          // linear frequency drift over the emission
  double baudErrorPpm = 0.0;     // transmitter clock error
  double qsbDepthDb = 0.0;       // flat fading, peak to trough
  double qsbRateHz = 0.1;
  double selectiveDepthDb = 0.0; // the two tones fading independently, peak to trough
  double selectiveRateHz = 0.2;
  double impulsesPerSecond = 0.0;  // static crashes
  double impulseDb = 20.0;         // crash peak relative to the signal peak
  double carrierHz = 0.0;          // steady interfering carrier (0: none)
  double carrierDb = 0.0;          // carrier level relative to the signal
};

struct SynthResult {
  std::vector<float> samples;  // mono
  double emissionStart = 0.0;  // time the first bit starts
  double emissionEnd = 0.0;    // time the last bit ends
};

// Random numbers that are the same with every compiler: std::mt19937 is
// fully specified, but the standard distributions are not, so the same seed
// would give other test signals on Windows than on Linux.
class Noise {
 public:
  explicit Noise(std::uint32_t seed) : rng_(seed) {}
  // Uniform in [0, 1).
  double uniform();
  // Normal, mean 0 and standard deviation 1.
  double gauss();

 private:
  std::mt19937 rng_;
  double spare_ = 0.0;
  bool haveSpare_ = false;
};

// Text as sent on a teleprinter circuit: every '\n' becomes CR LF.
std::string teleprinterText(const std::string& text);

// Full text of one NAVTEX message: "ZCZC " id CR LF body CR LF "NNNN" CR LF.
// `body` uses '\n' for line ends; they are sent as CR LF.
std::string navtexMessage(const std::string& id, const std::string& body);

// Text as the decoder prints it: CR removed, lower case upper-cased,
// characters CCIR 476 cannot carry removed.
std::string expectedText(const std::string& text);

// FEC-interleaved slot sequence (7-bit codes) for an emission carrying `text`.
std::vector<std::uint8_t> fecStream(const std::string& text, int phasingPairs, int idlePairs);

// A SITOR-B emission carrying `text`: phasingSeconds of phasing signals,
// the text with FEC interleaving, idleSeconds of idle signals.
SynthResult synthesize(const std::string& text, const SynthConfig& config);

// An RTTY emission carrying `text` in ITA2 at config.baud, each character
// with a start bit, five data bits and `stopBits` of stop: phasingSeconds of
// letters shifts first, idleSeconds of steady mark after.
SynthResult synthesizeRtty(const std::string& text, const SynthConfig& config, double stopBits = 1.5);

// Synchronous bits at config.baud (true: the "B" / mark tone).
SynthResult synthesizeBits(const std::vector<bool>& bits, const SynthConfig& config);

// The keying as a sequence of elements, each a tone held for a number of
// (possibly fractional) bit periods, modulated with the impairments.
struct KeyingElement {
  bool mark;    // true: the higher tone, unless config.reversed
  double bits;  // duration in bit periods
};
std::vector<KeyingElement> rttyKeying(const std::string& text, const SynthConfig& config, double stopBits);
SynthResult modulate(const std::vector<KeyingElement>& elements, const SynthConfig& config);

}  // namespace navtex
