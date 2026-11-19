// The complete audio-to-text Morse decoder.
//
//   audio -> ToneTracker (AFC) --------------------------.
//     \                                                   v
//      `-> complex mixer at the tone -> 4th-order low-pass (= band-pass
//          around the tone) -> decimate to ~2 kHz -> magnitude envelope
//          -> smoothing -> noise floor / signal level tracking (AGC)
//          -> adaptive threshold with hysteresis -> debounced key state
//          -> TimingDecoder -> characters
//
// process() is real-time safe: it never allocates, locks or blocks, so it can
// run directly in an audio callback. Output goes to a DecoderSink.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "morse/dsp.hpp"
#include "morse/level_estimator.hpp"
#include "morse/timing_decoder.hpp"
#include "morse/tone_tracker.hpp"
#include "morse/types.hpp"

namespace morse {

class MorseDecoder {
 public:
  explicit MorseDecoder(const DecoderConfig& config = DecoderConfig());

  // Applies a new configuration. Resets the decoder state if the sample rate,
  // channel count or speed range changed; other settings apply live.
  void configure(const DecoderConfig& config);
  const DecoderConfig& config() const { return cfg_; }
  void reset();

  void setSink(DecoderSink* sink);

  // `frames` frames of `config().channels` interleaved samples.
  void process(const float* samples, std::size_t frames);

  // Flushes a character still being assembled, as if a long gap followed.
  void flush();

  DecoderStatus status() const;
  double streamTime() const { return static_cast<double>(envCount_) / envRate_; }

 private:
  void updateFilters(bool force);
  void demodulate(double x);
  void drainPreBuffer();
  void emitSpectrum();
  void envelopeStep(double magnitude);
  void decide(double envelope, double time, bool levelsOk);
  void setKey(bool down, double edgeTime);

  DecoderConfig cfg_;
  DecoderSink* sink_ = nullptr;

  ToneTracker tracker_;
  TimingDecoder timing_;

  // Audio held back while the tone tracker acquires.
  std::vector<float> pre_;
  std::size_t preHead_ = 0, preCount_ = 0;

  // Mixer and low-pass.
  double toneHz_ = 700.0, bandwidthHz_ = 200.0;
  double phaseRe_ = 1.0, phaseIm_ = 0.0, stepRe_ = 1.0, stepIm_ = 0.0;
  std::uint32_t renorm_ = 0;
  Butterworth4 lpI_, lpQ_;
  std::size_t decim_ = 24, decimCount_ = 0;
  double accI_ = 0.0, accQ_ = 0.0;
  double envRate_ = 2000.0;
  std::uint64_t envCount_ = 0;

  // Envelope processing.
  MovingAverage smoother_;
  LevelEstimator levels_;
  std::vector<float> backlog_;  // envelope kept while levels are unknown
  std::size_t backlogHead_ = 0, backlogCount_ = 0;
  double noise_ = 0.0, signal_ = 0.0;
  bool envInit_ = false;
  bool rawKey_ = false, key_ = false;
  double candidateTime_ = -1.0, lastEdge_ = 0.0;
  double snrDb_ = 0.0, thresholdLevel_ = 0.0;
  bool signalPresent_ = false;
  double presentUntil_ = -1.0;

  // Activity trace.
  std::size_t activityDecim_ = 10, activityCount_ = 0;
  float activityPeak_ = 0.0f;
  int spectrumCount_ = 0;
};

}  // namespace morse
