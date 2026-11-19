// Automatic frequency control: finds the pair of FSK tones in the audio and
// follows their centre frequency.
//
// A Blackman-Harris windowed FFT (about 11 Hz per bin) is averaged over
// about a second.
// For every candidate centre c, the power in a 70 Hz band around each tone
// (c - shift/2 and c + shift/2) is measured, and the score is the weaker of
// the two. An FSK signal puts power into both bands; a steady carrier or a
// Morse signal puts it into only one, so it scores at noise level and never
// captures the tracker. The score is compared with the median band power over
// the search range, which is the noise level.
//
// Some interference can still look like a tone pair (for example the skirt of
// a very strong carrier next to a genuine tone). The decoder then finds no
// character sync, and calls reject(): the tracker unlocks and leaves that
// frequency alone for a while.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "navtex/dsp.hpp"
#include "navtex/types.hpp"

namespace navtex {

class CentreTracker {
 public:
  void configure(const SitorBConfig& config);
  void reset();

  // One mono sample. Returns true when a new spectrum was analysed.
  bool process(double x);

  // The locked signal is not SITOR-B: unlock and exclude this centre for 30 s.
  void reject();

  double centreHz() const { return centre_; }
  bool locked() const { return locked_; }
  bool signalPresent() const { return present_; }
  double snrDb() const { return snrDb_; }
  // Distance between the two tones' centroids while the signal is present,
  // averaged; 0 before it is known. Only tones within about 35 Hz of the
  // configured shift are measured (the tracker looks for that shift).
  double measuredShiftHz() const { return measuredShift_; }
  void fillSpectrum(SpectrumFrame& frame) const;

 private:
  void analyse();
  double cumulative(double bin) const;
  double band(double bin) const;  // power in the tone band centred on a (fractional) bin
  double score(double centreBin) const;
  double refine(double centreBin) const;  // centre from the centroids of both tones, in Hz
  double peakCentroid(double bin) const;  // centroid of one tone's band, in bins; <0 if none
  bool excluded(double hz) const;

  struct Excluded {
    double hz = 0.0, until = -1.0;
  };
  Excluded excluded_[4];
  double time_ = 0.0;  // seconds analysed since reset

  SitorBConfig cfg_;
  Fft fft_;
  std::size_t n_ = 4096, hop_ = 2048, fill_ = 0, sinceFrame_ = 0;
  std::vector<double> ring_, window_;
  std::size_t ringPos_ = 0;
  std::vector<std::complex<double>> work_;
  std::vector<double> power_, prefix_;
  std::vector<double> scratch_;
  double binHz_ = 11.7, alpha_ = 0.05;
  std::size_t frames_ = 0;
  std::size_t loBin_ = 0, hiBin_ = 0;  // analysed bin range
  double noiseBand_ = 0.0, noiseBin_ = 0.0;

  double centre_ = 1000.0;
  bool locked_ = false, present_ = false;
  double snrDb_ = 0.0;
  double measuredShift_ = 0.0;
  double candidate_ = -1.0;
  int candidateFrames_ = 0, absentFrames_ = 0;
};

}  // namespace navtex
