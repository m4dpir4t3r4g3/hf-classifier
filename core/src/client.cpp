#include "hfc/client.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace hfc::sdrconnect {

namespace {

double steadyNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double utcNow() {
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int deviceIndex(Device d) { return d == Device::Secondary ? 1 : 0; }

Device deviceOf(BinaryType t) {
  return t == BinaryType::AudioSecondary || t == BinaryType::IqSecondary || t == BinaryType::SpectrumSecondary
             ? Device::Secondary
             : Device::Primary;
}

double toDouble(const std::string& s) { return s.empty() ? 0.0 : std::strtod(s.c_str(), nullptr); }

}  // namespace

Client::Client() = default;

Client::Client(Handlers h) : handlers_(std::move(h)) {}

Client::~Client() { disconnect(); }

bool Client::connect(const std::string& host, uint16_t port, int timeoutMs, std::string* error) {
  disconnect();
  if (!ws_.connect(host, port, "/", timeoutMs, error)) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    props_.clear();
    reports_.clear();
    clocks_[0] = IqClock();
    clocks_[1] = IqClock();
    disconnectReason_.clear();
  }
  stopping_ = false;
  connected_ = true;
  reader_ = std::thread([this] { readerLoop(); });
  // What every user of the stream needs to know; the answers fill the cache.
  for (Device d : {Device::Primary, Device::Secondary}) {
    requestProperty(prop::kSampleRate, d);
    requestProperty(prop::kCenterFrequency, d);
  }
  requestProperty(prop::kApiVersion);
  requestProperty(prop::kCanControl);
  return true;
}

void Client::disconnect() {
  stopping_ = true;
  if (connected_) ws_.sendClose(1000);
  ws_.shutdown();
  if (reader_.joinable()) reader_.join();
  connected_ = false;
}

std::string Client::disconnectReason() const {
  std::lock_guard<std::mutex> lock(mu_);
  return disconnectReason_;
}

void Client::readerLoop() {
  WsMessage msg;
  std::string err;
  while (ws_.receive(msg, &err)) {
    const double steady = steadyNow();
    if (msg.binary) {
      handleBinary(msg, steady, utcNow());
    } else {
      ++textCount_;
      handleText(msg.text(), steady);
    }
  }
  connected_ = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    disconnectReason_ = stopping_ ? "disconnected" : err;
  }
  cv_.notify_all();
  if (!stopping_ && handlers_.onDisconnected) handlers_.onDisconnected(err);
}

void Client::handleText(const std::string& text, double steady) {
  Message m;
  if (!decodeMessage(text, m, nullptr)) return;
  if (m.eventType != event::kPropertyChanged && m.eventType != event::kGetPropertyResponse) return;
  Device d;
  if (!parseDevice(m.device, d)) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const Key k{deviceIndex(d), m.property};
    const auto old = props_.find(k);
    const bool changed = old == props_.end() || old->second != m.value;
    props_[k] = m.value;
    ++reports_[k];
    IqClock& clock = clocks_[deviceIndex(d)];
    if (m.property == prop::kSampleRate) clock.setNominalRate(toDouble(m.value), steady);
    if (m.property == prop::kCenterFrequency && changed && old != props_.end()) clock.markRetune(steady);
  }
  cv_.notify_all();
  if (handlers_.onProperty) handlers_.onProperty(d, m.property, m.value);
}

void Client::handleBinary(const WsMessage& msg, double steady, double utc) {
  if (msg.data.size() < 2) return;
  const uint16_t type = static_cast<uint16_t>(msg.data[0] | (msg.data[1] << 8));
  if (type < 1 || type > 6) return;
  const auto t = static_cast<BinaryType>(type);
  ++binCount_[type];
  binBytes_[type] += msg.data.size();
  const Device d = deviceOf(t);
  const uint8_t* payload = msg.data.data() + 2;
  const std::size_t bytes = msg.data.size() - 2;
  switch (t) {
    case BinaryType::IqPrimary:
    case BinaryType::IqSecondary: {
      IqBlock b;
      b.pairs = bytes / 4;
      if (b.pairs == 0) return;
      // SDRconnect sends little-endian samples; copying also keeps the
      // handlers independent of the message buffer's alignment.
      thread_local std::vector<int16_t> samples;
      samples.resize(b.pairs * 2);
      for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(payload[2 * i] | (payload[2 * i + 1] << 8));
      b.iq = samples.data();
      {
        std::lock_guard<std::mutex> lock(mu_);
        IqClock& clock = clocks_[deviceIndex(d)];
        const auto s = clock.onBlock(b.pairs, steady, utc);
        b.index = s.index;
        b.utc = s.utc;
        b.discontinuity = s.discontinuity;
        b.settling = s.settling;
        b.retuned = s.retuned;
        b.gap = s.gap;
        b.gapIndex = s.gapIndex;
        b.gapSeconds = s.gapSeconds;
        b.sampleRate = clock.rateForTiming();
        b.nominalRate = clock.nominalRate();
        const auto c = props_.find(Key{deviceIndex(d), prop::kCenterFrequency});
        if (c != props_.end()) b.centerHz = toDouble(c->second);
      }
      if (handlers_.onIq) handlers_.onIq(d, b);
      break;
    }
    case BinaryType::AudioPrimary:
    case BinaryType::AudioSecondary:
      if (handlers_.onAudio) {
        thread_local std::vector<int16_t> audio;
        audio.resize(bytes / 2);
        for (std::size_t i = 0; i < audio.size(); ++i)
          audio[i] = static_cast<int16_t>(payload[2 * i] | (payload[2 * i + 1] << 8));
        handlers_.onAudio(d, audio.data(), audio.size() / 2);
      }
      break;
    case BinaryType::SpectrumPrimary:
    case BinaryType::SpectrumSecondary:
      if (handlers_.onSpectrum) handlers_.onSpectrum(d, payload, bytes);
      break;
  }
}

bool Client::send(const Message& m) { return connected_ && ws_.sendText(encodeMessage(m)); }

bool Client::setProperty(const std::string& property, const std::string& value, Device d) {
  return send({event::kSetProperty, property, value, deviceName(d)});
}

bool Client::requestProperty(const std::string& property, Device d) {
  return send({event::kGetProperty, property, "", deviceName(d)});
}

bool Client::enableIq(bool on) { return send({event::kIqStreamEnable, "", on ? "true" : "false", ""}); }
bool Client::enableAudio(bool on) { return send({event::kAudioStreamEnable, "", on ? "true" : "false", ""}); }
bool Client::enableSpectrum(bool on) { return send({event::kSpectrumEnable, "", on ? "true" : "false", ""}); }
bool Client::setDeviceStream(bool on) { return send({event::kDeviceStreamEnable, "", on ? "true" : "false", ""}); }

bool Client::enableDevice(Device d, bool on) {
  return send({d == Device::Primary ? event::kSetPrimaryDeviceEnable : event::kSetSecondaryDeviceEnable, "",
               on ? "true" : "false", ""});
}

bool Client::selectDeviceIndex(int index) { return send({event::kSelectedDevice, "", std::to_string(index), ""}); }
bool Client::selectDeviceSerial(const std::string& s) { return send({event::kSelectedDeviceSerial, "", s, ""}); }
bool Client::selectDeviceName(const std::string& n) { return send({event::kSelectedDeviceName, "", n, ""}); }
bool Client::startRecording(const std::string& type) { return send({event::kStartRecording, "", type, ""}); }
bool Client::stopRecording() { return send({event::kStopRecording, "", "", ""}); }
bool Client::applyDeviceProfile(const std::string& n) { return send({event::kApplyDeviceProfile, "", n, ""}); }

uint64_t Client::reportCount(const Key& k) const {
  const auto it = reports_.find(k);
  return it == reports_.end() ? 0 : it->second;
}

bool Client::waitForReport(const Key& k, uint64_t after, int timeoutMs, std::string& value) {
  std::unique_lock<std::mutex> lock(mu_);
  const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                [&] { return reportCount(k) > after || !connected_; });
  if (!got || reportCount(k) <= after) return false;
  value = props_[k];
  return true;
}

std::optional<std::string> Client::query(const std::string& property, Device d, int timeoutMs) {
  const Key k{deviceIndex(d), property};
  uint64_t before;
  {
    std::lock_guard<std::mutex> lock(mu_);
    before = reportCount(k);
  }
  if (!requestProperty(property, d)) return std::nullopt;
  std::string v;
  if (!waitForReport(k, before, timeoutMs, v)) return std::nullopt;
  return v;
}

std::optional<std::string> Client::setAndConfirm(const std::string& property, const std::string& value, Device d,
                                                 int timeoutMs) {
  const Key k{deviceIndex(d), property};
  uint64_t before;
  {
    std::lock_guard<std::mutex> lock(mu_);
    before = reportCount(k);
    // IQ from the new frequency can arrive before SDRconnect reports it.
    if (property == prop::kCenterFrequency)
      clocks_[deviceIndex(d)].holdSettling(steadyNow(), timeoutMs / 1000.0 + 1.5);
  }
  if (!setProperty(property, value, d)) return std::nullopt;
  std::string v;
  if (waitForReport(k, before, timeoutMs, v)) return v;
  return query(property, d, 1500);
}

std::optional<std::string> Client::cached(const std::string& property, Device d) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = props_.find(Key{deviceIndex(d), property});
  if (it == props_.end()) return std::nullopt;
  return it->second;
}

IqClock::Stats Client::iqStats(Device d) const {
  std::lock_guard<std::mutex> lock(mu_);
  return clocks_[deviceIndex(d)].stats();
}

void Client::resetIqStats(Device d) {
  std::lock_guard<std::mutex> lock(mu_);
  clocks_[deviceIndex(d)].reset();
}

uint64_t Client::binaryMessages(BinaryType t) const { return binCount_[static_cast<int>(t)]; }
uint64_t Client::binaryBytes(BinaryType t) const { return binBytes_[static_cast<int>(t)]; }

}  // namespace hfc::sdrconnect
