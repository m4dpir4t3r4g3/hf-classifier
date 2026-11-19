#include "navtex/sitorb_decoder.hpp"

#include <algorithm>

#include "navtex/ccir476.hpp"

namespace navtex {

SitorBDecoder::SitorBDecoder(const SitorBConfig& config) : cfg_(config), rx_(config) {
  fec_.configure(cfg_.polarity, cfg_.softFec);
  fec_.setOutput(this);
  reset();
}

bool SitorBDecoder::configure(const SitorBConfig& config) {
  const bool resets = rx_.resetsOn(config);
  cfg_ = config;
  if (resets && tx_.active()) endTransmission(false);
  rx_.configure(cfg_);
  fec_.configure(cfg_.polarity, cfg_.softFec);
  if (resets) reset();
  return resets;
}

void SitorBDecoder::reset() {
  if (tx_.active()) endTransmission(false);
  rx_.reset();
  fec_.reset();
  figures_ = false;
  tx_.resetErrors();
  idleNow_ = idleAfterText_ = false;
}

void SitorBDecoder::onBit(float soft, double time) {
  fec_.pushBit(soft, time);
  const bool synced = fec_.synced();
  const bool idle = fec_.idle();
  if (idle && !idleNow_) idleStart_ = time;
  idleNow_ = idle;
  if (synced) lastSynced_ = time;
  if (!tx_.active()) return;
  if (idle) {
    idleAfterText_ = true;
    // Idle signals for a while after the text: the transmission is over,
    // even if the station keeps sending idle signals.
    if (time - std::max(idleStart_, tx_.lastCharTime()) >= cfg_.idleEndSeconds) endTransmission(true);
  } else if (!synced) {
    // Sync lost after idle signals: the end of the emission. Lost in the
    // middle of the text: wait a little for the signal to come back.
    if (idleAfterText_) endTransmission(true);
    else if (time - lastSynced_ >= cfg_.lostEndSeconds) endTransmission(false);
  }
}

void SitorBDecoder::flush() {
  if (tx_.active()) endTransmission(idleAfterText_);
}

void SitorBDecoder::onCode(const FecCode& code) {
  if (code.error) {
    // If the best guess is a shift, take it: a lost shift would garble every
    // character up to the next one.
    if (code.code == ccir::kLetters) figures_ = false;
    if (code.code == ccir::kFigures) figures_ = true;
    emit('*', code);
    return;
  }
  switch (code.code) {
    case ccir::kLetters:
      figures_ = false;
      return;
    case ccir::kFigures:
      figures_ = true;
      return;
    default:
      break;
  }
  if (ccir::isIdle(code.code)) return;
  char c = ccir::toChar(code.code, figures_);
  // Line ends arrive as CR LF: print one new line, at the LF.
  if (c == 0 || c == '\r') return;
  emit(c, code);
}

void SitorBDecoder::emit(char ch, const FecCode& code) {
  DecodedChar c;
  c.text[0] = ch;
  c.confidence = code.confidence;
  c.corrected = code.corrected;
  c.time = code.time;
  idleAfterText_ = false;
  tx_.character(c, fec_.reversed(), false, sink_);
}

SitorBStatus SitorBDecoder::status() const {
  SitorBStatus s;
  rx_.fillStatus(s, fec_.synced());
  s.reversed = fec_.reversed();
  s.idle = fec_.idle();
  tx_.fillStatus(s);
  return s;
}

}  // namespace navtex
