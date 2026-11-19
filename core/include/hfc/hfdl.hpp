// HFDL (High Frequency Data Link, ARINC 635 / ICAO Annex 10 vol. III): the
// data link between aircraft and 16 ground stations. Bursts of 1800 Bd
// BPSK, QPSK or 8PSK on a 1440 Hz subcarrier of an upper-sideband channel:
//
//   prekey (a tone, 249 ms)
//   preamble: A A (127 BPSK symbols each, a fixed sequence), M1 (127: one
//             of 8 shifts of an m-sequence, naming the bit rate and the
//             interleaver), M2 (15), 9 x T (15 each, known)
//   data:     72 (single slot, 1.8 s) or 168 (double slot, 4.2 s) frames
//             of 30 data symbols and the 15 known symbols T
//
// Data: convolutional code K = 7, rate 1/2 (generators 0x6d and 0x4f; at
// 300 bit/s every coded bit is sent twice), a 40-row block interleaver,
// and a scrambler that turns a data symbol by 180 degrees (15-stage
// register, restarted every 120 symbols). The decoded octets are one PDU
// whose header carries a 16-bit frame check sequence (the CCITT one of
// X.25): a squitter (SPDU, what each ground station sends every 32 s) or
// an MPDU from or to aircraft, with LPDUs that have their own check.
//
// Only PDUs whose check is right are reported, so a report can be believed
// at once. Reported: who sent it to whom, the bit rate, and the kind of
// every LPDU with its 24-bit ICAO aircraft address where the LPDU carries
// one (log-on and log-off). The contents of data LPDUs are not read here.
//
// Sources: the sequences, interleaver, scrambler and PDU layouts as
// dumphfdl implements them (github.com/szpajder/dumphfdl, GPL-3.0: read
// for these facts, no code taken); verified on the Signal Identification
// Wiki's five HFDL recordings (19 of 20 bursts pass their check: BPSK at
// 300 and 600 bit/s and QPSK). 8PSK (1800 bit/s) is not in them: it is
// tested only against this file's own modulator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

// The frame check sequence of `n` octets (to be stored low octet first).
uint16_t hfdlFcs(const uint8_t* data, std::size_t n);

// What a decoded PDU says, "" if its header's check fails or it is too
// short. `bitRate` (300, 600, 1200 or 1800) is named in the text.
std::string hfdlDescribe(const std::vector<uint8_t>& pdu, int bitRate);

// The name of ground station `id` ("" if unknown): the HFDL system table
// as dumphfdl ships it.
const char* hfdlGroundStation(int id);

class HfdlDecoder {
 public:
  // Real audio at `rate`; `carrierHz` is where the 1440 Hz subcarrier (the
  // middle of the signal) is in it. Tolerates about +-100 Hz.
  HfdlDecoder(double rate, double carrierHz);
  ~HfdlDecoder();
  void process(const float* audio, std::size_t n);
  // The reports of the PDUs decoded since the last call.
  std::vector<std::string> take();
  // For tests and the benchmark: preambles found, bursts whose PDU passed.
  int preambles() const;
  int decoded() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --- for tests and the demo ---

// A squitter of ground station `gs` (66 octets with its check).
std::vector<uint8_t> hfdlSquitter(int gs, int frameIndex, int slot);
// A downlink MPDU from aircraft `aircraftId` to ground station `gs` with
// one log-on request LPDU carrying `icaoAddress`.
std::vector<uint8_t> hfdlLogon(int gs, int aircraftId, uint32_t icaoAddress);
// The burst carrying `pdu` as audio at `rate` with the subcarrier at
// `carrierHz`, peak amplitude about 1. `mode` 0..7: 300, 600, 1200, 1800
// bit/s single slot, then the same in a double slot. Empty if the PDU does
// not fit.
std::vector<float> hfdlModulate(const std::vector<uint8_t>& pdu, int mode, double rate, double carrierHz);

}  // namespace hfc
