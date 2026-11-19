#include "navtex/ccir476.hpp"

#include <cctype>

namespace navtex {

namespace {

struct Entry {
  std::uint8_t code;
  char letter;
  char figure;  // 0: none (bell)
};

// Letters and the ITA2 (international) figures case. D in figures is
// "Who are you?", shown as '$' as on most teleprinters; F, G and H are left
// to national use and shown as on US teleprinters.
const Entry kTable[] = {
    {0x47, 'A', '-'},  {0x72, 'B', '?'}, {0x1d, 'C', ':'},  {0x53, 'D', '$'}, {0x56, 'E', '3'},
    {0x1b, 'F', '!'},  {0x35, 'G', '&'}, {0x69, 'H', '#'},  {0x4d, 'I', '8'}, {0x17, 'J', 0},
    {0x1e, 'K', '('},  {0x65, 'L', ')'}, {0x39, 'M', '.'},  {0x59, 'N', ','}, {0x71, 'O', '9'},
    {0x2d, 'P', '0'},  {0x2e, 'Q', '1'}, {0x55, 'R', '4'},  {0x4b, 'S', '\''}, {0x74, 'T', '5'},
    {0x4e, 'U', '7'},  {0x3c, 'V', '='}, {0x27, 'W', '2'},  {0x3a, 'X', '/'}, {0x2b, 'Y', '6'},
    {0x63, 'Z', '+'},  {ccir::kSpace, ' ', ' '}, {ccir::kCr, '\r', '\r'}, {ccir::kLf, '\n', '\n'},
};

struct Tables {
  char letters[128] = {};
  char figures[128] = {};
  std::vector<std::uint8_t> valid;
  Tables() {
    for (const auto& e : kTable) {
      letters[e.code] = e.letter;
      figures[e.code] = e.figure;
    }
    for (unsigned c = 0; c < 128; ++c)
      if (ccir::isValid(c)) valid.push_back(static_cast<std::uint8_t>(c));
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

}  // namespace

namespace ccir {

const std::vector<std::uint8_t>& validCodes() { return tables().valid; }

char toChar(unsigned code, bool figures) {
  if (code >= 128) return 0;
  return figures ? tables().figures[code] : tables().letters[code];
}

char figureOf(char letter) {
  for (const auto& e : kTable)
    if (e.letter == letter) return e.figure;
  return 0;
}

bool fromChar(char c, std::uint8_t& code, bool& figures) {
  c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const auto& e : kTable) {
    if (e.letter == c) {
      code = e.code;
      figures = false;
      return true;
    }
  }
  for (const auto& e : kTable) {
    if (e.figure == c) {
      code = e.code;
      figures = true;
      return true;
    }
  }
  return false;
}

}  // namespace ccir

std::vector<std::uint8_t> encodeText(const std::string& text) {
  std::vector<std::uint8_t> out;
  bool figures = false;
  out.push_back(ccir::kLetters);
  for (char c : text) {
    std::uint8_t code;
    bool fig;
    if (!ccir::fromChar(c, code, fig)) continue;
    // Space, CR and LF exist in both cases: no shift needed.
    bool both = code == ccir::kSpace || code == ccir::kCr || code == ccir::kLf;
    if (!both && fig != figures) {
      out.push_back(fig ? ccir::kFigures : ccir::kLetters);
      figures = fig;
    }
    out.push_back(code);
  }
  return out;
}

}  // namespace navtex
