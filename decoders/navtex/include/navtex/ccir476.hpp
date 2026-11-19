// The CCIR 476 (ITU-R M.476 / M.625) seven-unit code used by NAVTEX and
// SITOR. Every valid code word has exactly four "B" (1) and three "Y" (0)
// bits, which gives 35 code words and lets the receiver detect any single
// bit error. Bit 0 of a code is the first bit on air.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace navtex {

namespace ccir {

// Control code words.
constexpr std::uint8_t kLetters = 0x5a;  // shift to letters
constexpr std::uint8_t kFigures = 0x36;  // shift to figures
constexpr std::uint8_t kAlpha = 0x0f;    // phasing signal 1 / idle signal alpha (RX position while phasing)
constexpr std::uint8_t kBeta = 0x33;     // idle signal beta
constexpr std::uint8_t kRep = 0x66;      // phasing signal 2 (DX position while phasing)
constexpr std::uint8_t kChar32 = 0x6a;   // unassigned signal, used as filler
constexpr std::uint8_t kSpace = 0x5c;
constexpr std::uint8_t kCr = 0x78;
constexpr std::uint8_t kLf = 0x6c;

inline bool isValid(unsigned code) {
  unsigned n = 0;
  for (unsigned c = code & 0x7f; c; c &= c - 1) ++n;
  return code < 128 && n == 4;
}

// Idle, phasing and filler signals carry no text.
inline bool isIdle(unsigned code) { return code == kAlpha || code == kBeta || code == kRep || code == kChar32; }

// The 35 valid code words.
const std::vector<std::uint8_t>& validCodes();

// Character for a code in the letters or figures case: an ASCII character,
// '\r' or '\n', or 0 for a code with no printable meaning (shifts, idle
// signals, bell, invalid codes).
char toChar(unsigned code, bool figures);

// Code for an ASCII character, and whether it is in the figures case. Returns
// false for a character the code cannot carry.
bool fromChar(char c, std::uint8_t& code, bool& figures);

// The figures-case character on the key of letter `letter` (A-Z), as in
// toChar; 0 for none. CCIR 476 carries the ITA2 characters, so this is also
// the ITA2 figures case (see ita2.hpp).
char figureOf(char letter);

}  // namespace ccir

// Converts text to CCIR 476 code words, inserting letters/figures shifts.
// Lower case is sent as upper case; characters the code lacks are skipped.
std::vector<std::uint8_t> encodeText(const std::string& text);

}  // namespace navtex
