#include "hfc/probe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "hfc/json.hpp"

namespace hfc {

using namespace sdrconnect;

void ProbeLevels::add(Device d, const IqBlock& b) {
  const int k = d == Device::Primary ? 0 : 1;
  double ss = 0, si = 0, sq = 0;
  int pk = 0;
  for (std::size_t i = 0; i < b.pairs; ++i) {
    const int I = b.iq[2 * i], Q = b.iq[2 * i + 1];
    ss += double(I) * I + double(Q) * Q;
    si += I;
    sq += Q;
    pk = std::max(pk, std::max(std::abs(I), std::abs(Q)));
  }
  std::lock_guard<std::mutex> lock(mu_);
  Snapshot& s = s_[k];
  s.sumSq += ss;
  s.sumI += si;
  s.sumQ += sq;
  s.n += b.pairs;
  s.peak = std::max(s.peak, pk);
  if (b.discontinuity) ++s.discontinuities;
}

void ProbeLevels::reset() {
  std::lock_guard<std::mutex> lock(mu_);
  s_[0] = s_[1] = Snapshot();
}

ProbeLevels::Snapshot ProbeLevels::get(Device d) const {
  std::lock_guard<std::mutex> lock(mu_);
  return s_[d == Device::Primary ? 0 : 1];
}

namespace {

double steadyNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

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

std::string fmtRate(double r) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.0f", r);
  return b;
}

struct Measurement {
  double seconds = 0, nominal = 0, measured = 0, ratio = 0, blocksPerSecond = 0, bytesPerBlock = 0;
  double rmsDbfs = -200, dcI = 0, dcQ = 0;
  int peak = 0;
  uint64_t gaps = 0, discontinuities = 0, samples = 0;
  double lostSeconds = 0;
};

class Prober {
 public:
  Prober(Client& c, ProbeLevels& lv, const ProbeOptions& o) : c_(c), lv_(lv), o_(o) {}

  bool stopped() const { return o_.stop && o_.stop(); }

  void say(const char* fmt, ...) {
    if (!o_.say) return;
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    o_.say(b);
  }

  void sleepFor(double s) {
    const double end = steadyNow() + s;
    while (!stopped() && steadyNow() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  Measurement measure(Device d, double seconds) {
    // The first half second after a change is not representative.
    sleepFor(0.5);
    c_.resetIqStats(d);
    lv_.reset();
    const BinaryType type = d == Device::Primary ? BinaryType::IqPrimary : BinaryType::IqSecondary;
    const uint64_t msgs0 = c_.binaryMessages(type), bytes0 = c_.binaryBytes(type);
    const double t0 = steadyNow();
    sleepFor(seconds);
    const double dt = std::max(1e-3, steadyNow() - t0);
    Measurement m;
    const auto st = c_.iqStats(d);
    m.seconds = dt;
    m.nominal = st.nominalRate;
    m.samples = st.samples;
    m.measured = static_cast<double>(st.samples) / dt;
    m.ratio = m.nominal > 0 ? m.measured / m.nominal : 0;
    const uint64_t msgs = c_.binaryMessages(type) - msgs0;
    m.blocksPerSecond = static_cast<double>(msgs) / dt;
    m.bytesPerBlock = msgs ? static_cast<double>(c_.binaryBytes(type) - bytes0) / static_cast<double>(msgs) : 0;
    m.gaps = st.gaps;
    m.lostSeconds = st.lostSeconds;
    const ProbeLevels::Snapshot s = lv_.get(d);
    if (s.n) {
      const double fs = 32768.0 * 32768.0;
      m.rmsDbfs = 10 * std::log10(s.sumSq / static_cast<double>(s.n) / fs + 1e-30);
      m.dcI = s.sumI / static_cast<double>(s.n);
      m.dcQ = s.sumQ / static_cast<double>(s.n);
    }
    m.peak = s.peak;
    m.discontinuities = s.discontinuities;
    return m;
  }

  static void write(JsonWriter& j, const Measurement& m) {
    j.beginObject();
    j.key("seconds").value(m.seconds);
    j.key("reported_rate").value(m.nominal);
    j.key("delivered_rate").value(m.measured);
    j.key("delivered_ratio").value(m.ratio);
    j.key("messages_per_second").value(m.blocksPerSecond);
    j.key("bytes_per_message").value(m.bytesPerBlock);
    j.key("rms_dbfs").value(m.rmsDbfs);
    j.key("dc_i").value(m.dcI);
    j.key("dc_q").value(m.dcQ);
    j.key("peak").value(m.peak);
    j.key("gaps_detected").value(m.gaps);
    j.key("lost_seconds").value(m.lostSeconds);
    j.endObject();
  }

  void print(const char* what, const Measurement& m) {
    say("  %-26s reported %10.0f  delivered %10.0f S/s (%5.1f %%)  %5.1f msg/s  %8.0f B/msg  %6.1f dBFS  gaps %llu",
        what, m.nominal, m.measured, 100 * m.ratio, m.blocksPerSecond, m.bytesPerBlock, m.rmsDbfs,
        static_cast<unsigned long long>(m.gaps));
  }

  std::string run();

 private:
  Client& c_;
  ProbeLevels& lv_;
  const ProbeOptions& o_;
};

const char* kProperties[] = {prop::kApiVersion,        prop::kActiveDevice,   prop::kValidDevices,
                             prop::kCanControl,        prop::kStarted,        prop::kSampleRate,
                             prop::kCenterFrequency,   prop::kVfoFrequency,   prop::kLnaState,
                             prop::kLnaStateMin,       prop::kLnaStateMax,    prop::kValidAntennas,
                             prop::kActiveAntenna,     prop::kDemodulator,    prop::kFilterBandwidth,
                             prop::kDemodMaxBandwidth, prop::kOverload,       prop::kAgcEnable};

std::string Prober::run() {
  const double seconds = o_.seconds;
  JsonWriter j;
  j.beginObject();

  // 1. Properties.
  say("Properties (primary device):");
  j.key("properties").beginObject();
  for (const char* p : kProperties) {
    const auto v = c_.query(p, Device::Primary, 1500);
    say("  %-22s %s", p, v ? v->c_str() : "(no answer)");
    j.key(p).value(v ? *v : std::string("(no answer)"));
  }
  j.endObject();
  const bool canControl = c_.cached(prop::kCanControl).value_or("") == "true";
  j.key("can_control").value(canControl);
  if (!canControl)
    say("SDRconnect reports can_control=%s: settings will not be changed, only measured.",
        c_.cached(prop::kCanControl).value_or("(no answer)").c_str());

  const std::string origRate = c_.cached(prop::kSampleRate).value_or("");
  const std::string origCentre = c_.cached(prop::kCenterFrequency).value_or("");
  const std::string origAntenna = c_.cached(prop::kActiveAntenna).value_or("");
  const std::string origDevice = c_.cached(prop::kActiveDevice).value_or("");

  // 2. IQ at the current rate.
  c_.enableDevice(Device::Primary, true);
  c_.enableIq(true);
  say("IQ stream at the current settings (%g s):", seconds);
  const Measurement cur = measure(Device::Primary, seconds);
  print("current rate", cur);
  j.key("iq_current");
  write(j, cur);
  if (cur.samples == 0) say("  No IQ arrived. Is the receiver started (play button) in SDRconnect?");

  // 3. Sample rates.
  j.key("sample_rates").beginArray();
  if (o_.tryRates && canControl && !stopped()) {
    say("Sample rates (%g s each):", seconds);
    for (const auto& r : o_.rates) {
      if (stopped()) break;
      // SDRconnect does not answer a set that changes nothing.
      const bool already = std::atof(c_.cached(prop::kSampleRate).value_or("0").c_str()) == std::atof(r.c_str());
      const double t0 = steadyNow();
      const auto got = already ? c_.cached(prop::kSampleRate) : c_.setAndConfirm(prop::kSampleRate, r, Device::Primary, 3000);
      const double confirmS = already ? 0.0 : steadyNow() - t0;
      const bool accepted = got && std::atof(got->c_str()) == std::atof(r.c_str());
      j.beginObject();
      j.key("requested").value(std::atof(r.c_str()));
      j.key("reported_after").value(got ? *got : std::string("(no answer)"));
      j.key("accepted").value(accepted);
      j.key("confirm_seconds").value(confirmS);
      j.key("was_already_set").value(already);
      if (accepted) {
        const Measurement m = measure(Device::Primary, seconds);
        print((r + " accepted").c_str(), m);
        j.key("iq");
        write(j, m);
      } else {
        say("  %-26s refused (SDRconnect reports %s)", r.c_str(), got ? got->c_str() : "nothing");
      }
      j.endObject();
    }
  }
  j.endArray();

  // 4. Antennas.
  j.key("antennas").beginArray();
  if (o_.antennas && canControl && !stopped()) {
    say("Antennas (%g s each):", std::min(seconds, 2.0));
    for (const auto& a : splitCsv(c_.cached(prop::kValidAntennas).value_or(""))) {
      if (stopped()) break;
      const auto got = c_.setAndConfirm(prop::kActiveAntenna, a, Device::Primary, 3000);
      const bool accepted = got && *got == a;
      j.beginObject();
      j.key("name").value(a);
      j.key("accepted").value(accepted);
      if (accepted) {
        const Measurement m = measure(Device::Primary, std::min(seconds, 2.0));
        print(a.c_str(), m);
        j.key("iq");
        write(j, m);
      } else {
        say("  %-26s not selected (SDRconnect reports %s)", a.c_str(), got ? got->c_str() : "nothing");
      }
      j.endObject();
    }
  }
  j.endArray();

  // 5. RSPduo dual-tuner mode.
  if (!o_.dualSerial.empty() && canControl && !stopped()) {
    say("Dual-tuner mode (%s:Dual):", o_.dualSerial.c_str());
    j.key("dual").beginObject();
    c_.selectDeviceSerial(o_.dualSerial + ":Dual");
    sleepFor(3.0);
    c_.enableDevice(Device::Secondary, true);
    c_.enableIq(true);
    for (Device d : {Device::Primary, Device::Secondary}) {
      const char* name = deviceName(d);
      j.key(name).beginObject();
      for (const char* p : {prop::kSampleRate, prop::kCenterFrequency, prop::kValidAntennas, prop::kActiveAntenna,
                            prop::kLnaStateMin, prop::kLnaStateMax}) {
        const auto v = c_.query(p, d, 1500);
        say("  %-9s %-22s %s", name, p, v ? v->c_str() : "(no answer)");
        j.key(p).value(v ? *v : std::string("(no answer)"));
      }
      j.endObject();
    }
    // Independent tuning: move the secondary 1 MHz above the primary.
    const double pc = std::atof(c_.cached(prop::kCenterFrequency, Device::Primary).value_or("0").c_str());
    if (pc > 0) {
      const std::string want = fmtRate(pc + 1e6);
      const auto got = c_.setAndConfirm(prop::kCenterFrequency, want, Device::Secondary, 3000);
      j.key("secondary_retune_to").value(want);
      j.key("secondary_reported").value(got ? *got : std::string("(no answer)"));
      say("  secondary tuned to %s: SDRconnect reports %s", want.c_str(), got ? got->c_str() : "nothing");
    }
    for (Device d : {Device::Primary, Device::Secondary}) {
      const Measurement m = measure(d, seconds);
      print(d == Device::Primary ? "dual: primary" : "dual: secondary", m);
      j.key(d == Device::Primary ? "iq_primary" : "iq_secondary");
      write(j, m);
    }
    if (o_.tryRates) {
      j.key("sample_rates").beginArray();
      for (const auto& r : o_.rates) {
        if (stopped()) break;
        const auto got = c_.setAndConfirm(prop::kSampleRate, r, Device::Primary, 3000);
        const bool accepted = got && std::atof(got->c_str()) == std::atof(r.c_str());
        const auto other = c_.query(prop::kSampleRate, Device::Secondary, 1500);
        say("  dual rate %-16s %s (secondary reports %s)", r.c_str(), accepted ? "accepted" : "refused",
            other ? other->c_str() : "nothing");
        j.beginObject();
        j.key("requested").value(std::atof(r.c_str()));
        j.key("accepted").value(accepted);
        j.key("secondary_reports").value(other ? *other : std::string("(no answer)"));
        j.endObject();
      }
      j.endArray();
    }
    c_.enableDevice(Device::Secondary, false);
    if (!origDevice.empty()) {
      c_.selectDeviceName(origDevice);
      // Not cut short by `stop`: the device mode must be back before the
      // settings are restored.
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    j.key("restored_device").value(c_.query(prop::kActiveDevice, Device::Primary, 1500).value_or("(no answer)"));
    j.endObject();
  }

  // 6. Restore (also when stopped early).
  c_.enableIq(false);
  j.key("restored").beginObject();
  if (canControl) {
    say("Restoring the original settings:");
    const std::pair<const char*, std::string> restore[] = {
        {prop::kSampleRate, origRate}, {prop::kCenterFrequency, origCentre}, {prop::kActiveAntenna, origAntenna}};
    for (const auto& [p, v] : restore) {
      if (v.empty()) continue;
      const bool same = c_.cached(p).value_or("") == v;
      const auto got = same ? c_.cached(p) : c_.setAndConfirm(p, v, Device::Primary, 3000);
      const bool ok = got && *got == v;
      say("  %-22s %s%s", p, v.c_str(), ok ? "" : "  (NOT restored - please check SDRconnect)");
      j.key(p).value(ok);
    }
  }
  j.endObject();
  j.key("stopped_early").value(stopped());
  j.endObject();
  return j.str();
}

}  // namespace

std::string runProbe(Client& c, ProbeLevels& levels, const ProbeOptions& o) {
  Prober p(c, levels, o);
  return p.run();
}

}  // namespace hfc
