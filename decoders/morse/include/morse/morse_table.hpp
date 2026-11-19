// Morse code tables: pattern <-> text lookup.
//
// Patterns are packed into an integer "code": a leading sentinel 1 bit followed
// by one bit per element (0 = dit, 1 = dah), first element most significant.
// For example ".-" (A) packs to 0b110 = 6. This keeps lookups allocation-free,
// which matters because decoding runs on the real-time audio thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace morse {

constexpr int kMaxElements = 10;  // longest pattern handled as a unit

using Code = std::uint32_t;
constexpr Code kEmptyCode = 1;  // sentinel only, zero elements

inline Code appendElement(Code code, bool dah) { return (code << 1) | (dah ? 1u : 0u); }
int codeLength(Code code);

// Text for a pattern code, or nullptr if the pattern is not a known character.
// Prosigns come back in angle brackets, for example "<SK>".
const char* lookup(Code code);

// Pattern such as ".-.-." to code; returns 0 for an invalid string.
Code codeFromPattern(const char* pattern);
std::string patternFromCode(Code code);

// Splits text into Morse tokens (single characters, "<XX>" prosigns and
// spaces) and returns the pattern for each; unknown characters are skipped.
// Used by the signal synthesiser and the tests.
struct Token {
  std::string text;     // " " for a word gap
  std::string pattern;  // empty for a word gap
};
std::vector<Token> tokenize(const std::string& text);

// Normalises text into the form the decoder emits: upper case, runs of
// whitespace collapsed to one space, characters without a Morse pattern dropped.
std::string normalizeText(const std::string& text);

}  // namespace morse
