#include "fake_sdrconnect.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>

#include "hfc/sdrconnect.hpp"

namespace hfc::testing {

namespace sc = hfc::sdrconnect;

namespace {

constexpr double kPi = 3.14159265358979323846;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const std::set<std::string>& readOnly() {
  static const std::set<std::string> s = {"lna_state_min", "lna_state_max", "demod_max_bandwidth", "started",
                                          "overload", "can_control", "signal_power", "signal_snr",
                                          "valid_antennas", "valid_devices", "active_device", "api_version"};
  return s;
}

std::string fmt(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f", v);
  return buf;
}

bool inList(const std::string& csv, const std::string& item) {
  std::size_t pos = 0;
  while (pos <= csv.size()) {
    std::size_t end = csv.find(',', pos);
    if (end == std::string::npos) end = csv.size();
    if (csv.substr(pos, end - pos) == item) return true;
    pos = end + 1;
  }
  return false;
}

}  // namespace

FakeSdrconnect::FakeSdrconnect(FakeConfig cfg) : cfg_(std::move(cfg)), rng_(cfg_.seed) {
  for (int d = 0; d < 2; ++d) {
    auto& p = props_[d];
    const double centre = cfg_.centerHz + (d == 1 ? 1000000.0 : 0.0);
    p[sc::prop::kCenterFrequency] = fmt(centre);
    p[sc::prop::kSampleRate] = fmt(cfg_.sampleRate);
    p[sc::prop::kVfoFrequency] = fmt(centre);
    p[sc::prop::kLnaState] = "3";
    p[sc::prop::kLnaStateMin] = "0";
    p[sc::prop::kLnaStateMax] = "9";
    p[sc::prop::kFilterBandwidth] = "2700";
    p[sc::prop::kDemodulator] = "USB";
    p[sc::prop::kDemodMaxBandwidth] = "12000";
    p[sc::prop::kStarted] = "true";
    p[sc::prop::kOverload] = "false";
    p[sc::prop::kCanControl] = cfg_.canControl ? "true" : "false";
    p[sc::prop::kAudioMute] = "false";
    p[sc::prop::kAgcEnable] = "true";
    p[sc::prop::kSignalPower] = "-80";
    p[sc::prop::kSignalSnr] = "10";
    p[sc::prop::kValidAntennas] = cfg_.antennas;
    p[sc::prop::kActiveAntenna] = cfg_.antennas.substr(0, cfg_.antennas.find(','));
    p[sc::prop::kValidDevices] = cfg_.deviceName;
    p[sc::prop::kActiveDevice] = cfg_.deviceName;
    p[sc::prop::kApiVersion] = cfg_.apiVersion;
  }
  if (cfg_.noDeviceUntilSelected) {
    deviceOpen_ = false;
    props_[0][sc::prop::kActiveDevice] = "";
  }
  noiseSigma_ = 32767.0 * std::pow(10.0, cfg_.noiseDbfs / 20.0) / std::sqrt(2.0);
  noiseState_ ^= cfg_.seed;
}

FakeSdrconnect::~FakeSdrconnect() { stop(); }

bool FakeSdrconnect::start(std::string* error) {
  if (!listener_.listen(cfg_.address, cfg_.port, error)) return false;
  startedAt_ = now();
  running_ = true;
  acceptThread_ = std::thread([this] { acceptLoop(); });
  streamThread_ = std::thread([this] { streamLoop(); });
  return true;
}

void FakeSdrconnect::stop() {
  if (!running_.exchange(false)) return;
  std::shared_ptr<WebSocket> c;
  {
    std::lock_guard<std::mutex> lock(connMu_);
    c = conn_;
  }
  if (c) c->shutdown();
  if (acceptThread_.joinable()) acceptThread_.join();
  if (streamThread_.joinable()) streamThread_.join();
  listener_.close();
}

std::string FakeSdrconnect::property(const std::string& name, int device) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = props_[device].find(name);
  return it == props_[device].end() ? std::string() : it->second;
}

std::vector<std::string> FakeSdrconnect::receivedEvents() const {
  std::lock_guard<std::mutex> lock(mu_);
  return events_;
}

bool FakeSdrconnect::sendTo(const std::string& text) {
  std::shared_ptr<WebSocket> c;
  {
    std::lock_guard<std::mutex> lock(connMu_);
    c = conn_;
  }
  return c && c->sendText(text);
}

void FakeSdrconnect::acceptLoop() {
  while (running_) {
    TcpSocket s = listener_.accept(100);
    if (!s.valid()) continue;
    auto ws = std::make_shared<WebSocket>();
    if (!ws->accept(std::move(s), 2000, nullptr)) continue;
    {
      std::lock_guard<std::mutex> lock(mu_);
      // Streams are enabled per connection.
      iqEnabled_ = audioEnabled_ = spectrumEnabled_ = false;
      pending_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(connMu_);
      conn_ = ws;
    }
    WsMessage msg;
    while (running_ && ws->receive(msg, nullptr))
      if (!msg.binary) handle(msg.text());
    std::lock_guard<std::mutex> lock(connMu_);
    conn_ = nullptr;
  }
}

void FakeSdrconnect::report(int device, const std::string& property, const char* eventType) {
  std::string value;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = props_[device].find(property);
    if (it == props_[device].end()) return;  // unknown property: no answer
    value = it->second;
  }
  sendTo(sc::encodeMessage({eventType, property, value, device == 1 ? "secondary" : "primary"}));
}

void FakeSdrconnect::handle(const std::string& text) {
  sc::Message m;
  if (!sc::decodeMessage(text, m, nullptr)) return;
  sc::Device dev;
  if (!sc::parseDevice(m.device, dev)) return;
  const int d = dev == sc::Device::Secondary ? 1 : 0;
  std::unique_lock<std::mutex> lock(mu_);
  events_.push_back(m.eventType + ":" + m.property + "=" + m.value + "@" + (m.device.empty() ? "-" : m.device));
  if (now() - startedAt_ < cfg_.refreshingSeconds) {
    // Still looking for radios: only this one answer, nothing done.
    lock.unlock();
    if (m.eventType == sc::event::kGetProperty && m.property == sc::prop::kActiveDevice)
      sendTo(sc::encodeMessage({sc::event::kGetPropertyResponse, m.property, "Refreshing...", "primary"}));
    return;
  }
  if (d == 1 && !dual_ && (m.eventType == sc::event::kGetProperty || m.eventType == sc::event::kSetProperty))
    return;  // no secondary device
  const bool on = m.value == "true";
  if (m.eventType == sc::event::kGetProperty) {
    lock.unlock();
    report(d, m.property, sc::event::kGetPropertyResponse);
  } else if (m.eventType == sc::event::kSetProperty) {
    if (!cfg_.canControl || readOnly().count(m.property)) return;
    const auto cur = props_[d].find(m.property);
    if (cur == props_[d].end()) return;
    std::string value = m.value;
    if (m.property == sc::prop::kSampleRate) {
      const double r = std::strtod(value.c_str(), nullptr);
      const auto& ok = dual_ ? cfg_.acceptedRatesDual : cfg_.acceptedRates;
      if (std::find(ok.begin(), ok.end(), r) == ok.end()) return;  // refused: silence
      value = fmt(r);
    } else if (m.property == sc::prop::kLnaState) {
      const int v = std::atoi(value.c_str());
      if (v < std::atoi(props_[d][sc::prop::kLnaStateMin].c_str()) ||
          v > std::atoi(props_[d][sc::prop::kLnaStateMax].c_str()))
        return;
    } else if (m.property == sc::prop::kActiveAntenna) {
      if (!inList(props_[d][sc::prop::kValidAntennas], value)) return;
    }
    if (cur->second == value) return;  // unchanged: no report
    const double due = now() + cfg_.reportDelaySeconds;
    // Both tuners of a dual-tuner device run at one rate (assumed).
    const bool both = m.property == sc::prop::kSampleRate && dual_;
    for (int k = 0; k < 2; ++k)
      if (k == d || both) pending_.push_back({due, k, m.property, value});
  } else if (m.eventType == sc::event::kIqStreamEnable) {
    iqEnabled_ = on;
  } else if (m.eventType == sc::event::kAudioStreamEnable) {
    audioEnabled_ = on;
  } else if (m.eventType == sc::event::kSpectrumEnable) {
    spectrumEnabled_ = on;
  } else if (m.eventType == sc::event::kSetPrimaryDeviceEnable) {
    deviceEnabled_[0] = on;
  } else if (m.eventType == sc::event::kSetSecondaryDeviceEnable) {
    deviceEnabled_[1] = on;
  } else if (m.eventType == sc::event::kSelectedDeviceName || m.eventType == sc::event::kSelectedDevice) {
    // Placeholder: selecting the device by name or index (no mode suffix)
    // returns it to single-tuner mode at its first accepted rate.
    if (m.eventType == sc::event::kSelectedDeviceName ? m.value != cfg_.deviceName : m.value != "0") return;
    if (!deviceOpen_) {
      deviceOpen_ = true;
      props_[0][sc::prop::kActiveDevice] = cfg_.deviceName;
    }
    const bool wasDual = dual_;
    dual_ = false;
    if (!wasDual) return;
    props_[0][sc::prop::kSampleRate] = fmt(cfg_.sampleRate);
    lock.unlock();
    report(0, sc::prop::kSampleRate, sc::event::kPropertyChanged);
  } else if (m.eventType == sc::event::kSelectedDeviceSerial) {
    const std::size_t colon = m.value.find(':');
    const std::string serial = m.value.substr(0, colon);
    const std::string mode = colon == std::string::npos ? std::string() : m.value.substr(colon + 1);
    if (serial != cfg_.serial) return;
    deviceOpen_ = true;
    props_[0][sc::prop::kActiveDevice] = cfg_.deviceName;
    dual_ = mode == "Dual";
    if (dual_) {
      // Placeholder behaviour: dual-tuner mode at the first accepted rate.
      const std::string r = fmt(cfg_.acceptedRatesDual.front());
      props_[0][sc::prop::kSampleRate] = props_[1][sc::prop::kSampleRate] = r;
    }
    lock.unlock();
    report(0, sc::prop::kSampleRate, sc::event::kPropertyChanged);
    if (dual_) report(1, sc::prop::kSampleRate, sc::event::kPropertyChanged);
  }
}

void FakeSdrconnect::generate(int d, std::size_t pairs, std::vector<int16_t>& out) {
  out.resize(pairs * 2);
  double rate;
  {
    std::lock_guard<std::mutex> lock(mu_);
    rate = std::strtod(props_[d][sc::prop::kSampleRate].c_str(), nullptr);
  }
  std::vector<double> acc(pairs * 2, 0.0);
  // White noise, new for every sample (a recycled noise table gives spectra
  // with structure, which the detector rightly reports). Uniform rather
  // than Gaussian, for speed at 10 MS/s: after the FFT each bin is Gaussian
  // anyway. Same power: a uniform on [-a, a] has variance a^2 / 3.
  const double a = noiseSigma_ * std::sqrt(3.0);
  for (std::size_t i = 0; i < pairs * 2; ++i) {
    noiseState_ ^= noiseState_ >> 12;  // xorshift64*
    noiseState_ ^= noiseState_ << 25;
    noiseState_ ^= noiseState_ >> 27;
    const uint64_t r = noiseState_ * 0x2545F4914F6CDD1Dull;
    acc[i] = a * (static_cast<double>(r >> 11) * (2.0 / 9007199254740992.0) - 1.0);
  }
  for (std::size_t t = 0; t < cfg_.tones.size() && t < 8; ++t) {
    const FakeTone& tone = cfg_.tones[t];
    if (std::abs(tone.offsetHz) >= rate / 2) continue;
    const double amp = 32767.0 * std::pow(10.0, tone.levelDbfs / 20.0);
    const double samplesPerBit = tone.fskBaud > 0 ? rate / tone.fskBaud : 0.0;
    double& ph = phase_[d][t];
    for (std::size_t i = 0; i < pairs; ++i) {
      double f = tone.offsetHz;
      if (tone.fskShiftHz > 0 && samplesPerBit > 0) {
        double& sym = fskPhase_[d][t];
        if ((sym += 1.0) >= samplesPerBit) {
          sym -= samplesPerBit;
          fskBit_[d][t] = static_cast<int>((rng_ = rng_ * 1664525u + 1013904223u) >> 31);
        }
        f += (fskBit_[d][t] ? 0.5 : -0.5) * tone.fskShiftHz;
      }
      acc[2 * i] += amp * std::cos(ph);
      acc[2 * i + 1] += amp * std::sin(ph);
      ph += 2 * kPi * f / rate;
      if (ph > kPi) ph -= 2 * kPi;
      if (ph < -kPi) ph += 2 * kPi;
    }
  }
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<int16_t>(std::lround(std::clamp(acc[i], -32768.0, 32767.0)));
}

void FakeSdrconnect::streamLoop() {
  const double tick = 1.0 / cfg_.messagesPerSecond;
  double next = now();
  double carry[2] = {0, 0};
  double streamStart = -1.0;
  bool dropDone = false;
  std::vector<int16_t> iq;
  std::vector<uint8_t> msg;
  while (running_) {
    const double t = now();
    if (t < next) {
      std::this_thread::sleep_for(std::chrono::duration<double>(std::min(next - t, 0.01)));
      continue;
    }
    // Do not try to catch up more than a few ticks after a stall.
    if (t - next > 5 * tick) next = t;
    next += tick;

    // Property changes that have become due.
    std::vector<Pending> due;
    bool iqOn, enabled[2], dual;
    double rate[2];
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->due <= t) {
          props_[it->device][it->property] = it->value;
          due.push_back(*it);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
      iqOn = iqEnabled_ && deviceOpen_;
      enabled[0] = deviceEnabled_[0];
      enabled[1] = deviceEnabled_[1];
      dual = dual_;
      for (int d = 0; d < 2; ++d) rate[d] = std::strtod(props_[d][sc::prop::kSampleRate].c_str(), nullptr);
    }
    for (const auto& p : due) report(p.device, p.property, sc::event::kPropertyChanged);

    if (!iqOn) {
      streamStart = -1.0;
      carry[0] = carry[1] = 0;
      continue;
    }
    if (streamStart < 0) streamStart = t;
    const double elapsed = t - streamStart;
    const bool dropping = cfg_.dropAfterSeconds >= 0 && !dropDone && elapsed >= cfg_.dropAfterSeconds &&
                          elapsed < cfg_.dropAfterSeconds + cfg_.dropSeconds;
    if (cfg_.dropAfterSeconds >= 0 && elapsed >= cfg_.dropAfterSeconds + cfg_.dropSeconds) dropDone = true;
    for (int d = 0; d < (dual ? 2 : 1); ++d) {
      if (!enabled[d]) continue;
      carry[d] += rate[d] * tick;
      const auto n = static_cast<std::size_t>(carry[d]);
      carry[d] -= static_cast<double>(n);
      if (n == 0) continue;
      generate(d, n, iq);
      if (dropping) {
        dropped_[d] += n;
        continue;
      }
      msg.resize(2 + iq.size() * 2);
      const uint16_t type = d == 0 ? 2 : 5;
      msg[0] = static_cast<uint8_t>(type);
      msg[1] = static_cast<uint8_t>(type >> 8);
      for (std::size_t i = 0; i < iq.size(); ++i) {
        const auto v = static_cast<uint16_t>(iq[i]);
        msg[2 + 2 * i] = static_cast<uint8_t>(v);
        msg[3 + 2 * i] = static_cast<uint8_t>(v >> 8);
      }
      std::shared_ptr<WebSocket> c;
      {
        std::lock_guard<std::mutex> lock(connMu_);
        c = conn_;
      }
      if (c && c->sendBinary(msg.data(), msg.size())) sent_[d] += n;
    }
  }
}

}  // namespace hfc::testing
