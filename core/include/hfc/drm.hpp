// DRM (Digital Radio Mondiale, ETSI ES 201 980): digital broadcasting on
// LF, MF and HF, OFDM in 4.5 to 20 kHz. Recognised here, not decoded (the
// programme is xHE-AAC or Opus audio behind several layers of coding).
//
// An OFDM symbol is a useful part of length Tu and a guard interval Tg
// that repeats the useful part's end: the signal at t and t + Tu is the
// same over Tg of every symbol, at every one of the carriers (so also in
// a slice of them, as a narrow channel passes). The robustness modes
// (ES 201 980, table 82):
//
//   mode   Tu        Tg        symbol
//   A      24 ms     2.66 ms   26.66 ms
//   B      21.33 ms  5.33 ms   26.66 ms
//   C      14.66 ms  5.33 ms   20 ms
//   D      9.33 ms   7.33 ms   16.66 ms
//
// The detector takes the audio to complex baseband around `centreHz` (the
// cut-out channel's frequency shift turns the phase between t and t + Tu
// by an arbitrary angle, which can cancel a correlation of real audio; the
// magnitude of a complex one does not mind), correlates it with itself Tu
// later, folds that over
// the symbol length, and looks for the guard interval's peak: a mode is
// named when, over three 2 s windows in a row, the correlation within
// Tg of the peak stands clearly above the rest of the symbol. The amateur
// programs WinDRM and EasyPal (pictures and files) use the same modes in
// about 2.5 kHz, and other systems may: a signal narrower than 3.2 kHz is
// labelled "DRM-type OFDM", not DRM.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

class DrmDetector {
 public:
  // Real audio at `rate`, the signal around `centreHz` (+-1400 Hz used).
  DrmDetector(double rate, double centreHz);
  ~DrmDetector();
  void process(const float* audio, std::size_t n);
  // 'A' to 'D' once a mode is found, else 0.
  char mode() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// For tests: OFDM audio in robustness mode `mode` ('A'..'D'), `carriers`
// carriers from `loHz` at the mode's spacing 1/Tu, random QPSK, `seconds`.
std::vector<float> drmTestAudio(char mode, double rate, double loHz, int carriers, double seconds, unsigned seed = 1);

}  // namespace hfc
