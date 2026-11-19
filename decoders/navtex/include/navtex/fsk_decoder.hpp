// The multi-mode FSK decoder of the FSK plugin: one of SITOR-B (AMTOR mode
// B), RTTY or raw bits, chosen by FskConfig::mode, with the baud rate and
// shift of the config. Each mode has its own decoder built on the same
// front end (FskReceiver); only the chosen one runs.
#pragma once

#include <cstddef>

#include "navtex/bits_decoder.hpp"
#include "navtex/rtty_decoder.hpp"
#include "navtex/sitorb_decoder.hpp"
#include "navtex/types.hpp"

namespace navtex {

class FskDecoder {
 public:
  using Config = FskConfig;

  explicit FskDecoder(const FskConfig& config = FskConfig());
  FskDecoder(const FskDecoder&) = delete;
  FskDecoder& operator=(const FskDecoder&) = delete;

  // A new mode ends the old mode's open transmission and starts afresh.
  void configure(const FskConfig& config);
  const FskConfig& config() const { return cfg_; }
  void reset();
  void setSink(DecoderSink* sink);
  void process(const float* samples, std::size_t frames);
  void flush();

  FskStatus status() const;
  double streamTime() const;

 private:
  FskConfig cfg_;
  SitorBDecoder sitorB_;
  RttyDecoder rtty_;
  BitsDecoder bits_;
};

}  // namespace navtex
