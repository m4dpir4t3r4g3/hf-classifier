// Radiofax (weather fax, WEFAX; press photos the same way): a picture sent
// line by line as frequency modulation of an audio subcarrier, black at
// 1500 Hz and white at 2300 Hz in an upper-sideband receiver tuned 1.9 kHz
// below the assigned frequency (deviation +-400 Hz). 120 lines a minute
// for almost all HF weather charts (60, 90 and 240 exist), and an "index of
// cooperation" of 576 (1810 picture elements a line) or 288.
//
// A transmission: a start tone (black and white alternating at 300 Hz for
// IOC 576, 675 Hz for IOC 288; 5 s), phasing lines (white with a short
// black pulse where the line begins; 30 s), the picture, a stop tone
// (450 Hz, 5 s), black.
//
// The decoder demodulates the frequency, finds black and white by
// themselves (the 3rd and 97th percentile of the frequency: a receiver
// tuned off, or a session centred on the signal's middle, moves both),
// finds the line rate from the signal's repetition, and builds the picture.
// It reports a fax when at least 20 s repeat at one of the line rates and
// do not repeat between them, with black and white 250 to 1050 Hz apart
// (long-wave stations use +-150 Hz);
// a start or stop tone is noted when seen but is not needed (most
// receptions start in the middle of a chart). Phasing lines, when seen,
// set where a line begins; otherwise the picture is cut where it is. The
// line's true length is measured from the picture itself (how it repeats
// 1, 10 and 100 lines later), so that a clock that differs from the
// transmitter's by up to 1 % does not slant it.
//
// Sources: the numbers above are those of the WMO's facsimile standard
// (Manual on the Global Telecommunication System, WMO-No. 386) as NOAA's
// "Worldwide Marine Radiofacsimile Broadcast Schedules" gives them; the
// phasing line is taken either way round (white with a black pulse, or
// black with a white one). Tested on this file's own modulator and on the
// Signal Identification Wiki's recordings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

// A received picture, row by row: 8-bit grey (0 black), or with `colour`
// three octets (red, green, blue) for every element (SSTV).
struct FaxImage {
  int width = 0, height = 0;
  bool colour = false;
  std::string what;  // SSTV: the mode's name
  int linesPerMinute = 0, ioc = 0;
  bool phased = false;  // the line start was found from phasing lines
  std::vector<uint8_t> grey;
};

// The picture as a PNG file's bytes (8 bits, grey or RGB). Compressed only by
// run lengths of equal bytes (charts are mostly white), with zlib's format.
std::string faxPng(const FaxImage& image);

class FaxDecoder {
 public:
  // Real audio at `rate`, the signal's middle (between black and white)
  // near `centreHz` (within about 400 Hz).
  FaxDecoder(double rate, double centreHz);
  ~FaxDecoder();
  void process(const float* audio, std::size_t n);
  // True once the audio so far is taken for a fax (see above); then
  // linesPerMinute() is its rate.
  bool recognised() const;
  int linesPerMinute() const;
  // Lines of picture so far.
  int lines() const;
  // Pictures completed since the last call (ended by a stop tone, by a new
  // start tone, or at the size limit of 30 minutes).
  std::vector<std::shared_ptr<FaxImage>> take();
  // The picture in progress (at the end of the signal); null if there is
  // none of at least 20 lines.
  std::shared_ptr<FaxImage> finish();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// For tests and the demo: `image` as radiofax audio at `rate` (black 1500,
// white 2300 Hz), optionally with start tone, phasing lines and stop tone.
std::vector<float> faxModulate(const FaxImage& image, double rate, bool withTones, double shiftHz = 0);
// A test chart: `lines` lines of 1810 elements with a frame, bars and
// diagonal lines, so that a wrong line rate or line start shows.
FaxImage faxTestChart(int lines);

}  // namespace hfc
