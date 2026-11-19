// A stand-in for SDRconnect's WebSocket server, for tests and for
// developing without a receiver. It follows the documented protocol (API
// 1.0.3) and imitates the behaviour third parties measured on a real
// SDRconnect (docs/RESEARCH.md, section 1.2):
//
//   - about 20 IQ messages a second, S16 interleaved, at the device rate;
//   - a refused sample rate (3 MS/s on an RSP1A) produces no answer;
//   - a set_property that changes nothing produces no property_changed;
//   - a changed property is reported after a delay (0.57 s measured; short
//     here by default so that the tests run fast).
//
// Where real behaviour is unknown (which rates dual-tuner mode accepts,
// device names, antenna names), the defaults are placeholders and say so.
// It is not evidence of how SDRconnect behaves; tools/sdrc_probe measures that.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/net.hpp"
#include "hfc/websocket.hpp"

namespace hfc::testing {

struct FakeTone {
  double offsetHz = 0.0;      // from the centre frequency
  double levelDbfs = -20.0;   // relative to a full-scale complex sine
  double fskShiftHz = 0.0;    // > 0: 2-FSK with this shift (phase continuous)
  double fskBaud = 0.0;
};

struct FakeConfig {
  std::string address = "127.0.0.1";
  uint16_t port = 0;  // 0: any free port
  std::vector<double> acceptedRates = {62500, 125000, 250000, 500000, 1000000, 2000000, 6000000, 8000000, 10000000};
  // Placeholder: what SDRconnect accepts in RSPduo dual-tuner mode is unknown.
  std::vector<double> acceptedRatesDual = {2000000};
  double sampleRate = 250000;
  double centerHz = 7000000;
  double reportDelaySeconds = 0.05;
  double messagesPerSecond = 20;
  std::string serial = "1806030000";  // placeholder
  std::string deviceName = "RSPduo (1806030000)";  // placeholder
  std::string antennas = "Tuner 1 50 ohm,Tuner 1 Hi-Z,Tuner 2 50 ohm";  // placeholder names
  std::string apiVersion = "1.0.3";
  bool canControl = true;
  // As SDRconnect_headless might start: no radio open (active_device
  // empty, no IQ) until a client selects one by index, name or serial.
  // Placeholder: how headless really behaves without a radio is unknown.
  bool noDeviceUntilSelected = false;
  // As SDRconnect_headless 2026-11 did on a user's PC just after starting:
  // for this long, active_device answers "Refreshing..." and nothing else
  // is answered or done. 0: ready at once.
  double refreshingSeconds = 0.0;
  double noiseDbfs = -45.0;
  std::vector<FakeTone> tones;
  // Fault injection: once, `dropSeconds` of IQ are generated but not sent,
  // starting `dropAfterSeconds` after the stream starts (< 0: never).
  double dropAfterSeconds = -1.0;
  double dropSeconds = 0.0;
  uint32_t seed = 1;
};

class FakeSdrconnect {
 public:
  explicit FakeSdrconnect(FakeConfig cfg);
  ~FakeSdrconnect();
  bool start(std::string* error);
  void stop();
  uint16_t port() const { return listener_.port(); }

  // The server's view, for tests.
  std::string property(const std::string& name, int device = 0) const;
  bool dualMode() const {
    std::lock_guard<std::mutex> lock(mu_);
    return dual_;
  }
  uint64_t pairsSent(int device) const { return sent_[device]; }
  uint64_t pairsDropped(int device) const { return dropped_[device]; }
  std::vector<std::string> receivedEvents() const;

 private:
  struct Pending {
    double due;
    int device;
    std::string property, value;
  };
  void acceptLoop();
  void streamLoop();
  void handle(const std::string& text);
  void report(int device, const std::string& property, const char* eventType);
  void generate(int device, std::size_t pairs, std::vector<int16_t>& out);
  bool sendTo(const std::string& text);

  FakeConfig cfg_;
  TcpListener listener_;
  std::thread acceptThread_, streamThread_;
  std::atomic<bool> running_{false};
  std::atomic<double> startedAt_{0.0};  // steady seconds, for refreshingSeconds

  mutable std::mutex mu_;
  // The connected client, if any. Senders copy it under connMu_ and send
  // without holding the lock, so that stop() is never stuck behind a send
  // to a client that does not read.
  std::shared_ptr<WebSocket> conn_;
  std::mutex connMu_;
  std::map<std::string, std::string> props_[2];
  std::vector<Pending> pending_;
  std::vector<std::string> events_;
  bool iqEnabled_ = false, audioEnabled_ = false, spectrumEnabled_ = false;
  bool deviceEnabled_[2] = {true, false};
  bool dual_ = false;
  bool deviceOpen_ = true;
  std::atomic<uint64_t> sent_[2] = {}, dropped_[2] = {};
  // Signal generator state per device.
  double phase_[2][8] = {};
  double fskPhase_[2][8] = {};
  int fskBit_[2][8] = {};
  uint32_t rng_;
  double noiseSigma_ = 0;      // per I and Q, in int16 units
  uint64_t noiseState_ = 0x9E3779B97F4A7C15ull;
  std::size_t noisePos_ = 0;
};

}  // namespace hfc::testing
