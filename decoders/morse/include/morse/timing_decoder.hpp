// Turns key-down (mark) intervals into characters.
//
// Speed tracking: the last 16 mark durations are clustered with 2-means in
// the log domain. Two well separated clusters give the dit and dah lengths
// directly, so the decoder follows both speed changes and "weighted" keying
// where dahs are not exactly three dits. When the window holds only one kind
// of element (for example "5" or "0"), the element is classified against the
// current dit/dah boundary and only that estimate is updated.
//
// Until the speed is known the decoder buffers raw marks rather than guessing,
// then decodes the buffer once it locks, so the first characters of a
// transmission come out right even when the speed differs a lot from the last
// one.
//
// Spacing uses the unit u = (dit + dah) / 4, which cancels the constant
// lengthening of marks (and shortening of spaces) that the detection filter
// introduces: a gap of 2u ends a character. The word gap threshold starts at
// 5u and is then learned from the gap distribution (so Farnsworth spacing,
// with its stretched character and word gaps, still splits words correctly).
// A character is emitted as soon as its gap passes 2u, which bounds the
// decode latency to that gap plus the filter delay.
#pragma once

#include <cstddef>

#include "morse/morse_table.hpp"
#include "morse/types.hpp"

namespace morse {

class TimingDecoder {
 public:
  void configure(double minWpm, double maxWpm, bool errorCorrection);
  void reset();
  void setSink(DecoderSink* sink) { sink_ = sink; }
  // Envelope SNR, used for character confidence.
  void setSnrDb(double snrDb) { snrDb_ = snrDb; }
  // When false, marks are decoded with the current speed estimate and only
  // change it if the last 16 marks show it is off by more than 1.5x. The
  // decoder turns adaptation off while the signal is too weak for element
  // decisions to be trusted, so bursts of noise-fragmented marks during a fade
  // cannot drag the speed estimate away, while a badly wrong estimate still
  // recovers.
  void setAdaptation(bool enabled) { adapt_ = enabled; }

  // A mark from `start` to `end` seconds of stream time.
  void onMark(double start, double end);
  // Called regularly while the key is up.
  void onTick(double now);

  bool locked() const { return locked_; }
  bool hasEstimate() const { return haveEstimate_; }
  double dit() const { return dot_; }
  double dah() const { return dash_; }
  double unit() const { return (dot_ + dash_) / 4.0; }
  double wpm() const { return 1.2 / unit(); }
  double quality() const { return quality_; }
  unsigned long long charsDecoded() const { return charsDecoded_; }
  // Minimum mark length accepted as a real element.
  double minMarkSeconds() const;

 private:
  struct Element {
    double duration;
    double gapBefore;
  };
  struct RawMark {
    double start, end;
  };
  static constexpr int kMaxCharElements = 12;
  static constexpr int kHistory = 16;
  static constexpr int kPending = 96;

  void updateEstimate(double duration, double gapBefore);
  void lockAndReplay(double now);
  void addElement(double start, double end, double now);
  void learnWordGap(double gapUnits);
  void processGap(double now);
  void emitCharacter(double now);
  void emitSpace(double now);
  void emit(const char* text, Code code, float confidence, bool corrected, double now);
  bool isDah(double d) const;
  double elementConfidence(double d) const;
  void clampEstimate(bool keepDot);

  DecoderSink* sink_ = nullptr;
  double minWpm_ = 5, maxWpm_ = 50;
  bool errorCorrection_ = true;
  double snrDb_ = 20;
  bool adapt_ = true;

  // Speed estimate.
  double dot_ = 0.06, dash_ = 0.18;
  bool haveEstimate_ = false, locked_ = false;
  double marks_[kHistory] = {};
  double gaps_[kHistory] = {};
  int markCount_ = 0, markPos_ = 0, gapCount_ = 0, gapPos_ = 0;
  int lastLo_ = 0, lastHi_ = 0;  // cluster sizes of the last 2-means run

  // Gaps between characters, in units, for the word-gap threshold.
  double interGaps_[kHistory] = {};
  int interCount_ = 0, interPos_ = 0;
  double wordGapUnits_ = 5.0;

  // Buffered marks while the speed is not yet known.
  RawMark pending_[kPending] = {};
  int pendingN_ = 0;
  bool replaying_ = false;

  // Character being assembled.
  Element cur_[kMaxCharElements] = {};
  int curN_ = 0;
  bool charOpen_ = false, anyOutput_ = false, wordSpaceDone_ = true;
  double lastMarkEnd_ = -1.0;

  double quality_ = 0.0;
  unsigned long long charsDecoded_ = 0;
};

}  // namespace morse
