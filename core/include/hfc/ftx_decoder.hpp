// FT8 / FT4 decoding of one time slot, with ft8_lib (third_party/ft8_lib,
// MIT): 12 kHz audio of a USB receiver at the band's dial frequency in,
// messages out. The decoding loop and the callsign hash table follow
// ft8_lib's demo/decode_ft8.c. Each message has passed the LDPC decoder and
// the 14-bit CRC, so a decode is ground truth that the signal was FT8/FT4.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

struct FtxMessage {
  std::string text;     // e.g. "CQ K1ABC FN42"
  double audioHz = 0;   // frequency of tone 0 above the dial
  double dtSeconds = 0; // start relative to the slot (nominally 0.5 s for FT8)
  int score = 0;        // sync score (ft8_lib's candidate score)
};

class FtxDecoder {
 public:
  explicit FtxDecoder(bool ft4);
  ~FtxDecoder();
  FtxDecoder(const FtxDecoder&) = delete;
  FtxDecoder& operator=(const FtxDecoder&) = delete;

  bool ft4() const { return ft4_; }
  double slotSeconds() const { return ft4_ ? 7.5 : 15.0; }
  // Decodes one slot of 12 kHz audio that starts at the slot boundary.
  std::vector<FtxMessage> decodeSlot(const float* audio, std::size_t n);

 private:
  struct Hashes;
  bool ft4_;
  std::unique_ptr<Hashes> hashes_;
};

// WSJT-X's default dial frequencies on HF (kHz). Not verified against the
// WSJT-X source; listed in catalogue/modes.json with that caveat.
const std::vector<double>& ft8DialFrequenciesKhz();
const std::vector<double>& ft4DialFrequenciesKhz();

}  // namespace hfc
