// NAVTEX message framing. A message starts with the header
//
//   ZCZC B1B2B3B4
//
// (B1: transmitter identity A-Z, B2: message type A-Z, B3B4: serial number
// 00-99) and ends with NNNN. The assembler finds these in the decoded text,
// marks the characters in between, counts their errors, and remembers the
// messages received with fewer than 4% errors so that later repeats can be
// recognised, as a NAVTEX receiver does. Number 00 is never a repeat.
//
// "ZCZC" and "NNNN" are accepted with one unrecoverable character ('*') in
// them; the four header characters must be clean.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "navtex/types.hpp"

namespace navtex {

class MessageAssembler {
 public:
  void configure(double repeatMemorySeconds) { memorySeconds_ = repeatMemorySeconds; }
  // Forgets the current message (without reporting its end) and recent
  // text. The repeat memory is kept.
  void reset();
  void clearMemory() { received_.clear(); }

  // Passes `c` to `sink`, marked as inside or outside a message, and reports
  // message starts and ends around it.
  void onChar(DecodedChar c, DecoderSink* sink);
  // Ends the open message, if any, as incomplete.
  void close(double time, DecoderSink* sink);

  bool inMessage() const { return inMessage_; }
  const char* id() const { return current_.id; }
  std::uint32_t started() const { return started_; }
  // Text of the current (or last) message, from "ZCZC" on, with '\n' line ends.
  const std::string& text() const { return text_; }

  // Human-readable name of a message type (B2), in English.
  static const char* subjectName(char subject);

 private:
  bool headerAtEnd(char id[5], std::size_t& length) const;
  bool endAtTail() const;
  void finish(double time, bool complete, DecoderSink* sink);

  double memorySeconds_ = 72.0 * 3600.0;
  std::map<std::string, double> received_;  // id -> time received
  std::string tail_;  // last few characters, for header/end detection
  std::string text_;
  bool inMessage_ = false;
  MessageEvent current_;
  std::uint32_t started_ = 0;
};

}  // namespace navtex
