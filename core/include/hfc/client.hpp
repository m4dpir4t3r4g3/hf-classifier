// SDRconnect client: one WebSocket connection to SDRconnect (GUI or
// headless), a cache of every property SDRconnect reports, and the IQ,
// audio and spectrum streams of the primary and secondary device.
//
// A reader thread receives everything. Handlers run on that thread and must
// return quickly: SDRconnect queues only about 2.5 s of IQ for a slow reader
// before it drops data (measured by a third party, see docs/RESEARCH.md).
// They must not call disconnect() or connect(), which wait for that thread.
//
// Known gaps in the API that this class works around:
//   - IQ messages carry no time, rate or counter: IqClock numbers and dates
//     the samples and detects lost data;
//   - a value SDRconnect refuses (e.g. an unsupported sample rate) produces
//     no answer at all: setAndConfirm() asks for the value afterwards;
//   - IQ that arrives right after a retune may predate it: such blocks are
//     flagged `settling` for a guard time.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "hfc/iq_clock.hpp"
#include "hfc/sdrconnect.hpp"
#include "hfc/websocket.hpp"

namespace hfc::sdrconnect {

struct IqBlock {
  const int16_t* iq = nullptr;  // interleaved I, Q
  std::size_t pairs = 0;
  uint64_t index = 0;        // stream index of iq[0..1]
  double sampleRate = 0.0;   // for timing: reported if known, else measured (0: unknown)
  double nominalRate = 0.0;  // as reported by SDRconnect (0: not reported yet). Use this one
                             // to process or record IQ: the measured rate varies block to block
  double centerHz = 0.0;     // device_center_frequency when the block arrived (0: unknown)
  double utc = 0.0;          // estimated Unix time of the first sample
  bool discontinuity = false;
  bool settling = false;
  bool retuned = false;
  // Samples were lost at stream index gapIndex (<= index), about gapSeconds
  // of them; see IqClock::Stamp.
  bool gap = false;
  uint64_t gapIndex = 0;
  double gapSeconds = 0.0;
};

class Client {
 public:
  struct Handlers {
    std::function<void(Device, const std::string& property, const std::string& value)> onProperty;
    std::function<void(Device, const IqBlock&)> onIq;
    // 48 kHz stereo, `frames` L/R pairs.
    std::function<void(Device, const int16_t* lr, std::size_t frames)> onAudio;
    std::function<void(Device, const uint8_t* bins, std::size_t n)> onSpectrum;
    std::function<void(const std::string& reason)> onDisconnected;
  };

  Client();
  explicit Client(Handlers h);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Connects and asks for the basic properties of both devices.
  bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string* error);
  void disconnect();
  bool connected() const { return connected_; }
  std::string disconnectReason() const;

  bool send(const Message& m);
  bool setProperty(const std::string& property, const std::string& value, Device d = Device::Primary);
  bool requestProperty(const std::string& property, Device d = Device::Primary);
  bool enableIq(bool on);
  bool enableAudio(bool on);
  bool enableSpectrum(bool on);
  bool setDeviceStream(bool on);
  bool enableDevice(Device d, bool on);  // set_primary/secondary_device_enable
  bool selectDeviceIndex(int index);
  // "<serial>" or "<serial>:<mode>"; SDRplay's RSPduo example uses ":Dual".
  bool selectDeviceSerial(const std::string& serial);
  bool selectDeviceName(const std::string& name);
  bool startRecording(const std::string& type);  // "iq", "audio", "compressed_audio"
  bool stopRecording();
  bool applyDeviceProfile(const std::string& name);

  // Asks for `property` and waits for the answer.
  std::optional<std::string> query(const std::string& property, Device d, int timeoutMs);
  // Sets `property` and returns what SDRconnect reports afterwards: its
  // property_changed, or, when none comes within timeoutMs (SDRconnect is
  // silent both for a refused value and for an unchanged one), the answer
  // to a get_property. Compare with `value` to see whether it was accepted.
  std::optional<std::string> setAndConfirm(const std::string& property, const std::string& value, Device d,
                                           int timeoutMs);
  // The last value SDRconnect reported, if any.
  std::optional<std::string> cached(const std::string& property, Device d = Device::Primary) const;

  IqClock::Stats iqStats(Device d) const;
  void resetIqStats(Device d);
  // Message counts: text messages received, binary messages per type.
  uint64_t textMessages() const { return textCount_; }
  uint64_t binaryMessages(BinaryType t) const;
  uint64_t binaryBytes(BinaryType t) const;

 private:
  using Key = std::pair<int, std::string>;
  void readerLoop();
  void handleText(const std::string& text, double steady);
  void handleBinary(const WsMessage& msg, double steady, double utc);
  uint64_t reportCount(const Key& k) const;
  bool waitForReport(const Key& k, uint64_t after, int timeoutMs, std::string& value);

  Handlers handlers_;
  WebSocket ws_;
  std::thread reader_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> stopping_{false};
  std::string disconnectReason_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<Key, std::string> props_;
  std::map<Key, uint64_t> reports_;  // number of reports received per property
  IqClock clocks_[2];
  std::atomic<uint64_t> textCount_{0};
  std::atomic<uint64_t> binCount_[7] = {};
  std::atomic<uint64_t> binBytes_[7] = {};
};

}  // namespace hfc::sdrconnect
