// Receiver for the 8PSK burst waveforms of 3G ALE (BW0: 3G-ALE PDUs, BW1:
// traffic management PDUs) and 4G ALE (Deep WALE, Fast WALE), on the
// matched-filtered baseband (4 samples per 2400 Bd symbol).
//
// Detection: every half symbol, the last stretch of baseband is correlated
// with each known preamble in 16-symbol segments whose powers are added (so
// a carrier offset of up to about +-75 Hz costs little). A peak above the
// threshold starts a burst.
// Acquisition: the timing is refined to a quarter symbol, the carrier offset
// estimated from the phase advance between preamble segments, and the
// channel's strongest paths (up to 4, from -3 to +12 symbols) measured, for a
// RAKE receiver.
// Data: Walsh symbols (BW0, BW1, Deep WALE) are despread and correlated with
// the 16 Walsh sequences, the channel estimate following each decision; Fast
// WALE's BPSK symbols are demodulated coherently with the channel measured on
// the known probe blocks around them. Soft bits go through the deinterleaver
// and a tail-biting Viterbi decoder; the CRC decides.
#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "navtex/ale_codes.hpp"
#include "navtex/ale_frontend.hpp"
#include "navtex/ale_pdu.hpp"

namespace navtex::ale {

enum class PskWaveform : std::uint8_t { Bw0, Bw1, DeepWale, FastWale };
const char* waveformName(PskWaveform w);

struct PskBurst {
  PskWaveform waveform = PskWaveform::Bw0;
  double time = 0;        // when the PDU (or the failure) was known
  bool ok = false;        // CRC right
  // Not ok, but every received coded bit agrees with the code word decoded
  // (codeWordDisagreements() == 0): the data came through intact and only
  // its CRC fails, i.e. it is encrypted (linking protection) or was not made
  // to the standard. A burst corrupted on the way gives 6 or more.
  bool intact = false;
  bool text = false;      // 4G: a complete text message (in `description`)
  double snrDb = 0;       // in 3 kHz
  double offsetHz = 0;
  float quality = 0;      // 0..1
  double preambleScore = 0;  // normalised correlation, 0..1
  int index = 0;          // PDU index in the transmission
  Bits bits;              // 3G PDU
  WalePdu pdu{};          // 4G PDU
  std::string description;
};

class AlePskReceiver {
 public:
  class Listener {
   public:
    virtual ~Listener() = default;
    virtual void onBurst(const PskBurst& b) = 0;
  };

  AlePskReceiver();
  ~AlePskReceiver();
  void reset();
  void enable(bool g3, bool g4);
  void setListener(Listener* l) { listener_ = l; }
  // One matched-filtered baseband sample at stream time `time`.
  void process(cf y, double time);
  double lastDetection() const { return lastDetection_; }

 private:
  struct Ref {
    PskWaveform waveform;
    std::vector<cf> conj;  // conjugated reference symbols
    std::vector<cf> autocorr;  // at lags -kLags .. kLags
    int length = 0;
    double threshold = 0;
    double peak = 0;
    long long peakAt = -1;
    int since = 0;
  };
  struct Job;
  static constexpr int kLags = 16;

  cf at(long long n) const { return ring_[static_cast<std::size_t>(n) & kMask]; }
  double correlate(const Ref& r, long long end, int segment, std::vector<cf>* segs) const;
  void detect();
  void start(Ref& r);
  void runJob();

  static constexpr std::size_t kRing = 1u << 16;
  static constexpr std::size_t kMask = kRing - 1;
  std::vector<cf> ring_;
  long long n_ = -1;
  double time_ = 0.0;
  bool g3_ = true, g4_ = true;
  std::vector<Ref> refs_;
  std::unique_ptr<Job> job3g_, job4g_;
  Listener* listener_ = nullptr;
  double lastDetection_ = -1e9;
  TailBitingViterbi viterbi_;
};

}  // namespace navtex::ale
