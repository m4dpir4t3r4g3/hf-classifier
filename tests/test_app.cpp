// Tests of the application: the web UI's HTTP and WebSocket interface on a
// playback-style feed, and live operation against the fake SDRconnect
// (emissions found, "listen" and "tune" reaching SDRconnect).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

#include "PskModulator.h"
#include <filesystem>

#include "classifier_app.hpp"
#include "fake_sdrconnect.hpp"
#include "hfc/net.hpp"
#include "hfc/sigmf.hpp"
#include "hfc/web_server.hpp"
#include "hfc/websocket.hpp"
#include "iq_scene.hpp"
#include "sdrconnect_source.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

// One HTTP GET; the whole answer (status line, header, body).
std::string httpGet(uint16_t port, const std::string& path, const std::string& host = "") {
  TcpSocket s;
  if (!s.connect("127.0.0.1", port, 2000, nullptr)) return "";
  const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " +
                          (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) + "\r\n\r\n";
  s.sendAll(req.data(), req.size());
  s.setRecvTimeout(5000);
  std::string out;
  char buf[4096];
  long r;
  while ((r = s.recvSome(buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(r));
  return out;
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

bool waitUntil(const std::function<bool()>& cond, int ms) {
  for (int t = 0; t < ms; t += 20) {
    if (cond()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return cond();
}

// Whether /api/emissions lists one centred within `tol` of `hz`.
bool hasEmissionAt(uint16_t port, double hz, double tol) {
  const std::string r = httpGet(port, "/api/emissions");
  const std::size_t b = r.find("\r\n\r\n");
  std::vector<JsonField> list, e;
  if (b == std::string::npos || !parseJsonArray(r.substr(b + 4), list, nullptr)) return false;
  for (const auto& item : list)
    if (parseJsonObject(item.value, e, nullptr) && std::fabs(std::atof(jsonField(e, "centre").c_str()) - hz) <= tol)
      return true;
  return false;
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

TEST(app_serves_the_ui_the_log_and_exports) {
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  std::string err;
  CHECK(app.start(cfg, "test", &err));
  const uint16_t port = app.httpPort();
  app.setReceiver(0, "Playback", false);

  const double rate = 62500;
  testing::IqScene sc(rate, 16);
  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
  auto audio = pm.Transmission("CQ CQ de G4XYZ G4XYZ testing the classifier k ");
  const double p = testing::meanPower(audio);
  for (auto& v : audio) v *= static_cast<float>(0.003 / std::sqrt(p));
  sc.addUsbAudio(audio, 12000, 10000 - 1000, 1);
  sc.addNoiseInBand(20 * std::log10(0.003) - 15, 2500, 4);
  const auto iq = sc.render();
  const std::size_t block = 3125;
  for (std::size_t i = 0; i < iq.size() / 2; i += block) {
    IqInput in;
    in.iq.assign(iq.begin() + static_cast<std::ptrdiff_t>(2 * i), iq.begin() + static_cast<std::ptrdiff_t>(2 * (i + block)));
    in.sampleRate = rate;
    in.centreHz = 7000000;
    in.utc = 1700000000 + static_cast<double>(i) / rate;
    app.feed(0, std::move(in));
    if (i % (block * 40) == 0) app.drain();
  }
  app.flushReceivers();
  CHECK(app.droppedSamples() == 0);

  const std::string page = httpGet(port, "/");
  CHECK(contains(page, "200 OK") && contains(page, "<title>HF класификатор</title>"));
  const std::string icon = httpGet(port, "/icon.png");
  CHECK(contains(icon, "200 OK") && contains(icon, "image/png") && contains(icon, "\x89PNG"));
  const std::string em = httpGet(port, "/api/emissions");
  CHECK(contains(em, "\"label\":\"PSK31\""));
  CHECK(hasEmissionAt(port, 7010000, 5));
  const std::string dec = httpGet(port, "/api/decodes?label=PSK31");
  CHECK(contains(dec, "G4XYZ"));
  const std::string csv = httpGet(port, "/export.csv?min_khz=7009&max_khz=7011");
  CHECK(contains(csv, "text/csv") && contains(csv, "attachment") && contains(csv, "PSK31"));
  CHECK(!contains(httpGet(port, "/export.csv?min_khz=7100"), "PSK31"));
  CHECK(contains(httpGet(port, "/export.json?label=PSK31"), "\"decoder\": \"PSK31\""));
  CHECK(contains(httpGet(port, "/nothing"), "404"));
  // Another name for this server (DNS rebinding) is refused.
  CHECK(contains(httpGet(port, "/api/emissions", "attacker.example:80"), "403"));

  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", port, "/ws", 2000, &err));
  const std::string status = nextOfType(ws, "status");
  CHECK(contains(status, "\"name\":\"Playback\"") && contains(status, "\"controllable\":false"));
  // The latest decodes are sent to a browser that connects.
  CHECK(contains(nextOfType(ws, "decode"), "PSK31"));
  ws.sendText("{\"cmd\":\"listen\",\"rx\":0,\"freq\":7010000,\"mode\":\"USB\",\"bw\":3000}");
  const std::string result = nextOfType(ws, "result");
  CHECK(contains(result, "\"ok\":false") && contains(result, "\"message\":\"not_controllable\""));
  ws.shutdown();
  app.stop();
}

// SDRconnect_headless with no radio open: connected, no IQ. The program
// selects the first radio, and the signal arrives.
TEST(app_selects_a_radio_when_no_iq_arrives) {
  testing::FakeConfig fc;
  fc.reportDelaySeconds = 0.05;
  fc.sampleRate = 250000;
  fc.centerHz = 7000000;
  fc.tones.push_back({30000, -30, 0, 0});
  fc.noDeviceUntilSelected = true;
  testing::FakeSdrconnect fake(fc);
  CHECK(fake.start(nullptr));
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "fake SDRconnect", nullptr));
  std::atomic<bool> stop{false};
  SdrconnectOptions opt;
  opt.port = fake.port();
  opt.noIqSeconds = 2;
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });
  const bool found = waitUntil([&] { return hasEmissionAt(app.httpPort(), 7030000, 20); }, 20000);
  CHECK(found);
  bool selected = false;
  // (By name from valid_devices, or by index.)
  for (const auto& e : fake.receivedEvents()) selected = selected || e.rfind("selected_device", 0) == 0;
  CHECK(selected);
  stop = true;
  t.join();
  app.stop();
  fake.stop();
}

// SDRconnect_headless as it behaved on a user's PC just after starting:
// active_device "Refreshing..." and nothing else answered. The program
// waits, then opens the radio, and the signal arrives.
TEST(app_waits_while_headless_looks_for_radios) {
  testing::FakeConfig fc;
  fc.reportDelaySeconds = 0.05;
  fc.sampleRate = 250000;
  fc.centerHz = 7000000;
  fc.tones.push_back({30000, -30, 0, 0});
  fc.refreshingSeconds = 3;
  fc.noDeviceUntilSelected = true;
  testing::FakeSdrconnect fake(fc);
  CHECK(fake.start(nullptr));
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "fake SDRconnect", nullptr));
  std::atomic<bool> stop{false};
  SdrconnectOptions opt;
  opt.port = fake.port();
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });
  CHECK(waitUntil([&] { return app.statusJson().find("\"source_state\":\"waiting_radio\"") != std::string::npos; }, 3000));
  CHECK(waitUntil([&] { return hasEmissionAt(app.httpPort(), 7030000, 20); }, 20000));
  bool byName = false;
  for (const auto& e : fake.receivedEvents()) byName = byName || e.rfind("selected_device_name:", 0) == 0;
  CHECK(byName);
  // The receiver's name is the radio's, not "Refreshing...".
  CHECK(app.statusJson().find("Refreshing") == std::string::npos);
  stop = true;
  t.join();
  app.stop();
  fake.stop();
}

// It never stops looking: the program says so and keeps saying it, rather
// than "connected" (with "Refreshing..." as the radio) or "connecting".
TEST(app_reports_no_radio_when_headless_keeps_looking) {
  testing::FakeConfig fc;
  fc.refreshingSeconds = 1e9;
  testing::FakeSdrconnect fake(fc);
  CHECK(fake.start(nullptr));
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "fake SDRconnect", nullptr));
  std::atomic<bool> stop{false};
  SdrconnectOptions opt;
  opt.port = fake.port();
  opt.radioWaitSeconds = 1.5;
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });
  CHECK(waitUntil([&] { return app.statusJson().find("\"source_state\":\"no_radio\"") != std::string::npos; }, 8000));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(app.statusJson().find("\"source_state\":\"no_radio\"") != std::string::npos);
  stop = true;
  t.join();
  app.stop();
  fake.stop();
}

TEST(app_follows_sdrconnect_and_controls_it) {
  testing::FakeConfig fc;
  fc.reportDelaySeconds = 0.05;
  fc.sampleRate = 250000;
  fc.centerHz = 7000000;
  fc.tones.push_back({30000, -30, 0, 0});
  testing::FakeSdrconnect fake(fc);
  CHECK(fake.start(nullptr));

  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "fake SDRconnect", nullptr));
  std::atomic<bool> stop{false};
  SdrconnectOptions opt;
  opt.port = fake.port();
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });

  // The carrier at +30 kHz becomes an emission.
  const bool found = waitUntil([&] { return hasEmissionAt(app.httpPort(), 7030000, 20); }, 15000);
  CHECK(found);

  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", app.httpPort(), "/ws", 2000, nullptr));
  CHECK(contains(nextOfType(ws, "status"), "\"controllable\":true"));
  ws.sendText("{\"cmd\":\"listen\",\"rx\":0,\"freq\":7030000,\"mode\":\"USB\",\"bw\":2400}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  CHECK(fake.property("device_vfo_frequency") == "7028800");
  CHECK(fake.property("demodulator") == "USB");
  CHECK(fake.property("filter_bandwidth") == "2400");
  ws.sendText("{\"cmd\":\"tune\",\"rx\":0,\"centre\":7100000}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  CHECK(fake.property("device_center_frequency") == "7100000");
  // The app follows: the receiver is now centred on 7.1 MHz, and the
  // carrier (still +30 kHz from the centre in the fake) is at 7130 kHz.
  const bool moved = waitUntil([&] { return hasEmissionAt(app.httpPort(), 7130000, 20); }, 15000);
  CHECK(moved);
  ws.shutdown();
  stop = true;
  t.join();
  app.stop();
  fake.stop();
}

TEST(app_runs_both_tuners_of_a_dual_tuner_rspduo) {
  // RSPduo in dual tuner mode: each tuner's carrier is logged under its own
  // receiver (both number their emissions from 1).
  testing::FakeConfig fc;
  fc.reportDelaySeconds = 0.05;
  fc.centerHz = 7000000;  // the fake puts the second tuner 1 MHz higher
  fc.tones.push_back({30000, -30, 0, 0});
  testing::FakeSdrconnect fake(fc);
  CHECK(fake.start(nullptr));
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "fake SDRconnect", nullptr));
  std::atomic<bool> stop{false};
  SdrconnectOptions opt;
  opt.port = fake.port();
  opt.tuner[0] = opt.tuner[1] = true;
  opt.dualSerial = fc.serial;
  SdrconnectSource src(app);
  std::thread t([&] { src.run(opt, stop); });
  const bool both = waitUntil(
      [&] { return hasEmissionAt(app.httpPort(), 7030000, 20) && hasEmissionAt(app.httpPort(), 8030000, 20); }, 20000);
  CHECK(both);
  const std::string em = httpGet(app.httpPort(), "/api/emissions");
  CHECK(contains(em, "\"rx\":0") && contains(em, "\"rx\":1"));
  stop = true;
  t.join();
  app.stop();
  fake.stop();
}

TEST(vfo_puts_the_emission_in_the_middle_of_the_passband) {
  CHECK(SdrconnectSource::vfoFor("USB", 7010000, 3000) == 7008500);
  CHECK(SdrconnectSource::vfoFor("LSB", 7010000, 3000) == 7011500);
  CHECK(SdrconnectSource::vfoFor("CW", 7010000, 500) == 7010000);
  CHECK(SdrconnectSource::vfoFor("AM", 7010000, 6000) == 7010000);
}

// Frequency databases for the UI, recording tasks, and recording from the UI.
TEST(app_serves_markers_tasks_and_records) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("hfc_rec_test_" + std::to_string(std::rand()));
  fs::remove_all(dir);
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  auto list = FrequencyList::builtin();
  list->addText("8453.0 STANAG 4285 net\n8470 Halifax weather\n", "mine.txt");
  cfg.pipeline.decoders.frequencies = list;
  std::string err;
  CHECK(app.start(cfg, "test", &err));
  const uint16_t port = app.httpPort();
  app.setReceiver(0, "Playback", false);
  app.setRecordings((dir / "rec").u8string(), true);
  int reloads = 0;
  app.setFrequencyReload(
      [&reloads]() -> std::shared_ptr<const FrequencyList> {
        ++reloads;
        auto l = FrequencyList::builtin();
        l->addText("9000 Link-11 net\n", "other.txt");
        return l;
      },
      (dir / "freq").u8string());

  // Markers of a band, and a search over everything.
  const std::string band = httpGet(port, "/api/markers?lo_khz=8400&hi_khz=8500");
  CHECK(contains(band, "\"hz\":8414500") && contains(band, "\"hz\":8453000") && contains(band, "\"mode\":\"STANAG 4285\"") &&
        contains(band, "\"demod\":\"USB\"") && contains(band, "\"source\":\"mine.txt\"") && !contains(band, "\"hz\":518000"));
  const std::string found = httpGet(port, "/api/markers?text=navtex");
  CHECK(contains(found, "\"hz\":518000") && !contains(found, "STANAG"));
  const std::string dbs = httpGet(port, "/api/databases");
  CHECK(contains(dbs, "{\"name\":\"ITU\",\"entries\":9}") && contains(dbs, "{\"name\":\"mine.txt\",\"entries\":2}") &&
        contains(dbs, "\"can_reload\":true"));
  // The tasks, with the frequencies the lists give: by mode, and by a whole
  // word of the label ("fax" is not in "Halifax").
  std::string tasks = httpGet(port, "/api/tasks");
  CHECK(contains(tasks, "\"can_record\":true"));
  auto task = [&tasks](const std::string& id) {
    const std::size_t a = tasks.find("{\"id\":\"" + id + "\""), b = tasks.find("{\"id\":", a + 1);
    return a == std::string::npos ? std::string() : tasks.substr(a, b - a);
  };
  CHECK(contains(task("stanag4285"), "\"hz\":8453000") && contains(task("stanag4285"), "\"have\":0"));
  CHECK(contains(task("dsc"), "\"listed\":6") && contains(task("navtex"), "\"hz\":518000"));
  CHECK(contains(task("fax"), "\"listed\":0") && contains(task("link11"), "\"listed\":0"));

  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", port, "/ws", 2000, &err));
  // No IQ yet: nothing to record.
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"task\":\"stanag4285\",\"freq\":8453000,\"seconds\":5}");
  CHECK(contains(nextOfType(ws, "result"), "no_signal"));
  const double rate = 62500;
  std::vector<int16_t> block(2 * 6250);
  for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<int16_t>((i * 37) % 200) - 100;
  double utc = 1700000000;
  auto feed = [&](int blocks, double centre) {
    for (int i = 0; i < blocks; ++i, utc += 0.1) {
      IqInput in;
      in.iq = block;
      in.sampleRate = rate;
      in.centreHz = centre;
      in.utc = utc;
      app.feed(0, std::move(in));
      app.drain();
    }
  };
  feed(5, 8450000);
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"task\":\"stanag4285\",\"freq\":8453000,\"seconds\":5}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  feed(20, 8450000);
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"task\":\"dsc\",\"seconds\":5}");
  CHECK(contains(nextOfType(ws, "result"), "already_recording"));
  feed(20, 8460000);  // retuned during the recording: a new capture
  feed(20, 8460000);  // 6 s fed: the 5 s are full
  std::string last;
  for (int i = 0; i < 40 && !contains(last, "\"active\":false"); ++i) last = nextOfType(ws, "recording");
  CHECK(contains(last, "\"ended\":\"done\"") && contains(last, "\"task\":\"stanag4285\""));
  std::string meta, data;
  for (const auto& f : fs::directory_iterator(dir / "rec")) {
    if (f.path().extension() == ".sigmf-meta") meta = f.path().u8string();
    if (f.path().extension() == ".sigmf-data") data = f.path().u8string();
  }
  CHECK(contains(meta, "_stanag4285_8453.0kHz.sigmf-meta"));
  CHECK(!data.empty() && fs::file_size(data) == 4u * 5 * 62500);
  SigmfReader rd;
  CHECK(rd.open(meta, &err));
  CHECK(rd.sampleRate() == rate && rd.captures().size() == 2 && contains(rd.description(), "task: stanag4285"));
  if (rd.captures().size() == 2) CHECK(rd.captures()[0].centerHz == 8450000 && rd.captures()[1].centerHz == 8460000);
  rd.close();
  tasks = httpGet(port, "/api/tasks");
  CHECK(contains(task("stanag4285"), "\"have\":1") && contains(task("dsc"), "\"have\":0"));

  // Reading the databases again replaces them, for the UI and the receivers.
  ws.sendText("{\"cmd\":\"reload_databases\"}");
  CHECK(contains(nextOfType(ws, "databases"), "other.txt"));
  CHECK(reloads == 1);
  feed(2, 8460000);
  tasks = httpGet(port, "/api/tasks");
  CHECK(contains(task("link11"), "\"hz\":9000000") && contains(task("stanag4285"), "\"listed\":0"));
  // Stopping by hand; and no recording where it is not allowed (the demo).
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"task\":\"unknown\",\"seconds\":60}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  feed(5, 8460000);
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"stop\":true}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  feed(2, 8460000);
  last.clear();
  for (int i = 0; i < 40 && !contains(last, "\"active\":false"); ++i) last = nextOfType(ws, "recording");
  CHECK(contains(last, "\"ended\":\"stopped\""));
  app.setRecordings((dir / "rec").u8string(), false);
  ws.sendText("{\"cmd\":\"record\",\"rx\":0,\"task\":\"unknown\"}");
  CHECK(contains(nextOfType(ws, "result"), "not_available"));
  ws.shutdown();
  app.stop();
  fs::remove_all(dir);
}

// Downloading lists into the frequency folder (from a local stand-in for
// the sites), and the user's own markers.
TEST(app_downloads_lists_and_keeps_my_markers) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("hfc_dl_test_" + std::to_string(std::rand()));
  fs::remove_all(dir);
  // The stand-in sites: a KiwiSDR-style label list, its categories, an
  // EiBi-style schedule that only exists for the previous season, and an
  // error page where a list should be.
  std::string kiwi = "{\"dx\":[";
  for (int i = 0; i < 150; ++i)
    kiwi += std::string(i ? "," : "") + "[" + std::to_string(4000 + 10 * i) + ",\"USB\",\"Station " + std::to_string(i) + "\",\"\",{\"T\":1}]";
  kiwi += ",[8453,\"USB\",\"STANAG 4285 net\",\"\",{\"T2\":1}]]}";
  std::string eibi = "kHz:75;Time(UTC):93;Days:59;ITU:49;Station:201;Lng:49;Target:62;Remarks:135;P:35;Start:60;Stop:60;\n";
  for (int i = 0; i < 1200; ++i) eibi += std::to_string(5000 + i * 5) + ";0000-2400;;D;Broadcaster " + std::to_string(i) + ";E;Eu;;1;;\n";
  WebServer site;
  site.handle("/dx/dx_community.json", [&](const HttpRequest&) { HttpResponse r; r.body = kiwi; return r; });
  site.handle("/dx/dx_community_config.json", [&](const HttpRequest&) {
    HttpResponse r;
    r.body = "{\"dx_type\":[{\"key\":1,\"name\":\"Channel\",\"color\":\"lightPink\"},"
             "{\"key\":2,\"name\":\"Milcom\",\"color\":\"hsl(30,100%,85%)\"},{\"key\":3,\"name\":\"Bad\",\"color\":\"red;background:url(x)\"}]}";
    return r;
  });
  site.handle("/dx/sked-b25.csv", [&](const HttpRequest&) { HttpResponse r; r.body = eibi; return r; });
  site.handle("/broken.json", [&](const HttpRequest&) { HttpResponse r; r.body = "<html>Service unavailable</html>"; return r; });
  std::string err;
  CHECK(site.start("127.0.0.1", 0, &err));
  const std::string base = "http://127.0.0.1:" + std::to_string(site.port());

  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "test", &err));
  const uint16_t port = app.httpPort();
  app.setReceiver(0, "Playback", false);
  const std::string folder = (dir / "freq").u8string();
  app.setFrequencyReload(
      [folder]() -> std::shared_ptr<const FrequencyList> {
        auto l = FrequencyList::builtin();
        std::error_code ec;
        for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
          const std::string name = it->path().filename().u8string();
          if (name != myMarkersFile() && name != "dx_community_config.json") l->addFile(it->path().u8string());
        }
        addMyMarkers((fs::u8path(folder) / myMarkersFile()).u8string(), *l);
        return l;
      },
      folder);
  app.setDownloads({{"KiwiSDR community labels", base + "/dx/dx_community.json", "dx_community.json", 100, "", ""},
                    {"KiwiSDR label categories", base + "/dx/dx_community_config.json", "dx_community_config.json", 0, "", ""},
                    {"EiBi schedule", base + "/dx/sked-a26.csv", "sked-a26.csv", 1000, base + "/dx/sked-b25.csv", "sked-b25.csv"},
                    {"A broken list", base + "/broken.json", "broken.json", 100, "", ""}});
  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", port, "/ws", 2000, &err));
  ws.sendText("{\"cmd\":\"download_databases\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  std::string done;
  for (int i = 0; i < 50 && !contains(done, "\"state\":\"done\""); ++i) done = nextOfType(ws, "download");
  CHECK(contains(done, "\"state\":\"done\""));
  CHECK(contains(done, "{\"name\":\"KiwiSDR community labels\",\"file\":\"dx_community.json\",\"ok\":true,\"entries\":151"));
  CHECK(contains(done, "\"file\":\"sked-b25.csv\",\"ok\":true,\"entries\":1200"));  // the previous season
  CHECK(contains(done, "\"name\":\"A broken list\",\"file\":\"broken.json\",\"ok\":false") && contains(done, "not_a_list"));
  CHECK(fs::exists(dir / "freq" / "dx_community.json") && fs::exists(dir / "freq" / "dx_community_config.json"));
  CHECK(!fs::exists(dir / "freq" / "broken.json"));
  // The databases were read again: the labels are markers now.
  const std::string band = httpGet(port, "/api/markers?lo_khz=8450&hi_khz=8456");
  CHECK(contains(band, "STANAG 4285 net") && contains(band, "\"source\":\"dx_community.json\""));
  // The categories: on the markers, and named and coloured by the category
  // file (a colour that is not only a colour is dropped).
  CHECK(contains(band, "\"cat\":2"));
  const std::string dbs = httpGet(port, "/api/databases");
  CHECK(contains(dbs, "{\"key\":2,\"name\":\"Milcom\",\"color\":\"hsl(30,100%,85%)\"}"));
  CHECK(contains(dbs, "{\"key\":3,\"name\":\"Bad\",\"color\":\"\"}"));

  // My markers: add, edit, delete; each is a marker at once.
  ws.sendText("{\"cmd\":\"marker_save\",\"hz\":9201000,\"label\":\"Akula?\",\"note\":\"bursts every 22 s\",\"demod\":\"USB\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  std::string mine = httpGet(port, "/api/my_markers");
  CHECK(contains(mine, "\"hz\":9201000,\"label\":\"Akula?\",\"note\":\"bursts every 22 s\",\"demod\":\"USB\""));
  const std::size_t at = mine.find("\"id\":\"") + 6;
  const std::string id = mine.substr(at, mine.find('"', at) - at);
  CHECK(contains(httpGet(port, "/api/markers?lo_khz=9200&hi_khz=9202"), "\"label\":\"Akula? - bursts every 22 s\",\"mode\":\"Akula\""));
  ws.sendText("{\"cmd\":\"marker_save\",\"id\":\"" + id + "\",\"hz\":9201500,\"label\":\"Akula\",\"note\":\"\",\"demod\":\"USB\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  CHECK(contains(httpGet(port, "/api/markers?lo_khz=9201&hi_khz=9202"), "\"hz\":9201500,\"label\":\"Akula\""));
  CHECK(contains(httpGet(port, "/api/databases"), "{\"name\":\"my markers\",\"entries\":1}"));
  ws.sendText("{\"cmd\":\"marker_save\",\"hz\":9201000,\"label\":\"\"}");
  CHECK(contains(nextOfType(ws, "result"), "no_label"));
  ws.sendText("{\"cmd\":\"marker_delete\",\"id\":\"" + id + "\"}");
  CHECK(contains(nextOfType(ws, "result"), "\"ok\":true"));
  CHECK(!contains(httpGet(port, "/api/markers?lo_khz=9201&hi_khz=9202"), "Akula"));
  ws.sendText("{\"cmd\":\"marker_delete\",\"id\":\"" + id + "\"}");
  CHECK(contains(nextOfType(ws, "result"), "no_such_marker"));
  ws.shutdown();
  app.stop();
  site.stop();
  fs::remove_all(dir);
}

// The user's windows and a marker's decoder: saved in the frequency folder
// (decode_windows.json, my_markers.json), listed for the UI, values checked.
TEST(app_saves_windows_and_marker_decoders) {
  const auto dir = std::filesystem::temp_directory_path() / "hfc_test_windows";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  app.setFrequencyReload([] { return std::make_shared<FrequencyList>(); }, dir.u8string());
  CHECK(app.start(cfg, "test", nullptr));
  const uint16_t port = app.httpPort();
  WebSocket ws;
  CHECK(ws.connect("127.0.0.1", port, "/ws", 2000, nullptr));
  auto result = [&](const std::string& cmd) {
    ws.sendText(cmd);
    return nextOfType(ws, "result");
  };
  CHECK(contains(result("{\"cmd\":\"window_save\",\"lo_hz\":\"7040000\",\"hi_hz\":\"7043000\",\"decoder\":\"RTTY 50/85\"}"), "\"ok\":true"));
  CHECK(contains(result("{\"cmd\":\"window_save\",\"lo_hz\":\"7100000\",\"hi_hz\":\"7200000\",\"decoder\":\"none\"}"), "\"ok\":true"));
  CHECK(contains(result("{\"cmd\":\"window_save\",\"lo_hz\":\"7100000\",\"hi_hz\":\"7100010\",\"decoder\":\"none\"}"), "bad_window"));
  CHECK(contains(result("{\"cmd\":\"window_save\",\"lo_hz\":\"7100000\",\"hi_hz\":\"7101000\",\"decoder\":\"FT8\"}"), "unknown_decoder"));
  const std::string list = httpGet(port, "/api/windows");
  CHECK(contains(list, "\"decoder\":\"RTTY 50/85\"") && contains(list, "\"decoder\":\"none\"") && contains(list, "\"decoders\":[\"CW\""));
  std::string file;
  {
    // (Closed again before the next save: Windows cannot replace an open file.)
    std::ifstream in(dir / "decode_windows.json");
    file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  CHECK(contains(file, "\"lo_hz\":7040000") && contains(file, "\"hi_hz\":7200000"));
  // Delete one by its id.
  const std::size_t at = list.find("\"id\":\"") + 6;
  const std::string id = list.substr(at, list.find('"', at) - at);
  CHECK(contains(result("{\"cmd\":\"window_delete\",\"id\":\"" + id + "\"}"), "\"ok\":true"));
  CHECK(contains(result("{\"cmd\":\"window_delete\",\"id\":\"" + id + "\"}"), "no_such_window"));
  // A marker with a decoder; a decoder that is none of them is refused.
  CHECK(contains(result("{\"cmd\":\"marker_save\",\"hz\":\"7074000\",\"label\":\"net\",\"decoder\":\"CW\"}"), "\"ok\":true"));
  CHECK(contains(result("{\"cmd\":\"marker_save\",\"hz\":\"7075000\",\"label\":\"x\",\"decoder\":\"WEFAX9\"}"), "unknown_decoder"));
  CHECK(contains(httpGet(port, "/api/my_markers"), "\"decoder\":\"CW\""));
  ws.shutdown();
  app.stop();
  std::filesystem::remove_all(dir, ec);
}
