// Public configuration, event and status types shared by the decoder, the
// command line tools and the SDRuno plugin.
#pragma once

#include <cstdint>

namespace morse {

struct DecoderConfig {
  double sampleRate = 48000.0;
  int channels = 1;  // interleaved input channels; they are averaged

  // Tone search range and automatic frequency control. The default covers
  // both a CW filter around the usual 600-800 Hz pitch and a 2.4-3 kHz SSB
  // (USB/LSB) passband. Narrow it to single out one of several stations.
  double minToneHz = 200.0;
  double maxToneHz = 2800.0;
  bool afc = true;
  double fixedToneHz = 700.0;  // used when afc == false, and as the start value

  // Detection bandwidth (the tone band-pass is implemented as a complex
  // mixer followed by a low-pass of half this width). With autoBandwidth the
  // width follows the keying speed within [minBandwidthHz, maxBandwidthHz].
  bool autoBandwidth = true;
  double bandwidthHz = 200.0;
  double minBandwidthHz = 100.0;
  double maxBandwidthHz = 500.0;

  // Key-down threshold as a fraction of the way from the noise floor to the
  // signal level (0..1), with +/- hysteresis around it.
  double threshold = 0.5;
  double hysteresis = 0.12;
  // Squelch: minimum envelope level above the noise floor, in dB, for a
  // key-down. 0 leaves it to the automatic noise-statistics floor; raise it
  // to ignore weak signals. (Noise alone is already kept from printing by the
  // spectral signal-presence check.)
  double squelchDb = 0.0;

  double minWpm = 5.0;
  double maxWpm = 50.0;

  // Try to split invalid patterns into two valid characters.
  bool errorCorrection = true;
};

// One decoded character (or a word space " ").
struct DecodedChar {
  char text[12] = {};     // "A", "<SK>", " ", or "*" for an undecodable pattern
  char pattern[16] = {};  // dits and dahs, empty for a word space
  float confidence = 0;   // 0..1
  float wpm = 0;          // speed estimate when the character was decoded
  double time = 0;        // stream time in seconds at which it was emitted
  bool corrected = false; // produced by error correction (pattern split)
};

struct DecoderStatus {
  double toneHz = 0;
  bool toneLocked = false;
  double wpm = 0;
  bool speedLocked = false;
  double snrDb = 0;       // envelope signal-to-noise ratio
  double bandwidthHz = 0;
  bool keyDown = false;
  bool signalPresent = false;
  double quality = 0;     // running mean of character confidence, 0..1
  std::uint64_t charsDecoded = 0;
};

// Envelope trace for the signal activity display (~200 per second).
struct ActivitySample {
  float level = 0;      // envelope, normalised to the signal level (1.0)
  float threshold = 0;  // key-down threshold, same scale
  float noise = 0;      // noise floor, same scale
  bool keyDown = false;
};

// Averaged audio spectrum for display (about 10 per second), from 0 Hz up.
struct SpectrumFrame {
  static constexpr int kBins = 320;  // 0-3750 Hz at 48 kHz, 0-3445 Hz at 44.1 kHz
  float binHz = 0;
  int bins = 0;                      // valid entries in db
  float db[kBins] = {};              // averaged power, dB (arbitrary reference)
  float toneHz = 0;
  bool toneLocked = false;
  float searchMinHz = 0, searchMaxHz = 0;
};

class DecoderSink {
 public:
  virtual ~DecoderSink() = default;
  virtual void onCharacter(const DecodedChar&) {}
  virtual void onActivity(const ActivitySample&) {}
  virtual void onSpectrum(const SpectrumFrame&) {}
};

}  // namespace morse
