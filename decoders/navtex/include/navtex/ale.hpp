// Automatic Link Establishment (ALE) decoder: 2G ALE (MIL-STD-188-141A/B
// appendix A, FED-STD-1045: 8-ary FSK, 125 baud), 3G ALE (MIL-STD-188-141B
// appendix C, STANAG 4538 robust link setup: 8PSK burst waveforms BW0 and
// BW1) and 4G ALE (MIL-STD-188-141D appendix G, WALE: the Deep and Fast WALE
// 8PSK burst waveforms), all in one 3 kHz USB channel.
//
//   audio -> AleFrontEnd: mixer to 1800 Hz, low-pass, resampler -> 9600 Hz complex baseband
//              |-> Ale2gReceiver: tone detectors, timing phases, majority vote, Golay,
//              |                  word sync -> Ale2gFrameAssembler -> calls, soundings, AMD
//              `-> RRC matched filter -> AlePskReceiver: preamble correlators -> burst
//                  demodulator (Walsh / BPSK, RAKE) -> deinterleaver -> tail-biting
//                  Viterbi -> CRC -> 3G and 4G PDUs
//
// The decoder reports what it decodes as AleEvents (one per 2G frame or word,
// one per 3G or 4G PDU) and, for the SDRuno plugin, as lines of text through
// DecoderSink::onCharacter. It does not allocate memory while processing
// samples; decoding a burst allocates a little.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "navtex/types.hpp"

namespace navtex {

struct AleConfig {
  double sampleRate = 48000.0;
  int channels = 1;  // interleaved input channels; they are averaged
  bool enable2g = true;
  bool enable3g = true;
  bool enable4g = true;
  // 2G: report every word as it is decoded (in addition to the frames).
  bool showWords = false;
  // 3G/4G: report bursts whose preamble was found but whose data failed to
  // decode (CRC error, e.g. linking protection or a weak signal).
  bool showFailed = true;
};

enum class AleGeneration : std::uint8_t { G2, G3, G4 };

// One decoded item.
struct AleEvent {
  enum Kind : std::uint8_t {
    Frame2g,  // a 2G ALE frame (call, response, sounding...), its words combined
    Word2g,   // one 2G ALE word (with AleConfig::showWords)
    Pdu,      // a 3G or 4G PDU
    Failed,   // a 3G or 4G burst whose data did not decode
  };
  Kind kind = Frame2g;
  AleGeneration generation = AleGeneration::G2;
  double time = 0;         // stream time in seconds (end of the frame, word or burst)
  char text[320] = {};     // what it says, one line, e.g. "TO K1ABC  TIS W2XYZ"
  char waveform[16] = {};  // "2G", "BW0", "BW1", "Deep WALE", "Fast WALE"
  float quality = 0;       // 0..1
  // Failed: the data came through intact (every coded bit agrees with the
  // code word decoded) but fails its CRC: encrypted (linking protection), or
  // not made to the standard. False when the burst was too weak or corrupted.
  bool intact = false;
  double snrDb = 0;        // estimated for 3G/4G bursts
  double offsetHz = 0;     // carrier offset (3G/4G) or tone offset (2G) measured
  // Raw content: the 2G word (Word2g), or the PDU bits (3G, MSB-first fields)
  // and octets (4G).
  std::uint32_t word = 0;
  std::uint8_t octets[12] = {};
  int bits = 0;            // PDU length in bits
};

// The event as one line of text, as the decoder sends it to its
// DecoderSink: "2G  TO K1ABC  TIS W2XYZ", "3G BW0  LE_Call ...  [SNR 3 dB, +12 Hz]".
void formatAleLine(const AleEvent& e, char* out, std::size_t size);

class AleSink {
 public:
  virtual ~AleSink() = default;
  virtual void onAle(const AleEvent&) = 0;
};

struct AleStatus {
  bool signalPresent = false;  // a 2G word or 3G/4G preamble in the last 3 s
  double levelDb = -200;       // audio level, dBFS
  std::uint32_t words2g = 0, frames2g = 0, pdus3g = 0, pdus4g = 0, failed = 0;
  double lastTime = -1;        // stream time of the last event
  char last[96] = {};          // its text, shortened
  double streamTime = 0;
};

class AleDecoder {
 public:
  using Config = AleConfig;

  explicit AleDecoder(const AleConfig& config = AleConfig());
  ~AleDecoder();
  AleDecoder(const AleDecoder&) = delete;
  AleDecoder& operator=(const AleDecoder&) = delete;

  void configure(const AleConfig& config);
  const AleConfig& config() const { return cfg_; }
  void reset();
  // Text lines (one character at a time, '\n' at the end of each) and
  // spectrum frames.
  void setSink(DecoderSink* sink) { sink_ = sink; }
  void setAleSink(AleSink* sink) { aleSink_ = sink; }
  // `frames` frames of `config().channels` interleaved samples.
  void process(const float* samples, std::size_t frames);
  // End of input: finishes an open 2G frame.
  void flush();

  AleStatus status() const;
  double streamTime() const;

 private:
  struct Impl;
  void emit(const AleEvent& e);
  AleConfig cfg_;
  DecoderSink* sink_ = nullptr;
  AleSink* aleSink_ = nullptr;
  std::unique_ptr<Impl> impl_;
  AleStatus status_;
};

}  // namespace navtex
