// ALE test-signal synthesiser: 2G ALE words (8-ary FSK, MIL-STD-188-141B
// appendix A), 3G burst waveforms BW0 and BW1 (appendix C) and the Deep and
// Fast WALE waveforms (MIL-STD-188-141D appendix G), through a channel with
// noise, a carrier offset, a clock error, a second path and fading. Used by
// the tests and ale_gen.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navtex/ale_pdu.hpp"

namespace navtex::ale {

struct AleChannel {
  double sampleRate = 48000.0;
  double amplitude = 0.3;     // the signal has the power of a sine of this amplitude
  double snrDb = 100.0;       // signal over noise in 3 kHz
  std::uint32_t seed = 1;
  double leadSeconds = 0.5;   // noise before the signal
  double tailSeconds = 0.5;   // and after
  double offsetHz = 0.0;      // carrier (mistuning) offset
  double clockPpm = 0.0;      // transmitter symbol clock error
  // A second path: delay and level relative to the first. With fadingHz > 0
  // both paths fade (Rayleigh, Doppler spread of about fadingHz).
  double pathDelayMs = 0.0;
  double pathDb = -100.0;
  double fadingHz = 0.0;
};

// A transmission: pieces sent one after another, with gaps.
struct AleSignal {
  std::vector<float> samples;
  double start = 0, end = 0;  // of the signal proper
};

// 2G: each word sent redundantly (three times, 392 ms), back to back.
AleSignal synth2g(const std::vector<std::uint32_t>& words, const AleChannel& ch);

// Words of a 2G call: a scanning call (the first TO word `scanWords` times),
// the leading call (TO address), an optional AMD message, and the conclusion
// (TIS, or TWAS when `twas`). Addresses up to 15 characters.
std::vector<std::uint32_t> ale2gCall(const std::string& to, const std::string& from, int scanWords,
                                     const std::string& amd = "", bool twas = false);
// A sounding: TWAS (or TIS) `from`, the first word `repeats` times.
std::vector<std::uint32_t> ale2gSound(const std::string& from, int repeats, bool tis = false);
// The words of an address: `first` preamble, then DATA, REP alternating, '@' stuffed.
std::vector<std::uint32_t> ale2gAddress(int first, const std::string& address);

// 3G: one BW0 (26-bit PDU) or BW1 (48-bit PDU) burst.
AleSignal synthBw0(const Bits& pdu, const AleChannel& ch);
AleSignal synthBw1(const Bits& pdu, const AleChannel& ch);

// 4G: one Deep or Fast WALE transmission carrying `pdus` (the M bits set by
// the caller), after `preambles` preambles (Deep WALE only: the counter).
AleSignal synthWale(bool deep, const std::vector<WalePdu>& pdus, const AleChannel& ch, int preambles = 1);

// The 8PSK channel symbols of the bursts (for tests).
std::vector<int> bw0Symbols(const Bits& pdu);
std::vector<int> bw1Symbols(const Bits& pdu);
std::vector<int> waleSymbols(bool deep, const std::vector<WalePdu>& pdus, int preambles = 1);

// Signals one after another (their lead and tail noise between them). The
// noise level depends only on the channel's amplitude and SNR, so parts made
// with the same channel settings join seamlessly.
AleSignal concatenate(const std::vector<AleSignal>& parts);

}  // namespace navtex::ale
