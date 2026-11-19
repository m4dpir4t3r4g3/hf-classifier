#include "navtex/ita2.hpp"

#include <cctype>

#include "navtex/ccir476.hpp"

namespace navtex {

namespace {
// Letters case, by code. '#' marks the shifts and null (no character).
const char kLetterOf[33] = "#E\nA SIU\rDRJNFCKTZLWHYPQOBG#MXV#";
}  // namespace

namespace ita2 {

char toChar(unsigned code, bool figures) {
  if (code >= 32) return 0;
  char c = kLetterOf[code];
  if (c == '#') return 0;
  if (c < 'A' || c > 'Z') return c;  // space, CR, LF: both cases
  return figures ? ccir::figureOf(c) : c;
}

bool fromChar(char c, std::uint8_t& code, bool& figures) {
  c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (unsigned k = 0; k < 32; ++k) {
    if (toChar(k, false) == c) {
      code = static_cast<std::uint8_t>(k);
      figures = false;
      return true;
    }
  }
  for (unsigned k = 0; k < 32; ++k) {
    if (toChar(k, true) == c && c != 0) {
      code = static_cast<std::uint8_t>(k);
      figures = true;
      return true;
    }
  }
  return false;
}

}  // namespace ita2

std::vector<std::uint8_t> encodeIta2(const std::string& text) {
  std::vector<std::uint8_t> out;
  bool figures = false, unshifted = false;  // unshifted: a space since the figures shift
  out.push_back(ita2::kLetters);
  for (char c : text) {
    std::uint8_t code;
    bool fig;
    if (!ita2::fromChar(c, code, fig)) continue;
    bool both = code == ita2::kSpace || code == ita2::kCr || code == ita2::kLf;
    if (!both) {
      if (fig && (!figures || unshifted)) {
        out.push_back(ita2::kFigures);
        figures = true;
        unshifted = false;
      } else if (!fig && figures) {
        out.push_back(ita2::kLetters);
        figures = false;
      }
    }
    if (code == ita2::kSpace && figures) unshifted = true;
    out.push_back(code);
  }
  return out;
}

}  // namespace navtex
