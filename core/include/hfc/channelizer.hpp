// One FFT for everything: the wideband spectrum (waterfall and detector) and
// any number of narrow channels at exactly 12 kHz, cut out of the same FFT.
//
// Overlap-save with 50 % overlap. Every hop of L = N/2 input samples:
//
//   last N samples --FFT N--> X[k] --+--> Hann-windowed power spectrum
//                                    |    (|0.5 X[k] - 0.25 (X[k-1] + X[k+1])|^2)
//                                    `--> per channel: K bins around its
//                                         centre x filter -> IFFT K -> keep
//                                         the middle K/2 samples
//
// N is chosen so that K = N * 12000 / input rate is an integer: the channels
// come out at exactly 12000 S/s for every rate SDRconnect offers (62.5 kS/s
// to 10 MS/s). The filter is zero-phase, defined in the frequency domain
// (flat passband, raised-cosine edges); keeping the middle half of each IFFT
// output discards the circular wrap. A channel's centre is rounded to an FFT
// bin and the rest of the offset removed by an oscillator at 12 kHz, so the
// channel is centred to within a millihertz.
//
// Cost per hop: one N-point FFT plus one K-point IFFT per channel. N is about
// input rate / 15.6 Hz up to 2.5 MS/s and input rate / 31 Hz above.
//
// History (setHistorySeconds): the spectra of the last hops are kept, so
// that a channel added now can be replayed from before it existed (replay):
// a burst whose start passed before it was detected is not lost.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "hfc/dsp.hpp"

namespace hfc {

class Channelizer {
 public:
  static constexpr double kChannelRate = 12000.0;

  // Spectrum of one hop: `power` holds N bins in ascending frequency, from
  // -rate/2 to +rate/2 (bin N/2 is the centre frequency). `centreSample` is
  // the input sample index at the middle of the FFT block.
  using SpectrumFn = std::function<void(const float* power, std::size_t n, uint64_t centreSample)>;
  // Channel output: complex baseband at kChannelRate, centred on the
  // channel's frequency. `firstSample` is the input sample index that the
  // first output sample corresponds to.
  using ChannelFn = std::function<void(int id, const cfloat* samples, std::size_t n, uint64_t firstSample)>;

  explicit Channelizer(double inputRate);
  ~Channelizer();
  Channelizer(const Channelizer&) = delete;
  Channelizer& operator=(const Channelizer&) = delete;

  double inputRate() const { return rate_; }
  double channelRate() const { return outRate_; }  // 12000 unless the input rate does not allow it
  std::size_t fftSize() const { return n_; }
  std::size_t hop() const { return n_ / 2; }
  double binHz() const { return rate_ / static_cast<double>(n_); }
  // Input samples between a sample entering and its channel output leaving.
  std::size_t delay() const { return n_ / 4; }

  void setSpectrumCallback(SpectrumFn fn) { spectrumFn_ = std::move(fn); }
  void setChannelCallback(ChannelFn fn) { channelFn_ = std::move(fn); }
  // After every block, once all channels have had their output (the
  // decoder host processes its sessions' output together then).
  void setBlockDoneCallback(std::function<void()> fn) { blockDoneFn_ = std::move(fn); }

  // A channel at `offsetHz` from the centre frequency, passing
  // +-bandwidthHz/2 (at most about 10 kHz). Returns its id.
  int addChannel(double offsetHz, double bandwidthHz);
  void removeChannel(int id);
  // Keeps the spectra of the hops of about the last `seconds` (at most
  // `maxBytes` of them), for replay(). 0 keeps none (the default).
  void setHistorySeconds(double seconds, std::size_t maxBytes = 64u << 20);
  double historySeconds() const;
  // Runs channel `id` over the kept hops of the last `seconds`, oldest
  // first, through the channel callback, as if it had existed then. Call it
  // right after addChannel, before the next block (from the spectrum
  // callback the current block follows: its spectrum is not kept yet).
  void replay(int id, double seconds);
  std::size_t channels() const { return channels_.size(); }

  // Interleaved int16 I/Q, as SDRconnect sends it.
  void process(const int16_t* iq, std::size_t pairs);
  void process(const cfloat* iq, std::size_t pairs);
  uint64_t samplesIn() const { return in_; }

 private:
  struct Channel {
    long bin;                    // centre bin, signed
    std::vector<float> filter;   // K weights, index 0 = -K/2
    Nco fine;                    // removes offset - bin * binHz
    std::vector<cfloat> spec, time;
    uint64_t blocks = 0;
  };
  void runBlock();
  // The channel's output for one block's spectrum.
  void extract(int id, Channel& c, const cfloat* spectrum, uint64_t block, uint64_t blockStart, bool full);
  struct Kept {
    std::vector<cfloat> spectrum;
    uint64_t block = 0, blockStart = 0;
    bool full = false;
  };
  std::vector<Kept> history_;  // ring
  std::size_t historyNext_ = 0, historyCount_ = 0;

  double rate_, outRate_;
  std::size_t n_ = 0, k_ = 0;
  std::unique_ptr<Fft> fft_, ifft_;
  std::vector<cfloat> buf_;   // last N input samples (ring, linearised per block)
  std::size_t fill_ = 0;      // samples in buf_ since the last block
  std::vector<cfloat> spectrum_;
  std::vector<float> power_;
  uint64_t in_ = 0, blocks_ = 0;
  std::map<int, Channel> channels_;
  int nextId_ = 1;
  SpectrumFn spectrumFn_;
  ChannelFn channelFn_;
  std::function<void()> blockDoneFn_;
};

}  // namespace hfc
