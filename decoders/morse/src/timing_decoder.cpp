#include "morse/timing_decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace morse {

namespace {
constexpr double kMinClusterRatio = 1.8;  // dah/dit ratio that counts as two clusters
constexpr double kMaxClusterRatio = 6.0;
constexpr double kMinDahRatio = 2.0;      // allowed dah/dit ratio of the estimate
constexpr double kMaxDahRatio = 5.0;
constexpr double kCharGapUnits = 2.0;
constexpr double kWordGapUnits = 5.0;     // default word gap threshold
constexpr double kRelockSeconds = 4.0;    // silence after which speed is re-acquired
constexpr double kForceLockSeconds = 1.5;
constexpr double kForceLockUnits = 20.0;  // a gap this long ends acquisition (covers Farnsworth gaps)
constexpr int kLockMarks = 12;
constexpr double kRecoverRatio = 1.5;     // estimate error that overrides the weak-signal freeze

void copyText(char* dst, std::size_t size, const char* src) {
  std::strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// 2-means in the log domain over `n` values. Returns false if the values do
// not form two separated clusters.
bool twoMeans(const double* v, int n, double& lo, double& hi, int& nLo, int& nHi) {
  if (n < 2) return false;
  double mn = v[0], mx = v[0];
  for (int i = 1; i < n; ++i) {
    mn = std::min(mn, v[i]);
    mx = std::max(mx, v[i]);
  }
  if (mx / mn < kMinClusterRatio) return false;
  double c1 = std::log(mn), c2 = std::log(mx);
  for (int iter = 0; iter < 12; ++iter) {
    double mid = 0.5 * (c1 + c2), s1 = 0, s2 = 0;
    nLo = nHi = 0;
    for (int i = 0; i < n; ++i) {
      double l = std::log(v[i]);
      if (l < mid) {
        s1 += l;
        ++nLo;
      } else {
        s2 += l;
        ++nHi;
      }
    }
    if (!nLo || !nHi) return false;
    double n1 = s1 / nLo, n2 = s2 / nHi;
    if (n1 == c1 && n2 == c2) break;
    c1 = n1;
    c2 = n2;
  }
  lo = std::exp(c1);
  hi = std::exp(c2);
  double ratio = hi / lo;
  return ratio >= kMinClusterRatio && ratio <= kMaxClusterRatio;
}
}  // namespace

void TimingDecoder::configure(double minWpm, double maxWpm, bool errorCorrection) {
  minWpm_ = std::max(1.0, std::min(minWpm, maxWpm));
  maxWpm_ = std::max(minWpm, maxWpm);
  errorCorrection_ = errorCorrection;
  reset();
}

void TimingDecoder::reset() {
  dot_ = 0.06;
  dash_ = 0.18;
  haveEstimate_ = locked_ = false;
  markCount_ = markPos_ = gapCount_ = gapPos_ = 0;
  lastLo_ = lastHi_ = 0;
  pendingN_ = 0;
  interCount_ = interPos_ = 0;
  replaying_ = false;
  wordGapUnits_ = kWordGapUnits;
  curN_ = 0;
  charOpen_ = anyOutput_ = false;
  wordSpaceDone_ = true;
  lastMarkEnd_ = -1.0;
  quality_ = 0.0;
  charsDecoded_ = 0;
}

double TimingDecoder::minMarkSeconds() const {
  double fastestDot = 1.2 / maxWpm_;
  return locked_ ? std::max(0.25 * fastestDot, 0.3 * dot_) : 0.25 * fastestDot;
}

bool TimingDecoder::isDah(double d) const { return d > std::sqrt(dot_ * dash_); }

double TimingDecoder::elementConfidence(double d) const {
  double b = std::sqrt(dot_ * dash_);
  double x = isDah(d) ? std::log(d / b) / std::log(dash_ / b) : std::log(b / d) / std::log(b / dot_);
  return std::clamp(x, 0.0, 1.0);
}

void TimingDecoder::clampEstimate(bool keepDot) {
  if (keepDot) dash_ = std::clamp(dash_, kMinDahRatio * dot_, kMaxDahRatio * dot_);
  else dot_ = std::clamp(dot_, dash_ / kMaxDahRatio, dash_ / kMinDahRatio);
  double u = unit(), lo = 1.2 / maxWpm_, hi = 1.2 / minWpm_;
  if (u < lo || u > hi) {
    double s = std::clamp(u, lo, hi) / u;
    dot_ *= s;
    dash_ *= s;
  }
}

void TimingDecoder::updateEstimate(double d, double gapBefore) {
  marks_[markPos_] = d;
  markPos_ = (markPos_ + 1) % kHistory;
  markCount_ = std::min(markCount_ + 1, kHistory);
  if (gapBefore > 0.0 && gapBefore < kForceLockSeconds) {
    gaps_[gapPos_] = gapBefore;
    gapPos_ = (gapPos_ + 1) % kHistory;
    gapCount_ = std::min(gapCount_ + 1, kHistory);
  }

  double lo, hi;
  bool two = twoMeans(marks_, markCount_, lo, hi, lastLo_, lastHi_);
  if (locked_ && !adapt_) {
    // Weak signal: keep the estimate unless the recent marks clearly show it
    // is badly wrong (e.g. a wrong lock at the start), then take theirs.
    double off = two ? std::max(lo / dot_, dot_ / lo) : 1.0;
    if (!two || off < kRecoverRatio || markCount_ < kHistory) return;
    dot_ = lo;
    dash_ = hi;
    clampEstimate(true);
    return;
  }
  if (two) {
    if (!haveEstimate_) {
      dot_ = lo;
      dash_ = hi;
    } else {
      dot_ += 0.5 * (lo - dot_);
      dash_ += 0.5 * (hi - dash_);
    }
    haveEstimate_ = true;
    clampEstimate(true);
    return;
  }
  lastLo_ = lastHi_ = 0;

  if (haveEstimate_) {
    // Single kind of element in the window: update the matching estimate.
    if (isDah(d)) {
      dash_ += 0.25 * (d - dash_);
      clampEstimate(false);
    } else {
      dot_ += 0.25 * (d - dot_);
      clampEstimate(true);
    }
    return;
  }

  // No estimate yet and all marks alike: decide dit or dah from the gaps.
  // The shortest gaps are element gaps, one unit long.
  if (gapCount_ == 0) return;
  double g = gaps_[0];
  for (int i = 1; i < gapCount_; ++i) g = std::min(g, gaps_[i]);
  double glo, ghi;
  int a, b;
  if (twoMeans(gaps_, gapCount_, glo, ghi, a, b)) g = glo;
  double m = 0;
  for (int i = 0; i < markCount_; ++i) m += std::log(marks_[i]);
  m = std::exp(m / markCount_);
  if (m < 2.0 * g) {
    dot_ = m;
    dash_ = 3.0 * m;
  } else {
    dash_ = m;
    dot_ = m / 3.0;
  }
  haveEstimate_ = true;
  clampEstimate(true);
}

void TimingDecoder::onMark(double start, double end) {
  double d = end - start;
  if (d < minMarkSeconds()) return;  // glitch: the surrounding space continues

  double lastEnd = locked_ ? lastMarkEnd_ : (pendingN_ ? pending_[pendingN_ - 1].end : -1.0);
  double gap = lastEnd >= 0.0 ? start - lastEnd : -1.0;

  if (!locked_) {
    pending_[pendingN_++] = {start, end};
    updateEstimate(d, gap);
    bool twoClear = lastLo_ >= 2 && lastHi_ >= 2;
    if (twoClear || pendingN_ >= kLockMarks || pendingN_ == kPending) lockAndReplay(end);
    return;
  }
  updateEstimate(d, gap);
  addElement(start, end, end);
}

void TimingDecoder::onTick(double now) {
  if (!locked_) {
    if (pendingN_ == 0) return;
    double gap = now - pending_[pendingN_ - 1].end;
    double limit = haveEstimate_ ? std::min(kForceLockSeconds, kForceLockUnits * unit()) : kForceLockSeconds;
    if (gap > limit) {
      if (!haveEstimate_) {
        // Too little to go on (e.g. a lone "E"): assume the geometric middle
        // of the speed range, classifying marks against it.
        double u = 1.2 / std::sqrt(minWpm_ * maxWpm_);
        dot_ = u;
        dash_ = 3.0 * u;
        haveEstimate_ = true;
      }
      lockAndReplay(now);
      processGap(now);
    }
    return;
  }
  processGap(now);
  if (!charOpen_ && lastMarkEnd_ >= 0.0 && now - lastMarkEnd_ > kRelockSeconds) {
    // Long silence: the next signal may be a different station. Re-acquire
    // its speed (buffering again), keeping the old estimate as a fallback.
    locked_ = false;
    markCount_ = markPos_ = gapCount_ = gapPos_ = 0;
    lastMarkEnd_ = -1.0;
  }
}

void TimingDecoder::lockAndReplay(double now) {
  locked_ = true;
  // Learn the word-gap threshold from the buffered gaps first, so that the
  // first word of a Farnsworth transmission is not split into letters.
  for (int i = 1; i < pendingN_; ++i) {
    double gapUnits = (pending_[i].start - pending_[i - 1].end) / unit();
    if (gapUnits >= kCharGapUnits && gapUnits < 60.0) learnWordGap(gapUnits);
  }
  replaying_ = true;
  for (int i = 0; i < pendingN_; ++i) {
    processGap(pending_[i].start);
    addElement(pending_[i].start, pending_[i].end, now);
  }
  pendingN_ = 0;
  replaying_ = false;
}

void TimingDecoder::processGap(double now) {
  if (lastMarkEnd_ < 0.0) return;
  double gap = now - lastMarkEnd_, u = unit();
  if (charOpen_ && gap >= kCharGapUnits * u) emitCharacter(now);
  if (!charOpen_ && anyOutput_ && !wordSpaceDone_ && gap >= wordGapUnits_ * u) emitSpace(now);
}

void TimingDecoder::learnWordGap(double gapUnits) {
  interGaps_[interPos_] = gapUnits;
  interPos_ = (interPos_ + 1) % kHistory;
  interCount_ = std::min(interCount_ + 1, kHistory);
  // Character and word gaps form two clusters; the threshold goes between
  // them. This follows Farnsworth timing, where both are stretched.
  double lo, hi;
  int nLo, nHi;
  if (twoMeans(interGaps_, interCount_, lo, hi, nLo, nHi) && nLo >= 2) {
    wordGapUnits_ = std::max(4.0, std::sqrt(lo * hi));
  } else if (interCount_ >= 2) {
    double m = 0, mn = interGaps_[0], mx = interGaps_[0];
    for (int i = 0; i < interCount_; ++i) {
      m += interGaps_[i];
      mn = std::min(mn, interGaps_[i]);
      mx = std::max(mx, interGaps_[i]);
    }
    m /= interCount_;
    if (m < kWordGapUnits && interCount_ >= 4) {
      wordGapUnits_ = std::max(kWordGapUnits, 1.7 * m);
    } else if (m >= kWordGapUnits && mx / mn < 1.6) {
      // Several equally long gaps above the default word gap: consecutive
      // one-letter words are rare, so these are stretched (Farnsworth)
      // character gaps.
      wordGapUnits_ = 1.7 * m;
    }
  }
}

void TimingDecoder::addElement(double start, double end, double) {
  if (curN_ == kMaxCharElements) emitCharacter(start);
  if (!charOpen_ && lastMarkEnd_ >= 0.0 && !replaying_) {
    double gapUnits = (start - lastMarkEnd_) / unit();
    if (gapUnits >= kCharGapUnits && gapUnits < 60.0) learnWordGap(gapUnits);
  }
  double gapBefore = (charOpen_ && lastMarkEnd_ >= 0.0) ? start - lastMarkEnd_ : 0.0;
  cur_[curN_++] = {end - start, gapBefore};
  charOpen_ = true;
  wordSpaceDone_ = false;
  lastMarkEnd_ = end;
}

void TimingDecoder::emit(const char* text, Code code, float confidence, bool corrected, double now) {
  DecodedChar c;
  copyText(c.text, sizeof c.text, text);
  if (code > kEmptyCode) copyText(c.pattern, sizeof c.pattern, patternFromCode(code).c_str());
  c.confidence = confidence;
  c.wpm = static_cast<float>(wpm());
  c.time = now;
  c.corrected = corrected;
  if (code > kEmptyCode) {
    quality_ += (charsDecoded_ == 0 ? 1.0 : 0.1) * (confidence - quality_);
    ++charsDecoded_;
  }
  anyOutput_ = true;
  if (sink_) sink_->onCharacter(c);
}

void TimingDecoder::emitCharacter(double now) {
  int n = curN_;
  curN_ = 0;
  charOpen_ = false;
  if (n == 0) return;

  Code code = kEmptyCode;
  double minConf = 1.0;
  for (int i = 0; i < n; ++i) {
    code = appendElement(code, isDah(cur_[i].duration));
    minConf = std::min(minConf, elementConfidence(cur_[i].duration));
  }
  double snrFactor = 0.6 + 0.4 * std::clamp((snrDb_ - 6.0) / 14.0, 0.0, 1.0);
  auto conf = static_cast<float>((0.4 + 0.6 * minConf) * snrFactor);

  if (n <= kMaxElements) {
    if (const char* text = lookup(code)) {
      emit(text, code, conf, false, now);
      return;
    }
  }

  if (errorCorrection_ && n >= 2 && n <= kMaxElements) {
    // Two characters sent with too short a gap between them: split at the
    // widest internal gaps first and accept the first split where both
    // halves are valid characters.
    int order[kMaxCharElements];
    for (int i = 1; i < n; ++i) order[i - 1] = i;
    std::sort(order, order + n - 1, [&](int a, int b) { return cur_[a].gapBefore > cur_[b].gapBefore; });
    for (int k = 0; k < n - 1; ++k) {
      int split = order[k];
      Code left = kEmptyCode, right = kEmptyCode;
      for (int i = 0; i < n; ++i) {
        bool dah = isDah(cur_[i].duration);
        if (i < split) left = appendElement(left, dah);
        else right = appendElement(right, dah);
      }
      const char* lt = lookup(left);
      const char* rt = lookup(right);
      if (lt && rt) {
        emit(lt, left, conf * 0.6f, true, now);
        emit(rt, right, conf * 0.6f, true, now);
        return;
      }
    }
  }
  emit("*", n <= kMaxElements ? code : kEmptyCode, 0.1f, false, now);
}

void TimingDecoder::emitSpace(double now) {
  wordSpaceDone_ = true;
  emit(" ", kEmptyCode, 1.0f, false, now);
}

}  // namespace morse
