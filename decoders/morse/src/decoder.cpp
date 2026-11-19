#include "morse/decoder.hpp"

#include <algorithm>
#include <cmath>

namespace morse {

namespace {
constexpr double kEnvelopeRate = 2000.0;   // target decimated envelope rate, Hz
constexpr double kActivityRate = 200.0;    // activity trace rate, Hz
constexpr double kMaxSmoothSeconds = 0.08;
constexpr double kTiny = 1e-12;
constexpr double kPreBufferSeconds = 0.25;  // audio held back until the first tone lock

// One-pole smoothing coefficient for time constant `tau` at step `dt`.
double coef(double dt, double tau) { return 1.0 - std::exp(-dt / tau); }

// Envelope level tracking (seconds).
constexpr double kSignalFollowInMark = 0.03;  // follow fading during a mark
constexpr double kLevelWindowDits = 25.0;     // histogram window, in dits
constexpr double kMinLevelWindow = 1.2;
constexpr double kMaxLevelWindow = 4.0;
constexpr double kBacklogSeconds = 0.6;       // envelope kept while levels are unknown
constexpr double kSignalRelax = 1.5;          // drift back to the histogram level
constexpr double kSmoothDits = 0.2;           // envelope smoothing length
constexpr double kHoldDits = 0.25;            // debounce time
constexpr double kAdaptSeparation = 5.0;      // level separation needed to adapt speed
constexpr double kWarmupSeconds = 0.03;       // filter settling before tracking starts
constexpr double kPresenceHang = 2.0;         // seconds signal presence is held
constexpr int kSpectrumEvery = 5;             // tracker frames per spectrum frame (~10/s)
}  // namespace

MorseDecoder::MorseDecoder(const DecoderConfig& config) {
  cfg_ = config;
  cfg_.sampleRate = 0;  // force a full reconfiguration
  configure(config);
}

void MorseDecoder::setSink(DecoderSink* sink) {
  sink_ = sink;
  timing_.setSink(sink);
}

void MorseDecoder::configure(const DecoderConfig& c) {
  bool structural = c.sampleRate != cfg_.sampleRate || c.channels != cfg_.channels ||
                    c.minWpm != cfg_.minWpm || c.maxWpm != cfg_.maxWpm ||
                    c.errorCorrection != cfg_.errorCorrection;
  bool rangeChanged = c.minToneHz != cfg_.minToneHz || c.maxToneHz != cfg_.maxToneHz;
  cfg_ = c;
  cfg_.channels = std::max(1, cfg_.channels);
  cfg_.threshold = std::clamp(cfg_.threshold, 0.1, 0.9);
  cfg_.hysteresis = std::clamp(cfg_.hysteresis, 0.0, 0.3);
  cfg_.minBandwidthHz = std::clamp(cfg_.minBandwidthHz, 20.0, 2000.0);
  cfg_.maxBandwidthHz = std::clamp(cfg_.maxBandwidthHz, cfg_.minBandwidthHz, 2000.0);

  if (structural) {
    decim_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(cfg_.sampleRate / kEnvelopeRate)));
    envRate_ = cfg_.sampleRate / static_cast<double>(decim_);
    activityDecim_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(envRate_ / kActivityRate)));
    smoother_.setMaxLength(static_cast<std::size_t>(envRate_ * kMaxSmoothSeconds) + 1);
    levels_.configure(envRate_, kMaxLevelWindow);
    backlog_.assign(static_cast<std::size_t>(envRate_ * kBacklogSeconds), 0.0f);
    pre_.assign(static_cast<std::size_t>(cfg_.sampleRate * kPreBufferSeconds), 0.0f);
    tracker_.configure(cfg_.sampleRate, cfg_.minToneHz, cfg_.maxToneHz);
    timing_.configure(cfg_.minWpm, cfg_.maxWpm, cfg_.errorCorrection);
    reset();
    return;
  }
  if (rangeChanged) tracker_.configure(cfg_.sampleRate, cfg_.minToneHz, cfg_.maxToneHz);
  if (!cfg_.afc) toneHz_ = -1.0;  // re-tuned below
  updateFilters(true);
}

void MorseDecoder::reset() {
  tracker_.reset();
  timing_.reset();
  toneHz_ = -1.0;
  bandwidthHz_ = cfg_.bandwidthHz;
  phaseRe_ = 1.0;
  phaseIm_ = 0.0;
  renorm_ = 0;
  lpI_.reset();
  lpQ_.reset();
  decimCount_ = 0;
  accI_ = accQ_ = 0.0;
  envCount_ = 0;
  smoother_.reset();
  noise_ = signal_ = 0.0;
  levels_.reset();
  backlogHead_ = backlogCount_ = 0;
  envInit_ = false;
  rawKey_ = key_ = false;
  candidateTime_ = -1.0;
  lastEdge_ = 0.0;
  snrDb_ = 0.0;
  signalPresent_ = false;
  presentUntil_ = -1.0;
  activityCount_ = 0;
  activityPeak_ = 0.0f;
  preHead_ = preCount_ = 0;
  spectrumCount_ = 0;
  updateFilters(true);
}

void MorseDecoder::updateFilters(bool force) {
  double tone = cfg_.afc && tracker_.locked() ? tracker_.frequency() : cfg_.fixedToneHz;
  if (force || std::fabs(tone - toneHz_) > 0.25) {
    toneHz_ = tone;
    double w = -2.0 * kPi * toneHz_ / cfg_.sampleRate;
    stepRe_ = std::cos(w);
    stepIm_ = std::sin(w);
  }

  double dit = timing_.hasEstimate() ? timing_.dit() : 1.2 / cfg_.maxWpm;
  double bw = cfg_.autoBandwidth ? 5.0 / dit : cfg_.bandwidthHz;
  bw = std::clamp(bw, cfg_.minBandwidthHz, cfg_.maxBandwidthHz);
  if (force || std::fabs(bw - bandwidthHz_) > 0.1 * bandwidthHz_) {
    bandwidthHz_ = bw;
    lpI_.setLowpass(cfg_.sampleRate, bw / 2.0);
    lpQ_.setLowpass(cfg_.sampleRate, bw / 2.0);
  }
  smoother_.setLength(static_cast<std::size_t>(std::lround(kSmoothDits * dit * envRate_)));
  levels_.setWindowSeconds(std::clamp(kLevelWindowDits * dit, kMinLevelWindow, kMaxLevelWindow));
}

void MorseDecoder::process(const float* samples, std::size_t frames) {
  const int ch = cfg_.channels;
  const double inv = 1.0 / ch;
  for (std::size_t n = 0; n < frames; ++n) {
    double x = samples[n * ch];
    for (int c = 1; c < ch; ++c) x += samples[n * ch + c];
    x *= inv;

    if (tracker_.process(x)) {
      if (cfg_.afc && tracker_.locked() && std::fabs(tracker_.frequency() - toneHz_) > 0.25) updateFilters(false);
      if (++spectrumCount_ >= kSpectrumEvery) {
        spectrumCount_ = 0;
        emitSpectrum();
      }
    }

    if (cfg_.afc && !tracker_.locked()) {
      // No tone lock yet: hold the audio back briefly so the elements heard
      // while the tracker acquires are demodulated at the right frequency.
      if (preCount_ == pre_.size()) {
        demodulate(pre_[preHead_]);
        preHead_ = (preHead_ + 1) % pre_.size();
        --preCount_;
      }
      pre_[(preHead_ + preCount_) % pre_.size()] = static_cast<float>(x);
      ++preCount_;
      continue;
    }
    drainPreBuffer();
    demodulate(x);
  }
}

void MorseDecoder::emitSpectrum() {
  if (!sink_) return;
  SpectrumFrame f;
  const auto& p = tracker_.power();
  f.binHz = static_cast<float>(tracker_.binHz());
  f.bins = static_cast<int>(std::min<std::size_t>(SpectrumFrame::kBins, p.size()));
  for (int k = 0; k < f.bins; ++k) f.db[k] = static_cast<float>(10.0 * std::log10(p[static_cast<std::size_t>(k)] + 1e-20));
  f.toneHz = static_cast<float>(toneHz_);
  f.toneLocked = !cfg_.afc || tracker_.locked();
  f.searchMinHz = static_cast<float>(tracker_.searchMinHz());
  f.searchMaxHz = static_cast<float>(tracker_.searchMaxHz());
  sink_->onSpectrum(f);
}

void MorseDecoder::drainPreBuffer() {
  while (preCount_) {
    demodulate(pre_[preHead_]);
    preHead_ = (preHead_ + 1) % pre_.size();
    --preCount_;
  }
}

void MorseDecoder::demodulate(double x) {
  // Mix the tone down to 0 Hz and low-pass: a band-pass centred on the tone.
  double i = lpI_.process(x * phaseRe_);
  double q = lpQ_.process(x * phaseIm_);
  double re = phaseRe_ * stepRe_ - phaseIm_ * stepIm_;
  phaseIm_ = phaseRe_ * stepIm_ + phaseIm_ * stepRe_;
  phaseRe_ = re;
  if ((++renorm_ & 1023u) == 0) {
    double g = 1.0 / std::sqrt(phaseRe_ * phaseRe_ + phaseIm_ * phaseIm_);
    phaseRe_ *= g;
    phaseIm_ *= g;
  }

  accI_ += i;
  accQ_ += q;
  if (++decimCount_ == decim_) {
    double mag = 2.0 * std::sqrt(accI_ * accI_ + accQ_ * accQ_) / static_cast<double>(decim_);
    accI_ = accQ_ = 0.0;
    decimCount_ = 0;
    envelopeStep(mag);
  }
}

void MorseDecoder::envelopeStep(double m) {
  const double t = static_cast<double>(envCount_) / envRate_;
  ++envCount_;

  double s = smoother_.process(m);
  if (t < kWarmupSeconds) return;  // let the filters settle
  levels_.push(s);

  // Spectral presence, held for a while so it cannot flicker inside a word.
  if (tracker_.signalPresentNear(toneHz_)) presentUntil_ = t + kPresenceHang;
  signalPresent_ = t < presentUntil_;

  const double dt = 1.0 / envRate_;
  if (!levels_.valid()) {
    // No usable levels: hold the recent envelope back so it can be decided
    // once levels are known, instead of losing the first elements. Samples
    // leaving the backlog are decided (as key-up) in time order, so the
    // timing decoder always sees time moving forward.
    if (backlogCount_ == backlog_.size()) {
      decide(backlog_[backlogHead_], t - static_cast<double>(backlogCount_) * dt, false);
      backlogHead_ = (backlogHead_ + 1) % backlog_.size();
      --backlogCount_;
    }
    backlog_[(backlogHead_ + backlogCount_) % backlog_.size()] = static_cast<float>(s);
    ++backlogCount_;
  } else {
    for (std::size_t i = 0; i < backlogCount_; ++i)
      decide(backlog_[(backlogHead_ + i) % backlog_.size()], t - static_cast<double>(backlogCount_ - i) * dt, true);
    backlogCount_ = backlogHead_ = 0;
    decide(s, t, true);
  }

  if ((envCount_ & 63u) == 0) {
    timing_.setSnrDb(snrDb_);
    updateFilters(false);
  }

  activityPeak_ = std::max(activityPeak_, static_cast<float>(s));
  if (++activityCount_ >= activityDecim_) {
    if (sink_) {
      ActivitySample a;
      float ref = static_cast<float>(std::max(signal_, kTiny));
      a.level = activityPeak_ / ref;
      a.threshold = static_cast<float>(thresholdLevel_ / ref);
      a.noise = static_cast<float>(noise_ / ref);
      a.keyDown = key_;
      sink_->onActivity(a);
    }
    activityCount_ = 0;
    activityPeak_ = 0.0f;
  }
}

void MorseDecoder::decide(double s, double t, bool levelsOk) {
  const double dt = 1.0 / envRate_;
  // Levels (the AGC): the space and mark levels come from the envelope
  // histogram (LevelEstimator), which needs no detection. A local signal
  // level follows the marks as they are keyed, to track fading faster than
  // the histogram window, and relaxes back to the histogram level between
  // them.
  if (levelsOk) {
    noise_ = levels_.low();
    if (!envInit_) {
      signal_ = levels_.high();
      envInit_ = true;
    }
    if (key_) signal_ += coef(dt, kSignalFollowInMark) * (s - signal_);
    else signal_ += coef(dt, kSignalRelax) * (levels_.high() - signal_);
    signal_ = std::max(signal_, noise_ * 1.05);
  }
  double span = signal_ - noise_;
  snrDb_ = levelsOk ? 20.0 * std::log10(levels_.high() / std::max(noise_, kTiny)) : 0.0;

  double on = std::clamp(cfg_.threshold + cfg_.hysteresis, 0.05, 0.95);
  double off = std::clamp(cfg_.threshold - cfg_.hysteresis, 0.05, 0.95);
  // Optional user squelch: a minimum level above the noise floor.
  double squelch = noise_ * std::pow(10.0, std::max(0.0, cfg_.squelchDb) / 20.0);
  double thr = std::max(noise_ + (key_ ? off : on) * span, key_ ? 0.9 * squelch : squelch);
  thresholdLevel_ = thr;
  rawKey_ = signalPresent_ && levelsOk && s > thr;
  timing_.setAdaptation(levels_.separation() >= kAdaptSeparation);

  // Debounce: a change must persist for a fraction of a dit. The edge is
  // back-dated to where the change started so durations stay accurate.
  double dit = timing_.hasEstimate() ? timing_.dit() : 1.2 / cfg_.maxWpm;
  double hold = std::clamp(kHoldDits * dit, 0.002, 0.04);
  if (rawKey_ != key_) {
    if (candidateTime_ < 0.0) candidateTime_ = t;
    if (t - candidateTime_ >= hold) {
      setKey(rawKey_, candidateTime_);
      candidateTime_ = -1.0;
    }
  } else {
    candidateTime_ = -1.0;
  }
  if (!key_) timing_.onTick(t);
}

void MorseDecoder::setKey(bool down, double edgeTime) {
  if (!down) timing_.onMark(lastEdge_, edgeTime);
  key_ = down;
  lastEdge_ = edgeTime;
}

void MorseDecoder::flush() {
  drainPreBuffer();
  double now = streamTime();
  if (key_) setKey(false, now);
  candidateTime_ = -1.0;
  // A gap far longer than any word gap closes the character and the word.
  timing_.onTick(now + 10.0);
  timing_.onTick(now + 20.0);
}

DecoderStatus MorseDecoder::status() const {
  DecoderStatus s;
  s.toneHz = toneHz_;
  s.toneLocked = !cfg_.afc || tracker_.locked();
  s.wpm = timing_.hasEstimate() ? timing_.wpm() : 0.0;
  s.speedLocked = timing_.locked();
  s.snrDb = snrDb_;
  s.bandwidthHz = bandwidthHz_;
  s.keyDown = key_;
  s.signalPresent = signalPresent_;
  s.quality = timing_.quality();
  s.charsDecoded = timing_.charsDecoded();
  return s;
}

}  // namespace morse
