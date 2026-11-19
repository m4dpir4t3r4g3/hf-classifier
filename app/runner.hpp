// Running the classifier, for both programs: hf_classifier (console) and
// HFClassifier (Windows, in the notification area). Reads the settings,
// starts the app and its web UI, opens the window, and runs the live source
// (restarting it when the settings change) or plays a recording.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "settings.hpp"

namespace hfc {

struct RunOptions {
  Settings settings;          // after the command line's overrides
  Settings fileSettings;      // as in the settings file, without them
  std::string settingsPath;   // changes from the UI are saved here ("" = not saved)
  std::string dbPath;
  std::string frequenciesDir;  // frequency lists are read from here ("" = none but the built-in)
  bool openWindow = false;    // open the UI once it is up
  // Live: set these on SDRconnect first ("" leaves them).
  std::string rate, centre;
  // Playback instead of SDRconnect.
  std::string play;
  bool demo = false;
  std::string picturesDir;    // where received pictures (radiofax) go (default: pictures in the data folder)
  std::string recordingsDir;  // where the UI's recordings go (default: recordings in the data folder)
  std::string reportsDir;     // where diagnostic reports go (default: reports in the data folder)
  double speed = 1;
  bool exitAtEnd = false;
  // --report / --report-rates: make a diagnostic report and exit (makeReport).
  bool report = false, reportRates = false;
  double reportProbeSeconds = 3;  // the report's measuring time per setting
};

// Reads the command line into `o` (whose settings were loaded first, and
// whose settingsPath and dbPath are the defaults). False with `error` set
// for a bad option; `help` set for -h / --help.
bool parseCommandLine(const std::vector<std::string>& args, RunOptions& o, std::string* error, bool* help);
std::string usageText();
// The options for a run with no command line: the settings file, the log
// and settings in the data folder.
RunOptions defaultRunOptions();

struct RunHooks {
  std::function<void(const std::string& url)> ready;  // the UI is up at url
  // Could not start: "port_busy" (detail: the port), "start_failed" (detail: why).
  std::function<void(const std::string& code, const std::string& detail)> failed;
};

enum RunResult { kRunOk = 0, kRunFailed = 1, kRunAlreadyRunning = 3 };

// Runs until `stop` (or, for playback with exitAtEnd, the end of the
// recording). If the program already runs on the same port, opens its
// window and returns kRunAlreadyRunning.
int runClassifier(const RunOptions& opt, const std::atomic<bool>& stop, const RunHooks& hooks);

// A diagnostic report (diagnostics.hpp) without running the classifier:
// "--report" from the command line or the Start menu. If the program runs
// on the window port, it is asked to make the report (it holds the
// connection to SDRconnect and measures on it); otherwise this measures on
// a connection of its own, when SDRconnect answers. `rates`: also each
// sample rate. Returns the report's path, or "" with `error` set.
std::string makeReport(const RunOptions& opt, bool rates, const std::function<void(const std::string&)>& progress,
                       std::string* error);

// "bg" or "en": the settings' language, or the system's.
std::string effectiveLanguage(const Settings& s);

}  // namespace hfc
