// Raw FSK bits: for any FSK signal at a known baud rate and shift, the bits
// on the recovered clock, as '0' and '1' (the higher tone is 1, or the lower
// one with reversed polarity), in groups of 8 and lines of 64. Bits are only
// output while the tone pair is there (AFC locked, or the signal at the fixed
// centre), and once it has been there for half a second (the bits meanwhile
// are held back, not lost), so noise prints nothing. There is no framing: finding characters in
// the bits is up to the reader.
#pragma once

#include <cstddef>
#include <cstdint>

#include "navtex/fsk_receiver.hpp"
#include "navtex/types.hpp"

namespace navtex {

class BitsDecoder : private FskReceiver::Client {
 public:
  using Config = FskConfig;

  explicit BitsDecoder(const FskConfig& config = FskConfig());

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
  void onBit(float soft, double time) override;
  bool synced() const override { return rx_.signalPresent(); }
  void put(char ch, float confidence, double time);

  FskConfig cfg_;
  DecoderSink* sink_ = nullptr;
  FskReceiver rx_;
  TransmissionTracker tx_;
  std::uint32_t bitsInLine_ = 0;
  double lastPresent_ = 0.0;
  double presentSince_ = -1.0;  // start of the current stretch with the tone pair
  struct HeldBit {
    float soft;
    double time;
  };
  static constexpr int kHeld = 512;
  HeldBit held_[kHeld];
  int heldCount_ = 0;
  void output(float soft, double time);
};

}  // namespace navtex
