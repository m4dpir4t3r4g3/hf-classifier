// The diagnostic report: one ZIP file with everything needed to see why the
// program does not work on someone's PC, made with one click (the UI's
// Settings, or "HFClassifier.exe --report" from the Start menu):
//
//   README.txt             what is inside (English and Bulgarian)
//   report.json            program, Windows, processor and memory; settings;
//                          the connection state, the receivers and their
//                          processing load; SDRconnect: where it is
//                          installed, its version, whether it runs, whether
//                          its port answers; the probe's measurements
//                          (hfc/probe.hpp) or why it did not run
//   probe.txt              the probe's progress, as sdrc_probe prints it
//   settings.json, hf_classifier.log, hf_classifier.previous.log,
//   sdrconnect_headless.log   as they are in the data folder (the logs'
//                          last 2 MB)
//
// No recordings, pictures or log of signals. The paths in it can contain
// the Windows user name; README.txt says so.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hfc {

struct DiagnosticInputs {
  std::string folder;            // where the report is written (made if missing)
  std::string dataFolder;        // listed (names and sizes), not copied
  std::string mode;              // "live", "playback", "report only"
  std::string settingsJson;      // the settings in effect (one line of JSON)
  std::string statusJson;        // the app's status ("" when not running)
  std::string usageJson;         // the app's processing report ("" when none)
  std::vector<std::pair<std::string, std::string>> files;  // name in the report, path ("" content if missing)
  std::string programLog;        // this run's messages as captured (log_capture.hpp): hf_classifier.log, if not ""
  std::string sdrHost = "127.0.0.1";
  int sdrPort = 5454;
  std::string headlessPath;      // from the settings ("" = looked for)
  std::string probeJson;         // the probe's result, or ""
  std::string probeSkipped;      // why it did not run (with probeJson "")
  std::string probeLog;          // its progress lines
  std::string note;              // anything else worth saying
  double unixTime = 0;           // when (0: now)
};

// report.json alone.
std::string diagnosticJson(const DiagnosticInputs& in);
// Writes the ZIP as hfc_report_YYYYMMDD-HHMMSS.zip (-2, -3... when that
// exists) in `in.folder` and
// returns its path; "" with `error` set if it could not be written.
std::string writeDiagnosticReport(const DiagnosticInputs& in, std::string* error);
// The last `maxBytes` of a file, marked as cut when longer; "" if missing.
std::string fileTail(const std::string& path, std::size_t maxBytes);

}  // namespace hfc
