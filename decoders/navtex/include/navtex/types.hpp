// Public configuration, event and status types shared by the decoders, the
// command line tools and the SDRuno plugins.
//
// The SitorB* types and TransmissionEvent describe any SITOR-B (ITU-R M.476 /
// M.625 mode B) signal. DecoderConfig, MessageEvent and DecoderStatus add
// what the NAVTEX layer knows on top of it: messages ("ZCZC B1B2B3B4" ...
// "NNNN") and repeats.
#pragma once

#include <cstdint>

namespace navtex {

// Which audio tone carries the "B" (1) bits. With Auto the decoder tries both
// and keeps the one that produces valid CCIR 476 code words, so it works in
// USB and LSB alike.
enum class Polarity : std::uint8_t { Auto, Normal, Reversed };

// Signal settings, common to every SITOR-B signal.
struct SitorBConfig {
  double sampleRate = 48000.0;
  int channels = 1;  // interleaved input channels; they are averaged

  // Centre frequency (midway between the two tones) and automatic
  // frequency control. With AFC the decoder searches [minCentreHz,
  // maxCentreHz] for a pair of tones `shiftHz` apart and follows it.
  bool afc = true;
  double minCentreHz = 500.0;
  double maxCentreHz = 2500.0;
  double fixedCentreHz = 1000.0;  // used when afc == false
  double shiftHz = 170.0;
  double baud = 100.0;

  Polarity polarity = Polarity::Auto;

  // Combine the two copies of each character (DX and RX) using the soft bit
  // values, so a character is often recovered when both copies are damaged.
  // Off: classic hard-decision FEC (take whichever copy is valid).
  bool softFec = true;

  // A tone pair that AFC has followed for this long without character sync
  // is not SITOR-B: AFC leaves it alone for a while and searches elsewhere.
  // (A NAVTEX emission starts with at least 10 s of phasing.)
  double rejectAfterSeconds = 10.0;
  // A transmission ends after this long of continuous idle signals following
  // its text, or when sync is lost after idle signals (an orderly end).
  double idleEndSeconds = 5.0;
  // ... or, when sync is lost in the middle of the text, once it has not
  // come back for this long (signal lost).
  double lostEndSeconds = 5.0;
  // Spectrum frames always cover the whole search range, instead of a
  // close-up of the signal once AFC has locked (for a waterfall).
  bool wideSpectrum = false;
};

// The modes of the FSK decoder (FskDecoder).
enum class FskMode : std::uint8_t {
  SitorB,  // SITOR-B / AMTOR mode B: CCIR 476 with FEC
  Rtty,    // RTTY: ITA2, start/stop framing
  Bits,    // any FSK: the raw bits on the recovered clock
};

// FSK decoder settings: the signal settings (baud and shift included) plus
// the mode.
struct FskConfig : SitorBConfig {
  FskMode mode = FskMode::SitorB;
  // RTTY: a space returns to the letters case (as most RTTY software does).
  bool unshiftOnSpace = true;
};

// NAVTEX decoder settings: the signal settings plus message handling.
struct DecoderConfig : SitorBConfig {
  // A message whose identity (e.g. "OA23") was already received with fewer
  // than 4% errors within this time is flagged as a repeat.
  double repeatMemoryHours = 72.0;
  // A message still open after this many seconds without text is closed as
  // incomplete (signal lost before NNNN).
  double messageTimeoutSeconds = 30.0;
};

enum class SyncState : std::uint8_t {
  NoSignal,   // no SITOR-B signal found
  Searching,  // signal present, looking for character sync
  Synced,     // decoding characters
};

// One decoded character.
struct DecodedChar {
  char text[4] = {};       // one character, "\n" for a new line, "*" for one that could not be recovered
  float confidence = 0;    // 0..1
  double time = 0;         // stream time in seconds at which it was decoded
  bool corrected = false;  // the two copies disagreed or were damaged, and FEC chose
  bool inMessage = false;  // NAVTEX only: between a "ZCZC B1B2B3B4" header and its "NNNN"
};

// Start or end of a transmission: text framed by character sync, from the
// first character decoded to idle signals or the loss of the signal. Says
// nothing about what the text is.
struct TransmissionEvent {
  enum Kind : std::uint8_t { Started, Ended };
  Kind kind = Started;
  double time = 0;         // stream time in seconds (Started: of its first character)
  bool reversed = false;   // B bits on the lower tone
  bool complete = false;   // Ended: ended with idle signals; false: signal lost or decoder reset
  std::uint32_t chars = 0, errors = 0;  // Ended: characters (not spaces or line ends) and unrecoverable ones
};

// Start or end of a NAVTEX message.
struct MessageEvent {
  enum Kind : std::uint8_t { Started, Ended };
  Kind kind = Started;
  char id[5] = {};        // B1 B2 B3 B4, e.g. "OA23"
  char station = 0;       // B1: transmitter identity, A-Z
  char subject = 0;       // B2: message type, A-Z
  int number = 0;         // B3B4: serial number, 0 = must always be printed
  double time = 0;        // stream time in seconds
  bool repeat = false;    // already received with < 4% errors (never for number 0)
  std::uint8_t headerChars = 0;  // Started: the header ("ZCZC OA23") was the last this many characters
  bool complete = false;  // Ended: NNNN received; false: signal lost, or a new header arrived
  std::uint32_t chars = 0, errors = 0;  // Ended: characters and unrecoverable characters in the message
};

struct SitorBStatus {
  double centreHz = 0;
  bool centreLocked = false;
  bool signalPresent = false;
  double snrDb = 0;       // in a 2500 Hz bandwidth
  SyncState sync = SyncState::NoSignal;
  bool reversed = false;  // B bits on the lower tone
  bool idle = false;      // synced, receiving phasing or idle signals
  double errorRate = 0;   // unrecoverable characters among the last 200, 0..1
  std::uint64_t charsDecoded = 0;
  bool inTransmission = false;
  std::uint32_t transmissions = 0;  // transmissions started
};

struct FskStatus : SitorBStatus {
  FskMode mode = FskMode::SitorB;
  double measuredShiftHz = 0;  // 0: not measured yet
};

struct DecoderStatus : SitorBStatus {
  bool inMessage = false;
  char messageId[5] = {};
  std::uint32_t messages = 0;  // messages started
};

// Power spectrum around the signal for the tuning display (~10 per second).
struct SpectrumFrame {
  static constexpr int kBins = 200;
  float db[kBins] = {};  // power in dB relative to the noise floor
  float lowHz = 0, highHz = 0;
  float centreHz = 0, shiftHz = 0;
  bool locked = false;
};

class DecoderSink {
 public:
  virtual ~DecoderSink() = default;
  virtual void onCharacter(const DecodedChar&) {}
  virtual void onMessage(const MessageEvent&) {}
  virtual void onTransmission(const TransmissionEvent&) {}
  virtual void onSpectrum(const SpectrumFrame&) {}
};

}  // namespace navtex
