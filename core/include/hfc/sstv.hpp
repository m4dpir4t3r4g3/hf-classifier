// SSTV (slow-scan television): amateur colour pictures sent as frequency
// modulation of an audio tone, 1500 Hz black to 2300 Hz white, with sync
// pulses at 1200 Hz. A picture begins with a header that names its mode
// (VIS): 1900 Hz for 300 ms, 1200 Hz for 10 ms, 1900 Hz for 300 ms, then a
// start bit (1200 Hz), seven data bits lowest first (1100 Hz a one, 1300 Hz
// a zero), an even parity bit and a stop bit (1200 Hz), 30 ms each.
//
// Modes decoded, with the line timing of each (N7CXI, "Proposal for SSTV
// Mode Specifications", Dayton 2000; the same numbers as in slowrx's
// table, which was read to check them):
//   Martin M1, M2; Scottie S1, S2, DX: green, blue, red, 320 x 256
//   Robot 36, Robot 72: brightness and colour differences, 320 x 240
//   PD 50, 90, 120, 160, 180, 240, 290: two lines per sync, up to 800 x 616
//
// The decoder finds the header (its parity must be right and its code one
// of the modes above); or, when reception began inside a picture, the mode
// from the time between sync pulses (at least 6 of them at one of the
// Martin, Scottie and PD modes' line times within 0.7 %; not the Robot
// modes, whose line times other modes share). The line length and start are then fitted to the sync
// pulses found, so that a clock that differs does not slant the picture,
// and the tuning error is taken from the header's or the sync's frequency.
// Reported: the mode when the header (or the sync train) is found, and
// the picture when it is complete or the signal ends.
//
// Tested on this file's own modulator and on the Signal Identification
// Wiki's recordings of Martin, Scottie and Robot modes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hfc/fax.hpp"

namespace hfc {

class SstvDecoder {
 public:
  // Real audio at `rate` with the signal as in an upper-sideband receiver
  // moved by `shiftHz` (1900 Hz, between sync and white, at 1900 + shift).
  // Tolerates about +-150 Hz more.
  explicit SstvDecoder(double rate, double shiftHz = 0);
  ~SstvDecoder();
  void process(const float* audio, std::size_t n);
  // The mode being received ("" if none): "Martin M1", ...; and how it
  // was found.
  std::string mode() const;
  bool fromHeader() const;
  // Pictures completed since the last call.
  std::vector<std::shared_ptr<FaxImage>> take();
  // What there is of a picture in progress (the signal ended); null if
  // fewer than 16 lines.
  std::shared_ptr<FaxImage> finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// For tests and the demo: the names of the modes; a test picture; and
// `image` (colour) as SSTV audio in `mode`, with its header.
std::vector<std::string> sstvModes();
FaxImage sstvTestPicture(const std::string& mode);
std::vector<float> sstvModulate(const FaxImage& image, const std::string& mode, double rate, bool withHeader = true,
                                double shiftHz = 0);

}  // namespace hfc
