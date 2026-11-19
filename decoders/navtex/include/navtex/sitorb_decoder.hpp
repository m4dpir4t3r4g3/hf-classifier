// The complete audio-to-text SITOR-B decoder (ITU-R M.476 / M.625 mode B:
// collective FEC), for any SITOR-B signal.
//
//   audio -> CentreTracker (AFC: finds the tone pair) ----------.
//     \                                                          v
//      `-> FskDemodulator (mixer, tone matched filters, ATC, bit clock)
//          -> FecDecoder (character sync, polarity, DX/RX FEC)
//          -> CCIR 476 letters/figures -> DecoderSink
//
// Besides the characters it reports transmissions: a transmission starts
// with its first decoded character, and ends with idle signals (an orderly
// end) or when the signal is lost. It knows nothing about what the text
// means; NavtexDecoder adds NAVTEX messages on top of it.
//
// process() is real-time safe: it never allocates, locks or blocks, so it can
// run directly in an audio callback. Output goes to a DecoderSink.
#pragma once

#include <cstddef>
#include <cstdint>

#include "navtex/fec_decoder.hpp"
#include "navtex/fsk_receiver.hpp"
#include "navtex/types.hpp"

namespace navtex {

class SitorBDecoder : private FecDecoder::Output, private FskReceiver::Client {
 public:
  using Config = SitorBConfig;

  explicit SitorBDecoder(const SitorBConfig& config = SitorBConfig());

  // Applies a new configuration. Resets the decoder, and returns true, if the
  // sample rate, channel count, shift, baud rate or frequency settings
  // changed; polarity, FEC and timing settings apply live.
  bool configure(const SitorBConfig& config);
  // Whether configure(config) would reset the decoder.
  bool resetsOn(const SitorBConfig& config) const { return rx_.resetsOn(config); }
  const SitorBConfig& config() const { return cfg_; }
  // Starts acquisition afresh. An open transmission ends, as not complete.
  void reset();

  void setSink(DecoderSink* sink) {
    sink_ = sink;
    rx_.setSink(sink);
  }

  // `frames` frames of `config().channels` interleaved samples.
  void process(const float* samples, std::size_t frames) { rx_.process(samples, frames, *this); }

  // End of the stream: ends an open transmission.
  void flush();

  SitorBStatus status() const;
  double streamTime() const { return rx_.time(); }
  double lastCharTime() const { return tx_.lastCharTime(); }
  double measuredShiftHz() const { return rx_.measuredShiftHz(); }
  std::uint64_t resyncs() const { return fec_.resyncs(); }

 private:
  // FecDecoder::Output
  void onCode(const FecCode& code) override;
  // FskReceiver::Client
  void onBit(float soft, double time) override;
  bool synced() const override { return fec_.synced(); }

  void emit(char ch, const FecCode& code);
  void endTransmission(bool complete) { tx_.end(complete, rx_.time(), sink_); idleAfterText_ = false; }

  SitorBConfig cfg_;
  DecoderSink* sink_ = nullptr;
  FskReceiver rx_;
  FecDecoder fec_;
  TransmissionTracker tx_;
  bool figures_ = false;

  // Transmission end detection.
  bool idleNow_ = false;          // receiving idle signals, since idleStart_
  double idleStart_ = 0.0;
  bool idleAfterText_ = false;    // idle signals came after the transmission's last character
  double lastSynced_ = 0.0;       // stream time sync was last held
};

}  // namespace navtex
