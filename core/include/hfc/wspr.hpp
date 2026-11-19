// WSPR (Weak Signal Propagation Reporter, K1JT): amateur beacons that send
// their callsign, Maidenhead locator and power in 2-minute slots. 162
// symbols of 4-FSK, 1.4648 Bd with tones 1.4648 Hz apart (12000/8192), from
// 1 s after an even minute: 110.6 s, 6 Hz wide, in a 200 Hz sub-band 1400
// to 1600 Hz above the dial frequency.
//
// The message (50 bits: callsign 28, locator and power 22) is encoded by a
// convolutional code K = 32, rate 1/2 (polynomials 0xF2D05351 and
// 0xE4613C47) with 31 zero bits of tail, interleaved by reversing the bits
// of each position's 8-bit index, and each coded bit d sent as the tone
// 2d + s, where s is the next bit of a fixed 162-bit sync vector. (G4JNT,
// "The WSPR Coding Process"; the constants as wsprd has them, read to
// check: GPL, no code taken.)
//
// The decoder searches the sub-band and the slot's first seconds for the
// sync vector, then decodes the data with a sequential (stack) decoder.
// WSPR has no checksum: a message is reported only if its callsign has
// the form of a callsign, its locator is a real square, its power one of
// the allowed values, and the message, coded again, agrees with at least
// 75 % of the received tones. Only the standard message (type 1) is
// decoded: compound callsigns and 6-character locators (types 2 and 3)
// are not.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hfc {

struct WsprMessage {
  std::string callsign, locator;
  int powerDbm = 0;
  double audioHz = 0;   // the signal's middle (between tones 1 and 2)
  double dtSeconds = 0; // start relative to the slot (nominal 1 s)
  double snrDb = 0;     // in 2500 Hz, as WSPR reports it (estimated)
  std::string text() const;  // "K1ABC FN42 37 dBm"
};

// Decodes the slot's audio (12000 samples a second from the slot's start,
// at least 113 s of it), signals between `loHz` and `hiHz`.
std::vector<WsprMessage> wsprDecode(const std::vector<float>& audio, double rate = 12000, double loHz = 1380,
                                    double hiHz = 1620);

// The dial frequencies of the WSPR sub-bands (kHz): WSJT-X's defaults.
const std::vector<double>& wsprDialFrequenciesKhz();

// For tests and the demo: the 162 tones of a message (empty if it cannot
// be packed), and the audio of a slot with it at `audioHz` from 1 s.
std::vector<int> wsprTones(const std::string& callsign, const std::string& locator, int powerDbm);
std::vector<float> wsprAudio(const std::vector<int>& tones, double audioHz, double rate = 12000, double seconds = 120);

}  // namespace hfc
