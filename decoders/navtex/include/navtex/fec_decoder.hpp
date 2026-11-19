// SITOR-B / NAVTEX forward error correction (ITU-R M.476 / M.625 mode B).
//
// The transmitter sends every character twice. The stream alternates DX and
// RX positions of 7 bits each; a character first sent in a DX position is
// repeated in the RX position five slots (35 bits, 350 ms) later:
//
//   slot:  0    1    2    3    4    5    6    7   ...
//          DX0  RX   DX1  RX   DX2  RX0  DX3  RX1 ...
//
// Before the text, the transmitter sends phasing signals: phasing signal 2
// ("rep") in the DX positions and phasing signal 1 (alpha) in the RX
// positions.
//
// Synchronisation: after every bit, the last 16 slots ending at that bit are
// checked for both tone polarities. The alignment is accepted when at least
// 10 of them are valid 4-of-7 code words and the DX/RX structure shows which
// slots are RX: characters repeated five slots apart, or the phasing
// pattern. Noise practically never shows that structure. Bits are kept for about 7 seconds, so once in sync the characters
// that arrived while synchronising are decoded too, and after a bit slip or
// a burst of noise the decoder resynchronises and fills in what it can,
// without printing anything twice.
//
// Error correction: when the two copies agree and are valid the character is
// certain. Otherwise, with soft FEC, the soft values of both copies are added
// and the closest of the 35 valid code words (or the phasing pair, whose
// copies differ by design) is chosen: maximum likelihood. If even that is
// ambiguous the character is reported as an error.
//
// Characters that FEC had to correct are held back until a confirmed one
// (both copies valid and equal, idle signals included) follows; noise
// practically never produces one. If sync is lost first, or they sit between
// idle signals, the held characters are dropped: that is what happens when a
// transmission ends, and it keeps noise from printing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "navtex/types.hpp"

namespace navtex {

struct FecCode {
  std::uint8_t code = 0;
  float confidence = 0;    // 0..1
  bool corrected = false;  // copies disagreed or were damaged
  bool error = false;      // could not be recovered; `code` is only the best guess
  double time = 0;         // stream time of the RX copy's last bit
};

class FecDecoder {
 public:
  class Output {
   public:
    virtual ~Output() = default;
    virtual void onCode(const FecCode&) = 0;
  };

  FecDecoder();
  void configure(Polarity polarity, bool softFec);
  void reset();
  void setOutput(Output* out) { out_ = out; }

  // One soft bit (> 0: higher tone) and its stream time.
  void pushBit(float soft, double time);

  bool synced() const { return synced_; }
  bool reversed() const { return pol_ < 0; }
  bool idle() const;  // synced and receiving phasing or idle signals
  std::uint64_t resyncs() const { return resyncs_; }

 private:
  struct Candidate {
    bool found = false;
    int pol = 1;
    std::uint64_t rxEnd = 0;  // last bit of an RX slot
    int valid = 0;
  };

  bool available(std::uint64_t bit) const { return bit < count_ && count_ - bit <= kHistory; }
  float soft(std::uint64_t bit) const { return hist_[bit % kHistory]; }
  unsigned code(std::uint64_t end, int pol) const;
  Candidate search() const;
  void acquire(const Candidate& c);
  void onSlot(std::uint64_t end);
  bool isRx(std::uint64_t end) const {
    auto d = static_cast<long long>(end) - static_cast<long long>(rxRef_);
    return (d / 7) % 2 == 0;
  }
  void decodeRx(std::uint64_t end, std::uint64_t firstGood);
  void deliver(const FecCode& code);
  void loseSync();

  static constexpr std::size_t kHistory = 7 * 100;  // bits kept (7 s)
  static constexpr int kWindow = 16;                // slots checked for sync
  static constexpr int kMaxPending = 48;            // doubtful characters held back

  Output* out_ = nullptr;
  Polarity polarity_ = Polarity::Auto;
  bool softFec_ = true;

  std::vector<float> hist_;
  std::vector<double> times_;
  std::uint64_t count_ = 0;

  bool synced_ = false;
  int pol_ = 1;
  std::uint64_t rxRef_ = 0;  // last bit of some RX slot; fixes alignment and parity
  std::uint32_t invalid_ = 0;       // last 16 slots: 1 = invalid code
  std::uint32_t idleSlots_ = 0;     // last 8 slots: 1 = idle or phasing signal
  std::uint64_t parityFor_ = 0, parityAgainst_ = 0;  // last 48 slots' DX/RX evidence
  bool decodedAny_ = false;
  std::uint64_t lastRx_ = 0;  // last RX slot decoded
  std::uint64_t resyncs_ = 0;
  FecCode pending_[kMaxPending];
  int pendingCount_ = 0;
  bool pendingAfterIdle_ = false;   // the held characters follow an idle signal
  bool lastConfirmedIdle_ = true;   // idle signals, not text, were the last thing confirmed or passed on
};

}  // namespace navtex
