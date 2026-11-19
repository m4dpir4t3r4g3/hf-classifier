// Estimates the key-up (noise) and key-down (signal) envelope levels without
// needing any detection first.
//
// The last few seconds of the detection envelope are kept (at 500 Hz) and
// split into two classes with Otsu's method, the threshold that maximises the
// between-class variance of the histogram. Morse is keyed roughly half the
// time, so both classes are well populated; their means are the space and
// mark levels. Levels are only reported when the classes are clearly
// separated, so a window of pure noise does not invent a "signal".
#pragma once

#include <cstddef>
#include <vector>

namespace morse {

class LevelEstimator {
 public:
  // `inputRate`: rate push() is called at. `maxWindowSeconds`: longest window.
  void configure(double inputRate, double maxWindowSeconds);
  void reset();
  void setWindowSeconds(double seconds);
  void push(double value);

  bool valid() const { return valid_; }
  double low() const { return low_; }
  double high() const { return high_; }
  // Separation of the classes in pooled standard deviations (like d').
  double separation() const { return separation_; }

 private:
  void update();

  static constexpr int kBins = 128;
  std::vector<float> ring_;
  std::vector<int> hist_;
  std::size_t pos_ = 0, count_ = 0, window_ = 1;
  int decim_ = 4, decimCount_ = 0, sinceUpdate_ = 0;
  double low_ = 0, high_ = 0, separation_ = 0;
  bool valid_ = false;
};

}  // namespace morse
