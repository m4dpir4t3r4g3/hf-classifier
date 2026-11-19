// sdrc_probe: measures how SDRconnect delivers IQ on this PC and receiver -
// the things the API document does not say (docs/RESEARCH.md, section 1.3) -
// and writes them to a report to send back. The measuring is in the core
// (hfc/probe.hpp), shared with the program's diagnostic report.
//
// It changes settings while it runs (the sample rate; the antenna with
// --antennas; the device mode with --dual) and restores them at the end.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/client.hpp"
#include "hfc/file.hpp"
#include "hfc/json.hpp"
#include "hfc/probe.hpp"
#include "hfc/sigmf.hpp"

using namespace hfc;
using namespace hfc::sdrconnect;

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: sdrc_probe [options]\n"
               "Measures how SDRconnect delivers IQ to other programs on this PC and\n"
               "receiver, and writes a report (JSON) to send back. It changes the sample\n"
               "rate while it runs (and the antenna or device mode if asked) and restores\n"
               "them at the end. Close other programs that use the WebSocket API first.\n"
               "\n"
               "  --host ADDR        SDRconnect address (default 127.0.0.1)\n"
               "  --port N           WebSocket port (default 5454)\n"
               "  --seconds S        measuring time per setting (default 5)\n"
               "  --rates LIST       comma-separated sample rates to try (default: 62500 ...\n"
               "                     10000000, plus 3000000 which an RSP1A refused)\n"
               "  --no-rates         measure the current sample rate only\n"
               "  --antennas         also select each antenna in turn and measure its noise level\n"
               "  --dual SERIAL      also test RSPduo dual-tuner mode (selects \"SERIAL:Dual\")\n"
               "  --out FILE         report file (default sdrc_probe_report.json)\n");
}

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

std::vector<std::string> splitCsv(const std::string& s) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= s.size()) {
    std::size_t end = s.find(',', pos);
    if (end == std::string::npos) end = s.size();
    std::string item = s.substr(pos, end - pos);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty()) out.push_back(item);
    pos = end + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", out = "sdrc_probe_report.json";
  int port = kDefaultPort;
  ProbeOptions po;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--host") host = next();
    else if (a == "--port") port = std::atoi(next().c_str());
    else if (a == "--seconds") po.seconds = std::atof(next().c_str());
    else if (a == "--rates") po.rates = splitCsv(next());
    else if (a == "--no-rates") po.tryRates = false;
    else if (a == "--antennas") po.antennas = true;
    else if (a == "--dual") po.dualSerial = next();
    else if (a == "--out") out = next();
    else {
      usage();
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  std::signal(SIGINT, onSignal);
  po.stop = [] { return g_stop.load(); };
  po.say = [](const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
  };

  ProbeLevels levels;
  Client::Handlers h;
  h.onIq = [&levels](Device d, const IqBlock& b) { levels.add(d, b); };
  Client c(h);
  std::string err;
  std::printf("sdrc_probe %s: connecting to ws://%s:%d/ ...\n", HFC_VERSION, host.c_str(), port);
  if (!c.connect(host, static_cast<uint16_t>(port), 3000, &err)) {
    std::fprintf(stderr,
                 "cannot connect: %s\n"
                 "  - Is SDRconnect (1.0.9 or later) running, with a receiver selected and started?\n"
                 "  - The WebSocket server is off by default: SDRconnect Preferences -> WebSocket Server.\n"
                 "  - Or start the headless server: SDRconnect_headless.exe --websocket_port=%d\n",
                 err.c_str(), port);
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));

  JsonWriter j;
  j.beginObject();
  j.key("tool").value(std::string("sdrc_probe ") + HFC_VERSION);
  j.key("utc").value(isoUtc(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count()));
#ifdef _WIN32
  j.key("host_os").value("windows");
#elif defined(__APPLE__)
  j.key("host_os").value("macos");
#else
  j.key("host_os").value("linux");
#endif
  j.key("endpoint").value(host + ":" + std::to_string(port));
  j.key("probe").raw(runProbe(c, levels, po));
  j.endObject();
  c.disconnect();

  std::FILE* f = openFile(out, "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", out.c_str());
    return 1;
  }
  std::fputs(j.str().c_str(), f);
  std::fputc('\n', f);
  std::fclose(f);
  std::printf("\nReport written to %s\n", out.c_str());
  return 0;
}
