#include "sdrconnect_source.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace hfc {

using namespace sdrconnect;

namespace {

double steadyNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Client::Handlers handlersFor(ClassifierApp& app, const bool* want, const std::atomic<ProbeLevels*>* probe) {
  Client::Handlers h;
  h.onIq = [&app, want, probe](Device d, const IqBlock& b) {
    if (ProbeLevels* lv = probe->load()) {
      lv->add(d, b);
      return;
    }
    const int rx = d == Device::Primary ? 0 : 1;
    // IQ around a retune may be from either frequency: skipped. So is IQ
    // whose rate SDRconnect has not reported yet (the measured rate varies
    // from block to block; each change would restart the processing).
    if (!want[rx] || b.settling || b.nominalRate <= 0 || b.centerHz <= 0) return;
    IqInput in;
    in.iq.assign(b.iq, b.iq + 2 * b.pairs);
    in.sampleRate = b.nominalRate;
    in.centreHz = b.centerHz;
    in.utc = b.utc;
    app.feed(rx, std::move(in));
  };
  return h;
}

}  // namespace

SdrconnectSource::SdrconnectSource(ClassifierApp& app) : app_(app), client_(handlersFor(app, want_, &probeLevels_)) {
  app_.setControl([this](int rx, const std::string& cmd, const std::vector<JsonField>& a) { return control(rx, cmd, a); });
}

SdrconnectSource::~SdrconnectSource() {
  app_.setControl(nullptr);
  client_.disconnect();
  headless_.stop();
}

void SdrconnectSource::state(const char* code, const std::string& detail) {
  const std::string key = std::string(code) + "|" + detail;
  if (key == lastState_) return;
  lastState_ = key;
  app_.setSourceState(code, detail);
  std::printf("SDRconnect: %s%s%s\n", code, detail.empty() ? "" : " ", detail.c_str());
  std::fflush(stdout);
}

double SdrconnectSource::vfoFor(const std::string& mode, double f, double bw) {
  if (mode == "USB") return f - bw / 2;
  if (mode == "LSB") return f + bw / 2;
  return f;
}

std::string SdrconnectSource::control(int rx, const std::string& cmd, const std::vector<JsonField>& a) {
  const Device d = rx == 0 ? Device::Primary : Device::Secondary;
  if (!client_.connected()) return "not_connected";
  if (cmd == "tune") {
    const std::string centre = jsonField(a, "centre");
    const auto got = client_.setAndConfirm(prop::kCenterFrequency, centre, d, 2000);
    if (!got || std::atof(got->c_str()) != std::atof(centre.c_str()))
      return "refused_centre:" + got.value_or("");
    return "";
  }
  if (cmd != "listen") return "unknown_command";
  const double f = std::atof(jsonField(a, "freq").c_str());
  const std::string mode = jsonField(a, "mode");
  double bw = std::atof(jsonField(a, "bw").c_str());
  if (!(bw > 0)) bw = 3000;
  if (mode != "USB" && mode != "LSB" && mode != "CW" && mode != "AM") return "unknown_mode";
  // "dial": `freq` is where the receiver is set (a frequency list's entry),
  // not the middle of a signal.
  const double vfo = jsonField(a, "dial") == "1" ? f : vfoFor(mode, f, bw);
  char vs[32], bs[32];
  std::snprintf(vs, sizeof(vs), "%.0f", vfo);
  std::snprintf(bs, sizeof(bs), "%.0f", bw);
  client_.setAndConfirm(prop::kDemodulator, mode, d, 1500);
  client_.setAndConfirm(prop::kFilterBandwidth, bs, d, 1500);
  const auto got = client_.setAndConfirm(prop::kVfoFrequency, vs, d, 1500);
  if (!got || std::fabs(std::atof(got->c_str()) - vfo) > 1)
    return "refused_vfo:" + got.value_or("");
  return "";
}

std::string SdrconnectSource::probe(const ProbeOptions& o, double waitSeconds, std::string* skipped) {
  ProbeJob job;
  job.options = o;
  std::unique_lock<std::mutex> l(probeMu_);
  if (job_) {
    if (skipped) *skipped = "busy";
    return "";
  }
  job_ = &job;
  // Taken up within `waitSeconds`, then given as long as it takes.
  const bool taken = probeCv_.wait_for(l, std::chrono::duration<double>(waitSeconds), [&] { return job.taken; });
  if (!taken) {
    job_ = nullptr;
    if (skipped) *skipped = "not_running";
    return "";
  }
  probeCv_.wait(l, [&] { return job.done; });
  job_ = nullptr;
  if (skipped) *skipped = job.skipped;
  return job.result;
}

bool SdrconnectSource::serveProbe(const SdrconnectOptions& opt, const std::function<bool()>& stop) {
  ProbeJob* job;
  {
    std::lock_guard<std::mutex> l(probeMu_);
    job = job_;
    if (!job || job->taken) return false;
    job->taken = true;
  }
  probeCv_.notify_all();
  std::string result, skipped;
  bool ran = false;
  if (!client_.connected()) {
    const std::size_t bar = lastState_.find('|');
    skipped = "not_connected:" + lastState_.substr(0, bar);
  } else {
    ran = true;
    state("measuring");
    std::printf("SDRconnect: measuring it for a diagnostic report\n");
    std::fflush(stdout);
    app_.flushReceivers();
    ProbeLevels levels;
    ProbeOptions o = job->options;
    const auto outer = o.stop;
    o.stop = [&] { return stop() || (outer && outer()) || !client_.connected(); };
    auto say = o.say;
    o.say = [say](const std::string& line) {
      std::printf("probe: %s\n", line.c_str());
      if (say) say(line);
    };
    probeLevels_ = &levels;
    result = runProbe(client_, levels, o);
    probeLevels_ = nullptr;
    std::fflush(stdout);
    // As before the measuring: devices, IQ, the rate and centre asked for.
    lastState_.clear();
    if (!client_.connected() || !setUp(opt)) client_.disconnect();
  }
  {
    std::lock_guard<std::mutex> l(probeMu_);
    job->result = result;
    job->skipped = skipped;
    job->done = true;
  }
  probeCv_.notify_all();
  return ran;
}

uint64_t SdrconnectSource::iqMessages() const {
  return client_.binaryMessages(sdrconnect::BinaryType::IqPrimary) + client_.binaryMessages(sdrconnect::BinaryType::IqSecondary);
}

std::string SdrconnectSource::describe() {
  auto q = [&](const char* p) { return client_.query(p, Device::Primary, 1500).value_or("(no answer)"); };
  return "api_version " + q(prop::kApiVersion) + ", can_control " + q(prop::kCanControl) + ", active_device " +
         q(prop::kActiveDevice) + ", sample rate " + q(prop::kSampleRate) + ", centre " + q(prop::kCenterFrequency);
}

namespace {

bool noRadio(const std::string& v) { return v.empty() || v == "none" || v == "None"; }

// The radio to open from valid_devices ("RSPduo (SERIAL) (Single),RSPduo
// (SERIAL) (Dual),IQ File" on a user's PC): not the file player; an
// RSPduo in single-tuner mode first.
std::string chooseRadio(const std::string& list) {
  std::vector<std::string> radios;
  std::size_t pos = 0;
  while (pos <= list.size()) {
    std::size_t end = list.find(',', pos);
    if (end == std::string::npos) end = list.size();
    std::string item = list.substr(pos, end - pos);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    while (!item.empty() && item.back() == ' ') item.pop_back();
    if (!item.empty() && item.find("IQ File") == std::string::npos) radios.push_back(item);
    pos = end + 1;
  }
  for (const auto& r : radios)
    if (r.find("(Single)") != std::string::npos) return r;
  return radios.empty() ? std::string() : radios.front();
}

}  // namespace

bool SdrconnectSource::awaitRadio(const SdrconnectOptions& opt, const std::function<bool()>& stop) {
  const double t0 = steadyNow();
  std::string last = "\x01";
  int selections = 0;
  bool sawLooking = false;
  auto pause = [&](double s) {
    const double end = steadyNow() + s;
    while (!stop() && client_.connected() && steadyNow() < end) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  };
  for (;;) {
    if (stop() || !client_.connected()) return false;
    const auto a = client_.query(prop::kActiveDevice, Device::Primary, 2000);
    const std::string v = a.value_or("(no answer)");
    if (v != last) {
      std::printf("SDRconnect: active_device \"%s\" %.1f s after connecting\n", v.c_str(), steadyNow() - t0);
      std::fflush(stdout);
      last = v;
    }
    // SDRconnect_headless, just started, looks for radios ("Refreshing...")
    // and answers little else meanwhile.
    const bool looking = !a || v.find("Refreshing") != std::string::npos;
    if (!looking && !noRadio(v)) {
      // It was still looking a moment ago: let it finish opening the radio
      // before it is asked anything more (headless stopped answering, and
      // then ended with an access violation, seconds after reporting the
      // radio, on a user's PC; whether a question then caused it is not
      // known).
      if (sawLooking) {
        std::printf("SDRconnect: radio reported; waiting 3 s before setting it up\n");
        std::fflush(stdout);
        pause(3.0);
      }
      return client_.connected();
    }
    if (looking) sawLooking = true;
    if (!looking) {
      // Ready, no radio open: open one (headless opens one only when told).
      const std::string list = client_.query(prop::kValidDevices, Device::Primary, 2000).value_or("(no answer)");
      const std::string pick = chooseRadio(list == "(no answer)" ? "" : list);
      if (pick.empty() || selections >= 2) {
        std::fprintf(stderr, "SDRconnect: no radio to use (active_device \"%s\", valid_devices \"%s\")\n", v.c_str(), list.c_str());
        state("no_radio", list);
        return false;
      }
      ++selections;
      std::printf("SDRconnect: no radio open; selecting \"%s\" (valid_devices \"%s\")\n", pick.c_str(), list.c_str());
      std::fflush(stdout);
      client_.selectDeviceName(pick);
      pause(3.0);
      continue;
    }
    if (steadyNow() - t0 > opt.radioWaitSeconds) {
      std::fprintf(stderr, "SDRconnect: still no radio %.0f s after connecting (active_device \"%s\")\n", opt.radioWaitSeconds, v.c_str());
      state("no_radio", v);
      return false;
    }
    state("waiting_radio", a ? v : std::string());
    pause(1.0);
  }
}

bool SdrconnectSource::setUp(const SdrconnectOptions& opt) {
  if (!opt.dualSerial.empty()) {
    client_.selectDeviceSerial(opt.dualSerial + ":Dual");
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  const bool canControl = client_.query(prop::kCanControl, Device::Primary, 1500).value_or("") == "true";
  const std::string dev = client_.query(prop::kActiveDevice, Device::Primary, 1500).value_or("SDRplay");
  for (int rx = 0; rx < 2; ++rx) {
    if (!want_[rx]) continue;
    const Device d = rx == 0 ? Device::Primary : Device::Secondary;
    client_.enableDevice(d, true);
    if (!opt.rate.empty()) {
      const auto got = client_.setAndConfirm(prop::kSampleRate, opt.rate, d, 3000);
      if (!got || std::atof(got->c_str()) != std::atof(opt.rate.c_str()))
        std::fprintf(stderr, "sample rate %s not accepted (SDRconnect reports %s)\n", opt.rate.c_str(),
                     got ? got->c_str() : "nothing");
    }
    if (!opt.centre.empty()) client_.setAndConfirm(prop::kCenterFrequency, opt.centre, d, 3000);
    // So that the rate and centre are known from the first IQ block on.
    client_.query(prop::kSampleRate, d, 1500);
    client_.query(prop::kCenterFrequency, d, 1500);
    app_.setReceiver(rx, dev + (want_[0] && want_[1] ? (rx == 0 ? ", tuner 1" : ", tuner 2") : ""), canControl);
  }
  if (!client_.enableIq(true)) return false;
  std::printf("SDRconnect reports: %s\n", describe().c_str());
  std::fflush(stdout);
  state(canControl ? "connected" : "view_only", dev);
  return true;
}

void SdrconnectSource::run(const SdrconnectOptions& opt, const std::function<bool()>& stop) {
  want_[0] = opt.tuner[0];
  want_[1] = opt.tuner[1];
  lastState_.clear();
  const bool local = isLocalHost(opt.host);
  const std::string where = opt.host + ":" + std::to_string(opt.port);
  bool wasConnected = false;
  double nextTry = 0, headlessSince = -1, lastLaunch = -1e9;
  // Connected: when IQ last arrived, and whether a radio was selected for it.
  double connectedAt = -1, lastIqAt = -1;
  uint64_t lastIq = 0;
  bool selectedRadio = false, reportedNoIq = false;
  bool headlessReported = true;
  state("connecting");
  while (!stop()) {
    // A diagnostic report's measuring: afterwards, IQ is awaited afresh.
    if (serveProbe(opt, stop)) {
      connectedAt = steadyNow();
      lastIqAt = -1;
      lastIq = iqMessages();
      reportedNoIq = false;
      continue;
    }
    // A headless SDRconnect this program started that has ended: say so,
    // with its exit code, once.
    if (!headlessReported && !headless_.running()) {
      headlessReported = true;
      std::fprintf(stderr, "SDRconnect_headless ended (exit code %d)%s\n", headless_.exitCode(),
                   opt.headlessLog.empty() ? "" : ("; its output is in " + opt.headlessLog).c_str());
    }
    if (client_.connected()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const double now = steadyNow();
      const uint64_t iq = iqMessages();
      if (iq != lastIq) {
        lastIq = iq;
        lastIqAt = now;
        if (reportedNoIq) {
          reportedNoIq = false;
          std::printf("SDRconnect: IQ arrives now\n");
          state(client_.cached(prop::kCanControl).value_or("") == "true" ? "connected" : "view_only",
                client_.cached(prop::kActiveDevice).value_or("SDRplay"));
        }
        continue;
      }
      const double since = now - (lastIqAt >= 0 ? lastIqAt : connectedAt);
      if (connectedAt < 0 || since < opt.noIqSeconds) continue;
      // Connected, and nothing arrives. SDRconnect_headless opens a radio
      // only when told which (SDRplay: "open by serial or name"): with
      // none open, ask for the first one, once, and set up again.
      const std::string active = client_.query(prop::kActiveDevice, Device::Primary, 1500).value_or("");
      if (!selectedRadio && (active.empty() || active == "none" || active == "None")) {
        selectedRadio = true;
        std::printf("SDRconnect: no IQ and no radio open (active_device \"%s\"): selecting the first radio\n", active.c_str());
        std::fflush(stdout);
        client_.selectDeviceIndex(0);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (!setUp(opt)) client_.disconnect();
        connectedAt = steadyNow();
        lastIqAt = -1;
        continue;
      }
      if (!reportedNoIq) {
        reportedNoIq = true;
        const std::string what = describe();
        std::fprintf(stderr, "SDRconnect: connected, but no IQ arrives (%s)\n", what.c_str());
        state("no_iq", what);
      }
      continue;
    }
    if (wasConnected) {
      std::fprintf(stderr, "SDRconnect connection lost (%s); reconnecting\n", client_.disconnectReason().c_str());
      app_.flushReceivers();
      wasConnected = false;
      state("connecting");
    }
    if (steadyNow() < nextTry) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    nextTry = steadyNow() + opt.retrySeconds;
    std::string err;
    if (client_.connect(opt.host, static_cast<uint16_t>(opt.port), 2000, &err)) {
      wasConnected = true;
      headlessSince = -1;
      std::printf("SDRconnect: connected to %s\n", where.c_str());
      std::fflush(stdout);
      if (!awaitRadio(opt, stop)) {
        // Its state (no_radio) stays shown; asked again in a while.
        client_.disconnect();
        wasConnected = false;
        nextTry = steadyNow() + std::max(opt.retrySeconds, 10.0);
        continue;
      }
      if (!setUp(opt)) client_.disconnect();
      connectedAt = steadyNow();
      lastIqAt = -1;
      lastIq = iqMessages();
      selectedRadio = false;
      reportedNoIq = false;
      continue;
    }
    // Nothing answers. On another computer, nothing more can be done here.
    if (!local) {
      state("no_answer", where);
      continue;
    }
    // A headless SDRconnect this program started: give it time to start.
    if (headless_.running()) {
      state(steadyNow() - headlessSince < opt.headlessWaitSeconds ? "starting_headless" : "headless_failed");
      continue;
    }
    // SDRconnect (the GUI) runs, so its WebSocket server must be off: only
    // the user can switch it on. Starting a second SDRconnect would fight
    // it for the receiver.
    const int gui = processRunning("SDRconnect.exe");
    if (gui == 1) {
      state("websocket_off");
      continue;
    }
    const SdrconnectInstall inst = findSdrconnect(opt.headlessPath);
    if (opt.startHeadless && !inst.headless.empty() && steadyNow() - lastLaunch > 60) {
      lastLaunch = steadyNow();
      const std::vector<std::string> args = {"--websocket_port=" + std::to_string(opt.port)};
      std::printf("starting %s %s%s\n", inst.headless.c_str(), args[0].c_str(),
                  opt.headlessLog.empty() ? "" : (" (its output: " + opt.headlessLog + ")").c_str());
      std::fflush(stdout);
      if (headless_.start(inst.headless, args, true, opt.headlessLog)) {
        headlessReported = false;
        headlessSince = steadyNow();
        nextTry = steadyNow() + 2;
        state("starting_headless", inst.headless);
      } else {
        state("headless_failed", inst.headless);
      }
      continue;
    }
    if (headlessSince >= 0) {
      state("headless_failed", inst.headless);  // it was started and has gone
    } else if (gui == 0 && inst.folder.empty()) {
      state("not_installed");
    } else {
      state("no_answer", where);
    }
  }
  if (client_.connected()) {
    client_.enableIq(false);
    client_.disconnect();
  }
  serveProbe(opt, stop);  // one asked for meanwhile: refused (not connected)
  headless_.stop();
}

}  // namespace hfc
