// Time signal stations: a carrier or tone that marks every second.
//
// Recognised: anything whose level repeats exactly every second (folded
// over 1 s it has a clear shape; folded over 0.95 or 1.05 s it has none)
// for at least 12 s: DCF77, MSF, RBU, RWM, BPM, WWV/WWVH, CHU, JJY, WWVB
// and the like, by their second pulses, whatever they send between them.
//
// Decoded, where the code is published and was checked on a recording:
//   DCF77 (77.5 kHz): the carrier drops for 0.1 s (a 0) or 0.2 s (a 1) at
//     each second, none at second 59. Minutes, hours and date in BCD with
//     even parity (PTB, "DCF77 time code"); also summer or winter time.
//   MSF (60 kHz): the carrier is off at each second for 0.1 s, plus 0.1 s
//     for bit A, plus 0.1 s for bit B; 0.5 s at second 0. Year to minute in
//     BCD on bits A, odd parity on bits B 54-57, summer time on B 58, and
//     the fixed pattern 01111110 on bits A 52-59 (NPL, "MSF 60 kHz time and
//     date code").
// A minute is reported only when every check passes: all parities, the
// fixed bits, BCD digits in range, a real date. The time is the station's
// civil time (CET/CEST for DCF77, UTC/BST for MSF), as it sends it.
//
// Checked on the Signal Identification Wiki's DCF77 and MSF recordings
// (both decode: 2015-04-05 17:32 CEST and 2024-09-20 20:55 BST); the other
// stations' recordings are recognised by their seconds only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

class TimeSignalDecoder {
 public:
  // Real audio at `rate`; the carrier (or the tone the receiver makes of
  // it) within 400 Hz of `toneHz` (the strongest frequency there in the
  // first second is taken).
  TimeSignalDecoder(double rate, double toneHz);
  ~TimeSignalDecoder();
  void process(const float* audio, std::size_t n);
  // True once the seconds are found.
  bool recognised() const;
  // Lines since the last call: "second pulses" once, then decoded minutes.
  std::vector<std::string> take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// For tests and the demo: the carrier's level (0..1) at 100 values a
// second for `minutes` whole minutes starting at the given civil time, as
// DCF77 ("DCF77") or MSF ("MSF") key it (a minute is found after the
// previous one's mark: the first is never decoded); and audio of a carrier at
// `toneHz` keyed so (the low level is 15 % for DCF77, off for MSF).
std::vector<float> timeCodeLevels(const std::string& station, int year, int month, int day, int hour, int minute, bool summer,
                                  int minutes);
std::vector<float> timeSignalAudio(const std::vector<float>& levels, double rate, double toneHz);

}  // namespace hfc
