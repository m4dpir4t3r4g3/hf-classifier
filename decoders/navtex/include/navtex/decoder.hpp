// The complete audio-to-text NAVTEX decoder: the SITOR-B decoder, with the
// NAVTEX message layer on top of its text.
//
//   audio -> SitorBDecoder (AFC, FSK demodulator, character sync and FEC,
//            CCIR 476 letters/figures)
//         -> MessageAssembler (ZCZC ... NNNN, repeats) -> DecoderSink
//
// process() is real-time safe: it never allocates, locks or blocks, so it can
// run directly in an audio callback. Output goes to a DecoderSink.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "navtex/message.hpp"
#include "navtex/sitorb_decoder.hpp"
#include "navtex/types.hpp"

namespace navtex {

class NavtexDecoder {
 public:
  using Config = DecoderConfig;

  explicit NavtexDecoder(const DecoderConfig& config = DecoderConfig());
  NavtexDecoder(const NavtexDecoder&) = delete;
  NavtexDecoder& operator=(const NavtexDecoder&) = delete;

  // Applies a new configuration. Resets the decoder if the sample rate,
  // channel count, shift, baud rate or frequency settings changed; polarity
  // and FEC settings apply live.
  void configure(const DecoderConfig& config);
  const DecoderConfig& config() const { return cfg_; }
  // Starts acquisition afresh (keeps the memory of received messages).
  void reset();

  void setSink(DecoderSink* sink) { sink_ = sink; }

  // `frames` frames of `config().channels` interleaved samples.
  void process(const float* samples, std::size_t frames);

  // End of the stream: closes an open message as incomplete.
  void flush();

  DecoderStatus status() const;
  double streamTime() const { return sitorB_.streamTime(); }
  // Text of the current or last message, from "ZCZC" on.
  const std::string& messageText() const { return messages_.text(); }
  std::uint64_t resyncs() const { return sitorB_.resyncs(); }

 private:
  // Receives the SITOR-B decoder's output and passes the characters through
  // the message assembler.
  class Layer : public DecoderSink {
   public:
    explicit Layer(NavtexDecoder& d) : d_(d) {}
    void onCharacter(const DecodedChar& c) override { d_.messages_.onChar(c, d_.sink_); }
    void onTransmission(const TransmissionEvent& t) override {
      if (d_.sink_) d_.sink_->onTransmission(t);
    }
    void onSpectrum(const SpectrumFrame& f) override {
      if (d_.sink_) d_.sink_->onSpectrum(f);
    }

   private:
    NavtexDecoder& d_;
  };

  DecoderConfig cfg_;
  DecoderSink* sink_ = nullptr;
  MessageAssembler messages_;
  Layer layer_{*this};
  SitorBDecoder sitorB_;
};

}  // namespace navtex
