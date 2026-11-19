#include "navtex/rtty_decoder.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>

#include "navtex/ita2.hpp"

namespace navtex {

namespace {
constexpr float kClearLevel = 0.25f;  // |soft value| of a bit that is clearly one tone
constexpr int kSyncRun = 6;           // clear characters in a row that make sync
constexpr int kLoseSyncErrors = 4;    // framing errors among the last 8 that end it
constexpr double kCompeteBits = 0.75; // edges this close compete for one character
constexpr double kOverlapBits = 0.5;  // timing slack between one character and the next
}  // namespace

FskConfig RttyDecoder::rttyDefaults() {
  FskConfig c;
  c.mode = FskMode::Rtty;
  c.baud = 45.45;
  c.shiftHz = 170.0;
  return c;
}

RttyDecoder::RttyDecoder(const FskConfig& config) : cfg_(config), rx_(config) {
  framers_[0].setPolarity(1);
  framers_[1].setPolarity(-1);
  reset();
}

bool RttyDecoder::configure(const FskConfig& config) {
  const bool resets = rx_.resetsOn(config) || config.polarity != cfg_.polarity;
  cfg_ = config;
  if (resets) tx_.end(false, rx_.time(), sink_);
  rx_.configure(cfg_);
  if (resets) reset();
  return resets;
}

void RttyDecoder::reset() {
  tx_.end(false, rx_.time(), sink_);
  rx_.reset();
  const double spb = rx_.demodRate() / cfg_.baud;
  for (auto& f : framers_) f.reset(spb);
  active_ = -1;
  figures_ = false;
  presentSince_ = -1.0;
  tx_.resetErrors();
}

void RttyDecoder::flush() { tx_.end(true, rx_.time(), sink_); }

// ------------------------------------------------------------- framing

void RttyDecoder::Framer::reset(double samplesPerBit) {
  spb_ = samplesPerBit;
  n_ = 0;
  prev_ = 0.0;
  candCount_ = 0;
  consumedUntil_ = 0.0;
  lastStart_ = -1e18;
  lastClear_ = false;
  period_ = 0.0;
  predicted_ = false;
  haveBest_ = false;
  synced = false;
  clearRun = 0;
  recent = 0;
  heldCount = 0;
}

void RttyDecoder::Framer::complete(const Candidate& c, double time) {
  Frame f;
  float weakest = 1e9f;
  for (int k = 0; k < 7; ++k) weakest = std::min(weakest, std::fabs(c.soft[k]));
  for (int k = 0; k < 5; ++k)
    if (c.soft[k + 1] > 0.0f) f.code = static_cast<std::uint8_t>(f.code | (1u << k));
  f.framed = c.soft[0] < 0.0f && c.soft[6] > 0.0f;
  f.clear = f.framed && weakest >= kClearLevel;
  f.weakest = weakest;
  f.time = time;
  const double score = f.framed ? 1.0 + weakest : weakest - 1.0;
  if (!haveBest_) {
    haveBest_ = true;
    best_ = f;
    bestStart_ = c.start;
    bestScore_ = score;
  } else if (c.start < bestStart_ + kCompeteBits * spb_ && score > bestScore_) {
    best_ = f;
    bestStart_ = c.start;
    bestScore_ = score;
  }
}

bool RttyDecoder::Framer::sample(double disc, double time, Frame& out) {
  const double v = pol_ * disc;
  const double at = static_cast<double>(n_);
  if (prev_ > 0.0 && v <= 0.0) {
    // Mark to space: the matched filter crosses zero half a bit after the
    // start bit begins.
    double crossing = at - 1.0 + prev_ / (prev_ - v);
    double start = crossing - 0.5 * spb_;
    if (start >= consumedUntil_ && candCount_ < kCandidates) {
      cand_[candCount_] = Candidate();
      cand_[candCount_++].start = start;
    }
  }
  // In sync, also try where the next back-to-back character is due: noise
  // on the stop bit can hide the start bit's edge.
  if (synced && lastClear_ && period_ > 0.0 && !predicted_ && at >= lastStart_ + period_) {
    predicted_ = true;
    const double start = lastStart_ + period_;
    bool near = false;
    for (int i = 0; i < candCount_; ++i) near = near || std::fabs(cand_[i].start - start) < 0.5 * spb_;
    if (!near && candCount_ < kCandidates) {
      cand_[candCount_] = Candidate();
      cand_[candCount_++].start = start;
    }
  }
  // Sample each element at its end, interpolating between samples.
  int kept = 0;
  for (int i = 0; i < candCount_; ++i) {
    Candidate c = cand_[i];
    bool alive = true;
    while (c.next < 7) {
      double target = c.start + (c.next + 1) * spb_;
      if (target > at) break;
      double frac = std::clamp(target - (at - 1.0), 0.0, 1.0);
      c.soft[c.next] = static_cast<float>(prev_ + frac * (v - prev_));
      if (c.next == 0 && c.soft[0] >= 0.0f) {
        // No start bit. In sync, where the next character is due, a weak
        // one is still read (as a framing error) rather than lose the frame.
        const bool due = synced && c.start > lastStart_ + (7.0 - kOverlapBits) * spb_ &&
                         c.start < lastStart_ + (8.0 + kOverlapBits) * spb_;
        if (!due || c.soft[0] >= kClearLevel) {
          alive = false;
          break;
        }
      }
      if (++c.next == 7) {
        complete(c, time);
        alive = false;
      }
    }
    if (alive) cand_[kept++] = c;
  }
  candCount_ = kept;
  // Once no later edge can compete, the best candidate is the character,
  // and edges inside it are not starts.
  bool framed = false;
  if (haveBest_ && at >= bestStart_ + (7.0 + kCompeteBits) * spb_) {
    out = best_;
    framed = true;
    haveBest_ = false;
    const double spacing = (bestStart_ - lastStart_) / spb_;
    if (best_.clear && lastClear_ && spacing > 6.8 && spacing < 8.3)
      period_ = period_ > 0.0 ? period_ + 0.2 * (spacing * spb_ - period_) : spacing * spb_;
    lastStart_ = bestStart_;
    lastClear_ = best_.clear;
    predicted_ = false;
    consumedUntil_ = bestStart_ + (7.0 - kOverlapBits) * spb_;
    kept = 0;
    for (int i = 0; i < candCount_; ++i)
      if (cand_[i].start >= consumedUntil_) cand_[kept++] = cand_[i];
    candCount_ = kept;
  }
  prev_ = v;
  ++n_;
  return framed;
}

void RttyDecoder::onSample(double disc, double time) {
  if (!rx_.signalPresent()) presentSince_ = -1.0;
  else if (presentSince_ < 0.0) presentSince_ = time;
  for (int i = 0; i < 2; ++i) {
    if ((cfg_.polarity == Polarity::Normal && i == 1) || (cfg_.polarity == Polarity::Reversed && i == 0)) continue;
    Frame f;
    if (framers_[i].sample(disc, time, f)) onFrame(i, f);
  }
  // A pause: whatever comes next must prove itself again.
  if (active_ >= 0 && time - framers_[active_].lastFrame > 2.0 * (6.0 + 1.5) / cfg_.baud) loseSync(active_);
  // Nothing for a while (the station stopped, or sends steady mark): the
  // transmission is over.
  if (tx_.active() && time - tx_.lastCharTime() > cfg_.idleEndSeconds) {
    tx_.end(true, time, sink_);
    if (active_ >= 0) loseSync(active_);
  }
}

void RttyDecoder::onFrame(int which, const Frame& f) {
  Framer& fr = framers_[which];
  fr.recent = static_cast<std::uint8_t>((fr.recent << 1) | (f.framed ? 0u : 1u));
  // Letters shifts sent with one stop bit ("0 11111 1") read with the tones
  // the other way round frame cleanly as nulls ("0 00000 1"): nulls prove
  // nothing, so they neither make nor break a run.
  const bool isNull = f.code == ita2::kNull;
  if (!f.clear) fr.clearRun = 0;
  else if (!isNull) ++fr.clearRun;
  fr.lastFrame = f.time;

  if (fr.synced) {
    if (static_cast<int>(std::bitset<8>(fr.recent).count()) >= kLoseSyncErrors) {
      loseSync(which);
      return;
    }
    if (!f.clear) {
      // Held until a clear character shows the signal is still there.
      if (fr.heldCount < Framer::kHeld) fr.held[fr.heldCount++] = f;
      return;
    }
    for (int i = 0; i < fr.heldCount; ++i) output(fr.held[i], fr.pol_ < 0);
    fr.heldCount = 0;
    output(f, fr.pol_ < 0);
    return;
  }
  // Not synced: hold the character. Only the current run of clear
  // characters matters, so an unclear one clears what was held; so does one
  // not framed entirely after AFC found the tone pair (while it is moving the
  // demodulator, regular keying can frame cleanly into wrong characters).
  const double charSeconds = (6.0 + 1.5) / cfg_.baud;
  if (!f.clear || presentSince_ < 0.0 || f.time - charSeconds < presentSince_) {
    fr.clearRun = 0;
    fr.heldCount = 0;
    return;
  }
  if (isNull) return;
  if (fr.heldCount == Framer::kHeld) {
    std::copy(fr.held + 1, fr.held + Framer::kHeld, fr.held);
    --fr.heldCount;
  }
  fr.held[fr.heldCount++] = f;
  // Sync needs the tone pair too (a lock, or the signal at a fixed centre),
  // and the other polarity not to have it.
  if (fr.clearRun >= kSyncRun && active_ < 0 && rx_.signalPresent()) {
    fr.synced = true;
    fr.recent = 0;
    active_ = which;
    for (int i = 0; i < fr.heldCount; ++i) output(fr.held[i], fr.pol_ < 0);
    fr.heldCount = 0;
  }
}

void RttyDecoder::loseSync(int which) {
  framers_[which].synced = false;
  framers_[which].clearRun = 0;
  framers_[which].heldCount = 0;
  if (active_ == which) active_ = -1;
}

void RttyDecoder::output(const Frame& f, bool reversed) {
  const std::uint8_t code = f.code;
  if (code == ita2::kLetters) {
    figures_ = false;
    return;
  }
  if (code == ita2::kFigures) {
    figures_ = true;
    return;
  }
  if (code == ita2::kSpace && cfg_.unshiftOnSpace) figures_ = false;
  char ch = ita2::toChar(code, figures_);
  // Line ends arrive as CR LF: print one new line, at the LF.
  if (ch == 0 || ch == '\r') return;
  DecodedChar c;
  c.text[0] = ch;
  c.time = f.time;
  c.confidence = !f.framed ? 0.2f : f.clear ? 1.0f : 0.6f;
  tx_.character(c, reversed, !f.framed, sink_);
}

SitorBStatus RttyDecoder::status() const {
  SitorBStatus s;
  rx_.fillStatus(s, active_ >= 0);
  s.reversed = active_ == 1;
  s.idle = active_ >= 0 && rx_.time() - tx_.lastCharTime() > 1.0;
  tx_.fillStatus(s);
  return s;
}

}  // namespace navtex
