#include "settings.hpp"

#include <algorithm>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hfc/file.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace hfc {

namespace {

bool isTrue(const std::string& v) { return v == "true" || v == "1"; }

// A host name or address, nothing that could be a URL or an option.
bool validHost(const std::string& h) {
  if (h.empty() || h.size() > 253) return false;
  for (char c : h)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'))
      return false;
  return h[0] != '-';
}

bool validSerial(const std::string& s) {
  if (s.size() > 32) return false;
  for (char c : s)
    if (!std::isalnum(static_cast<unsigned char>(c))) return false;
  return true;
}

std::string joinedDecoders(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& d : v) s += (s.empty() ? "" : ",") + d;
  return s;
}

}  // namespace

bool Settings::operator==(const Settings& o) const {
  return language == o.language && sameSource(o) && httpPort == o.httpPort && openWindow == o.openWindow &&
         ft8 == o.ft8 && maxSessions == o.maxSessions && measureUnknown == o.measureUnknown &&
         giveUpSeconds == o.giveUpSeconds && minSnrDb == o.minSnrDb && disabledDecoders == o.disabledDecoders;
}

bool Settings::sameSource(const Settings& o) const {
  return sdrHost == o.sdrHost && sdrPort == o.sdrPort && tuner == o.tuner && dualSerial == o.dualSerial &&
         startHeadless == o.startHeadless && headlessPath == o.headlessPath;
}

std::string dataDirectory() {
  static const std::string dir = [] {
#ifdef _WIN32
    PWSTR p = nullptr;
    std::string base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) base = narrow(p);
    CoTaskMemFree(p);
    if (base.empty()) return std::string();
    const std::string d = base + "\\HF Classifier";
#else
    std::string base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
      base = x;
    } else if (const char* h = std::getenv("HOME"); h && *h) {
      base = std::string(h) + "/.local";
      makeDirectory(base);
      base += "/share";
    } else {
      return std::string();
    }
    makeDirectory(base);
    const std::string d = base + "/hf_classifier";
#endif
    return makeDirectory(d) ? d : std::string();
  }();
  return dir;
}

std::string dataFile(const std::string& name) {
  const std::string d = dataDirectory();
  if (d.empty()) return name;
#ifdef _WIN32
  return d + "\\" + name;
#else
  return d + "/" + name;
#endif
}

bool loadSettings(const std::string& path, Settings& s, std::string* error) {
  std::FILE* f = openFile(path, "rb");
  if (!f) return true;  // none yet: the defaults
  std::string text;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (1 << 20)) text.append(buf, n);
  std::fclose(f);
  std::vector<JsonField> fields;
  if (!parseJsonObject(text, fields, error)) return false;
  Settings t = s;
  std::string code;
  if (!applySettingsFields(fields, t, &code)) {
    if (error) *error = "invalid value in " + path + " (" + code + ")";
    return false;
  }
  s = t;
  return true;
}

bool saveSettings(const std::string& path, const Settings& s, std::string* error) {
  JsonWriter w;
  w.beginObject();
  w.key("language").value(s.language);
  w.key("sdr_host").value(s.sdrHost);
  w.key("sdr_port").value(s.sdrPort);
  w.key("tuner").value(s.tuner);
  w.key("dual_serial").value(s.dualSerial);
  w.key("start_headless").value(s.startHeadless);
  w.key("headless_path").value(s.headlessPath);
  w.key("http_port").value(s.httpPort);
  w.key("open_window").value(s.openWindow);
  w.key("ft8").value(s.ft8);
  w.key("max_sessions").value(s.maxSessions);
  w.key("measure_unknown").value(s.measureUnknown);
  w.key("give_up_seconds").value(s.giveUpSeconds);
  w.key("min_snr_db").value(s.minSnrDb);
  w.key("disabled_decoders").value(joinedDecoders(s.disabledDecoders));
  w.endObject();
  // Written whole to a new file, then put in place: a crash never leaves
  // half a settings file.
  const std::string tmp = path + ".new";
  std::FILE* f = openFile(tmp, "wb");
  if (!f) {
    if (error) *error = "cannot write " + tmp;
    return false;
  }
  const std::string text = w.str() + "\n";
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) != 0 || !ok) {
    if (error) *error = "cannot write " + tmp;
    return false;
  }
#ifdef _WIN32
  const bool moved = MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  const bool moved = std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
  if (!moved && error) *error = "cannot replace " + path;
  return moved;
}

std::string settingsJson(const Settings& s) {
  auto b = [](bool v) { return v ? "true" : "false"; };
  return std::string("{\"language\":") + jsonQuote(s.language) + ",\"sdr_host\":" + jsonQuote(s.sdrHost) +
         ",\"sdr_port\":" + std::to_string(s.sdrPort) + ",\"tuner\":" + jsonQuote(s.tuner) +
         ",\"dual_serial\":" + jsonQuote(s.dualSerial) + ",\"start_headless\":" + b(s.startHeadless) +
         ",\"headless_path\":" + jsonQuote(s.headlessPath) + ",\"http_port\":" + std::to_string(s.httpPort) +
         ",\"open_window\":" + b(s.openWindow) + ",\"ft8\":" + b(s.ft8) +
         ",\"max_sessions\":" + std::to_string(s.maxSessions) + ",\"measure_unknown\":" + b(s.measureUnknown) +
         ",\"give_up_seconds\":" + std::to_string(s.giveUpSeconds) + ",\"min_snr_db\":" + std::to_string(s.minSnrDb) +
         ",\"disabled_decoders\":" + jsonQuote(joinedDecoders(s.disabledDecoders)) + "}";
}

bool applySettingsFields(const std::vector<JsonField>& fields, Settings& s, std::string* errorCode) {
  Settings t = s;
  auto fail = [&](const char* code) {
    if (errorCode) *errorCode = code;
    return false;
  };
  for (const auto& f : fields) {
    const std::string& v = f.value;
    if (f.key == "language") {
      if (v != "" && v != "bg" && v != "en") return fail("bad_language");
      t.language = v;
    } else if (f.key == "sdr_host") {
      if (!validHost(v)) return fail("bad_host");
      t.sdrHost = v;
    } else if (f.key == "sdr_port") {
      const int p = std::atoi(v.c_str());
      if (p < 1 || p > 65535) return fail("bad_port");
      t.sdrPort = p;
    } else if (f.key == "tuner") {
      if (v != "1" && v != "2" && v != "both") return fail("bad_tuner");
      t.tuner = v;
    } else if (f.key == "dual_serial") {
      if (!validSerial(v)) return fail("bad_serial");
      t.dualSerial = v;
    } else if (f.key == "start_headless") {
      t.startHeadless = isTrue(v);
    } else if (f.key == "headless_path") {
      if (v.size() > 1024) return fail("bad_path");
      t.headlessPath = v;
    } else if (f.key == "http_port") {
      const int p = std::atoi(v.c_str());
      if (p < 0 || p > 65535) return fail("bad_port");
      t.httpPort = p;
    } else if (f.key == "open_window") {
      t.openWindow = isTrue(v);
    } else if (f.key == "ft8") {
      t.ft8 = isTrue(v);
    } else if (f.key == "max_sessions") {
      const int m = std::atoi(v.c_str());
      if (m < 1 || m > 500) return fail("bad_sessions");
      t.maxSessions = m;
    } else if (f.key == "measure_unknown") {
      t.measureUnknown = isTrue(v);
    } else if (f.key == "give_up_seconds") {
      const int g = std::atoi(v.c_str());
      if (g < 0 || g > 86400) return fail("bad_give_up");
      t.giveUpSeconds = g;
    } else if (f.key == "min_snr_db") {
      const int m = std::atoi(v.c_str());
      if (m < -20 || m > 60) return fail("bad_snr");
      t.minSnrDb = m;
    } else if (f.key == "disabled_decoders") {
      // Comma-separated families; unknown names are kept (a newer version's).
      t.disabledDecoders.clear();
      std::string cur;
      for (char c : v + ",") {
        if (c == ',') {
          while (!cur.empty() && cur.back() == ' ') cur.pop_back();
          while (!cur.empty() && cur.front() == ' ') cur.erase(0, 1);
          if (!cur.empty() && cur.size() <= 32 &&
              std::find(t.disabledDecoders.begin(), t.disabledDecoders.end(), cur) == t.disabledDecoders.end())
            t.disabledDecoders.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      if (t.disabledDecoders.size() > 64) return fail("bad_decoders");
    }
    // Other keys (the command's own, or from a newer version) are ignored.
  }
  s = t;
  return true;
}

}  // namespace hfc
