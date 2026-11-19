// Measured features of one emission, from its audio (USB, the emission
// centred at a known audio frequency): the first layer of classifying what
// no decoder recognises (docs/PLAN.md, milestone 4).
//
//   tones          distinct spectral lines in the emission's band (FSK: one
//                  per frequency used; a PSK or OFDM signal has none or a
//                  few; a carrier one)
//   tone spacing   the median gap between adjacent lines; 2-FSK: the shift
//   symbol rate    the strongest periodicity of the signal's transitions:
//                  a line at the symbol rate in the spectrum of the
//                  discriminator's changes (FSK, PSK) or of the envelope
//                  (PSK, ASK); its strength above the spectrum around it
//   envelope       variation of the amplitude (standard deviation / mean):
//                  FSK and MFSK are near 0, PSK31's reversals and OFDM are
//                  0.4 and more, Morse keying is bimodal
//   frame          the period at which the signal repeats itself, from the
//                  autocorrelation of its complex baseband at lags of 10 to
//                  650 ms: a modem's frame (a known sequence sent in every
//                  frame: STANAG 4285's 80-symbol preamble every 256
//                  symbols, 106.67 ms) or any other repeated pattern. Its
//                  strength |R(T)| / R(0) is about the fraction of the
//                  signal that repeats (0.31 for STANAG 4285), less with
//                  noise; random data gives about 1 / sqrt(bandwidth x
//                  duration). Taken only when it stands out from the lags
//                  around it and repeats at twice the period: a carrier, the
//                  beat of two FSK tones and a modem's symbols, which also
//                  correlate at many lags, give no frame
//   modulation     a cautious summary of the above: "FSK", "MFSK",
//                  "multi-carrier", "PSK", "OOK", "carrier", or "" when the
//                  measurements do not agree with one of these
//
// Measured on up to `maxSeconds` of audio after the emission started. These
// are measurements, not identifications: a 2-FSK signal of 850 Hz shift and
// 75 Bd is consistent with several catalogued modes.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace hfc {

// A catalogued mode the features are consistent with (Catalogue::match).
struct ConsistentMode {
  std::string mode, provenance;
  bool partial = false;  // a parameter the catalogue lists was not measured
  bool frame = false;    // the measured frame period agreed with the catalogue's
  bool listed = false;   // a frequency list names this mode here (FrequencyList)
};

// A frequency list's entry at or near the emission (FrequencyList::lookup).
struct ListedHere {
  double hz = 0;
  std::string label, mode, source;
};

constexpr std::size_t kFeatureVector = 86;

struct EmissionFeatures {
  bool valid = false;           // enough audio was measured
  double seconds = 0;           // audio measured
  int tones = 0;
  std::vector<double> toneHz;   // offsets of the lines from the emission's centre
  double toneSpacingHz = 0;     // median adjacent spacing (0: fewer than two tones)
  double symbolRate = 0;        // Bd, 0: no clear line
  double symbolRateDb = 0;      // the line's strength above its surroundings
  double envelopeCv = 0;        // standard deviation / mean of the amplitude
  double offFraction = 0;       // fraction of the time with the amplitude below 20 % of its median when on (keying)
  double frameMs = 0;           // repetition period (ms), 0: none found
  double frameStrength = 0;     // |R(T)| / R(0) at that period
  std::string modulation;
  // A fixed-length description of the signal for the learned family model
  // (FamilyModel): kFeatureVector numbers, see features.cpp. Not in json().
  std::vector<float> vector;
  // The learned model's opinion (FamilyModel; filled in by the decoder
  // host): the family it finds most probable, that probability, whether it
  // is sure enough to name it, and the model's version. A prediction,
  // unlike everything above, which is measured.
  std::string predicted;
  double predictedConfidence = 0;
  bool predictedSure = false;
  std::string modelVersion;
  // Catalogued modes consistent with these measurements, best first
  // (filled in by the decoder host from Catalogue::builtin()).
  std::vector<ConsistentMode> consistent;
  // What frequency lists give at or near this emission's frequency (filled
  // in by the decoder host). Evidence, not an identification.
  std::vector<ListedHere> listed;
  // "2-FSK, shift 170 Hz, 45.5 Bd", "PSK, 2400 Bd, frame 106.7 ms": for
  // logs and exports.
  std::string summary() const;
  // {"modulation":"FSK","tones":2,"spacing_hz":170,"symbol_rate":45.45,...}
  std::string json() const;
};

class FeatureMeter {
 public:
  // `rate`: the audio's sample rate; `audioCentre`: where the emission's
  // centre lies in the audio.
  FeatureMeter(double rate, double audioCentre, double maxSeconds = 8.0);
  void add(const float* audio, std::size_t n);  // stores until maxSeconds
  double seconds() const;
  bool full() const;
  // The audio stored so far (from the start of the emission's session).
  const std::vector<float>& audio() const { return audio_; }
  // Measures the stored audio; `bandwidthHz`: the emission's occupied band.
  EmissionFeatures measure(double bandwidthHz) const;

 private:
  double rate_, centre_;
  std::size_t max_;
  std::vector<float> audio_;
};

}  // namespace hfc
