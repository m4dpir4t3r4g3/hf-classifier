#include "navtex/fsk_decoder.hpp"

namespace navtex {

FskDecoder::FskDecoder(const FskConfig& config) : cfg_(config), sitorB_(config), rtty_(config), bits_(config) {}

void FskDecoder::configure(const FskConfig& config) {
  if (config.mode != cfg_.mode) {
    // The old mode's decoder stops: close what it had open.
    switch (cfg_.mode) {
      case FskMode::SitorB: sitorB_.reset(); break;
      case FskMode::Rtty: rtty_.reset(); break;
      case FskMode::Bits: bits_.reset(); break;
    }
  }
  const bool modeChanged = config.mode != cfg_.mode;
  cfg_ = config;
  switch (cfg_.mode) {
    case FskMode::SitorB: sitorB_.configure(cfg_); if (modeChanged) sitorB_.reset(); break;
    case FskMode::Rtty: rtty_.configure(cfg_); if (modeChanged) rtty_.reset(); break;
    case FskMode::Bits: bits_.configure(cfg_); if (modeChanged) bits_.reset(); break;
  }
}

void FskDecoder::reset() {
  switch (cfg_.mode) {
    case FskMode::SitorB: sitorB_.reset(); break;
    case FskMode::Rtty: rtty_.reset(); break;
    case FskMode::Bits: bits_.reset(); break;
  }
}

void FskDecoder::setSink(DecoderSink* sink) {
  sitorB_.setSink(sink);
  rtty_.setSink(sink);
  bits_.setSink(sink);
}

void FskDecoder::process(const float* samples, std::size_t frames) {
  switch (cfg_.mode) {
    case FskMode::SitorB: sitorB_.process(samples, frames); break;
    case FskMode::Rtty: rtty_.process(samples, frames); break;
    case FskMode::Bits: bits_.process(samples, frames); break;
  }
}

void FskDecoder::flush() {
  switch (cfg_.mode) {
    case FskMode::SitorB: sitorB_.flush(); break;
    case FskMode::Rtty: rtty_.flush(); break;
    case FskMode::Bits: bits_.flush(); break;
  }
}

FskStatus FskDecoder::status() const {
  FskStatus s;
  switch (cfg_.mode) {
    case FskMode::SitorB:
      static_cast<SitorBStatus&>(s) = sitorB_.status();
      s.measuredShiftHz = sitorB_.measuredShiftHz();
      break;
    case FskMode::Rtty:
      static_cast<SitorBStatus&>(s) = rtty_.status();
      s.measuredShiftHz = rtty_.measuredShiftHz();
      break;
    case FskMode::Bits:
      static_cast<SitorBStatus&>(s) = bits_.status();
      s.measuredShiftHz = bits_.measuredShiftHz();
      break;
  }
  s.mode = cfg_.mode;
  return s;
}

double FskDecoder::streamTime() const {
  switch (cfg_.mode) {
    case FskMode::SitorB: return sitorB_.streamTime();
    case FskMode::Rtty: return rtty_.streamTime();
    default: return bits_.streamTime();
  }
}

}  // namespace navtex
