// IQ in, emissions and decodes out: the Channelizer, Detector and
// DecoderHost wired together, with the bookkeeping between them (sample
// numbering to UTC, retunes, a new sample rate) and a reduced waterfall for
// displays.
//
//   IQ blocks (index, UTC) -> Channelizer -> spectra -> Detector -> emissions
//                                        `-> channels -> DecoderHost -> decodes
//
// Single-threaded except for FT8/FT4 decoding (DecoderHost); every callback
// comes from process(), poll() or flush().
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "hfc/channelizer.hpp"
#include "hfc/decoder_host.hpp"
#include "hfc/detector.hpp"

namespace hfc {

struct WaterfallRow {
  double utc = 0;
  double lowHz = 0, highHz = 0;  // RF of the first bin's lower edge and the last bin's upper edge
  std::vector<float> db;         // power in dB, ascending frequency, max over the bins merged
  std::vector<float> floorDb;    // the detector's noise floor, same bins
};

class Pipeline {
 public:
  struct Config {
    Detector::Config detector;
    DecoderHost::Config decoders;
    std::size_t waterfallBins = 1024;
  };
  enum class EmissionEvent { Started, Updated, Ended };
  using EmissionFn = std::function<void(EmissionEvent, const Emission&)>;
  using DecodeFn = std::function<void(const DecodeEvent&)>;
  using WaterfallFn = std::function<void(const WaterfallRow&)>;
  using FeaturesFn = DecoderHost::FeaturesFn;

  explicit Pipeline(const Config& config);
  Pipeline();
  ~Pipeline();

  void setEmissionCallback(EmissionFn fn) { emissionFn_ = std::move(fn); }
  void setDecodeCallback(DecodeFn fn) { decodeFn_ = std::move(fn); }
  void setWaterfallCallback(WaterfallFn fn) { waterfallFn_ = std::move(fn); }
  // An emission's measured features (DecoderHost), once per emission.
  void setFeaturesCallback(FeaturesFn fn) { featuresFn_ = std::move(fn); }

  // Sample rate and centre frequency of the IQ that follows. A new rate
  // rebuilds everything (ending all emissions); a new centre ends all
  // emissions and re-plans the FT8/FT4 sub-bands. Emission ids are unique
  // over the pipeline's life. False for a rate outside 24 kS/s .. 100 MS/s:
  // the IQ is then ignored until the next configure().
  // The decoders' policy (DecoderHost::setPolicy) and leaving an emission
  // (DecoderHost::ignore), from the thread that calls process().
  void setDecoderPolicy(const DecoderHost::Config& policy);
  void ignore(uint64_t emissionId);
  // Where the processing time went since the start: the decoder host's
  // account, plus "spectrum and detector" (the FFT, the detector and
  // cutting out channels), and the seconds of IQ processed.
  DecoderHost::Usage usage(double* iqSeconds = nullptr) const;
  // Replaces the known frequencies (Config::decoders.frequencies), from the
  // thread that calls process(). Emissions already running keep what they had.
  void setFrequencies(std::shared_ptr<const FrequencyList> list);
  bool configure(double sampleRate, double centreHz, double utc);
  // `pairs` interleaved int16 I/Q pairs whose first sample was received at
  // `utc` (IqClock). Gaps need nothing special: UTC simply jumps.
  void process(const int16_t* iq, std::size_t pairs, double utc);
  void process(const cfloat* iq, std::size_t pairs, double utc);
  // Delivers FT8/FT4 results; call regularly (e.g. after each block).
  void poll();
  // End of input: ends all emissions, flushes the decoders.
  void flush();

  double sampleRate() const { return rate_; }
  double centre() const { return centre_; }
  const std::vector<Emission>& active() const;
  // The decoder that identified an emission ("" if none yet).
  std::string label(uint64_t emissionId) const;
  const Channelizer* channelizer() const { return ch_.get(); }

 private:
  void build();
  void planNoGroup();
  double utcOf(uint64_t sample) const;
  void anchor(double utc);

  Config cfg_;
  double rate_ = 0, centre_ = 0;
  uint64_t nextId_ = 1;
  std::unique_ptr<Channelizer> ch_;
  std::unique_ptr<Detector> det_;
  std::unique_ptr<DecoderHost> host_;
  double processSeconds_ = 0, iqSeconds_ = 0;
  struct Anchor {
    uint64_t sample;
    double utc;
  };
  std::deque<Anchor> anchors_;
  std::map<uint64_t, std::string> labels_;
  std::set<uint64_t> live_;  // emissions started and not ended
  EmissionFn emissionFn_;
  DecodeFn decodeFn_;
  WaterfallFn waterfallFn_;
  FeaturesFn featuresFn_;
  static const std::vector<Emission> kNone;
};

}  // namespace hfc
