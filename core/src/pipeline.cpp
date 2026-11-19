#include "hfc/pipeline.hpp"

#include <chrono>

#include <algorithm>
#include <cmath>

namespace hfc {

const std::vector<Emission> Pipeline::kNone;

Pipeline::Pipeline() : Pipeline(Config()) {}
Pipeline::Pipeline(const Config& config) : cfg_(config) {}

Pipeline::~Pipeline() {
  // The host removes its channels from the channelizer: destroy it first.
  host_.reset();
}

void Pipeline::setDecoderPolicy(const DecoderHost::Config& p) {
  cfg_.decoders.maxSessions = p.maxSessions;
  cfg_.decoders.measureUnknown = p.measureUnknown;
  cfg_.decoders.giveUpSeconds = p.giveUpSeconds;
  cfg_.decoders.minSnrDb = p.minSnrDb;
  cfg_.decoders.disabled = p.disabled;
  if (host_) {
    host_->setPolicy(p);
    planNoGroup();
  }
}

void Pipeline::ignore(uint64_t id) {
  if (host_) host_->ignore(id);
}

DecoderHost::Usage Pipeline::usage(double* iqSeconds) const {
  DecoderHost::Usage u;
  if (host_) u = host_->usage();
  u.seconds["spectrum and detector"] += std::max(0.0, processSeconds_ - u.inside);
  if (iqSeconds) *iqSeconds = iqSeconds_;
  return u;
}

void Pipeline::setFrequencies(std::shared_ptr<const FrequencyList> list) {
  cfg_.decoders.frequencies = list;
  if (host_) host_->setFrequencies(std::move(list));
}

bool Pipeline::configure(double sampleRate, double centreHz, double utc) {
  if (!(sampleRate >= 24000.0 && sampleRate <= 100e6)) {
    // Not a rate this can process: everything ends, input is ignored until
    // the next configure().
    if (det_) {
      det_->endAll(utc);
      nextId_ = det_->nextId();
    }
    if (host_) host_->flush();
    host_.reset();
    det_.reset();
    ch_.reset();
    rate_ = 0;
    return false;
  }
  if (sampleRate != rate_ || !ch_) {
    if (det_) det_->endAll(utc);
    if (host_) host_->flush();
    host_.reset();
    rate_ = sampleRate;
    centre_ = centreHz;
    anchors_.clear();
    build();
    return true;
  }
  if (centreHz != centre_) {
    centre_ = centreHz;
    det_->setCentre(centreHz, utc);  // ends every emission
    host_->setCentre(centreHz);
    planNoGroup();
  }
  return true;
}

void Pipeline::build() {
  const uint64_t nextId = std::max(nextId_, det_ ? det_->nextId() : uint64_t(1));
  host_.reset();
  ch_ = std::make_unique<Channelizer>(rate_);
  det_ = std::make_unique<Detector>(rate_, ch_->fftSize(), static_cast<double>(ch_->hop()) / rate_, cfg_.detector);
  det_->setNextId(nextId);  // emission ids stay unique across sample rate changes
  nextId_ = nextId;
  host_ = std::make_unique<DecoderHost>(*ch_, cfg_.decoders);
  det_->setCentre(centre_, 0);
  host_->setCentre(centre_);
  planNoGroup();
  host_->setUtcFunction([this](uint64_t s) { return utcOf(s); });
  host_->setDecodeCallback([this](const DecodeEvent& e) {
    // Only emissions still on the air are labelled here (an FT8 decode can
    // come after its emission ended; the Store labels it from the decode).
    if (e.emissionId && e.first && live_.count(e.emissionId) && !labels_.count(e.emissionId))
      labels_[e.emissionId] = e.decoder;
    if (decodeFn_) decodeFn_(e);
  });
  host_->setFeaturesCallback([this](uint64_t id, const EmissionFeatures& f) {
    if (featuresFn_) featuresFn_(id, f);
  });
  ch_->setSpectrumCallback([this](const float* p, std::size_t n, uint64_t centreSample) {
    det_->addSpectrum(p, n, utcOf(centreSample));
  });
  ch_->setChannelCallback([this](int id, const cfloat* s, std::size_t n, uint64_t first) {
    host_->channelSamples(id, s, n, first);
  });
  det_->setEmissionCallbacks(
      [this](const Emission& e) {
        live_.insert(e.id);
        host_->emissionStarted(e);
        if (emissionFn_) emissionFn_(EmissionEvent::Started, e);
      },
      [this](const Emission& e) {
        host_->emissionUpdated(e);
        if (emissionFn_) emissionFn_(EmissionEvent::Updated, e);
      },
      [this](const Emission& e) {
        host_->emissionEnded(e);
        if (emissionFn_) emissionFn_(EmissionEvent::Ended, e);
        labels_.erase(e.id);
        live_.erase(e.id);
      });
  det_->setFrameCallback([this](const std::vector<float>& db, const std::vector<float>& floorDb, double utc) {
    if (!waterfallFn_ || db.empty()) return;
    const std::size_t n = db.size();
    const std::size_t out = std::min(cfg_.waterfallBins, n);
    WaterfallRow row;
    row.utc = utc;
    row.lowHz = centre_ - rate_ / 2.0 - det_->binHz() / 2.0;
    row.highHz = row.lowHz + rate_;
    row.db.assign(out, -300.0f);
    row.floorDb.assign(out, 0.0f);
    std::vector<int> counts(out, 0);
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t j = i * out / n;
      row.db[j] = std::max(row.db[j], db[i]);
      row.floorDb[j] += floorDb[i];
      ++counts[j];
    }
    for (std::size_t j = 0; j < out; ++j) row.floorDb[j] /= static_cast<float>(std::max(1, counts[j]));
    waterfallFn_(row);
  });
}

// In the FT8/FT4 and WSPR sub-bands every station starts at the slot boundary:
// the detector must not take them for one emission.
void Pipeline::planNoGroup() {
  std::vector<std::pair<double, double>> r;
  for (const auto& b : host_->ftxBands())
    if (b.wspr) r.emplace_back(b.dialHz + 1350.0, b.dialHz + 1650.0);
    else r.emplace_back(b.dialHz - 50.0, b.dialHz + 3050.0);
  det_->setNoGroupRanges(std::move(r));
}

void Pipeline::anchor(double utc) {
  const uint64_t s = ch_->samplesIn();
  // A block that continues the previous one adds nothing.
  if (!anchors_.empty()) {
    const Anchor& a = anchors_.back();
    const double expect = a.utc + static_cast<double>(s - a.sample) / rate_;
    if (std::abs(utc - expect) < 0.5 / rate_ + 1e-9) return;
  }
  anchors_.push_back({s, utc});
  while (anchors_.size() > 2 && anchors_[1].sample + static_cast<uint64_t>(30.0 * rate_) < s) anchors_.pop_front();
}

double Pipeline::utcOf(uint64_t sample) const {
  if (anchors_.empty()) return static_cast<double>(sample) / rate_;
  // The newest anchor at or before the sample (lookups are for recent samples).
  for (auto it = anchors_.rbegin(); it != anchors_.rend(); ++it)
    if (it->sample <= sample) return it->utc + static_cast<double>(sample - it->sample) / rate_;
  const Anchor& a = anchors_.front();
  return a.utc - static_cast<double>(a.sample - sample) / rate_;
}

void Pipeline::process(const int16_t* iq, std::size_t pairs, double utc) {
  if (!ch_) return;
  anchor(utc);
  const auto t0 = std::chrono::steady_clock::now();
  ch_->process(iq, pairs);
  processSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  iqSeconds_ += static_cast<double>(pairs) / rate_;
}

void Pipeline::process(const cfloat* iq, std::size_t pairs, double utc) {
  if (!ch_) return;
  anchor(utc);
  const auto t0 = std::chrono::steady_clock::now();
  ch_->process(iq, pairs);
  processSeconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  iqSeconds_ += static_cast<double>(pairs) / rate_;
}

void Pipeline::poll() {
  if (host_) host_->poll();
}

void Pipeline::flush() {
  if (!ch_) return;
  det_->endAll(utcOf(ch_->samplesIn()));
  host_->flush();
}

const std::vector<Emission>& Pipeline::active() const { return det_ ? det_->active() : kNone; }

std::string Pipeline::label(uint64_t id) const {
  auto it = labels_.find(id);
  return it == labels_.end() ? std::string() : it->second;
}

}  // namespace hfc
