// Tests of the diagnostic report: the ZIP format, the probe against the
// fake SDRconnect, the report made by the running program (the UI's
// "diagnostics" command, measuring on its own connection and setting it up
// again afterwards), and "--report" with and without the program running.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "classifier_app.hpp"
#include "diagnostics.hpp"
#include "fake_sdrconnect.hpp"
#include "hfc/file.hpp"
#include "hfc/http_get.hpp"
#include "hfc/net.hpp"
#include "hfc/probe.hpp"
#include "hfc/websocket.hpp"
#include "hfc/zip.hpp"
#include "runner.hpp"
#include "sdrconnect_source.hpp"
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

uint16_t freePort() {
  TcpListener l;
  l.listen("127.0.0.1", 0, nullptr);
  return l.port();
}

std::string readFile(const std::string& path) {
  std::FILE* f = openFile(path, "rb");
  if (!f) return "";
  std::string s;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

const ZipEntry* entry(const std::vector<ZipEntry>& es, const std::string& name) {
  for (const auto& e : es)
    if (e.name == name) return &e;
  return nullptr;
}

// A fresh, empty folder for one test's reports.
std::string reportsFolder(const char* name) {
  const auto p = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p.u8string();
}

testing::FakeConfig fakeConfig() {
  testing::FakeConfig fc;
  fc.reportDelaySeconds = 0.05;
  fc.sampleRate = 250000;
  fc.centerHz = 7000000;
  fc.tones.push_back({30000, -30, 0, 0});
  return fc;
}

}  // namespace

TEST(zip_crc_and_round_trip) {
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  CHECK(crc32("", 0) == 0u);
  // Continued in parts, the same.
  CHECK(crc32("6789", 4, crc32("12345", 5)) == 0xCBF43926u);
  std::string binary(70000, '\0');
  for (std::size_t i = 0; i < binary.size(); ++i) binary[i] = static_cast<char>(i * 7);
  const std::vector<ZipEntry> in = {{"README.txt", "hello\r\n"}, {u8"отчет/данни.bin", binary}, {"empty", ""}};
  const std::string zip = makeZip(in, 1763200000);
  CHECK(zip.compare(0, 4, "PK\x03\x04") == 0);
  std::vector<ZipEntry> out;
  CHECK(readStoredZip(zip, out));
  CHECK(out.size() == 3);
  for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) CHECK(out[i].name == in[i].name && out[i].data == in[i].data);
  // A damaged byte is caught by the CRC.
  std::string bad = zip;
  bad[40] = static_cast<char>(bad[40] ^ 1);
  CHECK(!readStoredZip(bad, out));
  CHECK(!readStoredZip("not a zip", out));
  // A trailing archive comment is fine; a name that climbs out of its folder is not.
  std::string commented = zip;
  commented[commented.size() - 2] = 3;
  commented += "abc";
  CHECK(readStoredZip(commented, out) && out.size() == 3);
  CHECK(!readStoredZip(makeZip({{"../evil", "x"}}, 1763200000), out));
}

TEST(file_tail_keeps_the_end_from_a_whole_line) {
  const std::string path = "hfc_test_tail.log";
  std::string text;
  for (int i = 0; i < 1000; ++i) text += "line " + std::to_string(i) + "\n";
  std::FILE* f = openFile(path, "wb");
  std::fputs(text.c_str(), f);
  std::fclose(f);
  CHECK(fileTail(path, 1 << 20) == text);
  const std::string tail = fileTail(path, 100);
  CHECK(tail.rfind("[... the first ", 0) == 0);
  CHECK(contains(tail, "\nline 999\n") && !contains(tail, "line 900\n"));
  CHECK(tail.size() < 160);
  CHECK(fileTail("no_such_file.log", 100).empty());
  std::remove(path.c_str());
}

TEST(probe_measures_and_restores) {
  testing::FakeSdrconnect fake(fakeConfig());
  CHECK(fake.start(nullptr));
  ProbeLevels levels;
  sdrconnect::Client::Handlers h;
  h.onIq = [&levels](sdrconnect::Device d, const sdrconnect::IqBlock& b) { levels.add(d, b); };
  sdrconnect::Client c(h);
  CHECK(c.connect("127.0.0.1", fake.port(), 2000, nullptr));
  ProbeOptions po;
  po.seconds = 0.6;
  po.rates = {"1000000", "3000000"};
  int lines = 0;
  po.say = [&lines](const std::string&) { ++lines; };
  const std::string j = runProbe(c, levels, po);
  CHECK(contains(j, "\"api_version\": \"1.0.3\""));
  CHECK(contains(j, "\"requested\": 1000000") && contains(j, "\"requested\": 3000000"));
  CHECK(contains(j, "\"accepted\": false"));  // the fake refuses 3 MS/s, as an RSP1A did
  CHECK(contains(j, "\"stopped_early\": false"));
  CHECK(lines > 10);
  // The rate it found is back.
  CHECK(waitFor([&] { return fake.property("device_sample_rate") == "250000"; }, 3000));
  c.disconnect();
  fake.stop();
}

TEST(report_from_the_running_program_measures_on_its_connection) {
  testing::FakeSdrconnect fake(fakeConfig());
  CHECK(fake.start(nullptr));
  RunOptions o;
  o.settingsPath = "";
  o.dbPath = ":memory:";
  o.reportsDir = reportsFolder("hfc_test_reports_live");
  o.reportProbeSeconds = 0.3;
  o.settings.httpPort = 0;
  o.settings.sdrPort = fake.port();
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
  // Connected first, so that the probe has a connection to measure on.
  bool connected = false, measuring = false, statusSaysAvailable = false;
  std::string done, lines;
  WsMessage msg;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool asked = false;
  while (done.empty() && std::chrono::steady_clock::now() < until && ws.receive(msg, nullptr)) {
    if (msg.binary) continue;
    std::vector<JsonField> f;
    if (!parseJsonObject(msg.text(), f, nullptr)) continue;
    const std::string type = jsonField(f, "type");
    if (type == "status") {
      statusSaysAvailable = statusSaysAvailable || jsonField(f, "diagnostics") == "true";
      const std::string st = jsonField(f, "source_state");
      measuring = measuring || st == "measuring";
      if (st == "connected" && !asked) {
        connected = true;
        asked = true;
        ws.sendText("{\"cmd\":\"diagnostics\",\"rates\":\"1\"}");
      }
    } else if (type == "diagnostics") {
      if (jsonField(f, "state") == "line") lines += jsonField(f, "line") + "\n";
      if (jsonField(f, "state") == "done") done = msg.text();
      if (jsonField(f, "state") == "failed") done = msg.text();
    }
  }
  CHECK(connected && statusSaysAvailable);
  CHECK(measuring);  // the UI was told
  CHECK(contains(done, "\"state\":\"done\""));
  CHECK(contains(lines, "Sample rates"));
  std::vector<JsonField> f;
  parseJsonObject(done, f, nullptr);
  const std::string file = jsonField(f, "file"), path = jsonField(f, "path");
  CHECK(file.rfind("hfc_report_", 0) == 0);
  std::vector<ZipEntry> es;
  CHECK(readStoredZip(readFile(path), es));
  const ZipEntry* report = entry(es, "report.json");
  CHECK(report != nullptr);
  if (report) {
    CHECK(contains(report->data, "\"mode\": \"live\""));
    CHECK(contains(report->data, "\"api_version\": \"1.0.3\""));
    CHECK(contains(report->data, "\"accepted\": false"));
    CHECK(contains(report->data, "\"port_answers\": true"));
    CHECK(contains(report->data, "\"receivers\":["));  // the app's status
    CHECK(contains(report->data, "\"busy\":") && contains(report->data, "\"left_ids\":{"));  // the usage report
  }
  CHECK(entry(es, "README.txt") && entry(es, "probe.txt") && contains(entry(es, "probe.txt")->data, "Properties"));
  // Served for the download button, and nothing outside the folder.
  const HttpResult got = httpGet("http://127.0.0.1:" + std::to_string(port) + "/reports/" + file);
  CHECK(got.error.empty() && got.body == readFile(path));
  CHECK(!httpGet("http://127.0.0.1:" + std::to_string(port) + "/reports/..%2Fsettings.json").error.empty());
  // Afterwards: connected again, with the rate it had, and IQ processed.
  CHECK(waitFor([&] { return fake.property("device_sample_rate") == "250000"; }, 5000));
  CHECK(waitFor([&] {
    return contains(httpGet("http://127.0.0.1:" + std::to_string(port) + "/api/status").body, "\"source_state\":\"connected\"");
  }, 5000));
  const uint64_t sent = fake.pairsSent(0);
  CHECK(waitFor([&] { return fake.pairsSent(0) > sent; }, 3000));
  // The standalone "--report" asks the running program.
  RunOptions r = o;
  r.settings.httpPort = port;
  std::string error;
  const std::string viaRunning = makeReport(r, false, nullptr, &error);
  CHECK(!viaRunning.empty() && viaRunning != path);
  CHECK(readStoredZip(readFile(viaRunning), es) && entry(es, "report.json") &&
        contains(entry(es, "report.json")->data, "\"mode\": \"live\""));
  ws.shutdown();
  stop = true;
  t.join();
  fake.stop();
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::u8path(o.reportsDir), ec);
}

TEST(report_without_the_program) {
  // SDRconnect answers: measured on a connection of its own.
  testing::FakeSdrconnect fake(fakeConfig());
  CHECK(fake.start(nullptr));
  RunOptions o;
  o.settingsPath = "";
  o.reportsDir = reportsFolder("hfc_test_reports_alone");
  o.reportProbeSeconds = 0.3;
  o.settings.httpPort = freePort();  // nothing runs there
  o.settings.sdrPort = fake.port();
  std::string error;
  const std::string path = makeReport(o, false, nullptr, &error);
  CHECK(!path.empty());
  std::vector<ZipEntry> es;
  CHECK(readStoredZip(readFile(path), es));
  const ZipEntry* report = entry(es, "report.json");
  CHECK(report && contains(report->data, "\"mode\": \"report only\"") && contains(report->data, "\"api_version\": \"1.0.3\""));
  CHECK(report && contains(report->data, "\"status\": null"));
  fake.stop();
  // Nothing answers: a report all the same, saying why nothing was measured.
  const std::string path2 = makeReport(o, false, nullptr, &error);
  CHECK(!path2.empty());
  CHECK(readStoredZip(readFile(path2), es));
  report = entry(es, "report.json");
  CHECK(report && contains(report->data, "\"skipped\": \"no_answer") && contains(report->data, "\"port_answers\": false"));
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::u8path(o.reportsDir), ec);
}

TEST(source_probe_when_not_connected_says_why) {
  ClassifierApp app;
  ClassifierApp::Config cfg;
  cfg.dbPath = ":memory:";
  cfg.httpPort = 0;
  CHECK(app.start(cfg, "test", nullptr));
  SdrconnectOptions opt;
  opt.port = freePort();
  opt.startHeadless = false;
  opt.retrySeconds = 0.3;
  std::atomic<bool> stop{false};
  {
    SdrconnectSource src(app);
    std::thread t([&] { src.run(opt, stop); });
    std::string skipped;
    CHECK(src.probe(ProbeOptions(), 5, &skipped).empty());
    CHECK(skipped.rfind("not_connected:", 0) == 0);
    stop = true;
    t.join();
    // Not running: not taken up.
    CHECK(src.probe(ProbeOptions(), 0.3, &skipped).empty());
    CHECK(skipped == "not_running");
  }
  app.stop();
}
