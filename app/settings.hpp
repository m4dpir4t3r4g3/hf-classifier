// The program's settings, kept in settings.json in the user's data folder
// (Windows: %LOCALAPPDATA%\HF Classifier; elsewhere $XDG_DATA_HOME or
// ~/.local/share, then hf_classifier) next to the log. Changed from the UI's
// Settings dialog or by editing the file; command-line options override
// them for one run.
#pragma once

#include <string>
#include <vector>

#include "hfc/json.hpp"

namespace hfc {

struct Settings {
  std::string language;           // "bg", "en"; "" = the system's language
  // SDRconnect.
  std::string sdrHost = "127.0.0.1";
  int sdrPort = 5454;
  std::string tuner = "1";        // "1", "2" or "both" (RSPduo in dual tuner mode)
  std::string dualSerial;         // RSPduo serial number, to switch it to dual tuner mode
  bool startHeadless = true;      // start SDRconnect_headless when SDRconnect is not running
  std::string headlessPath;       // "" = look in the usual places
  // This program.
  int httpPort = 8073;
  bool openWindow = true;         // open the window when the program starts
  bool ft8 = true;                // decode the FT8/FT4 sub-bands
  int maxSessions = 48;           // emissions decoded at once
  // Where the processing goes (applied at once, no restart).
  bool measureUnknown = true;     // measure and classify emissions no decoder fits
  int giveUpSeconds = 120;        // leave an emission no decoder has read in this long (0: never)
  int minSnrDb = 0;               // no session for emissions weaker than this (dB)
  std::vector<std::string> disabledDecoders;  // decoder families switched off ("PSK", "FT8", ...)

  bool operator==(const Settings& o) const;
  bool operator!=(const Settings& o) const { return !(*this == o); }
  // Do the two differ in anything that needs a new connection to SDRconnect?
  bool sameSource(const Settings& o) const;
};

// The user's data folder, created if needed. "" if it cannot be made.
std::string dataDirectory();
// `name` inside the data folder (or the current folder if there is none).
std::string dataFile(const std::string& name);

// Reads settings; fields missing from the file keep their values in `s`.
// False if the file exists but cannot be read (error says why); a missing
// file is not an error.
bool loadSettings(const std::string& path, Settings& s, std::string* error);
bool saveSettings(const std::string& path, const Settings& s, std::string* error);

// As JSON, for the UI (one line).
std::string settingsJson(const Settings& s);
// Changes `s` from the fields of a UI command; false (with an error code for
// the UI: "bad_port", "bad_tuner", "bad_host", "bad_language") if a value is
// not acceptable, and then `s` is unchanged.
bool applySettingsFields(const std::vector<JsonField>& fields, Settings& s, std::string* errorCode);

}  // namespace hfc
