// hf_classifier: watches what SDRconnect receives, finds every emission,
// decodes those it can, logs everything, and shows it in the browser at
// http://127.0.0.1:8073/. Or plays back a SigMF recording through the same
// processing. The console version; on Windows, HFClassifier.exe runs the
// same in the notification area (tray_main.cpp).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "hfc/file.hpp"
#include "runner.hpp"

using namespace hfc;

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args = utf8Arguments(argc, argv);
  RunOptions o = defaultRunOptions();
  // Started without options (e.g. from a shortcut): the window opens if the
  // settings say so. With options (scripts, tests): only with --open.
  if (!args.empty()) o.openWindow = false;
  std::string err;
  bool help = false;
  if (!parseCommandLine(args, o, &err, &help)) {
    std::fprintf(stderr, "%s\n\n%s", err.c_str(), usageText().c_str());
    return 2;
  }
  if (help) {
    std::printf("%s", usageText().c_str());
    return 0;
  }
  if (o.report) {
    std::string error;
    const std::string path = makeReport(o, o.reportRates, [](const std::string& line) {
      std::printf("%s\n", line.c_str());
      std::fflush(stdout);
    }, &error);
    if (path.empty()) {
      std::fprintf(stderr, "no report: %s\n", error.c_str());
      return 1;
    }
    std::printf("diagnostic report: %s\n", path.c_str());
    return 0;
  }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  RunHooks hooks;
  hooks.failed = [](const std::string& code, const std::string& detail) {
    if (code == "port_busy")
      std::fprintf(stderr, "The web UI's port is in use (%s). Choose another with --http PORT.\n", detail.c_str());
  };
  const int rc = runClassifier(o, g_stop, hooks);
  return rc == kRunAlreadyRunning ? 0 : rc;
}
