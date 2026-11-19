#include "navtex/message.hpp"

#include <cctype>
#include <cstring>

namespace navtex {

namespace {
constexpr std::size_t kTailLength = 24;
constexpr std::size_t kMaxMessageChars = 30000;  // longer: the end was missed

bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// `s` spells `word`, except for at most one unrecoverable character.
bool matches(const char* s, const char* word) {
  int errors = 0;
  for (std::size_t i = 0; word[i]; ++i) {
    if (s[i] == word[i]) continue;
    if (s[i] != '*' || ++errors > 1) return false;
  }
  return true;
}
}  // namespace

const char* MessageAssembler::subjectName(char subject) {
  switch (subject) {
    case 'A': return "Navigational warning";
    case 'B': return "Meteorological warning";
    case 'C': return "Ice report";
    case 'D': return "Search and rescue information, piracy warning";
    case 'E': return "Meteorological forecast";
    case 'F': return "Pilot and VTS service message";
    case 'G': return "AIS service message";
    case 'H': return "LORAN message";
    case 'I': return "Not used";
    case 'J': return "SATNAV (GNSS) message";
    case 'K': return "Other electronic navaid message";
    case 'L': return "Navigational warning (additional to A)";
    case 'T': return "Test transmission";
    case 'U': case 'V': case 'W': case 'X': case 'Y': return "Special service";
    case 'Z': return "No message on hand";
    default: return "Reserved";
  }
}

void MessageAssembler::reset() {
  tail_.clear();
  inMessage_ = false;
}

bool MessageAssembler::headerAtEnd(char id[5], std::size_t& length) const {
  // ... "ZCZC" " "+ B1 B2 B3 B4, then a line end or space.
  // The separator and the line end may be damaged ('*').
  const std::size_t n = tail_.size();
  if (n < 10) return false;
  char last = tail_[n - 1];
  if (last != '\n' && last != ' ' && last != '*') return false;
  const char* b = &tail_[n - 5];
  if (!isUpper(b[0]) || !isUpper(b[1]) || !isDigit(b[2]) || !isDigit(b[3])) return false;
  std::size_t p = n - 5;
  if (tail_[p - 1] != ' ' && tail_[p - 1] != '*') return false;
  --p;
  while (p > 0 && tail_[p - 1] == ' ') --p;
  if (p < 4 || !matches(&tail_[p - 4], "ZCZC")) return false;
  std::memcpy(id, b, 4);
  id[4] = 0;
  length = n - 1 - (p - 4);  // from the first Z to the last header character
  return true;
}

bool MessageAssembler::endAtTail() const {
  const std::size_t n = tail_.size();
  return n >= 4 && matches(&tail_[n - 4], "NNNN");
}

void MessageAssembler::onChar(DecodedChar c, DecoderSink* sink) {
  tail_ += c.text[0];
  if (tail_.size() > kTailLength) tail_.erase(0, tail_.size() - kTailLength);

  char id[5];
  std::size_t headerLength = 0;
  if (headerAtEnd(id, headerLength)) {
    if (inMessage_) finish(c.time, false, sink);
    current_ = MessageEvent();
    current_.kind = MessageEvent::Started;
    std::memcpy(current_.id, id, 5);
    current_.station = id[0];
    current_.subject = id[1];
    current_.number = (id[2] - '0') * 10 + (id[3] - '0');
    current_.time = c.time;
    current_.headerChars = static_cast<std::uint8_t>(headerLength);
    auto it = received_.find(id);
    current_.repeat = current_.number != 0 && it != received_.end() && c.time - it->second < memorySeconds_;
    inMessage_ = true;
    ++started_;
    text_ = std::string("ZCZC ") + id;
    if (sink) sink->onMessage(current_);
  }

  c.inMessage = inMessage_;
  if (sink) sink->onCharacter(c);
  if (!inMessage_) return;

  text_ += c.text[0];
  char ch = c.text[0];
  if (ch != ' ' && ch != '\n') {
    ++current_.chars;
    if (ch == '*') ++current_.errors;
  }
  if (endAtTail()) finish(c.time, true, sink);
  else if (text_.size() > kMaxMessageChars) finish(c.time, false, sink);
}

void MessageAssembler::close(double time, DecoderSink* sink) {
  if (inMessage_) finish(time, false, sink);
}

void MessageAssembler::finish(double time, bool complete, DecoderSink* sink) {
  inMessage_ = false;
  current_.kind = MessageEvent::Ended;
  current_.time = time;
  current_.complete = complete;
  // Received: stored so that repeats of it are recognised.
  if (complete && current_.number != 0 && current_.errors * 25 < current_.chars) received_[current_.id] = time;
  if (sink) sink->onMessage(current_);
}

}  // namespace navtex
