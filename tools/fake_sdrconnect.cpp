// fake_sdrconnect: runs the stand-in SDRconnect WebSocket server, so that the
// tools (and later the classifier) can be tried without a receiver. Its
// behaviour where the real SDRconnect is unknown is a placeholder: see
// testing/fake_sdrconnect.hpp.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "fake_sdrconnect.hpp"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: fake_sdrconnect [options]\n"
               "A stand-in for SDRconnect's WebSocket server (API 1.0.3) with synthetic IQ.\n"
               "\n"
               "  --port N                 listen port (default 5454; also --websocket_port=N)\n"
               "  --rate HZ                initial sample rate (default 250000)\n"
               "  --center HZ              initial centre frequency (default 7000000)\n"
               "  --tone OFFSET:DBFS       a carrier OFFSET Hz from the centre (repeatable)\n"
               "  --fsk OFFSET:DBFS:SHIFT:BAUD   a 2-FSK signal (repeatable)\n"
               "  --noise DBFS             noise level (default -45)\n"
               "  --report-delay S         delay before a change is reported (default 0.5)\n"
               "  --drop-after S --drop S  once, do not send S seconds of IQ\n"
               "  --read-only              report can_control=false\n"
               "  --refreshing S           for S s after start, active_device is \"Refreshing...\"\n"
               "                           and nothing else is answered (as SDRconnect_headless did)\n"
               "  --no-device              no radio open until one is selected (as headless may start)\n");
}

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
  hfc::testing::FakeConfig cfg;
  cfg.port = 5454;
  cfg.reportDelaySeconds = 0.5;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--port") cfg.port = static_cast<uint16_t>(std::atoi(next().c_str()));
    // As SDRconnect_headless is started (third-party guides), so that the
    // classifier can start the fake in its place in tests.
    else if (a.rfind("--websocket_port=", 0) == 0) cfg.port = static_cast<uint16_t>(std::atoi(a.c_str() + 17));
    else if (a == "--rate") cfg.sampleRate = std::atof(next().c_str());
    else if (a == "--center" || a == "--centre") cfg.centerHz = std::atof(next().c_str());
    else if (a == "--tone" || a == "--fsk") {
      hfc::testing::FakeTone t;
      const std::string v = next();
      if (a == "--tone") std::sscanf(v.c_str(), "%lf:%lf", &t.offsetHz, &t.levelDbfs);
      else std::sscanf(v.c_str(), "%lf:%lf:%lf:%lf", &t.offsetHz, &t.levelDbfs, &t.fskShiftHz, &t.fskBaud);
      cfg.tones.push_back(t);
    } else if (a == "--noise") cfg.noiseDbfs = std::atof(next().c_str());
    else if (a == "--report-delay") cfg.reportDelaySeconds = std::atof(next().c_str());
    else if (a == "--drop-after") cfg.dropAfterSeconds = std::atof(next().c_str());
    else if (a == "--drop") cfg.dropSeconds = std::atof(next().c_str());
    else if (a == "--read-only") cfg.canControl = false;
    else if (a == "--refreshing") cfg.refreshingSeconds = std::atof(next().c_str());
    else if (a == "--no-device") cfg.noDeviceUntilSelected = true;
    else {
      usage();
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  hfc::testing::FakeSdrconnect server(cfg);
  std::string err;
  if (!server.start(&err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("fake SDRconnect listening on ws://%s:%u/ (Ctrl+C to stop)\n", cfg.address.c_str(), server.port());
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server.stop();
  return 0;
}
