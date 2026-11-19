#include "navtex/bits_decoder.hpp"

#include <cmath>

namespace navtex {

namespace {
constexpr std::uint32_t kGroup = 8, kLine = 64;
constexpr double kConfirmSeconds = 0.5;  // the tone pair must be there this long
}  // namespace

BitsDecoder::BitsDecoder(const FskConfig& config) : cfg_(config), rx_(config) { reset(); }

bool BitsDecoder::configure(const FskConfig& config) {
  const bool resets = rx_.resetsOn(config) || config.polarity != cfg_.polarity;
  cfg_ = config;
  if (resets) tx_.end(false, rx_.time(), sink_);
  rx_.configure(cfg_);
  if (resets) reset();
  return resets;
}

void BitsDecoder::reset() {
  tx_.end(false, rx_.time(), sink_);
  rx_.reset();
  tx_.resetErrors();
  bitsInLine_ = 0;
  presentSince_ = -1.0;
  heldCount_ = 0;
}

void BitsDecoder::flush() { tx_.end(true, rx_.time(), sink_); }

void BitsDecoder::onBit(float soft, double time) {
  if (!rx_.signalPresent()) {
    presentSince_ = -1.0;
    heldCount_ = 0;
    // The tone pair has gone: after lostEndSeconds, so has the transmission.
    if (tx_.active() && time - lastPresent_ >= cfg_.lostEndSeconds) {
      tx_.end(true, time, sink_);
      bitsInLine_ = 0;
    }
    return;
  }
  lastPresent_ = time;
  if (presentSince_ < 0.0) presentSince_ = time;
  if (!tx_.active() && time - presentSince_ < kConfirmSeconds) {
    // Not yet sure it is a signal: hold the bits.
    if (heldCount_ < kHeld) held_[heldCount_++] = {soft, time};
    return;
  }
  for (int i = 0; i < heldCount_; ++i) output(held_[i].soft, held_[i].time);
  heldCount_ = 0;
  output(soft, time);
}

void BitsDecoder::output(float soft, double time) {
  const float v = cfg_.polarity == Polarity::Reversed ? -soft : soft;
  const float a = std::fabs(v);
  // Clearly one tone: certain; less so: doubtful.
  const float confidence = a >= 0.5f ? 1.0f : a >= 0.2f ? 0.6f : 0.3f;
  if (bitsInLine_ > 0 && bitsInLine_ % kGroup == 0) put(bitsInLine_ % kLine == 0 ? '\n' : ' ', 1.0f, time);
  if (bitsInLine_ == kLine) bitsInLine_ = 0;
  put(v > 0.0f ? '1' : '0', confidence, time);
  ++bitsInLine_;
}

void BitsDecoder::put(char ch, float confidence, double time) {
  DecodedChar c;
  c.text[0] = ch;
  c.confidence = confidence;
  c.time = time;
  tx_.character(c, cfg_.polarity == Polarity::Reversed, false, sink_);
}

SitorBStatus BitsDecoder::status() const {
  SitorBStatus s;
  rx_.fillStatus(s, rx_.signalPresent());
  s.reversed = cfg_.polarity == Polarity::Reversed;
  tx_.fillStatus(s);
  return s;
}

}  // namespace navtex
