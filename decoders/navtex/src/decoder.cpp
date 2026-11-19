#include "navtex/decoder.hpp"

#include <cstring>

namespace navtex {

NavtexDecoder::NavtexDecoder(const DecoderConfig& config) : cfg_(config), sitorB_(config) {
  messages_.configure(cfg_.repeatMemoryHours * 3600.0);
  sitorB_.setSink(&layer_);
}

void NavtexDecoder::configure(const DecoderConfig& config) {
  cfg_ = config;
  messages_.configure(cfg_.repeatMemoryHours * 3600.0);
  // The SITOR-B decoder resets itself if needed; the open message goes with it.
  if (sitorB_.resetsOn(cfg_)) {
    if (messages_.inMessage()) messages_.close(sitorB_.streamTime(), sink_);
    messages_.reset();
  }
  sitorB_.configure(cfg_);
}

void NavtexDecoder::reset() {
  if (messages_.inMessage()) messages_.close(sitorB_.streamTime(), sink_);
  sitorB_.reset();
  messages_.reset();
}

void NavtexDecoder::process(const float* samples, std::size_t frames) {
  sitorB_.process(samples, frames);
  if (messages_.inMessage() && sitorB_.streamTime() - sitorB_.lastCharTime() > cfg_.messageTimeoutSeconds)
    messages_.close(sitorB_.streamTime(), sink_);
}

void NavtexDecoder::flush() {
  messages_.close(sitorB_.streamTime(), sink_);
  sitorB_.flush();
}

DecoderStatus NavtexDecoder::status() const {
  DecoderStatus s;
  static_cast<SitorBStatus&>(s) = sitorB_.status();
  s.inMessage = messages_.inMessage();
  if (s.inMessage) std::memcpy(s.messageId, messages_.id(), 5);
  s.messages = messages_.started();
  return s;
}

}  // namespace navtex
