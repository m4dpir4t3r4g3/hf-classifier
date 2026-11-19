#include "morse/morse_table.hpp"

#include <array>
#include <cctype>
#include <iterator>

namespace morse {
namespace {

struct Entry {
  const char* pattern;
  const char* text;
};

// ITU-R M.1677-1 characters plus common amateur prosigns. Where a prosign and
// a punctuation mark share a pattern, the prosign wins because it is far more
// common on the air: .-.-. is shown as <AR> (not '+'), -.--. as <KN> (not '(').
// -...- is shown as '=' which is also how BT is normally written.
constexpr Entry kTable[] = {
    {".-", "A"},      {"-...", "B"},    {"-.-.", "C"},    {"-..", "D"},      {".", "E"},
    {"..-.", "F"},    {"--.", "G"},     {"....", "H"},    {"..", "I"},       {".---", "J"},
    {"-.-", "K"},     {".-..", "L"},    {"--", "M"},      {"-.", "N"},       {"---", "O"},
    {".--.", "P"},    {"--.-", "Q"},    {".-.", "R"},     {"...", "S"},      {"-", "T"},
    {"..-", "U"},     {"...-", "V"},    {".--", "W"},     {"-..-", "X"},     {"-.--", "Y"},
    {"--..", "Z"},    {"-----", "0"},   {".----", "1"},   {"..---", "2"},    {"...--", "3"},
    {"....-", "4"},   {".....", "5"},   {"-....", "6"},   {"--...", "7"},    {"---..", "8"},
    {"----.", "9"},   {".-.-.-", "."},  {"--..--", ","},  {"..--..", "?"},   {".----.", "'"},
    {"-.-.--", "!"},  {"-..-.", "/"},   {"-.--.-", ")"},  {".-...", "&"},    {"---...", ":"},
    {"-.-.-.", ";"},  {"-...-", "="},   {"-....-", "-"},  {"..--.-", "_"},   {".-..-.", "\""},
    {"...-..-", "$"}, {".--.-.", "@"},  {".-.-.", "<AR>"}, {"...-.-", "<SK>"}, {"-.--.", "<KN>"},
    {"...-.", "<SN>"}, {"-.-.-", "<KA>"}, {"...---...", "<SOS>"}, {"........", "<HH>"},
};

constexpr std::size_t kCodeSpace = std::size_t{1} << (kMaxElements + 1);

const std::array<const char*, kCodeSpace>& table() {
  static const std::array<const char*, kCodeSpace> t = [] {
    std::array<const char*, kCodeSpace> a{};
    for (const auto& e : kTable) a[codeFromPattern(e.pattern)] = e.text;
    return a;
  }();
  return t;
}

}  // namespace

int codeLength(Code code) {
  int n = 0;
  while (code > 1) {
    code >>= 1;
    ++n;
  }
  return n;
}

const char* lookup(Code code) {
  if (code <= kEmptyCode || code >= kCodeSpace) return nullptr;
  return table()[code];
}

Code codeFromPattern(const char* pattern) {
  Code c = kEmptyCode;
  int n = 0;
  for (const char* p = pattern; *p; ++p) {
    if (*p != '.' && *p != '-') return 0;
    if (++n > kMaxElements) return 0;
    c = appendElement(c, *p == '-');
  }
  return n ? c : 0;
}

std::string patternFromCode(Code code) {
  std::string s;
  for (int i = codeLength(code) - 1; i >= 0; --i) s += ((code >> i) & 1u) ? '-' : '.';
  return s;
}

std::vector<Token> tokenize(const std::string& text) {
  // Reverse lookup text -> pattern, built once.
  static const std::vector<Entry> entries(std::begin(kTable), std::end(kTable));
  auto find = [](const std::string& t) -> const char* {
    for (const auto& e : entries)
      if (t == e.text) return e.pattern;
    return nullptr;
  };

  std::vector<Token> out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!out.empty() && out.back().text != " ") out.push_back({" ", ""});
      continue;
    }
    std::string t(1, static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    if (ch == '<') {
      auto close = text.find('>', i);
      if (close != std::string::npos) {
        t = text.substr(i, close - i + 1);
        for (auto& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        i = close;
      }
    }
    if (const char* p = find(t)) out.push_back({t, p});
  }
  while (!out.empty() && out.back().text == " ") out.pop_back();
  return out;
}

std::string normalizeText(const std::string& text) {
  std::string s;
  for (const auto& t : tokenize(text)) s += t.text;
  return s;
}

}  // namespace morse
