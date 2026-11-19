// RTTY decoder: ITA2 characters in start/stop framing, at any baud rate
// and shift (45.45 baud / 170 Hz amateur, 50 baud / 450 Hz weather
// broadcasts, 75 baud / 850 Hz, ...).
//
//   audio -> FskReceiver (AFC, demodulator) -> soft decision at ~2 kHz
//         -> Framer, one per polarity: a mark-to-space transition starts a
//            character; its start bit, five data bits and stop bit are each
//            sampled at the end of their bit, where the demodulator's matched
//            filter covers exactly that bit. The stop element may be 1, 1.5
//            or 2 bits: the next start is looked for after the first.
//         -> sync: six well-framed characters in a row (start bit space,
//            stop bit mark, every bit clear). Characters are held back until
//            then, so noise prints nothing; then the held ones are printed.
//            In sync, a character that is not clear is held back until a
//            clear one follows. Sync ends after four framing errors among
//            eight characters, or a pause of two characters' time (steady
//            mark, or the signal gone); what is held then is dropped, which
//            keeps the noise after a transmission from printing.
//         -> ITA2 letters/figures -> DecoderSink
//
// RTTY has no error correction: a character whose start or stop bit is
// wrong (a framing error) is still printed, marked unrecoverable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "navtex/fsk_receiver.hpp"
#include "navtex/types.hpp"

namespace navtex {

class RttyDecoder : private FskReceiver::Client {
 public:
  using Config = FskConfig;

  explicit RttyDecoder(const FskConfig& config = rttyDefaults());
  static FskConfig rttyDefaults();

  bool configure(const FskConfig& config);
  const FskConfig& config() const { return cfg_; }
  void reset();
  void setSink(DecoderSink* sink) {
    sink_ = sink;
    rx_.setSink(sink);
  }
  void process(const float* samples, std::size_t frames) { rx_.process(samples, frames, *this); }
  void flush();

  SitorBStatus status() const;
  double streamTime() const { return rx_.time(); }
  double measuredShiftHz() const { return rx_.measuredShiftHz(); }

 private:
  // One character as framed: its five data bits and how clear it was.
  struct Frame {
    std::uint8_t code = 0;
    bool framed = false;  // start bit space and stop bit mark
    bool clear = false;   // ... and every bit clearly one tone
    float weakest = 0;    // smallest |soft value| of the seven
    double time = 0;      // stream time of the stop bit
  };

  // Frames characters for one polarity.
  class Framer {
   public:
    static constexpr int kHeld = 24;
    void setPolarity(int pol) { pol_ = pol; }
    void reset(double samplesPerBit);
    // One soft decision; returns true when a character was framed (in `out`).
    bool sample(double disc, double time, Frame& out);

    int pol_ = 1;
    // Sync state, driven by the decoder.
    bool synced = false;
    int clearRun = 0;           // consecutive clear characters
    std::uint8_t recent = 0;    // last 8 characters: 1 = framing error
    Frame held[kHeld];          // characters held back: before sync, or unclear ones in sync
    int heldCount = 0;
    double lastFrame = 0.0;     // stream time of the last character framed

   private:
    // A possible character from one mark-to-space edge. Every edge is tried,
    // not just the first after the last character, so that one bad start
    // bit costs one character rather than misframing the next several.
    struct Candidate {
      double start = 0.0;       // sample position where the start bit begins
      int next = 0;             // next element to sample (0 start .. 6 stop)
      float soft[7] = {};
    };
    static constexpr int kCandidates = 16;

    void complete(const Candidate& c, double time);

    double spb_ = 44.0;         // samples per bit
    std::uint64_t n_ = 0;       // samples seen
    double prev_ = 0.0;
    Candidate cand_[kCandidates];
    int candCount_ = 0;
    double consumedUntil_ = 0.0;  // candidates starting before this overlap the last character
    double lastStart_ = -1e18;    // start of the last character
    bool lastClear_ = false;
    double period_ = 0.0;         // learned start-to-start spacing of back-to-back characters (0: none yet)
    bool predicted_ = false;      // the candidate where the next character is due has been made
    bool haveBest_ = false;       // the best of the candidates competing for the next character
    Frame best_;
    double bestStart_ = 0.0, bestScore_ = 0.0;
  };

  // FskReceiver::Client
  void onSample(double disc, double time) override;
  bool synced() const override { return active_ >= 0; }

  void onFrame(int which, const Frame& f);
  void output(const Frame& f, bool reversed);
  void loseSync(int which);

  FskConfig cfg_;
  DecoderSink* sink_ = nullptr;
  FskReceiver rx_;
  TransmissionTracker tx_;
  Framer framers_[2];
  int active_ = -1;  // the synced framer, if any
  bool figures_ = false;
  double presentSince_ = -1.0;  // since when AFC has had the tone pair
};

}  // namespace navtex
