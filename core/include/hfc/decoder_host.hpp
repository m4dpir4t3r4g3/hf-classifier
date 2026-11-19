// Decodes what can be decoded: every emission the detector reports gets its
// own channel out of the channelizer, turned into the audio of a USB
// receiver, and fed to the decoders that fit its bandwidth. A decoder's text
// is only published once it passes that decoder's quality gate; the first
// decoder to pass wins the emission and the other candidates are dropped.
//
// The gates: SITOR-B/NAVTEX and ALE have their own error detection (FEC,
// Golay, CRC). CW, PSK and RTTY have none, and fed a signal of another mode
// (random-data FSK, one tone of an MFSK signal, BPSK without Varicode) they
// print letters that pass any per-character statistic; their text must also
// read like what people send (callsigns, radio and common words, signal
// reports, or five-character code groups), and PSK must have a high decoder
// quality. Measured on synthetic signals of other modes: no false decodes.
//
//   Emission (Detector) -> session: Channelizer channel centred on the
//     emission, 12 kHz complex -> USB audio with the emission centred at
//     `audioCentre` -> candidate decoders -> quality gate -> DecodeEvent
//
// Candidates by occupied bandwidth B (the band holding 99 % of the power;
// measured, so generous ranges):
//
//   B <= 700 Hz        CW (morse; a strong hard-keyed tone measures wide)
//   B <= 250 Hz        PSK31, PSK63, PSK125 from 100 Hz; QPSK at the same rates
//   250 .. 800 Hz      PSK250, QPSK250
//   500 .. 1250 Hz     PSK500, QPSK500 (below ALE: a session is ALE or not)
//   200 .. 700 Hz      SITOR-B / NAVTEX (100 Bd, 170 Hz), RTTY 45.45 Bd / 170 Hz
//   350 .. 900 Hz      RTTY 50 Bd / 450 Hz
//   1300 .. 3600 Hz    ALE 2G (8FSK) and 3G/4G (8PSK bursts)
//
// After 3 s of audio, decoders for what the feature meter measured join:
// RTTY at a measured 2-FSK shift and rate, BPSK and QPSK at a measured PSK
// rate of 31.25 to 500 Bd (addMeasuredCandidates).
//
// FT8 and FT4 are decoded separately, per sub-band rather than per emission:
// a 3 kHz channel at each dial frequency of ft8DialFrequenciesKhz() /
// ft4DialFrequenciesKhz() inside the band, cut into UTC slots (15 s / 7.5 s)
// and decoded in a worker thread (at most 50 messages per slot). Emissions
// inside these sub-bands get no session of their own. Only when the channels
// are exactly 12 kHz (every SDRconnect rate gives that).
//
// Threads: channelSamples() is the Channelizer's channel callback. It only
// keeps a session's (and a listed HFDL channel's) samples; at the end of
// each Channelizer block the sessions process theirs in parallel, on a
// pool of Config::threads, each touching only its own state, and their
// decodes and features are handed on afterwards, in channel order, in the
// caller's thread (so the results do not depend on the threads). The
// emission calls may come from the Channelizer's spectrum callback (adding
// and removing channels is safe there, but not from the channel callback).
// FT8/FT4 are decoded in a worker thread of their own; their results are
// delivered by poll() (call it regularly) and flush().
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "hfc/channelizer.hpp"
#include "hfc/detector.hpp"
#include "hfc/features.hpp"
#include "hfc/fax.hpp"
#include "hfc/frequency_list.hpp"
#include "hfc/dsp.hpp"
#include "hfc/worker_pool.hpp"

namespace hfc {

// Does decoded text read like something people send (callsigns, radio
// words, signal reports; or groups of five)? The gate of the decoders
// without error detection of their own (CW, PSK, RTTY). Exposed for tests.
bool looksLikeTraffic(const std::string& text);
// Morse that may pass without known words when its emission is measured as
// a keyed tone (OOK): at least 6 words, not mostly noise-like ones.
bool looksLikeKeyedText(const std::string& text);

struct DecodeEvent {
  uint64_t emissionId = 0;  // 0: not tied to a detected emission (FT8 below the detection threshold)
  double utc = 0;           // when the text was received (FT8: start of the transmission)
  std::string decoder;      // "CW", "PSK31", "NAVTEX", "SITOR-B", "RTTY 45.45/170", "ALE 2G", "FT8", ...
  std::string text;
  double freqHz = 0;        // RF: the emission's centre (FT8/FT4: of the signal)
  double quality = 0;       // 0..1, the decoder's own measure
  bool first = false;       // the first text of this decoder on this emission: the identification
  // A received picture (radiofax) that `text`, which holds "[picture]",
  // is about; else null.
  std::shared_ptr<const FaxImage> image;
};

// A frequency window the user set (on the waterfall, or a marker's "decode
// only with"): an emission centred in it gets only `decoder` (a name
// DecoderHost::chosenDecoders() lists, or "RTTY <baud>/<shift>"), or with
// "none" no session at all (no decoding, no classification). Where windows
// overlap, the narrowest decides.
struct DecodeRule {
  double loHz = 0, hiHz = 0;  // RF
  std::string decoder;
};

class DecoderHost {
 public:
  struct Config {
    bool enable = true;       // per-emission decoding
    bool ftx = true;          // FT8 / FT4 sub-bands
    int maxSessions = 48;     // emissions decoded at once; more are left undecoded
    // What the processing is spent on (changeable while running: setPolicy).
    bool measureUnknown = true;   // measure and classify emissions no decoder fits
    double giveUpSeconds = 120;   // an emission no decoder has read in this long is left (0: never)
    double minSnrDb = 0;          // emissions weaker than this get no session
    int threads = 0;              // threads for the sessions, the caller's included (0: as many as useful)
    // Decoder families switched off: "CW", "PSK", "RTTY", "SITOR-B", "DSC",
    // "ALE", "Akula", "4:3 code", "FAX", "SSTV", "TIME", "DRM" (per
    // emission) and "FT8", "FT4", "WSPR", "HFDL" (the sub-bands and
    // listed channels decoded all the time).
    std::set<std::string> disabled;
    double audioCentre = 1500.0;
    // Known frequencies (may be null): shown with an emission's features,
    // rank the catalogue's modes, and name decoders to try.
    std::shared_ptr<const FrequencyList> frequencies;
    std::shared_ptr<const std::vector<DecodeRule>> rules;  // the user's windows (setPolicy changes them)
  };
  using DecodeFn = std::function<void(const DecodeEvent&)>;
  // An emission's measured features (FeatureMeter): once after 8 s of
  // audio, or when it ends earlier (from 1.5 s). Before that, when the
  // emission starts on a listed frequency, once with only `listed` filled
  // in (`valid` false).
  using FeaturesFn = std::function<void(uint64_t emissionId, const EmissionFeatures&)>;
  void setFrequencies(std::shared_ptr<const FrequencyList> list) {
    cfg_.frequencies = std::move(list);
    planHfdl();
  }
  // The listed HFDL channels inside the band (their dial frequencies).
  std::vector<double> hfdlChannels() const;

  // Changes maxSessions, measureUnknown, giveUpSeconds, minSnrDb and
  // disabled of the Config (from the thread that feeds it). Sessions of
  // decoders now disabled end; sub-bands are planned again.
  void setPolicy(const Config& policy);
  // Is decoder family `family` (or the decoder named `name`, by its
  // family) switched off?
  bool disabled(const std::string& name) const;
  // Leave emission `id`: its session ends and it gets no new one.
  void ignore(uint64_t emissionId);

  // Where the processing time went, since the start (seconds of CPU time
  // of this thread and of the FT8/WSPR worker), and what each session is
  // doing now.
  struct SessionInfo {
    uint64_t emissionId = 0;
    double centreHz = 0, widthHz = 0, snrDb = 0;
    std::string state;      // "decoding", "trying", "measuring"
    std::string decoder;    // the winner (decoding), else the decoders tried, comma-separated
    double ageSeconds = 0;  // since the session started
    double cpuSeconds = 0;
  };
  struct Usage {
    std::map<std::string, double> seconds;  // by activity: "decoder CW", "measuring", "FT8/FT4", ...
    double inside = 0;                      // of this thread's, spent in channelSamples
    std::vector<SessionInfo> sessions;
    int givenUp = 0, ignored = 0, measuredOnly = 0;  // emissions left so far, by reason
    // Emissions still known (on the air, or ended within a minute) that were
    // left, with why: 'g' given up, 'i' stopped by the user, 'm' measured,
    // no decoder for them.
    std::vector<std::pair<uint64_t, char>> leftRecent;
  };
  Usage usage() const;
  using UtcFn = std::function<double(uint64_t inputSample)>;

  DecoderHost(Channelizer& channelizer, const Config& config);
  ~DecoderHost();
  DecoderHost(const DecoderHost&) = delete;
  DecoderHost& operator=(const DecoderHost&) = delete;

  void setDecodeCallback(DecodeFn fn) { decodeFn_ = std::move(fn); }
  void setFeaturesCallback(FeaturesFn fn) { featuresFn_ = std::move(fn); }
  // Maps an input sample index (Channelizer numbering) to UTC.
  void setUtcFunction(UtcFn fn) { utcFn_ = std::move(fn); }
  // Receiver centre frequency (RF Hz). A change ends all sessions (flushing
  // their decoders) and re-plans the FT8/FT4 sub-bands.
  void setCentre(double hz);

  void emissionStarted(const Emission& e);
  void emissionUpdated(const Emission& e);
  void emissionEnded(const Emission& e);
  void channelSamples(int channelId, const cfloat* samples, std::size_t n, uint64_t firstSample);

  // Delivers finished FT8/FT4 slots.
  void poll();
  // End of input: flushes every decoder, decodes the last FT8/FT4 slots if
  // long enough, waits for the worker and delivers everything.
  void flush();

  std::size_t sessions() const { return sessions_.size(); }
  // RF ranges of the FT8/FT4 sub-bands being decoded.
  struct Band {
    double dialHz;
    bool ft4;
    bool wspr;
  };
  std::vector<Band> ftxBands() const;
  // Decoder names a session would try for an emission of this bandwidth.
  static std::vector<std::string> candidatesFor(double bandwidthHz);
  // The decoders a window or marker can be given ("RTTY <baud>/<shift>" is
  // accepted too, at 30-300 Bd and 50-1100 Hz), and whether `name` is one.
  static std::vector<std::string> chosenDecoders();
  static bool choosable(const std::string& name);

 private:
  struct Session;
  struct FtxBand;
  struct HfdlBand;
  class FtxWorker;

  bool inFtxBand(double loHz, double hiHz) const;
  void startSession(const Emission& e);
  // Ends a session, publishing what its decoder still holds.
  void endSession(std::map<int, std::unique_ptr<Session>>::iterator it);
  // Ends it silently (its emission turned out to be part of another).
  void dropSession(std::map<int, std::unique_ptr<Session>>::iterator it);
  void planFtx();
  void planHfdl();
  bool inHfdlBand(double loHz, double hiHz) const;
  void hfdlSamples(HfdlBand& b, const cfloat* samples, std::size_t n, uint64_t firstSample);
  void ftxSamples(FtxBand& b, const cfloat* samples, std::size_t n, uint64_t firstSample);
  void submitSlot(FtxBand& b);
  void emit(const DecodeEvent& e);
  // The sessions' and HFDL channels' kept samples, processed in parallel.
  void runPending();
  void sessionSamples(Session& s, const cfloat* samples, std::size_t n, uint64_t firstSample);
  // The time accounting of the job running in this thread, or the host's.
  std::map<std::string, double>& busyMap();
  double& insideTime();
  // Measures the session's features and reports them (once).
  void reportFeatures(Session& s);
  // At 3 s: decoders for the parameters measured (RTTY at the measured
  // shift and rate, BPSK and QPSK at the measured rate), fed the audio so far.
  void addMeasuredCandidates(Session& s);
  void markKeyed(Session& s);
  double utcOf(uint64_t sample) const;

  Channelizer& ch_;
  Config cfg_;
  double rate_;  // the channels' sample rate: 12 kHz for every SDRconnect rate
  DecodeFn decodeFn_;
  FeaturesFn featuresFn_;
  UtcFn utcFn_;
  double centre_ = 0;
  bool centreSet_ = false;
  std::map<int, std::unique_ptr<Session>> sessions_;  // by channel id
  std::map<uint64_t, int> byEmission_;                // emission id -> channel id
  std::map<uint64_t, Emission> active_;               // for tying FT8 decodes to emissions
  std::map<int, std::unique_ptr<FtxBand>> ftx_;       // by channel id
  std::map<int, std::unique_ptr<HfdlBand>> hfdl_;     // by channel id
  std::unique_ptr<FtxWorker> worker_;
  // Time accounting (this thread) and emissions left, by reason.
  struct Chunk {
    std::vector<cfloat> samples;
    uint64_t firstSample = 0;
  };
  std::map<int, std::vector<Chunk>> pending_;  // by channel id, until the block ends
  std::unique_ptr<WorkerPool> pool_;
  std::map<std::string, double> busy_;
  double inside_ = 0;
  double lastEnforce_ = -1;  // stream time of the last enforce()
  std::map<uint64_t, char> left_;  // 'g' given up, 'i' ignored, 'm' measured, no decoder for it
  // Ends sessions that the policy no longer wants (given up, disabled).
  void enforce();
  // Removes the session's decoders of families switched off.
  void prune(Session& s);
  // A session to make room for one that can be decoded: one measuring an
  // emission no decoder fits, if there is one.
  bool evictForDecodable();
  bool evictUndecided();
  bool evictAnyUnchosen();  // for an emission in a window the user gave a decoder
  const DecodeRule* ruleFor(double hz) const;
  bool excluded(double loHz, double hiHz) const;  // in a "none" window
};

}  // namespace hfc
