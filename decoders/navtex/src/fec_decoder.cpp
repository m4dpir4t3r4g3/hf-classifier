#include "navtex/fec_decoder.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>

#include "navtex/ccir476.hpp"

namespace navtex {

namespace {
constexpr int kMinValidToSync = 10;    // of kWindow slots (the DX/RX structure must show too)
constexpr int kMinValidToSwitch = 15;  // ... to leave a working alignment for another
constexpr int kLoseSyncInvalid = 10;   // invalid slots among the last 16 that end sync, unless the
                                       // last 48 slots still showed a repeat (then kLoseSyncAlways)
constexpr int kLoseSyncAlways = 14;
constexpr int kCheckOthersInvalid = 4; // ... that start looking for a better alignment
constexpr float kMinMargin = 1.0f;     // ML score margin below which a character is an error
constexpr std::uint32_t kMask16 = 0xffff;
constexpr std::uint64_t kMask24 = 0xffffff, kMask48 = 0xffffffffffffull;

int ones(std::uint64_t v) { return static_cast<int>(std::bitset<64>(v).count()); }
}  // namespace

FecDecoder::FecDecoder() : hist_(kHistory, 0.0f), times_(kHistory, 0.0) {}

void FecDecoder::configure(Polarity polarity, bool softFec) {
  if (polarity != polarity_) {
    polarity_ = polarity;
    loseSync();
  }
  softFec_ = softFec;
}

void FecDecoder::reset() {
  std::fill(hist_.begin(), hist_.end(), 0.0f);
  std::fill(times_.begin(), times_.end(), 0.0);
  count_ = 0;
  synced_ = false;
  pol_ = polarity_ == Polarity::Reversed ? -1 : 1;
  rxRef_ = 0;
  invalid_ = idleSlots_ = parityFor_ = parityAgainst_ = 0;
  decodedAny_ = false;
  lastRx_ = 0;
  resyncs_ = 0;
  pendingCount_ = 0;
  lastConfirmedIdle_ = true;
}

bool FecDecoder::idle() const { return synced_ && ones(idleSlots_ & 0xff) >= 7; }

unsigned FecDecoder::code(std::uint64_t end, int pol) const {
  unsigned c = 0;
  for (int i = 0; i < 7; ++i)
    if (pol * soft(end - 6 + static_cast<std::uint64_t>(i)) > 0.0f) c |= 1u << i;
  return c;
}

FecDecoder::Candidate FecDecoder::search() const {
  Candidate best;
  const std::uint64_t e = count_ - 1;
  if (count_ < static_cast<std::uint64_t>(7 * kWindow)) return best;
  int bestScore = -1;
  for (int pol : {1, -1}) {
    if ((polarity_ == Polarity::Normal && pol < 0) || (polarity_ == Polarity::Reversed && pol > 0)) continue;
    unsigned c[kWindow];
    int valid = 0;
    for (int k = 0; k < kWindow; ++k) {
      c[k] = code(e - 7 * static_cast<std::uint64_t>(kWindow - 1 - k), pol);
      valid += ccir::isValid(c[k]);
    }
    if (valid < kMinValidToSync) continue;
    // Evidence for "the newest slot is an RX slot" (same) or not (other).
    int same = 0, other = 0;
    for (int k = 0; k < kWindow; ++k) {
      bool newestParity = (kWindow - 1 - k) % 2 == 0;
      // Slot k repeats slot k-5: k is an RX slot. Idle alpha repeats
      // everywhere, so it says nothing.
      if (k >= 5 && ccir::isValid(c[k]) && c[k] == c[k - 5] && c[k] != ccir::kAlpha) (newestParity ? same : other)++;
      // Phasing: rep in DX, followed by alpha in RX.
      if (k >= 1 && c[k] == ccir::kAlpha && c[k - 1] == ccir::kRep) (newestParity ? same : other)++;
    }
    int hi = std::max(same, other), lo = std::min(same, other);
    if (hi < 4 || hi < 2 * lo + 2) continue;
    int score = valid * 100 + hi;
    if (score > bestScore) {
      bestScore = score;
      best.found = true;
      best.pol = pol;
      best.rxEnd = same >= other ? e : e - 7;
      best.valid = valid;
    }
  }
  return best;
}

void FecDecoder::pushBit(float s, double time) {
  hist_[count_ % kHistory] = s;
  times_[count_ % kHistory] = time;
  ++count_;
  const std::uint64_t e = count_ - 1;

  if (!synced_) {
    Candidate c = search();
    if (c.found) acquire(c);
    return;
  }
  if (e % 7 == rxRef_ % 7) onSlot(e);
  if (!synced_) return;
  // The current alignment is struggling: is another one clearly better
  // (after a bit slip, or a stronger station)?
  if (ones(invalid_) >= kCheckOthersInvalid) {
    Candidate c = search();
    if (c.found && c.valid >= kMinValidToSwitch && (c.pol != pol_ || c.rxEnd % 7 != rxRef_ % 7)) acquire(c);
  }
}

void FecDecoder::loseSync() {
  synced_ = false;
  pendingCount_ = 0;
  lastConfirmedIdle_ = true;
}

void FecDecoder::acquire(const Candidate& c) {
  if (decodedAny_) ++resyncs_;
  pendingCount_ = 0;
  synced_ = true;
  pol_ = c.pol;
  rxRef_ = c.rxEnd;
  const std::uint64_t newest = count_ - 1 - (count_ - 1 - rxRef_) % 7;  // last complete slot

  // How far back the good signal goes: step back until two consecutive
  // slots are invalid or the history runs out.
  std::uint64_t start = newest;
  int badRun = 0;
  for (std::uint64_t s = newest; s >= 6 && available(s - 6); s -= 7) {
    if (ccir::isValid(code(s, pol_))) {
      badRun = 0;
      start = s;
    } else if (++badRun >= 2) {
      break;
    }
    if (s < 7) break;
  }
  invalid_ = idleSlots_ = parityFor_ = parityAgainst_ = 0;
  for (std::uint64_t s = start; s <= newest; s += 7) {
    unsigned cs = code(s, pol_);
    invalid_ = ((invalid_ << 1) | (ccir::isValid(cs) ? 0u : 1u)) & kMask16;
    idleSlots_ = ((idleSlots_ << 1) | (ccir::isIdle(cs) ? 1u : 0u)) & 0xff;
    if (isRx(s)) decodeRx(s, start);
  }
}

void FecDecoder::onSlot(std::uint64_t e) {
  unsigned c = code(e, pol_);
  bool valid = ccir::isValid(c);
  invalid_ = ((invalid_ << 1) | (valid ? 0u : 1u)) & kMask16;
  idleSlots_ = ((idleSlots_ << 1) | (ccir::isIdle(c) ? 1u : 0u)) & 0xff;
  bool rx = isRx(e);

  // Parity check: repeats five slots apart, and phasing, should fall on RX slots.
  bool evidence = false;
  if (valid && e >= 41 && available(e - 41)) {
    unsigned dx = code(e - 35, pol_);
    evidence = (c == dx && c != ccir::kAlpha) || (c == ccir::kAlpha && code(e - 7, pol_) == ccir::kRep);
  }
  parityFor_ = ((parityFor_ << 1) | (evidence && rx ? 1u : 0u)) & kMask48;
  parityAgainst_ = ((parityAgainst_ << 1) | (evidence && !rx ? 1u : 0u)) & kMask48;
  if (ones(parityAgainst_ & kMask24) >= 4 && ones(parityFor_ & kMask24) <= 1) {
    rxRef_ += 7;
    std::swap(parityFor_, parityAgainst_);
    rx = !rx;
  }

  if (rx) decodeRx(e, 0);
  int bad = ones(invalid_);
  if (bad >= kLoseSyncAlways || (bad >= kLoseSyncInvalid && parityFor_ == 0)) loseSync();
}

void FecDecoder::decodeRx(std::uint64_t e, std::uint64_t firstGood) {
  // While catching up after sync, only characters with both copies in the
  // good part of the signal.
  if (e < firstGood + 35) return;
  // After a resync the slots can shift by up to 6 bits; anything closer
  // than half a DX/RX pair to the last decoded slot is the same character.
  if (decodedAny_ && e <= lastRx_ + 7) return;
  decodedAny_ = true;
  lastRx_ = e;

  const std::uint64_t dxEnd = e - 35;
  bool haveDx = e >= 41 && available(dxEnd - 6);
  float r[7], d[7] = {};
  for (int i = 0; i < 7; ++i) {
    r[i] = pol_ * soft(e - 6 + static_cast<std::uint64_t>(i));
    if (haveDx) d[i] = pol_ * soft(dxEnd - 6 + static_cast<std::uint64_t>(i));
  }
  unsigned cr = code(e, pol_), cd = haveDx ? code(dxEnd, pol_) : 0;
  bool vr = ccir::isValid(cr), vd = haveDx && ccir::isValid(cd);

  FecCode out;
  out.time = times_[e % kHistory];
  if (vr && vd && (cr == cd || (ccir::isIdle(cr) && ccir::isIdle(cd)))) {
    // Both copies valid and equal (or both idle signals, as while phasing).
    out.code = static_cast<std::uint8_t>(cr);
    out.confidence = 1.0f;
  } else if (softFec_) {
    // Maximum likelihood, both copies combined. The hypotheses are "both
    // copies are code word c" for the 35 valid code words, plus the phasing
    // pair (rep in DX, alpha in RX), whose copies differ by design.
    auto corr = [](unsigned c, const float* v) {
      float s = 0.0f;
      for (int i = 0; i < 7; ++i) s += ((c >> i) & 1 ? v[i] : -v[i]);
      return s;
    };
    float best = corr(ccir::kRep, d) + corr(ccir::kAlpha, r), second = -1e9f;
    unsigned bestCode = ccir::kAlpha;
    for (std::uint8_t cand : ccir::validCodes()) {
      float s = corr(cand, r) + corr(cand, d);
      if (s > best) {
        second = best;
        best = s;
        bestCode = cand;
      } else if (s > second) {
        second = s;
      }
    }
    out.code = static_cast<std::uint8_t>(bestCode);
    out.corrected = true;
    if (ccir::isIdle(bestCode) || (vr && bestCode == cr) || (vd && bestCode == cd)) out.confidence = 0.75f;
    else if (best - second >= kMinMargin) out.confidence = 0.4f;
    else out.error = true;
  } else {
    // Classic hard decision: take a valid copy; if both are valid but
    // differ, the one whose weakest bit is stronger.
    out.corrected = true;
    auto weakest = [](const float* b) {
      float m = 1e9f;
      for (int i = 0; i < 7; ++i) m = std::min(m, std::fabs(b[i]));
      return m;
    };
    if ((vr && ccir::isIdle(cr)) || (vd && ccir::isIdle(cd))) {
      out.code = ccir::kAlpha;  // phasing or idle
      out.confidence = 0.75f;
    } else if (vr && vd) {
      out.code = static_cast<std::uint8_t>(weakest(r) >= weakest(d) ? cr : cd);
      out.confidence = 0.5f;
    } else if (vr || vd) {
      out.code = static_cast<std::uint8_t>(vr ? cr : cd);
      out.confidence = 0.75f;
    } else {
      out.error = true;
    }
  }
  if (out.error) out.confidence = 0.0f;  // `code` stays the best guess
  deliver(out);
}

void FecDecoder::deliver(const FecCode& code) {
  // Noise almost never produces two matching copies, so a confirmed
  // character proves that the characters held before it came from the
  // signal, unless they sit between idle signals: then they are damaged
  // idle signals.
  bool confirmed = !code.error && !code.corrected;
  bool idle = !code.error && ccir::isIdle(code.code);
  if (confirmed) {
    if (!(idle && pendingAfterIdle_))
      for (int i = 0; i < pendingCount_; ++i)
        if (out_) out_->onCode(pending_[i]);
    pendingCount_ = 0;
    lastConfirmedIdle_ = idle;
    if (!idle && out_) out_->onCode(code);
    return;
  }
  if (idle) return;
  if (pendingCount_ == kMaxPending) {
    for (int i = 0; i < pendingCount_; ++i)
      if (out_) out_->onCode(pending_[i]);
    pendingCount_ = 0;
    lastConfirmedIdle_ = false;  // text was passed on
  }
  if (pendingCount_ == 0) pendingAfterIdle_ = lastConfirmedIdle_;
  pending_[pendingCount_++] = code;
}

}  // namespace navtex
