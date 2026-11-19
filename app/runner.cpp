#include "runner.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "classifier_app.hpp"
#include "diagnostics.hpp"
#include "log_capture.hpp"
#include "demo_scene.hpp"
#include "hfc/frequency_list.hpp"
#include "hfc/probe.hpp"
#include "hfc/websocket.hpp"
#include "hfc/sigmf.hpp"
#include "platform.hpp"
#include "sdrconnect_source.hpp"

namespace hfc {

// The decoders' policy from the settings (the rest of `base` kept).
static DecoderHost::Config decoderPolicy(const Settings& s, DecoderHost::Config base) {
  base.maxSessions = s.maxSessions;
  base.measureUnknown = s.measureUnknown;
  base.giveUpSeconds = s.giveUpSeconds;
  base.minSnrDb = s.minSnrDb;
  base.disabled = std::set<std::string>(s.disabledDecoders.begin(), s.disabledDecoders.end());
  return base;
}


namespace {

int playback(ClassifierApp& app, const RunOptions& o, const std::atomic<bool>& stop) {
  SigmfReader rd;
  std::string err;
  if (!rd.open(o.play, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    app.setSourceState("playback_failed", err);
    return kRunFailed;
  }
  const double rate = rd.sampleRate();
  std::printf("playing %s: %.0f S/s, %.1f s, %s\n", o.play.c_str(), rate, static_cast<double>(rd.samples()) / rate,
              rd.datatype().c_str());
  app.setReceiver(0, "Playback", false);
  app.setSourceState("playback", o.play);
  // Stretches without a time (the reader counts them from 0, or on from the
  // last stretch that had one) play as if received now.
  const double base = nowUtc();
  const auto block = static_cast<std::size_t>(std::max(1.0, rate / 20));
  std::vector<int16_t> buf(2 * block);
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t sinceDrain = 0;
  SigmfReader::Capture cap;
  double utc;
  std::size_t n;
  while (!stop && (n = rd.read(buf.data(), block, cap, utc)) > 0) {
    IqInput in;
    in.iq.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(2 * n));
    in.sampleRate = rate;
    in.centreHz = cap.centerHz;
    in.utc = utc > 1e9 ? utc : utc + base;  // 1e9: September 2001
    app.feed(0, std::move(in));
    sinceDrain += n;
    if (o.speed > 0) {
      const double due = static_cast<double>(rd.position()) / rate / o.speed;
      std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                             std::chrono::duration<double>(due)));
    } else if (static_cast<double>(sinceDrain) > 2 * rate) {
      app.drain();  // as fast as possible, but never more queued than the app keeps
      sinceDrain = 0;
    }
  }
  app.flushReceivers();
  app.setSourceState("playback_done", o.play);
  std::printf("playback done in %.1f s\n",
              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  if (!o.exitAtEnd) {
    std::printf("the UI stays up\n");
    while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return kRunOk;
}

// The demo: the synthetic band of testing/demo_scene.hpp, its parts one
// after the other, in a loop, until stopped.
int demo(ClassifierApp& app, const RunOptions& o, const std::atomic<bool>& stop) {
  std::printf("demo: making the synthetic band...\n");
  std::fflush(stdout);
  const std::vector<testing::DemoPart> parts = testing::demoScene();
  app.setReceiver(0, "Demo", false);
  app.setSourceState("demo", "");
  const double speed = o.speed > 0 ? o.speed : 1.0;
  const auto t0 = std::chrono::steady_clock::now();
  // On a 15 s boundary, as FT8 and FT4 need (the parts last whole slots).
  const double utc0 = std::floor(nowUtc() / 15.0) * 15.0;
  double played = 0;  // seconds of IQ fed so far
  for (int round = 0; !stop; ++round) {
    for (const auto& p : parts) {
      std::printf("demo: %s\n", p.description.c_str());
      std::fflush(stdout);
      const std::size_t pairs = p.iq.size() / 2;
      const auto block = static_cast<std::size_t>(p.sampleRate / 20);
      for (std::size_t i = 0; i < pairs && !stop; i += block) {
        const std::size_t n = std::min(block, pairs - i);
        IqInput in;
        in.iq.assign(p.iq.begin() + static_cast<std::ptrdiff_t>(2 * i), p.iq.begin() + static_cast<std::ptrdiff_t>(2 * (i + n)));
        in.sampleRate = p.sampleRate;
        in.centreHz = p.centreHz;
        in.utc = utc0 + played;
        app.feed(0, std::move(in));
        played += static_cast<double>(n) / p.sampleRate;
        std::this_thread::sleep_until(t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                               std::chrono::duration<double>(played / speed)));
      }
      if (stop) break;
    }
    if (o.exitAtEnd) break;
  }
  app.flushReceivers();
  return kRunOk;
}

// What every diagnostic report has: the settings, the files, where SDRconnect is.
DiagnosticInputs reportInputs(const RunOptions& o, const Settings& s, const std::string& mode) {
  DiagnosticInputs in;
  in.folder = o.reportsDir.empty() ? dataFile("reports") : o.reportsDir;
  in.dataFolder = dataDirectory();
  in.mode = mode;
  in.settingsJson = settingsJson(s);
  in.sdrHost = s.sdrHost;
  in.sdrPort = s.sdrPort;
  in.headlessPath = s.headlessPath;
  in.files = {{"settings.json", o.settingsPath},
              {"hf_classifier.log", dataFile("hf_classifier.log")},
              {"hf_classifier.previous.log", dataFile("hf_classifier.previous.log")},
              {"sdrconnect_headless.log", dataFile("sdrconnect_headless.log")}};
  in.programLog = capturedOutput();
  return in;
}

ProbeOptions reportProbeOptions(double seconds, bool rates, std::string& log,
                                const std::function<void(const std::string&)>& progress) {
  ProbeOptions po;
  po.seconds = seconds;
  po.tryRates = rates;
  po.say = [&log, progress](const std::string& line) {
    log += line + "\n";
    if (progress) progress(line);
  };
  return po;
}

// Asks the program running on `port` for a report; "" with `error` if it
// cannot be asked.
std::string reportFromRunning(uint16_t port, bool rates, const std::function<void(const std::string&)>& progress,
                              std::string* error) {
  WebSocket ws;
  std::string err;
  if (!ws.connect("127.0.0.1", port, "/ws", 3000, &err)) {
    if (error) *error = "cannot reach the running program: " + err;
    return "";
  }
  ws.sendText(std::string("{\"cmd\":\"diagnostics\",\"rates\":\"") + (rates ? "1" : "0") + "\"}");
  const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  WsMessage m;
  while (std::chrono::steady_clock::now() < until && ws.receive(m, nullptr)) {
    if (m.binary) continue;
    std::vector<JsonField> f;
    if (!parseJsonObject(m.text(), f, nullptr)) continue;
    const std::string type = jsonField(f, "type");
    if (type == "result" && jsonField(f, "cmd") == "diagnostics" && jsonField(f, "ok") == "false") {
      if (error) *error = "the running program refused: " + jsonField(f, "message");
      return "";
    }
    if (type != "diagnostics") continue;
    const std::string state = jsonField(f, "state");
    if (state == "line" && progress) progress(jsonField(f, "line"));
    if (state == "done") return jsonField(f, "path");
    if (state == "failed") {
      if (error) *error = jsonField(f, "message");
      return "";
    }
  }
  if (error) *error = "the running program did not finish the report";
  return "";
}

}  // namespace

std::string makeReport(const RunOptions& opt, bool rates, const std::function<void(const std::string&)>& progress,
                       std::string* error) {
  const uint16_t port = static_cast<uint16_t>(opt.settings.httpPort);
  std::string note;
  if (port != 0 && otherInstanceAt(port)) {
    if (progress) progress("HF Classifier is running: asking it for the report");
    std::string err;
    const std::string path = reportFromRunning(port, rates, progress, &err);
    if (!path.empty()) return path;
    note = err + "; made without it";
    if (progress) progress(note);
  }
  DiagnosticInputs in = reportInputs(opt, opt.settings, "report only");
  in.note = note;
  // Measuring on a connection of its own, unless the running program holds
  // SDRconnect (two programs changing its settings would disturb each other).
  if (!note.empty()) {
    in.probeSkipped = "the program runs; not measured beside it";
  } else {
    ProbeLevels levels;
    sdrconnect::Client::Handlers h;
    h.onIq = [&levels](sdrconnect::Device d, const sdrconnect::IqBlock& b) { levels.add(d, b); };
    sdrconnect::Client c(h);
    std::string err;
    if (progress) progress("connecting to SDRconnect at " + opt.settings.sdrHost + ":" + std::to_string(opt.settings.sdrPort));
    if (!c.connect(opt.settings.sdrHost, static_cast<uint16_t>(opt.settings.sdrPort), 3000, &err)) {
      in.probeSkipped = "no_answer: " + err;
      if (progress) progress("SDRconnect does not answer (" + err + "): not measured");
    } else {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      in.probeJson = runProbe(c, levels, reportProbeOptions(opt.reportProbeSeconds, rates, in.probeLog, progress));
      c.disconnect();
    }
  }
  return writeDiagnosticReport(in, error);
}

std::string usageText() {
  char buf[8192];
  std::snprintf(
      buf, sizeof(buf),
      "hf_classifier %s: finds, classifies and decodes HF emissions in SDRconnect's IQ\n\n"
      "usage: hf_classifier [options]            live, from SDRconnect\n"
      "       hf_classifier --play FILE [options] playback of a SigMF recording\n\n"
      "Settings come from %s (the Settings dialog writes it);\n"
      "these options override them for this run.\n\n"
      "SDRconnect:\n"
      "  --host ADDR        SDRconnect address (default 127.0.0.1)\n"
      "  --port N           its WebSocket port (default 5454)\n"
      "  --tuner T          1 (default), 2, or both (RSPduo dual tuner mode)\n"
      "  --dual SERIAL      switch the RSPduo to dual tuner mode first (\"SERIAL:Dual\")\n"
      "  --rate HZ          set the sample rate first\n"
      "  --centre HZ        tune first\n"
      "  --headless PATH    SDRconnect_headless to start when SDRconnect is not running\n"
      "  --no-headless      never start SDRconnect_headless\n"
      "Playback:\n"
      "  --play FILE        a .sigmf-meta / .sigmf-data (ci16_le, cf32_le, ci8, cu8)\n"
      "  --speed X          1 = real time (default), 0 = as fast as possible\n"
      "  --exit             quit when the recording ends (default: keep the UI up)\n"
      "  --demo             no receiver: a synthetic band (amateur and utility signals) in a loop,\n"
      "                     logged to demo.db, on the window port + 1\n"
      "Common:\n"
      "  --http PORT        web UI port (default 8073; 0 = any free port)\n"
      "  --db PATH          log database (default: in the data folder)\n"
      "  --settings PATH    settings file (default: in the data folder)\n"
      "  --frequencies DIR  frequency lists to read (default: the frequencies folder in the data folder)\n"
      "  --recordings DIR   where recordings made from the window go (default: recordings in the data folder)\n"
      "  --pictures DIR     where received pictures (radiofax) go (default: pictures in the data folder)\n"
      "  --open / --no-open open the window at start (or not)\n"
      "  --language bg|en   language of the UI\n"
      "  --max-sessions N   emissions decoded at once (default 48)\n"
      "  --no-ft8           do not decode the FT8/FT4 sub-bands\n"
      "Diagnostics:\n"
      "  --report           make a diagnostic report (a ZIP in the reports folder) and exit\n"
      "  --report-rates     the same, also measuring each sample rate\n",
      HFC_VERSION, dataFile("settings.json").c_str());
  return buf;
}

RunOptions defaultRunOptions() {
  RunOptions o;
  o.settingsPath = dataFile("settings.json");
  o.dbPath = dataFile("hf_classifier.db");
  o.frequenciesDir = dataFile("frequencies");
  o.recordingsDir = dataFile("recordings");
  o.picturesDir = dataFile("pictures");
  o.reportsDir = dataFile("reports");
  std::string err;
  if (!loadSettings(o.settingsPath, o.settings, &err)) std::fprintf(stderr, "settings: %s (using defaults)\n", err.c_str());
  o.fileSettings = o.settings;
  o.openWindow = o.settings.openWindow;
  return o;
}

bool parseCommandLine(const std::vector<std::string>& args, RunOptions& o, std::string* error, bool* help) {
  Settings& s = o.settings;
  bool openSet = false, httpSet = false, dbSet = false, picturesSet = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](std::string& v) {
      if (i + 1 >= args.size()) {
        if (error) *error = a + " needs a value";
        return false;
      }
      v = args[++i];
      return true;
    };
    std::string v;
    if (a == "-h" || a == "--help") {
      if (help) *help = true;
      return true;
    } else if (a == "--settings") {
      if (!next(v)) return false;
      // Read first, so that the other options override it.
      o.settingsPath = v;
      Settings t;
      std::string err;
      if (!loadSettings(v, t, &err)) {
        if (error) *error = err;
        return false;
      }
      s = t;
      o.fileSettings = t;
      if (!openSet) o.openWindow = false;
    } else if (a == "--host") {
      if (!next(s.sdrHost)) return false;
    } else if (a == "--port") {
      if (!next(v)) return false;
      s.sdrPort = std::atoi(v.c_str());
    } else if (a == "--tuner") {
      if (!next(s.tuner)) return false;
    } else if (a == "--dual") {
      if (!next(s.dualSerial)) return false;
    } else if (a == "--rate") {
      if (!next(o.rate)) return false;
    } else if (a == "--centre" || a == "--center") {
      if (!next(o.centre)) return false;
    } else if (a == "--headless") {
      if (!next(s.headlessPath)) return false;
      s.startHeadless = true;
    } else if (a == "--no-headless") {
      s.startHeadless = false;
    } else if (a == "--demo") {
      o.demo = true;
    } else if (a == "--play") {
      if (!next(o.play)) return false;
    } else if (a == "--speed") {
      if (!next(v)) return false;
      o.speed = std::atof(v.c_str());
    } else if (a == "--exit") {
      o.exitAtEnd = true;
    } else if (a == "--http") {
      if (!next(v)) return false;
      s.httpPort = std::atoi(v.c_str());
      httpSet = true;
    } else if (a == "--db") {
      if (!next(o.dbPath)) return false;
      dbSet = true;
    } else if (a == "--frequencies") {
      if (!next(o.frequenciesDir)) return false;
    } else if (a == "--recordings") {
      if (!next(o.recordingsDir)) return false;
    } else if (a == "--pictures") {
      if (!next(o.picturesDir)) return false;
      picturesSet = true;
    } else if (a == "--open") {
      o.openWindow = openSet = true;
    } else if (a == "--no-open") {
      o.openWindow = false;
      openSet = true;
    } else if (a == "--language") {
      if (!next(s.language)) return false;
    } else if (a == "--max-sessions") {
      if (!next(v)) return false;
      s.maxSessions = std::atoi(v.c_str());
    } else if (a == "--no-ft8") {
      s.ft8 = false;
    } else if (a == "--report") {
      o.report = true;
    } else if (a == "--report-rates") {
      o.report = o.reportRates = true;
    } else {
      if (error) *error = "unknown option " + a;
      return false;
    }
  }
  // The demo keeps its own log, and its own port: it can run beside the
  // program proper.
  if (o.demo) {
    if (!dbSet) o.dbPath = dataFile("demo.db");
    if (!picturesSet) o.picturesDir = dataFile("demo-pictures");
    if (!httpSet && s.httpPort != 0) s.httpPort = s.httpPort < 65535 ? s.httpPort + 1 : s.httpPort - 1;
  }
  // The same checks as the Settings dialog's.
  std::vector<JsonField> check = {{"tuner", s.tuner, true},
                                  {"sdr_port", std::to_string(s.sdrPort), false},
                                  {"http_port", std::to_string(s.httpPort), false},
                                  {"sdr_host", s.sdrHost, true},
                                  {"language", s.language, true},
                                  {"dual_serial", s.dualSerial, true},
                                  {"max_sessions", std::to_string(s.maxSessions), false}};
  Settings t = s;
  std::string code;
  if (!applySettingsFields(check, t, &code)) {
    if (error) *error = "invalid value (" + code + ")";
    return false;
  }
  return true;
}

std::string effectiveLanguage(const Settings& s) { return s.language.empty() ? systemLanguage() : s.language; }

int runClassifier(const RunOptions& opt, const std::atomic<bool>& stop, const RunHooks& hooks) {
  const uint16_t httpPort = static_cast<uint16_t>(opt.settings.httpPort);
  // Started twice (a second click on the shortcut): show the first one.
  if (httpPort != 0 && otherInstanceAt(httpPort)) {
    const std::string url = "http://127.0.0.1:" + std::to_string(httpPort) + "/";
    std::printf("already running: %s\n", url.c_str());
    openWindow(url);
    return kRunAlreadyRunning;
  }
#ifdef _WIN32
  // Not "efficiency mode", which the tray program (no window) would get.
  std::printf("full speed (not efficiency mode): %s\n", preferFullSpeed() ? "yes" : "not available");
#endif
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = opt.dbPath;
  cfg.httpPort = httpPort;
  cfg.pipeline.decoders = decoderPolicy(opt.settings, cfg.pipeline.decoders);
  cfg.pipeline.decoders.ftx = opt.settings.ft8;
  // Known frequencies: the built-in ones, and every file in the folder (made
  // if missing, so that there is somewhere to put them). EiBi's schedule
  // comes with the program; a sked-*.csv in the folder replaces it.
  const std::string frequenciesDir = opt.frequenciesDir;
  const bool demoList = opt.demo;
  auto loadFrequencies = [frequenciesDir, demoList](bool print) -> std::shared_ptr<const FrequencyList> {
    auto list = FrequencyList::builtin();
    std::size_t files = 0;
    bool ownEibi = false;
    if (!frequenciesDir.empty()) {
      std::error_code ec;
      const auto dir = std::filesystem::u8path(frequenciesDir);
      std::filesystem::create_directories(dir, ec);
      std::vector<std::string> paths;
      for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) paths.push_back(it->path().u8string());
      std::sort(paths.begin(), paths.end());
      // Of EiBi's schedules (sked-a26.csv, sked-b26.csv ...), only the newest:
      // older seasons are stale and would list every station twice.
      std::string newestSked;
      auto season = [](const std::string& n) {  // "sked-b26.csv" -> 26 * 2 + 1
        if (n.size() < 9 || (std::tolower(static_cast<unsigned char>(n[5])) != 'a' && std::tolower(static_cast<unsigned char>(n[5])) != 'b')) return -1;
        return std::atoi(n.substr(6, 2).c_str()) * 2 + (std::tolower(static_cast<unsigned char>(n[5])) == 'b');
      };
      for (const auto& p : paths) {
        const std::string name = std::filesystem::u8path(p).filename().u8string();
        if (name.rfind("sked-", 0) == 0 && (newestSked.empty() || season(name) > season(newestSked))) newestSked = name;
      }
      for (const auto& p : paths) {
        const std::string name = std::filesystem::u8path(p).filename().u8string();
        // Not lists: the user's markers (read below), KiwiSDR's categories,
        // the user's decode windows, files being written.
        if (name == myMarkersFile() || name == "dx_community_config.json" || name == "decode_windows.json" ||
            (name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0))
          continue;
        if (name.rfind("sked-", 0) == 0 && name != newestSked) {
          if (print) std::printf("frequency list %s: skipped (%s is newer)\n", p.c_str(), newestSked.c_str());
          continue;
        }
        const std::size_t n = list->addFile(p);
        if (print) std::printf("frequency list %s: %zu entries\n", p.c_str(), n);
        files += n ? 1 : 0;
        ownEibi = ownEibi || (n > 0 && name.rfind("sked-", 0) == 0);
      }
      if (!ownEibi) list->addText(FrequencyList::bundledEibi(), FrequencyList::bundledEibiName());
      addMyMarkers((dir / myMarkersFile()).u8string(), *list);
    }
    if (demoList) list->addText(testing::demoFrequencyList(), "demo");
    if (print)
      std::printf("known frequencies: %zu (%zu lists from %s)\n", list->size(), files,
                  frequenciesDir.empty() ? "nowhere" : frequenciesDir.c_str());
    return list;
  };
  cfg.pipeline.decoders.frequencies = loadFrequencies(true);
  const bool live = opt.play.empty() && !opt.demo;
  const std::string source =
      live ? "SDRconnect " + opt.settings.sdrHost + ":" + std::to_string(opt.settings.sdrPort) : opt.demo ? "demo" : opt.play;
  app.setMode(live ? "live" : "playback");
  app.setFrequencyReload([loadFrequencies] { return loadFrequencies(false); }, opt.frequenciesDir);
  app.setRecordings(opt.recordingsDir, !opt.demo);  // a recording of the demo would count as one of a real signal
  app.setPictures(opt.picturesDir);
  app.setFolderOpener([](const std::string& path) { openFolder(path); });
  app.setSystemLanguage(systemLanguage());

  // The settings as the UI shows and changes them. A change is made to
  // both those in effect and the file's, so that the file never takes up
  // this run's command-line overrides.
  std::mutex mu;
  Settings current = opt.settings;
  Settings file = opt.fileSettings;
  std::atomic<bool> restart{false};
  app.setSettingsHandlers(
      [&] {
        std::lock_guard<std::mutex> l(mu);
        return settingsJson(current);
      },
      [&](const std::vector<JsonField>& fields) -> std::string {
        std::lock_guard<std::mutex> l(mu);
        Settings t = current, f = file;
        std::string code;
        if (!applySettingsFields(fields, t, &code) || !applySettingsFields(fields, f, &code)) return code;
        if (!opt.settingsPath.empty()) {
          std::string err;
          if (!saveSettings(opt.settingsPath, f, &err)) {
            std::fprintf(stderr, "settings: %s\n", err.c_str());
            return "save_failed";
          }
        }
        if (!t.sameSource(current)) restart = true;
        // Where the processing goes: at once.
        app.setDecoderPolicy(decoderPolicy(t, cfg.pipeline.decoders));
        current = t;
        file = f;
        return "";
      },
      [&] { restart = true; });

  // Diagnostic reports: from the Settings dialog. Live, SDRconnect is
  // measured on the source's own connection.
  std::mutex srcMu;
  SdrconnectSource* activeSource = nullptr;
  const std::string runMode = live ? "live" : opt.demo ? "demo" : "playback";
  app.setDiagnostics(
      [&](bool rates, const std::function<void(const std::string&)>& progress, std::string* error) -> std::string {
        Settings s;
        {
          std::lock_guard<std::mutex> l(mu);
          s = current;
        }
        DiagnosticInputs in = reportInputs(opt, s, runMode);
        if (!live) {
          in.probeSkipped = runMode;
        } else {
          progress("measuring SDRconnect");
          std::lock_guard<std::mutex> l(srcMu);
          if (!activeSource) in.probeSkipped = "not_running";
          else in.probeJson = activeSource->probe(reportProbeOptions(opt.reportProbeSeconds, rates, in.probeLog, progress), 15, &in.probeSkipped);
          if (in.probeJson.empty()) progress("not measured: " + in.probeSkipped);
        }
        in.statusJson = app.statusJson();
        in.usageJson = app.usageJson();
        const std::string path = writeDiagnosticReport(in, error);
        if (!path.empty()) std::printf("diagnostic report: %s\n", path.c_str());
        std::fflush(stdout);
        return path;
      },
      opt.reportsDir.empty() ? dataFile("reports") : opt.reportsDir);

  std::string err;
  if (!app.start(cfg, source, &err)) {
    std::fprintf(stderr, "cannot start: %s\n", err.c_str());
    if (hooks.failed)
      hooks.failed(err.find("cannot listen") != std::string::npos ? "port_busy" : "start_failed", err);
    return kRunFailed;
  }
  const std::string url = "http://127.0.0.1:" + std::to_string(app.httpPort()) + "/";
  std::printf("hf_classifier %s: UI at %s, log in %s\n", HFC_VERSION, url.c_str(), opt.dbPath.c_str());
  std::fflush(stdout);
  if (hooks.ready) hooks.ready(url);
  if (opt.openWindow) openWindow(url);

  int rc = kRunOk;
  if (opt.demo) {
    rc = demo(app, opt, stop);
  } else if (!live) {
    rc = playback(app, opt, stop);
  } else {
    while (!stop) {
      Settings s;
      {
        std::lock_guard<std::mutex> l(mu);
        s = current;
      }
      SdrconnectOptions so;
      so.host = s.sdrHost;
      so.port = s.sdrPort;
      so.tuner[0] = s.tuner != "2";
      so.tuner[1] = s.tuner != "1";
      so.dualSerial = s.dualSerial;
      so.rate = opt.rate;
      so.centre = opt.centre;
      so.startHeadless = s.startHeadless;
      so.headlessPath = s.headlessPath;
      so.headlessLog = dataFile("sdrconnect_headless.log");
      restart = false;
      {
        SdrconnectSource src(app);
        {
          std::lock_guard<std::mutex> l(srcMu);
          activeSource = &src;
        }
        src.run(so, [&] { return stop.load() || restart.load(); });
        // Waits for a report's measuring that is still asking for it.
        std::lock_guard<std::mutex> l(srcMu);
        activeSource = nullptr;
      }
      app.flushReceivers();
    }
  }
  app.stop();
  const uint64_t dropped = app.droppedSamples();
  if (dropped) std::printf("%llu samples dropped: processing was too slow\n", static_cast<unsigned long long>(dropped));
  return rc;
}

}  // namespace hfc
