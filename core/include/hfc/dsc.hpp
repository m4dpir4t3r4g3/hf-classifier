// Digital selective calling (GMDSS DSC on MF/HF, ITU-R M.493): 100 Bd,
// 170 Hz shift FSK. Implemented from the recommendation as remembered, and
// checked only against synthetic calls built here and the recordings named
// in core/README.md; the field layout printed is deliberately plain.
//
//   symbols   10 bits: 7 information bits, least significant first, then the
//             number of B (0) elements among them in binary, most
//             significant first. 128 values, each with its check.
//   sequence  a dot pattern, then phasing: the DX positions carry 125 six
//             times, the RX positions between them 111, 110 ... 104. Every
//             message symbol is sent twice, in a DX position and again in
//             the RX position five symbols later.
//   message   format specifier (twice), then by format: address, category,
//             self-identification, messages; an end-of-sequence symbol
//             (117 acknowledge request, 122 acknowledgement, 127 other)
//             and an error-check character: the XOR of the information
//             bits of everything from one format specifier to the end of
//             sequence.
//
// A call is reported only when phasing was found, every symbol up to the
// end of sequence was received valid in one of its two copies, and the
// error-check character agrees: decoded text can be believed as it stands.
// Both polarities are tried.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

// The ten-unit symbol of a value (bit 0 sent first), and back (-1: the
// check does not agree).
uint16_t dscEncode(int value);
int dscDecode(uint16_t tenBits);

// A call in words, from its symbols (one format specifier first, the end of
// sequence last): "selective call to 002191000 from 219015591, safety, ...".
std::string dscDescribe(const std::vector<int>& symbols);

// From bits (one polarity) to calls.
class DscParser {
 public:
  void reset();
  // Returns true when a call was completed; its symbols are then in call().
  bool bit(bool one);
  const std::vector<int>& call() const { return call_; }

 private:
  std::vector<int8_t> sym_;      // per bit received: the symbol ending there (-1: invalid)
  uint16_t shift_ = 0;
  std::size_t bits_ = 0;         // received in all
  std::size_t base_ = 0;         // bit index of sym_[0]
  bool collecting_ = false;
  std::size_t start_ = 0;        // bit index at which the first format specifier ends
  std::vector<int> call_;
  int at(std::size_t bitIndex) const;
  bool tryPhasing();
  bool collect();
};

// From audio to calls: `rate` in Hz, the emission centred at `centreHz`.
class DscDecoder {
 public:
  DscDecoder(double rate, double centreHz);
  ~DscDecoder();
  void process(const float* audio, std::size_t n);
  // Calls completed since the last take(), as text.
  std::vector<std::string> take();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Bits of a whole call sequence (dot pattern, phasing, the symbols with
// their repetitions, error-check character) for tests: `symbols` as for
// dscDescribe.
std::vector<bool> dscSequence(const std::vector<int>& symbols, int dotBits = 200);

}  // namespace hfc
