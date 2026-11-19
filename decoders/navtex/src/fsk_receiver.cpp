#include "navtex/fsk_receiver.hpp"

namespace navtex {

namespace {
bool rateChanged(const SitorBConfig& a, const SitorBConfig& b) {
  return a.sampleRate != b.sampleRate || a.shiftHz != b.shiftHz || a.baud != b.baud;
}
bool trackerChanged(const SitorBConfig& a, const SitorBConfig& b) {
  return rateChanged(a, b) || a.afc != b.afc || a.minCentreHz != b.minCentreHz || a.maxCentreHz != b.maxCentreHz ||
         a.fixedCentreHz != b.fixedCentreHz || a.wideSpectrum != b.wideSpectrum;
}
}  // namespace

FskReceiver::FskReceiver(const SitorBConfig& config) : cfg_(config) {
  tracker_.configure(cfg_);
  demod_.configure(cfg_.sampleRate, cfg_.baud, cfg_.shiftHz);
  dt_ = 1.0 / cfg_.sampleRate;
  reset();
}

bool FskReceiver::resetsOn(const SitorBConfig& config) const {
  return trackerChanged(cfg_, config) || cfg_.channels != config.channels;
}

bool FskReceiver::configure(const SitorBConfig& config) {
  const SitorBConfig old = cfg_;
  const bool resets = resetsOn(config);
  cfg_ = config;
  if (trackerChanged(old, cfg_)) tracker_.configure(cfg_);
  if (rateChanged(old, cfg_)) demod_.configure(cfg_.sampleRate, cfg_.baud, cfg_.shiftHz);
  dt_ = 1.0 / cfg_.sampleRate;
  if (resets) reset();
  return resets;
}

void FskReceiver::reset() {
  tracker_.reset();
  demod_.reset();
  demod_.setCentre(tracker_.centreHz());
  spectrumCount_ = 0;
}

void FskReceiver::process(const float* samples, std::size_t frames, Client& client) {
  const int ch = cfg_.channels < 1 ? 1 : cfg_.channels;
  const double scale = 1.0 / ch;
  for (std::size_t f = 0; f < frames; ++f) {
    double x = 0.0;
    for (int c = 0; c < ch; ++c) x += samples[f * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)];
    x *= scale;
    time_ += dt_;
    if (tracker_.process(x)) {
      // Locked for a while without finding the signal's structure: not a
      // signal of this mode.
      if (!tracker_.locked() || client.synced()) lastSyncOrUnlock_ = time_;
      else if (time_ - lastSyncOrUnlock_ > cfg_.rejectAfterSeconds) {
        tracker_.reject();
        lastSyncOrUnlock_ = time_;
      }
      demod_.setCentre(tracker_.centreHz());
      if (sink_ && ++spectrumCount_ % 2 == 0) {
        SpectrumFrame frame;
        tracker_.fillSpectrum(frame);
        sink_->onSpectrum(frame);
      }
    }
    float soft;
    bool bit = demod_.process(x, soft);
    if (demod_.newSample()) client.onSample(demod_.discriminator(), time_);
    if (bit) client.onBit(soft, time_);
  }
}

void FskReceiver::fillStatus(SitorBStatus& s, bool synced) const {
  s.centreHz = tracker_.centreHz();
  s.centreLocked = tracker_.locked();
  s.signalPresent = tracker_.signalPresent() || synced;
  s.snrDb = tracker_.snrDb();
  s.sync = synced ? SyncState::Synced : s.signalPresent ? SyncState::Searching : SyncState::NoSignal;
}

// ------------------------------------------------------------- transmissions

void TransmissionTracker::character(const DecodedChar& c, bool reversed, bool error, DecoderSink* sink) {
  ++chars_;
  lastCharTime_ = c.time;
  if (!active_) {
    active_ = true;
    ++count_;
    current_ = TransmissionEvent();
    current_.kind = TransmissionEvent::Started;
    current_.time = c.time;
    current_.reversed = reversed;
    if (sink) sink->onTransmission(current_);
  }
  const char ch = c.text[0];
  if (ch != ' ' && ch != '\n') {
    bool err = error || ch == '*';
    ++current_.chars;
    current_.errors += err ? 1 : 0;
    if (errorFill_ == 200) errorCount_ -= errorRing_[errorPos_] ? 1 : 0;
    else ++errorFill_;
    errorRing_[errorPos_] = err;
    errorCount_ += err ? 1 : 0;
    errorPos_ = (errorPos_ + 1) % 200;
  }
  if (sink) sink->onCharacter(c);
}

void TransmissionTracker::end(bool complete, double time, DecoderSink* sink) {
  if (!active_) return;
  active_ = false;
  current_.kind = TransmissionEvent::Ended;
  current_.time = time;
  current_.complete = complete;
  if (sink) sink->onTransmission(current_);
}

void TransmissionTracker::fillStatus(SitorBStatus& s) const {
  s.errorRate = errorRate();
  s.charsDecoded = chars_;
  s.inTransmission = active_;
  s.transmissions = count_;
}

}  // namespace navtex
