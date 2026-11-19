// The live source: SDRconnect's IQ into the ClassifierApp, and the UI's
// commands ("listen", "tune") back to SDRconnect. Connects by itself and
// reconnects when the connection drops; when nothing answers on this
// computer and SDRconnect is not running, starts SDRconnect_headless (and
// stops it again at the end). Tells the UI what it is doing, as a state
// code the UI words in the user's language (ClassifierApp::setSourceState).
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

#include "classifier_app.hpp"
#include "hfc/client.hpp"
#include "hfc/probe.hpp"
#include "platform.hpp"

namespace hfc {

struct SdrconnectOptions {
  std::string host = "127.0.0.1";
  int port = sdrconnect::kDefaultPort;
  bool tuner[2] = {true, false};  // which receivers: RSPduo tuner 1, tuner 2
  std::string dualSerial;         // switch the RSPduo to "SERIAL:Dual" first
  std::string rate, centre;       // set first ("" leaves them)
  bool startHeadless = false;     // start SDRconnect_headless when SDRconnect is not running here
  std::string headlessPath;       // where it is ("" = look for it)
  double retrySeconds = 3;
  double headlessWaitSeconds = 20;  // for it to start answering
  std::string headlessLog;        // where SDRconnect_headless's own output goes ("" = nowhere)
  double noIqSeconds = 5;         // connected but no IQ this long: select a radio, then report it
  double radioWaitSeconds = 30;   // for SDRconnect to report a radio after connecting (headless looks for them first)
};

// The states reported to the UI (ClassifierApp::setSourceState):
//   connecting         trying to connect
//   connected          IQ is coming (detail: the device's name)
//   view_only          connected, but SDRconnect does not accept control
//   no_answer          nothing answers at host:port (detail: "host:port")
//   websocket_off      SDRconnect runs here, but its WebSocket server is off
//   not_installed      SDRconnect is not running and not found on this computer
//   starting_headless  SDRconnect_headless was started; waiting for it
//   headless_failed    it was started but does not answer, or could not start
//   no_iq              connected, but no IQ arrives (detail: what SDRconnect reports)
//   measuring          measuring SDRconnect for a diagnostic report (probe())
//   waiting_radio      connected; SDRconnect is still looking for radios
//                      (detail: what it reports, e.g. "Refreshing...")
//   no_radio           connected, but SDRconnect reports no radio to use
//                      (detail: what it reports)
class SdrconnectSource {
 public:
  explicit SdrconnectSource(ClassifierApp& app);
  ~SdrconnectSource();
  // Runs until stop() returns true: connects, sets up, feeds the app,
  // reconnects. Messages go to stdout/stderr too.
  void run(const SdrconnectOptions& opt, const std::function<bool()>& stop);
  void run(const SdrconnectOptions& opt, const std::atomic<bool>& stop) {
    run(opt, [&stop] { return stop.load(); });
  }
  bool connected() const { return client_.connected(); }

  // A UI command for receiver `rx` (ClassifierApp::ControlFn). Returns ""
  // or an error code: "not_connected", "unknown_mode", "refused_vfo:<what
  // SDRconnect reports>", "refused_centre:<...>", "unknown_command".
  std::string control(int rx, const std::string& cmd, const std::vector<JsonField>& args);
  // The VFO frequency at which an emission centred at `centreHz` is heard
  // in the middle of a `bandwidthHz` passband in `mode` (USB, LSB, CW, AM).
  // In USB and LSB the VFO is taken to be the suppressed carrier (the
  // dial), the usual convention; SDRconnect's documentation does not say.
  static double vfoFor(const std::string& mode, double centreHz, double bandwidthHz);

  // For the diagnostic report, from another thread while run() runs:
  // measures SDRconnect on this source's connection (hfc/probe.hpp; IQ is
  // not processed meanwhile, and the connection is set up again after).
  // Returns the probe's JSON, or "" with `skipped` set: "not_connected:<the
  // state>" or "not_running" (run() did not take it up within
  // `waitSeconds`).
  std::string probe(const ProbeOptions& o, double waitSeconds, std::string* skipped);

 private:
  bool setUp(const SdrconnectOptions& opt);
  // After connecting: waits while SDRconnect is still looking for radios,
  // and opens one if none is open. False if it gave up.
  bool awaitRadio(const SdrconnectOptions& opt, const std::function<bool()>& stop);
  // What SDRconnect reports about itself, for the log and the UI.
  std::string describe();
  uint64_t iqMessages() const;
  void state(const char* code, const std::string& detail = std::string());
  // In run()'s thread: a pending probe() is carried out (true) or refused.
  bool serveProbe(const SdrconnectOptions& opt, const std::function<bool()>& stop);

  struct ProbeJob {
    ProbeOptions options;
    bool taken = false, done = false;
    std::string result, skipped;
  };

  ClassifierApp& app_;
  sdrconnect::Client client_;
  bool want_[2] = {true, false};
  ChildProcess headless_;
  std::string lastState_;
  std::atomic<ProbeLevels*> probeLevels_{nullptr};  // set while measuring: IQ goes there, not to the app
  std::mutex probeMu_;
  std::condition_variable probeCv_;
  ProbeJob* job_ = nullptr;
};

}  // namespace hfc
