// The coding layers of Automatic Link Establishment, as specified in
// MIL-STD-188-141B appendix A (2G ALE), appendix C (3G ALE) and
// MIL-STD-188-141D appendix G (4G ALE, WALE): FEC codes, interleavers, CRCs,
// Walsh modulation, scramblers and the known symbol sequences. Shared by the
// ALE receiver (ale2g.hpp, ale_psk.hpp) and the test signal synthesiser
// (ale_synth.hpp), and unit tested against the examples in the standards.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace navtex::ale {

// ------------------------------------------------------------- 2G ALE

// Word types: the three preamble bits W1..W3 of a 24-bit ALE word (table A-VIII).
enum Preamble : std::uint8_t { kData = 0, kThru = 1, kTo = 2, kTwas = 3, kFrom = 4, kTis = 5, kCmd = 6, kRep = 7 };
const char* preambleName(int preamble);

// Extended Golay (24,12) code of figure A-6. A code word has the 12 data bits
// in bits 23..12 (W1 = bit 23) and the 12 check bits in bits 11..0.
std::uint32_t golayEncode(std::uint32_t data12);
// Corrects up to three errors: the data bits, or -1 when four or more bits
// are wrong (detected). `errors` is set to the number of bits corrected.
int golayDecode(std::uint32_t received24, int& errors);

// ALE word W1 (bit 23) .. W24 (bit 0): preamble, then three 7-bit characters.
inline std::uint32_t makeWord(int preamble, char c1, char c2, char c3) {
  return (static_cast<std::uint32_t>(preamble & 7) << 21) | (static_cast<std::uint32_t>(c1 & 0x7f) << 14) |
         (static_cast<std::uint32_t>(c2 & 0x7f) << 7) | static_cast<std::uint32_t>(c3 & 0x7f);
}
inline int wordPreamble(std::uint32_t w) { return static_cast<int>((w >> 21) & 7); }
inline char wordChar(std::uint32_t w, int i) { return static_cast<char>((w >> (14 - 7 * i)) & 0x7f); }

// The 49 bits of a transmitted word (figure A-10): A1 B1 A2 B2 ... A24 B24 S49,
// A and B the Golay code words of W1..W12 and W13..W24, the check bits of B
// inverted, S49 = 0.
void interleaveWord(std::uint32_t word, std::uint8_t bits[49]);
// The reverse, from 48 received bits (S49 dropped): the A and B code words,
// with B's check bits put back.
void deinterleaveWord(const std::uint8_t bits[48], std::uint32_t& a, std::uint32_t& b);

// 8-ary FSK tones: tone k is 750 + 250 k Hz and carries the tribit of table
// A.5.1.2 (Gray coded: 000, 001, 011, 010, 110, 111, 101, 100).
int tribitOfTone(int tone);
int toneOfTribit(int tribit);
constexpr double kToneHz0 = 750.0, kToneStepHz = 250.0, kBaud2g = 125.0;
// A word sent three times (the redundant word, 392 ms): 147 bits, 49 tones.
void redundantWordTones(std::uint32_t word, std::uint8_t tones[49]);

// Character sets (figures A-13 and A-46).
bool isBasic38(char c);     // A-Z 0-9 @ ?
bool isExpanded64(char c);  // 0x20 .. 0x5f

// ------------------------------------------------------------- CRCs

// Appendix C (C.4.12): a K-bit CRC over bits u[0..n) (first bit first),
// register initialised to zero; `poly` holds the coefficients g(K-1)..g0 as
// bits K-1..0. The CRC is sent MSB (C(K-1)) first.
std::uint32_t crcAppendixC(const std::uint8_t* bits, int n, std::uint32_t poly, int k);
// 4G (G.5.2.2): CRC-16 x^16+x^15+x^12+x^11+x^8+x^6+x^3+1 as computed by the
// STANAG 5066 routine (reflected, all ones in, inverted out). The PDU carries
// it low octet first.
std::uint16_t crc16Wale(const std::uint8_t* octets, std::size_t n);

// ------------------------------------------------------------- convolutional codes

// A tail-biting convolutional code: rate 1/n (n <= 3), constraint length k
// (k <= 9). taps[j] has bit d set when output j includes the input from d
// steps ago (bit 0: the newest input).
struct ConvCode {
  int k;
  int n;
  unsigned taps[3];
};
// The codes, from the polynomials in the standards (x^(k-1) is the newest bit).
extern const ConvCode kBw0Code;   // 3G BW0: k = 7, rate 1/2
extern const ConvCode kBw1Code;   // 3G BW1: k = 9, rate 1/3
extern const ConvCode kWaleCode;  // 4G WALE: k = 9, rate 1/2

// Tail-biting encoding: the register starts holding the last k-1 input bits.
std::vector<std::uint8_t> convEncode(const ConvCode& code, const std::vector<std::uint8_t>& bits);
// Received coded bits (soft values, > 0 for a one) that disagree with the
// tail-biting code word of `decoded`: 0 when what was received is exactly a
// code word. A burst corrupted on the way rarely lands on another code word
// (that takes an error pattern as heavy as the code's free distance), so 0
// with a failed CRC means the data came through intact and only the CRC
// fails: encrypted (linking protection), or not made to the standard.
int codeWordDisagreements(const ConvCode& code, const std::vector<std::uint8_t>& decoded, const std::vector<float>& soft);

// Maximum-likelihood decoder for a tail-biting code (wrap-around Viterbi).
// `soft` holds n values per input bit (> 0: a 1 is more likely, magnitude:
// confidence). Returns the decoded bits; `metric` is the correlation of the
// re-encoded bits with the soft values, divided by their total magnitude
// (1: every soft value agrees).
class TailBitingViterbi {
 public:
  std::vector<std::uint8_t> decode(const ConvCode& code, const std::vector<float>& soft, double* metric = nullptr);

 private:
  std::vector<float> pm_, next_;
  std::vector<std::uint8_t> decisions_;
};

// ------------------------------------------------------------- 3G burst waveforms

// The block interleaver of C.5.1.2: order[f] is the index of the input bit
// fetched f-th.
std::vector<int> blockInterleaverOrder(int rows, int cols, int irs, int ics, int dirs, int dics, int irf, int icf,
                                       int dirf, int dicf);
const std::vector<int>& bw0InterleaverOrder();  // 4 x 13
const std::vector<int>& bw1InterleaverOrder();  // 16 x 9

// 16-ary orthogonal Walsh modulation (tables C-VIII and G-IX): chip j of the
// sequence for the four coded bits b3b2b1b0 (b3 sent first) = `value`: 0 or 4.
inline int parity(unsigned v) {
  v ^= v >> 16;
  v ^= v >> 8;
  v ^= v >> 4;
  v ^= v >> 2;
  v ^= v >> 1;
  return static_cast<int>(v & 1);
}
inline int walshChip(int value, int j) { return parity(static_cast<unsigned>(value & j)) ? 4 : 0; }

// The known sequences of BW0 and BW1 (8PSK symbols 0..7).
extern const std::uint8_t k3gTlc[256];        // TLC/AGC guard sequence (tables C-V, C-X)
extern const std::uint8_t kBw0Preamble[384];  // table C-VI
extern const std::uint8_t kBw1Preamble[576];  // table C-XI
extern const std::uint8_t k3gPn[256];         // PN spreading (tables C-IX, C-XIII)

// ------------------------------------------------------------- 4G WALE

// Interleaver of G.5.1.2: coded bit n goes to position 25 n mod 192.
inline int waleInterleavePos(int n) { return (n * 25) % 192; }

// Preamble Walsh symbols of G.5.1.6: 32 chips (0 or 4) for a di-bit, as a
// normal or exceptional set.
int walePreambleChip(int dibit, bool exceptional, int j);
extern const std::uint8_t kWalePreambleScramble[32];  // nScramble
extern const std::uint8_t kDeepPreambleDibits[14];
extern const std::uint8_t kFastPreambleDibits[5];
extern const std::uint8_t kFastProbe[16];            // known symbols, sent twice

// Deep WALE data scrambler (159-bit shift register, G.5.1.7.2).
extern const std::uint8_t kDeepScramblerInit[159];
class DeepScrambler {
 public:
  DeepScrambler() { reset(); }
  void reset();
  int next();  // the next scrambling symbol 0..7

 private:
  std::uint8_t reg_[159];
};

// Fast WALE data scrambler (x^9 + x^4 + 1, figure G-10).
class FastScrambler {
 public:
  FastScrambler() { reset(); }
  void reset() { state_ = 1; }
  int next();  // the next scrambling symbol 0..7

 private:
  unsigned state_ = 1;
};

// ------------------------------------------------------------- bit helpers

std::vector<std::uint8_t> bitsMsbFirst(std::uint64_t value, int count);
std::uint64_t valueMsbFirst(const std::uint8_t* bits, int count);

}  // namespace navtex::ale
