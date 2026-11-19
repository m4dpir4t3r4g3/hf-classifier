// The part of every FSK decoder that does not depend on the mode: the
// front end that turns audio into the demodulator's soft decisions, and the
// bookkeeping of decoded characters and transmissions.
//
//   audio -> CentreTracker (AFC: finds the tone pair) ----------.
//     \                                                          v
//      `-> FskDemodulator (mixer, tone matched filters, ATC, bit clock)
//          -> Client: bits on the recovered clock (SITOR-B, raw bits), or
//             the soft decision at the demodulator rate (RTTY framing)
//
// The mode decoders (SitorBDecoder, RttyDecoder, BitsDecoder) own one each.
#pragma once

#include <cstddef>
#include <cstdint>

#include "navtex/centre_tracker.hpp"
#include "navtex/fsk_demodulator.hpp"
#include "navtex/types.hpp"

namespace navtex {

class FskReceiver {
 public:
  // What the mode decoder does with the demodulator's output.
  class Client {
   public:
    virtual ~Client() = default;
    // A bit on the recovered clock: soft value (> 0: higher tone) and stream time.
    virtual void onBit(float, double) {}
    // A new soft decision at the demodulator rate (demodRate() per second).
    virtual void onSample(double, double) {}
    // Whether the decoder has found the signal's structure. AFC gives up on a
    // tone pair it has followed for rejectAfterSeconds without it.
    virtual bool synced() const = 0;
  };

  explicit FskReceiver(const SitorBConfig& config);

  // Whether configure(config) would restart acquisition.
  bool resetsOn(const SitorBConfig& config) const;
  // Applies the settings; returns true (and resets) if acquisition restarts.
  bool configure(const SitorBConfig& config);
  const SitorBConfig& config() const { return cfg_; }
  void reset();
  void setSink(DecoderSink* sink) { sink_ = sink; }

  // `frames` frames of `config().channels` interleaved samples.
  void process(const float* samples, std::size_t frames, Client& client);

  double time() const { return time_; }
  double demodRate() const { return demod_.demodRate(); }
  bool signalPresent() const { return tracker_.signalPresent(); }
  // Tone and signal fields of a status.
  void fillStatus(SitorBStatus& s, bool synced) const;
  // Distance between the two tones as measured in the spectrum (0: unknown).
  double measuredShiftHz() const { return tracker_.measuredShiftHz(); }

 private:
  SitorBConfig cfg_;
  DecoderSink* sink_ = nullptr;
  CentreTracker tracker_;
  FskDemodulator demod_;
  double time_ = 0.0, dt_ = 1.0 / 48000.0;
  double lastSyncOrUnlock_ = 0.0;
  int spectrumCount_ = 0;
};

// Counts decoded characters and unrecoverable ones, and reports
// transmissions: one starts with its first character.
class TransmissionTracker {
 public:
  // Counts `c`, starting a transmission if none is open (`reversed`: the
  // polarity to report), and passes it to `sink`. `error`: unrecoverable
  // (besides '*', which always is).
  void character(const DecodedChar& c, bool reversed, bool error, DecoderSink* sink);
  void end(bool complete, double time, DecoderSink* sink);
  // Forgets the recent error rate (not the counts).
  void resetErrors() { errorPos_ = errorFill_ = errorCount_ = 0; }

  bool active() const { return active_; }
  std::uint32_t count() const { return count_; }
  std::uint64_t chars() const { return chars_; }
  double lastCharTime() const { return lastCharTime_; }
  double errorRate() const { return errorFill_ ? static_cast<double>(errorCount_) / errorFill_ : 0.0; }
  void fillStatus(SitorBStatus& s) const;

 private:
  bool active_ = false;
  TransmissionEvent current_;
  std::uint32_t count_ = 0;
  std::uint64_t chars_ = 0;
  double lastCharTime_ = 0.0;
  // Recent characters: 1 = unrecoverable.
  bool errorRing_[200] = {};
  int errorPos_ = 0, errorFill_ = 0, errorCount_ = 0;
};

}  // namespace navtex
