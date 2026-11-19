// 7-bit constant-weight codes: every character a 7-bit word with four bits
// of one kind and three of the other. CIS-36-50 and CIS-50-50 (T-600) use
// one (Priyom, i56578 and Rivet; docs/RESEARCH.md section 3.1); so do
// SITOR (ITU-R M.476) and the ARQ systems built on its alphabet or on ITA3
// (ARQ-E3, ARQ-N, POL-ARQ, SWED-ARQ: on the Signal Identification Wiki's
// samples of those the code is found as well). The finding names the code,
// not the system; rate and shift (the catalogue) narrow it down. That
// can be tested without reading anything: over 60 consecutive words at the
// right alignment nearly all have that weight, where random bits manage
// 27 % (35 of the 128 words), or about 40 % at the best of the seven
// alignments. Reported once, at 75 % or more (nine standard deviations
// above chance); the traffic is encrypted and nothing of it is shown.
//
// At 100 Bd / 170 Hz, where SITOR is decoded, this detector is not run.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

// From bits to the finding.
class ConstantWeightParser {
 public:
  static constexpr int kWords = 60;
  // Returns true once, when 60 consecutive words at one alignment reach the
  // share; share() and ones() then describe it.
  bool bit(bool one);
  double share() const { return share_; }
  int ones() const { return ones_; }  // 4 or 3: which kind the four are

 private:
  uint64_t count_ = 0;
  uint8_t word_ = 0;
  // Per alignment and weight (4 ones, 3 ones): the last kWords words' flags.
  std::deque<bool> flags_[7][2];
  int hits_[7][2] = {};
  bool found_ = false;
  double share_ = 0;
  int ones_ = 0;
};

// From audio: 2-FSK at `baud` and `shiftHz`, centred within 150 Hz of
// `centreHz`.
class ConstantWeightDetector {
 public:
  ConstantWeightDetector(double rate, double centreHz, double baud, double shiftHz);
  ~ConstantWeightDetector();
  void process(const float* audio, std::size_t n);
  // The finding in words, once ("" otherwise).
  std::string take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hfc
