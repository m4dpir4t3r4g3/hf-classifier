// Tests of running the program by itself: settings, the command line, the
// connection states shown to the user, the Settings dialog's commands, and
// starting a headless SDRconnect when nothing answers.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "classifier_app.hpp"
#include "hfc/file.hpp"
#include "hfc/net.hpp"
#include "hfc/websocket.hpp"
#include "runner.hpp"
#include "sdrconnect_source.hpp"
#include "settings.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

bool waitFor(const std::function<bool()>& cond, int ms) {
  for (int t = 0; t < ms; t += 20) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// A port nothing listens on (taken and given back).
uint16_t freePort() {
  TcpListener l;
  l.listen("127.0.0.1", 0, nullptr);
  return l.port();
}

std::string readAll(const std::string& path) {
  std::FILE* f = openFile(path, "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// The next text message whose "type" is `type`.
std::string nextOfType(WebSocket& ws, const std::string& type) {
  WsMessage m;
  for (int i = 0; i < 200 && ws.receive(m, nullptr); ++i) {
    if (m.binary) continue;
    std::vector<JsonField> f;
    if (parseJsonObject(m.text(), f, nullptr) && jsonField(f, "type") == type) return m.text();
  }
  return "";
}

}  // namespace

TEST(settings_round_trip_and_validation) {
  const std::string path = "hfc_test_settings.json";
  std::remove(path.c_str());
  Settings s;
  std::string err;
  CHECK(loadSettings(path, s, &err));  // no file: the defaults
  CHECK(s == Settings());
  s.language = "bg";
  s.sdrHost = "192.168.1.20";
  s.sdrPort = 5455;
  s.tuner = "both";
  s.dualSerial = "1806030000";
  s.startHeadless = false;
  s.headlessPath = "C:\\Program Files\\SDRplay\\SDRconnect\\SDRconnect_headless.exe";
  s.httpPort = 8080;
  s.openWindow = false;
  s.ft8 = false;
  s.maxSessions = 20;
  s.measureUnknown = false;
  s.giveUpSeconds = 45;
  s.minSnrDb = 6;
  s.disabledDecoders = {"PSK", "FT8", "4:3 code"};
  CHECK(saveSettings(path, s, &err));
  Settings t;
  CHECK(loadSettings(path, t, &err));
  CHECK(t == s);
  CHECK(contains(readAll(path), "\"tuner\": \"both\""));
  // A value the dialog would refuse leaves the settings as they were.
  Settings u = s;
  std::string code;
  CHECK(!applySettingsFields({{"tuner", "3", true}}, u, &code) && code == "bad_tuner" && u == s);
  CHECK(!applySettingsFields({{"sdr_port", "70000", false}}, u, &code) && code == "bad_port");
  CHECK(!applySettingsFields({{"sdr_host", "-x", true}}, u, &code) && code == "bad_host");
  CHECK(!applySettingsFields({{"sdr_host", "a b", true}}, u, &code) && code == "bad_host");
  CHECK(!applySettingsFields({{"language", "de", true}}, u, &code) && code == "bad_language");
  // Where the processing goes: values checked, families comma-separated.
  CHECK(!applySettingsFields({{"give_up_seconds", "-1", false}}, u, &code) && code == "bad_give_up");
  CHECK(!applySettingsFields({{"min_snr_db", "99", false}}, u, &code) && code == "bad_snr");
  CHECK(applySettingsFields({{"disabled_decoders", " CW, RTTY,,CW ", true}, {"measure_unknown", "true", false}}, u, &code));
  CHECK(u.disabledDecoders == std::vector<std::string>({"CW", "RTTY"}) && u.measureUnknown);
  CHECK(contains(settingsJson(u), "\"disabled_decoders\":\"CW,RTTY\"") && contains(settingsJson(u), "\"give_up_seconds\":45"));
  CHECK(!applySettingsFields({{"dual_serial", "1806;rm", true}}, u, &code) && code == "bad_serial");
  // Unknown keys (the command's own, a newer version's) are ignored.
  CHECK(applySettingsFields({{"cmd", "settings", true}, {"tuner", "2", true}, {"future", "x", true}}, u, &code));
  CHECK(u.tuner == "2");
  // A file that is not JSON is reported, and the settings stay.
  std::FILE* f = openFile(path, "wb");
  std::fputs("not json", f);
  std::fclose(f);
  Settings v;
  CHECK(!loadSettings(path, v, &err) && v == Settings());
  std::remove(path.c_str());
}

TEST(command_line_overrides_settings) {
  RunOptions o;
  o.settings.tuner = "2";
  std::string err;
  bool help = false;
  CHECK(parseCommandLine({"--host", "10.0.0.5", "--port", "5460", "--tuner", "both", "--no-headless", "--http", "0",
                          "--db", "x.db", "--language", "bg", "--no-ft8", "--play", "f.sigmf-meta", "--speed", "0",
                          "--exit", "--open"},
                         o, &err, &help));
  CHECK(!help);
  CHECK(o.settings.sdrHost == "10.0.0.5" && o.settings.sdrPort == 5460 && o.settings.tuner == "both");
  CHECK(!o.settings.startHeadless && o.settings.httpPort == 0 && o.dbPath == "x.db");
  CHECK(o.settings.language == "bg" && !o.settings.ft8 && o.play == "f.sigmf-meta" && o.speed == 0);
  CHECK(o.exitAtEnd && o.openWindow);
  RunOptions bad;
  CHECK(!parseCommandLine({"--tuner", "3"}, bad, &err, &help));
  CHECK(!parseCommandLine({"--port"}, bad, &err, &help));
  CHECK(!parseCommandLine({"--frobnicate"}, bad, &err, &help));
  RunOptions h;
  CHECK(parseCommandLine({"--help"}, h, &err, &help) && help);
  // The demo: its own log and the next port, unless given.
  RunOptions d;
  d.settings.httpPort = 8073;
  d.dbPath = "real.db";
  CHECK(parseCommandLine({"--demo"}, d, &err, &help) && d.demo && d.settings.httpPort == 8074 && d.dbPath != "real.db" &&
        d.dbPath.find("demo.db") != std::string::npos);
  RunOptions d2;
  CHECK(parseCommandLine({"--demo", "--http", "9000", "--db", "x.db"}, d2, &err, &help) && d2.settings.httpPort == 9000 &&
        d2.dbPath == "x.db");
}

TEST(source_reports_when_nothing_answers) {
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "test", nullptr));
  SdrconnectOptions opt;
  opt.port = freePort();
  opt.startHeadless = false;
  std::atomic<bool> stop{false};
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });
#ifdef _WIN32
  // Windows tells more, depending on the machine: SDRconnect not installed
  // (a build machine), its window running with the server off, or else
  // just no answer (installed, headless start turned off).
  CHECK(waitFor([&] {
    const std::string s = app.statusJson();
    return contains(s, "\"source_state\":\"not_installed\"") || contains(s, "\"source_state\":\"websocket_off\"") ||
           contains(s, "\"source_state\":\"no_answer\"");
  }, 8000));
#else
  // Elsewhere, whether SDRconnect runs cannot be told: "no answer".
  CHECK(waitFor([&] { return contains(app.statusJson(), "\"source_state\":\"no_answer\""); }, 8000));
  CHECK(contains(app.statusJson(), "\"source_detail\":\"127.0.0.1:" + std::to_string(opt.port) + "\""));
#endif
  stop = true;
  t.join();
  app.stop();
}

TEST(settings_dialog_commands_reach_the_runner) {
  const std::string path = "hfc_test_run_settings.json";
  std::remove(path.c_str());
  RunOptions o;
  o.settingsPath = path;
  o.dbPath = ":memory:";
  o.settings.httpPort = 0;
  o.settings.sdrPort = freePort();
  o.settings.startHeadless = false;
  std::atomic<bool> stop{false};
  std::string url;
  std::mutex m;
  RunHooks hooks;
  hooks.ready = [&](const std::string& u) {
    std::lock_guard<std::mutex> l(m);
    url = u;
  };
  std::thread t([&] { runClassifier(o, stop, hooks); });
  CHECK(waitFor([&] {
    std::lock_guard<std::mutex> l(m);
    return !url.empty();
  }, 5000));
  uint16_t port;
  {
    std::lock_guard<std::mutex> l(m);
    port = static_cast<uint16_t>(std::atoi(url.substr(url.rfind(':') + 1).c_str()));
  }
  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", port, "/ws", 2000, nullptr));
  const std::string status = nextOfType(ws, "status");
  CHECK(contains(status, "\"mode\":\"live\"") && contains(status, "\"settings\":{"));
  CHECK(contains(status, "\"system_language\":"));
  // A good change: saved, and the next status shows it.
  ws.sendText("{\"cmd\":\"settings\",\"tuner\":\"both\",\"language\":\"bg\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  CHECK(contains(readAll(path), "\"tuner\": \"both\"") && contains(readAll(path), "\"language\": \"bg\""));
  // This run's own options (here, as if from its command line: any free
  // port, no headless start) are not saved with the change.
  CHECK(contains(readAll(path), "\"http_port\": 8073") && contains(readAll(path), "\"start_headless\": true"));
  // A bad one: refused with a code the UI words, nothing saved.
  ws.sendText("{\"cmd\":\"settings\",\"sdr_port\":\"99999\"}");
  const std::string r = nextOfType(ws, "result");
  CHECK(contains(r, "\"ok\":false") && contains(r, "\"message\":\"bad_port\""));
  CHECK(!contains(readAll(path), "99999"));
  ws.sendText("{\"cmd\":\"reconnect\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  ws.shutdown();
  stop = true;
  t.join();
  std::remove(path.c_str());
}

#ifdef HFC_FAKE_SDRCONNECT
TEST(source_starts_a_headless_sdrconnect_and_stops_it_again) {
  // Nothing answers; the "headless SDRconnect" (here the fake, which takes
  // the same --websocket_port option) is started, the program connects to
  // it, and stops it at the end.
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "test", nullptr));
  SdrconnectOptions opt;
  opt.port = freePort();
  opt.startHeadless = true;
  opt.headlessPath = HFC_FAKE_SDRCONNECT;
  opt.retrySeconds = 0.5;
  std::atomic<bool> stop{false};
  {
    SdrconnectSource src(app);
    std::thread t([&] { src.run(opt, stop); });
    CHECK(waitFor([&] { return contains(app.statusJson(), "\"source_state\":\"connected\""); }, 15000));
    stop = true;
    t.join();
  }
  // Stopped: nothing listens there any more.
  TcpSocket s;
  CHECK(!s.connect("127.0.0.1", opt.port, 1000, nullptr));
  app.stop();
}
#endif
