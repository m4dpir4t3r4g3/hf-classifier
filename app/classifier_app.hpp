// The classifier application without its IQ source: one Pipeline per
// receiver (RSPduo: primary and secondary tuner), each in its own thread,
// the SQLite log, and the local web UI (waterfall, emissions, decodes,
// search and export, click-to-listen) on http://127.0.0.1:8073/.
//
//   source thread --feed()--> queue --> receiver thread: Pipeline
//        emissions, decodes --> Store (SQLite) and every browser (WebSocket)
//        waterfall rows       --> every browser (dropped for slow ones)
//   browser commands ("listen", "tune") --> the ControlFn the source set
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/frequency_list.hpp"
#include "hfc/json.hpp"
#include "hfc/pipeline.hpp"
#include "hfc/store.hpp"
#include "hfc/web_server.hpp"

namespace hfc {

struct IqInput {
  std::vector<int16_t> iq;  // interleaved I, Q
  double sampleRate = 0;
  double centreHz = 0;
  double utc = 0;           // of the first sample
};

class ClassifierApp {
 public:
  struct Config {
    std::string dbPath = "hf_classifier.db";
    std::string httpAddress = "127.0.0.1";
    uint16_t httpPort = 8073;  // 0: any free port
    Pipeline::Config pipeline;
    double maxQueueSeconds = 5;  // IQ waiting per receiver before blocks are dropped
  };
  // A command from the UI for receiver `rx`: "listen" (freq, mode, bw) or
  // "tune" (centre). Returns "" on success, else why not.
  using ControlFn = std::function<std::string(int rx, const std::string& cmd, const std::vector<JsonField>& args)>;

  ClassifierApp();
  ~ClassifierApp();
  ClassifierApp(const ClassifierApp&) = delete;
  ClassifierApp& operator=(const ClassifierApp&) = delete;

  bool start(const Config& cfg, const std::string& sourceDescription, std::string* error);
  // Flushes the receivers, closes the session.
  void stop();
  uint16_t httpPort() const { return web_.port(); }

  // Declares receiver `rx` (0 or 1), what it is and whether it can be
  // controlled (tuned, listened to).
  void setReceiver(int rx, const std::string& name, bool controllable);
  void setControl(ControlFn fn) {
    std::lock_guard<std::mutex> l(controlMu_);
    control_ = std::move(fn);
  }
  // From any thread. Blocks beyond maxQueueSeconds are dropped (counted).
  void feed(int rx, IqInput block);
  // Waits until every queued block has been processed (playback, tests).
  void drain();
  // End of the input on every receiver: emissions end, decoders flush.
  void flushReceivers();
  uint64_t droppedSamples() const { return dropped_; }
  std::string statusJson() const;

  // What the source is doing, for the UI: a state code and a detail
  // (SdrconnectSource lists them; "playback" for a recording).
  void setSourceState(const std::string& state, const std::string& detail);
  void setMode(const std::string& mode);  // "live" or "playback"
  void setSystemLanguage(const std::string& language);
  // The Settings dialog: `provider` gives the settings as JSON; `apply`
  // takes the dialog's fields and returns "" or an error code; `reconnect`
  // makes the source try SDRconnect again now.
  using SettingsProvider = std::function<std::string()>;
  using SettingsHandler = std::function<std::string(const std::vector<JsonField>&)>;
  void setSettingsHandlers(SettingsProvider provider, SettingsHandler apply, std::function<void()> reconnect);

  // Frequency databases (the markers above the waterfall, the listings of
  // an emission, the frequencies of the recording tasks). `list` replaces
  // the one in Config for the UI at once and for the receivers from their
  // next block. `reload` (optional) reads the lists again, for the UI's
  // "reload" command; `folder` is where the user puts them.
  void setFrequencies(std::shared_ptr<const FrequencyList> list);
  void setFrequencyReload(std::function<std::shared_ptr<const FrequencyList>()> reload, const std::string& folder);
  // Where the UI's "record" command writes (SigMF, the receiver's whole IQ);
  // "" (the default) refuses it. `allowed` false refuses it too (the demo).
  void setRecordings(const std::string& folder, bool allowed);
  // Where received pictures (radiofax) are written as PNG files, served
  // to the UI under /pictures/; "" (the default): they are not kept.
  void setPictures(const std::string& folder);
  // What the decoders spend the processing on (DecoderHost::Config's
  // maxSessions, measureUnknown, giveUpSeconds, minSnrDb, disabled): for
  // every receiver, from its next block.
  void setDecoderPolicy(const DecoderHost::Config& policy);
  // Where the processing goes, per receiver, as the UI shows it (the last
  // "usage" message of each).
  std::string usageJson() const;
  // The lists the UI's "download_databases" fetches into the frequency
  // folder (by default KiwiSDR's community labels and their categories,
  // and EiBi's schedule of the current season), each kept only if it reads
  // as at least `minEntries` entries (0: kept as it is, not a list).
  struct Download {
    std::string name, url, file;
    std::size_t minEntries = 0;
    std::string fallbackUrl, fallbackFile;  // tried when url fails (EiBi: the previous season)
  };
  void setDownloads(std::vector<Download> downloads);
  static std::vector<Download> defaultDownloads(double unixTime);
  // Opens a folder in the system's file manager (the UI's "open_folder").
  void setFolderOpener(std::function<void(const std::string&)> open);
  // The UI's "diagnostics" command: `make` writes a diagnostic report
  // (`rates`: also measure each sample rate), passes its progress lines to
  // `progress`, and returns the report's path, or "" with `error` set. It
  // runs in a thread of its own; the browsers get "diagnostics" messages
  // (state running, line, done with the file's name, failed). Reports are
  // served from `folder` under /reports/.
  using DiagnosticsFn =
      std::function<std::string(bool rates, const std::function<void(const std::string&)>& progress, std::string* error)>;
  void setDiagnostics(DiagnosticsFn make, const std::string& folder);

 private:
  struct Receiver;
  void addReceiverLocked(int rx, const std::string& name, bool controllable);
  void run(Receiver& r);
  void onEmission(Receiver& r, Pipeline::EmissionEvent k, const Emission& e);
  void onDecode(Receiver& r, const DecodeEvent& d);
  void onFeatures(Receiver& r, uint64_t id, const EmissionFeatures& f);
  void onWaterfall(Receiver& r, const WaterfallRow& row);
  void onWsOpen(int client);
  void onWsMessage(int client, const std::string& text);
  HttpResponse apiEmissions(const HttpRequest& q);
  HttpResponse apiDecodes(const HttpRequest& q);
  HttpResponse apiExport(const HttpRequest& q, bool csv);
  HttpResponse apiMarkers(const HttpRequest& q);
  std::string databasesJson() const;
  std::string tasksJson() const;
  std::string myMarkersJson() const;
  // The user's windows on the waterfall: decode_windows.json in the frequency
  // folder; with the markers that have a decoder, the receivers' rules.
  std::string windowsJson() const;
  std::string saveWindow(const std::vector<JsonField>& args);
  std::string deleteWindow(const std::string& id);
  void reloadRules();
  std::string saveMarker(const std::vector<JsonField>& args);
  std::string deleteMarker(const std::string& id);
  void reloadDatabases();
  void runDownloads();
  void runDiagnostics(bool rates);
  std::string recordingJson(const Receiver& r) const;
  std::string record(int rx, const std::vector<JsonField>& args);
  void recordBlock(Receiver& r, const IqInput& b);
  void stopRecording(Receiver& r, const char* why);
  // Every 2 s: where the receiver's processing went since the last report.
  void reportUsage(Receiver& r);
  std::shared_ptr<const FrequencyList> frequencies() const;

  Config cfg_;
  std::string source_;
  WebServer web_;
  mutable std::mutex storeMu_;
  Store store_;
  std::map<int, std::unique_ptr<Receiver>> rx_;
  mutable std::mutex rxMu_;
  std::mutex controlMu_;  // held while a command runs, so the source cannot go away under it
  ControlFn control_;
  std::atomic<uint64_t> dropped_{0};
  std::atomic<bool> started_{false};
  mutable std::mutex sideMu_;  // the members below
  std::string mode_ = "live", sourceState_, sourceDetail_, systemLanguage_ = "en";
  SettingsProvider settingsProvider_;
  SettingsHandler settingsApply_;
  std::function<void()> reconnect_;
  std::shared_ptr<const FrequencyList> frequencies_;
  std::function<std::shared_ptr<const FrequencyList>()> frequencyReload_;
  std::function<void(const std::string&)> openFolder_;
  std::string frequenciesFolder_, recordingsFolder_, picturesFolder_;
  bool recordingAllowed_ = false;
  std::vector<Download> downloads_;
  std::thread downloadThread_;
  std::atomic<bool> downloading_{false};
  std::mutex markersMu_;  // the user's markers file
  DiagnosticsFn diagnostics_;
  std::string reportsFolder_;
  std::thread diagnosticsThread_;
  std::atomic<bool> diagnosing_{false};
};

// The user's own markers: the file in the frequency folder, and reading it
// into a list (FrequencyList::addEntry, source "my markers").
const char* myMarkersFile();
void addMyMarkers(const std::string& path, FrequencyList& list);

// The recording tasks (catalogue/recording_tasks.json), embedded.
const std::string& recordingTasksText();

// Live messages, as sent to the browsers (one line of JSON each).
std::string emissionJson(int rx, const char* event, const Emission& e, const std::string& label,
                         const std::string& features = "");
std::string decodeJson(int rx, const DecodeEvent& d);
// Waterfall row: "WF01", rx (u8), bins (u16 LE), utc, lowHz, highHz
// (f64 LE), then one byte per bin: 40 + 4 * (dB above the row's median
// floor), clipped to 0..255.
std::vector<uint8_t> waterfallMessage(int rx, const WaterfallRow& row);

// The page (app/ui/index.html), embedded at build time.
const std::string& uiPage();
// The program's icon (app/resources/hf_classifier.png), for the page.
const std::string& uiIcon();

}  // namespace hfc
