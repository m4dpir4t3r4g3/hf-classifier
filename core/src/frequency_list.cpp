#include "hfc/frequency_list.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "hfc/json.hpp"

namespace hfc {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

// A whole token that is a number ("8414.5", "14074000"); false otherwise.
bool number(const std::string& token, double& v) {
  const std::string t = trim(token);
  if (t.empty() || !(std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.')) return false;
  char* end = nullptr;
  v = std::strtod(t.c_str(), &end);
  return end && *end == '\0' && std::isfinite(v) && v > 0;
}

// Label keywords and the catalogue's names, the more specific first.
const std::pair<const char*, const char*> kAliases[] = {
    {"4285", "STANAG 4285"},
    {"4529", "STANAG 4529"},
    {"4539", "STANAG 4539 / MIL-STD-188-110 App C"},
    {"110b", "STANAG 4539 / MIL-STD-188-110 App C"},
    {"4415", "MIL-STD-188-110 serial / STANAG 4415"},
    {"39-tone", "MIL-STD-188-110 App B (39-tone) / STANAG 4197"},
    {"39 tone", "MIL-STD-188-110 App B (39-tone) / STANAG 4197"},
    {"4197", "MIL-STD-188-110 App B (39-tone) / STANAG 4197"},
    {"16-tone", "MIL-STD-188-110 App A (16-tone)"},
    {"188-110", "MIL-STD-188-110 serial / STANAG 4415"},
    {"110a", "MIL-STD-188-110 serial / STANAG 4415"},
    {"link-11", "Link-11 CLEW"},
    {"link 11", "Link-11 CLEW"},
    {"link11", "Link-11 CLEW"},
    {"hfdl", "HFDL"},
    {"navtex", "NAVTEX / SITOR-B"},
    {"sitor-b", "NAVTEX / SITOR-B"},
    {"sitor b", "NAVTEX / SITOR-B"},
    {"sitor-a", "SITOR-A"},
    {"sitor a", "SITOR-A"},
    {"amtor", "SITOR-A"},
    {"gmdss", "DSC-HF"},
    {"dsc", "DSC-HF"},
    {"wale", "ALE 3G / 4G (WALE)"},
    {"3g ale", "ALE 3G / 4G (WALE)"},
    {"3g-ale", "ALE 3G / 4G (WALE)"},
    {"4g ale", "ALE 3G / 4G (WALE)"},
    {"141a", "ALE 2G"},
    {"2g ale", "ALE 2G"},
    {"ale", "ALE 2G"},
    {"akula", "Akula"},
    {"t-600", "CIS-36-50 (T-600)"},
    {"t600", "CIS-36-50 (T-600)"},
    {"cis-36-50", "CIS-36-50 (T-600)"},
    {"36-50", "CIS-36-50 (T-600)"},
    {"cis-50-50", "CIS-50-50"},
    {"50-50", "CIS-50-50"},
    {"makhovik", "CIS BPSK 1200"},
    {"cis-1200", "CIS BPSK 1200"},
    {"cis-112", "CIS-112"},
    {"cis-3000", "CIS-3000"},
    {"cis-20", "CIS-20"},
    {"cis-12", "CIS-12"},
    {"xpa2", "CIS MFSK-16 (XPA2)"},
    {"crowd36", "CROWD36 (CIS-36 MFSK)"},
    {"serdolik", "Serdolik (command state)"},
    {"codan", "CODAN Selcal"},
    {"pactor iii", "PACTOR III"},
    {"pactor-iii", "PACTOR III"},
    {"pactor 3", "PACTOR III"},
    {"pactor ii", "PACTOR II"},
    {"pactor-ii", "PACTOR II"},
    {"pactor 2", "PACTOR II"},
    {"pactor", "PACTOR I"},
    {"chinese 4+4", "Chinese 4+4"},
    {"olivia", "Olivia"},
    {"contestia", "Contestia"},
    {"mfsk32", "MFSK32"},
    {"mfsk16", "MFSK16"},
    {"time signal", "Time signal"},
    {"dcf77", "Time signal"},
    {"msf", "Time signal"},
    {"wefax", "Radiofax (WEFAX)"},
    {"radiofax", "Radiofax (WEFAX)"},
    {"fax", "Radiofax (WEFAX)"},
    {"hell", "Feld Hell"},
    {"qpsk31", "QPSK31"},
    {"psk63", "PSK63"},
    {"psk125", "PSK125"},
    {"psk31", "PSK31"},
    {"bpsk31", "PSK31"},
    {"ft8", "FT8"},
    {"ft4", "FT4"},
    {"wspr", "WSPR"},
    {"js8", "JS8"},
    {"rtty", "RTTY (other rates and shifts)"},
    {"baudot", "RTTY (other rates and shifts)"},
    {"morse", "CW"},
};

// A keyword counts where it is not part of a longer word or number ("ale"
// in "WALE" or "female" does not, "dsc" in "GMDSS/DSC" does).
bool hasWord(const std::string& text, const std::string& word) {
  auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
  for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
    const bool left = at == 0 || !alnum(text[at - 1]);
    const bool right = at + word.size() >= text.size() || !alnum(text[at + word.size()]);
    if (left && right) return true;
  }
  return false;
}

// The number just before `unit` in `text` ("50 bd", "450hz"), 0 if none.
double before(const std::string& text, const std::string& unit) {
  for (std::size_t at = text.find(unit); at != std::string::npos; at = text.find(unit, at + 1)) {
    if (at + unit.size() < text.size() && std::isalpha(static_cast<unsigned char>(text[at + unit.size()]))) continue;
    std::size_t e = at;
    while (e > 0 && text[e - 1] == ' ') --e;
    std::size_t b = e;
    while (b > 0 && (std::isdigit(static_cast<unsigned char>(text[b - 1])) || text[b - 1] == '.')) --b;
    if (b < e) return std::atof(text.substr(b, e - b).c_str());
  }
  return 0;
}

// A receiver's demodulator setting, which such lists carry beside the label
// ("CW" there is how to listen, not Morse).
bool demodulator(const std::string& token, std::string* ours = nullptr) {
  // The setting, and which of ours ("USB", "LSB", "CW", "AM") comes closest.
  static const std::pair<const char*, const char*> kModes[] = {
      {"am", "AM"},   {"amn", "AM"},  {"amw", "AM"},  {"sam", "AM"},  {"sau", "AM"},  {"sal", "AM"},
      {"sas", "AM"},  {"qam", "AM"},  {"dsb", "AM"},  {"drm", "AM"},  {"usb", "USB"}, {"usn", "USB"},
      {"iq", "USB"},  {"lsb", "LSB"}, {"lsn", "LSB"}, {"cw", "CW"},   {"cwn", "CW"},  {"cwu", "CW"},
      {"cwl", "CW"},  {"nbfm", ""},   {"nnfm", ""},   {"nfm", ""},    {"fm", ""},     {"wfm", ""}};
  const std::string t = lower(trim(token));
  for (const auto& [m, o] : kModes)
    if (t == m) {
      if (ours) *ours = o;
      return true;
    }
  return false;
}

// Walks any JSON value for entries (see the header).
template <typename Add>
void walk(const std::string& value, const Add& add) {
  const std::string v = trim(value);
  if (v.empty()) return;
  if (v[0] == '[') {
    std::vector<JsonField> el;
    if (!parseJsonArray(v, el, nullptr)) return;
    double f = 0;
    if (!el.empty() && number(el[0].value, f)) {
      std::string label, demod;
      int category = -1;
      for (std::size_t i = 1; i < el.size(); ++i) {
        const std::string t = trim(el[i].value);
        double n = 0;
        if (!t.empty() && t[0] == '{') {
          // KiwiSDR's labels: {"T4":1} gives the label's type (category 4).
          std::vector<JsonField> o;
          if (parseJsonObject(t, o, nullptr))
            for (const auto& kv : o)
              if (kv.key.size() >= 2 && kv.key[0] == 'T' && std::isdigit(static_cast<unsigned char>(kv.key[1])))
                category = std::atoi(kv.key.c_str() + 1);
          continue;
        }
        if (t.empty() || t[0] == '[' || number(t, n) || t == "true" || t == "false" || t == "null") continue;
        if (demodulator(t, demod.empty() ? &demod : nullptr)) continue;
        // (KiwiSDR's notes hold "\n" for line breaks.)
        std::string clean = t;
        for (std::size_t at = clean.find('\n'); at != std::string::npos; at = clean.find('\n', at)) clean[at] = ' ';
        label += (label.empty() ? "" : " ") + clean;
      }
      add(f, label, demod, category);
      return;
    }
    for (const auto& e : el) walk(e.value, add);
  } else if (v[0] == '{') {
    std::vector<JsonField> fields;
    if (!parseJsonObject(v, fields, nullptr)) return;
    double f = 0;
    bool have = false;
    for (const char* key : {"frequency", "freq", "khz", "kHz", "hz", "Hz", "f"})
      if (!have) have = number(jsonField(fields, key), f);
    if (have) {
      std::string label, demod;
      for (const char* key : {"name", "ident", "label", "description", "mode", "modulation", "notes", "comment"}) {
        const std::string t = trim(jsonField(fields, key));
        if (t.empty() || t[0] == '{' || t[0] == '[') continue;
        if (demodulator(t, demod.empty() ? &demod : nullptr)) continue;
        label += (label.empty() ? "" : " ") + t;
      }
      add(f, label, demod, -1);
      return;
    }
    for (const auto& e : fields) walk(e.value, add);
  }
}

}  // namespace

std::string FrequencyList::modeOf(const std::string& label, double* baud, double* shiftHz) {
  const std::string t = lower(label);
  double b = before(t, "bd"), s = before(t, "hz");
  if (b == 0) b = before(t, "baud");
  // "50/450", "45.45/170": rate / shift.
  for (std::size_t at = t.find('/'); at != std::string::npos && (b == 0 || s == 0); at = t.find('/', at + 1)) {
    std::size_t a = at;
    while (a > 0 && (std::isdigit(static_cast<unsigned char>(t[a - 1])) || t[a - 1] == '.')) --a;
    std::size_t e = at + 1;
    while (e < t.size() && std::isdigit(static_cast<unsigned char>(t[e]))) ++e;
    if (e == at + 1) continue;
    const double y = std::atof(t.substr(at + 1, e - at - 1).c_str());
    if (a < at) {
      const double x = std::atof(t.substr(a, at - a).c_str());
      if (x >= 30 && x <= 2400 && y >= 50 && y <= 2000) b = x, s = y;
    } else if (b > 0 && s == 0 && y >= 50 && y <= 2000) {
      s = y;  // "50 Bd/200"
    }
  }
  if (baud) *baud = b >= 1 && b <= 20000 ? b : 0;
  if (shiftHz) *shiftHz = s >= 20 && s <= 5000 ? s : 0;
  for (const auto& [word, mode] : kAliases)
    if (hasWord(t, word)) return mode;
  if (hasWord(t, "cw")) return "CW";
  return "";
}

namespace {

// How to listen to a catalogued mode when the list does not say.
std::string demodFor(const std::string& mode) {
  if (mode == "CW") return "CW";
  if (mode == "AM broadcast") return "AM";
  return mode.empty() ? "" : "USB";
}

bool byHz(const ListedFrequency& a, const ListedFrequency& b) { return a.hz < b.hz; }

}  // namespace

bool ListedFrequency::onAir(double utc) const {
  if (startMin < 0 || stopMin < 0 || utc < 0) return true;
  const int m = static_cast<int>(std::fmod(utc, 86400.0) / 60.0);
  return startMin <= stopMin ? (m >= startMin && m < stopMin) : (m >= startMin || m < stopMin);
}

std::vector<std::pair<std::string, std::size_t>> FrequencyList::sources() const { return sources_; }

void FrequencyList::add(double hz, const std::string& label, const std::string& source, const std::string& demod, int category) {
  ListedFrequency e;
  e.hz = hz;
  if (e.hz < 3e3 || e.hz > 3e9) return;
  e.label = trim(label);
  if (e.label.size() > 120) e.label.resize(120);
  e.source = source;
  e.mode = modeOf(e.label, &e.baud, &e.shiftHz);
  e.demod = demod.empty() ? demodFor(e.mode) : demod;
  e.category = category;
  entries_.insert(std::upper_bound(entries_.begin(), entries_.end(), e, byHz), std::move(e));
}

// EiBi's CSV: kHz;Time(UTC);Days;ITU;Station;Lng;Target;Remarks;P;Start;Stop
// (README.TXT of eibispace.de). "Lng" is the language of a broadcast, or
// for utility stations one of -CW (Morse), -HF (HFDL), -TY (teletype or
// other digital), -TS (time signal), -EC (empty carrier), -MX (music).
std::size_t FrequencyList::addEibi(const std::string& text, const std::string& source) {
  std::size_t count = 0;
  std::vector<ListedFrequency> rows;
  std::istringstream in(text);
  std::string line;
  std::getline(in, line);  // the header
  while (std::getline(in, line)) {
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) {
      if (c == ';') {
        f.push_back(trim(cur));
        cur.clear();
      } else if (c != '\r') {
        cur += c;
      }
    }
    f.push_back(trim(cur));
    double khz = 0;
    if (f.size() < 6 || !number(f[0], khz) || f[4].empty()) continue;
    ListedFrequency e;
    e.hz = khz * 1e3;
    if (e.hz < 3e3 || e.hz > 3e9) continue;
    e.source = source;
    const std::string& kind = f[5];
    e.label = f[4] + (f[3].empty() ? "" : " (" + f[3] + ")");
    if (kind == "-CW") {
      e.label += " Morse";
      e.mode = "CW";
      e.demod = "CW";
    } else if (kind == "-HF") {
      e.label += " HFDL";
      e.mode = "HFDL";
      e.demod = "USB";
    } else if (kind == "-TY") {
      e.label += " teletype";
      e.mode = modeOf(f[4], &e.baud, &e.shiftHz);
      e.demod = "USB";
    } else if (kind == "-TS") {
      e.label += " time signal";
      e.mode = "Time signal";
      e.demod = "AM";
    } else {
      e.mode = e.hz >= 148.5e3 ? "AM broadcast" : "";  // below long wave: VLF stations, no programme
      e.demod = "AM";
    }
    if (e.label.size() > 120) e.label.resize(120);
    // "0000-2400" is the whole day: no hours to check.
    if (f[1].size() == 9 && f[1][4] == '-' && f[1] != "0000-2400") {
      const int a = std::atoi(f[1].substr(0, 4).c_str()), b = std::atoi(f[1].substr(5, 4).c_str());
      e.startMin = a / 100 * 60 + a % 100;
      e.stopMin = b / 100 * 60 + b % 100;
    }
    rows.push_back(std::move(e));
    ++count;
  }
  entries_.insert(entries_.end(), rows.begin(), rows.end());
  std::stable_sort(entries_.begin(), entries_.end(), byHz);
  return count;
}

std::size_t FrequencyList::addText(const std::string& text, const std::string& source) {
  const std::size_t before = entries_.size();
  // Collected first: the list's unit comes from its largest number.
  struct Found {
    double f;
    std::string label, demod;
    int category = -1;
  };
  std::vector<Found> found;
  auto finish = [&] {
    // The unit from the typical value (the median), not the largest: a
    // KiwiSDR list in kHz has VHF entries above 100000 too.
    std::vector<double> v;
    for (const auto& f : found) v.push_back(f.f);
    double typical = 0;
    if (!v.empty()) {
      std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
      typical = v[v.size() / 2];
    }
    const double unit = typical >= 100000 ? 1.0 : typical <= 30 ? 1e6 : 1e3;
    for (const auto& f : found) add(f.f * unit, f.label, source, f.demod, f.category);
    const std::size_t n = entries_.size() - before;
    if (n) sources_.emplace_back(source, n);
    return n;
  };
  const std::string t = trim(text);
  if (t.rfind("kHz:", 0) == 0 && t.find(';') != std::string::npos) {
    const std::size_t n = addEibi(t, source);
    if (n) sources_.emplace_back(source, n);
    return n;
  }
  if (!t.empty() && (t[0] == '{' || t[0] == '[')) {
    walk(t, [&](double f, const std::string& label, const std::string& demod, int category) { found.push_back({f, label, demod, category}); });
    return finish();
  }
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
    // The fields: by comma, semicolon or tab when the line has any, else by
    // spaces; quotes around a field go. A line with such a character but no
    // field that is a number (a comma in free text after the frequency) is
    // split by spaces after all.
    auto split = [&](bool delimited) {
      std::vector<std::string> fields;
      std::string cur;
      for (char c : line) {
        const bool sep = delimited ? (c == ',' || c == ';' || c == '\t') : (c == ' ');
        if (sep) {
          if (!cur.empty() || delimited) fields.push_back(trim(cur));
          cur.clear();
        } else if (c != '"') {
          cur += c;
        }
      }
      fields.push_back(trim(cur));
      return fields;
    };
    double f = 0;
    std::vector<std::string> fields;
    std::size_t at = 0;
    for (bool delimited : {line.find_first_of(",;\t") != std::string::npos, false}) {
      fields = split(delimited);
      at = fields.size();
      for (std::size_t i = 0; i < fields.size() && at == fields.size(); ++i)
        if (number(fields[i], f)) at = i;
      if (at < fields.size() || !delimited) break;
    }
    if (at == fields.size()) continue;
    std::string label;
    for (std::size_t i = 0; i < fields.size(); ++i)
      if (i != at && !fields[i].empty()) label += (label.empty() ? "" : " ") + fields[i];
    found.push_back({f, label, "", -1});
  }
  return finish();
}

void FrequencyList::addEntry(double hz, const std::string& label, const std::string& source, const std::string& demod) {
  const std::size_t before = entries_.size();
  add(hz, label, source, demod);
  if (entries_.size() == before) return;
  for (auto& [name, n] : sources_)
    if (name == source) {
      ++n;
      return;
    }
  sources_.emplace_back(source, 1);
}

std::size_t FrequencyList::addFile(const std::string& path) {
  std::ifstream in(std::filesystem::u8path(path), std::ios::binary);  // (UTF-8 paths, on Windows too)
  if (!in) return 0;
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  if (text.size() > 64u * 1024 * 1024) return 0;
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // a UTF-8 byte order mark
  // Not UTF-8 (EiBi's files are Latin-1): read as Latin-1, so that labels
  // stay valid text.
  auto utf8 = [](const std::string& t) {
    for (std::size_t i = 0; i < t.size();) {
      const unsigned char c = static_cast<unsigned char>(t[i]);
      const std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
      if (n == 0 || i + n > t.size()) return false;
      for (std::size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(t[i + k]) >> 6) != 2) return false;
      i += n;
    }
    return true;
  };
  if (!utf8(text)) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (char ch : text) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (c < 0x80) out += ch;
      else out += static_cast<char>(0xC0 | (c >> 6)), out += static_cast<char>(0x80 | (c & 0x3F));
    }
    text.swap(out);
  }
  const std::size_t slash = path.find_last_of("/\\");
  return addText(text, slash == std::string::npos ? path : path.substr(slash + 1));
}

std::vector<ListedFrequency> FrequencyList::lookup(double loHz, double hiHz, std::size_t max, double utc) const {
  std::vector<std::pair<double, const ListedFrequency*>> found;  // distance from the band
  ListedFrequency key;
  key.hz = loHz - 2500;
  auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                             [](const ListedFrequency& a, const ListedFrequency& b) { return a.hz < b.hz; });
  for (; it != entries_.end() && it->hz <= hiHz + 150; ++it) {
    if (!it->onAir(utc)) continue;
    const double d = it->hz < loHz ? loHz - it->hz : it->hz > hiHz ? it->hz - hiHz : 0;
    found.emplace_back(d, &*it);
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<ListedFrequency> out;
  for (std::size_t i = 0; i < found.size() && out.size() < max; ++i) out.push_back(*found[i].second);
  return out;
}

// catalogue/eibi/sked-b26.csv, embedded at build time.
const std::string& eibiScheduleText();

const std::string& FrequencyList::bundledEibi() { return eibiScheduleText(); }
const char* FrequencyList::bundledEibiName() { return "EiBi B26 (built in)"; }

std::shared_ptr<FrequencyList> FrequencyList::builtin() {
  auto list = std::make_shared<FrequencyList>();
  // ITU Radio Regulations, Appendix 15: the GMDSS distress and safety
  // frequencies for digital selective calling. ITU-R M.540 and the IMO
  // NAVTEX manual: 518 kHz (international), 490 kHz (national), 4209.5 kHz.
  list->addText("2187.5 GMDSS DSC distress and safety\n"
                "4207.5 GMDSS DSC distress and safety\n"
                "6312 GMDSS DSC distress and safety\n"
                "8414.5 GMDSS DSC distress and safety\n"
                "12577 GMDSS DSC distress and safety\n"
                "16804.5 GMDSS DSC distress and safety\n"
                "490 NAVTEX (national)\n"
                "518 NAVTEX (international)\n"
                "4209.5 NAVTEX (HF)\n",
                "ITU");
  return list;
}

}  // namespace hfc
