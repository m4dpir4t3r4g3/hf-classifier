// The demo: a synthetic band to try the program without a receiver
// (hf_classifier --demo). Two parts, played one after the other in a loop:
//
//   14.080 MHz   amateur modes, made by each decoder's own generator: NAVTEX,
//                RTTY, CW, PSK31, ALE 2G, FT8, FT4 (as make_test_scene)
//   8.400 MHz    a maritime and utility band: a GMDSS DSC call on
//                8414.5 kHz (a listed frequency), SITOR-B, and signals built
//                like military systems from their published parameters:
//                serial 8PSK at 2400 Bd with a known preamble every 256
//                symbols (as STANAG 4285), bursts of 500 Bd / 1000 Hz FSK
//                with Akula's published sync and end groups, 50 Bd / 200 Hz
//                FSK in 7-bit words of four ones (T-600's kind of code), a
//                16-tone parallel modem, and a 16-tone MFSK
//
// Every signal is synthetic and carries random or test data; none is a
// recording. Signals are 15 dB over the noise in 2.5 kHz.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hfc {
namespace testing {

struct DemoPart {
  double centreHz = 0;      // the receiver's centre
  double sampleRate = 0;
  std::vector<int16_t> iq;  // interleaved I, Q
  std::string description;
};

std::vector<DemoPart> demoScene();

// A frequency list for the demo's second part, in FrequencyList's text
// format: one entry for the STANAG-4285-like modem.
std::string demoFrequencyList();

}  // namespace testing
}  // namespace hfc
