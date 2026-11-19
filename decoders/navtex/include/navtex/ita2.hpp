// ITA2 (International Telegraph Alphabet No. 2, "Baudot"): the five-unit
// code of RTTY. A character is sent as a start bit (space, 0), five data
// bits, first bit first, and a stop element (mark, 1) of at least one bit,
// often 1.5 or 2. Letters and figures shifts select the case. The characters
// are those of CCIR 476 (the same letters, figures and controls), so the
// figures case is shown as there.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace navtex {

namespace ita2 {

constexpr std::uint8_t kNull = 0x00;
constexpr std::uint8_t kLf = 0x02;
constexpr std::uint8_t kSpace = 0x04;
constexpr std::uint8_t kCr = 0x08;
constexpr std::uint8_t kFigures = 0x1b;  // shift to figures
constexpr std::uint8_t kLetters = 0x1f;  // shift to letters

// Character for a code (bit 0 = first data bit on air) in the letters or
// figures case: an ASCII character, '\r' or '\n', or 0 for none (shifts,
// null, bell).
char toChar(unsigned code, bool figures);

// Code for an ASCII character, and whether it is in the figures case.
// Returns false for a character the code cannot carry.
bool fromChar(char c, std::uint8_t& code, bool& figures);

}  // namespace ita2

// Converts text to ITA2 codes, starting with a letters shift and inserting
// shifts. After a space in the figures case the figures shift is sent again
// before the next figure, so receivers that unshift on space (as most RTTY
// software does by default) print the same as those that do not.
std::vector<std::uint8_t> encodeIta2(const std::string& text);

}  // namespace navtex
