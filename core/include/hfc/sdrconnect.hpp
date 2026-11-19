// SDRconnect WebSocket API 1.0.3 (SDRplay, "SDRconnect_WebSocket_API.pdf"):
// message envelope, event types, properties and binary payload types, as
// documented. Nothing here is guessed; behaviour the document does not
// state (what rate the IQ arrives at, what a refused value looks like, ...)
// is handled in client.hpp and measured by tools/sdrc_probe.
#pragma once

#include <cstdint>
#include <string>

namespace hfc::sdrconnect {

constexpr uint16_t kDefaultPort = 5454;

// A dual-tuner device (RSPduo in dual tuner mode) has a primary and a
// secondary device; messages without a device field mean the primary.
enum class Device { Primary = 0, Secondary = 1 };
const char* deviceName(Device d);
// "primary", "secondary" (any case) or "" (= primary). False for anything else.
bool parseDevice(const std::string& s, Device& d);

namespace event {
// From SDRconnect.
constexpr const char* kPropertyChanged = "property_changed";
constexpr const char* kGetPropertyResponse = "get_property_response";
// To SDRconnect.
constexpr const char* kSetProperty = "set_property";
constexpr const char* kGetProperty = "get_property";
constexpr const char* kIqStreamEnable = "iq_stream_enable";
constexpr const char* kAudioStreamEnable = "audio_stream_enable";
constexpr const char* kSpectrumEnable = "spectrum_enable";
constexpr const char* kDeviceStreamEnable = "device_stream_enable";
constexpr const char* kSelectedDevice = "selected_device";
constexpr const char* kSelectedDeviceSerial = "selected_device_serial";
constexpr const char* kSelectedDeviceName = "selected_device_name";
constexpr const char* kStartRecording = "start_recording";
constexpr const char* kStopRecording = "stop_recording";
constexpr const char* kApplyDeviceProfile = "apply_device_profile";
constexpr const char* kSetPrimaryDeviceEnable = "set_primary_device_enable";
constexpr const char* kSetSecondaryDeviceEnable = "set_secondary_device_enable";
}  // namespace event

namespace prop {
constexpr const char* kCenterFrequency = "device_center_frequency";  // Hz, hardware LO
constexpr const char* kSampleRate = "device_sample_rate";            // Hz
constexpr const char* kVfoFrequency = "device_vfo_frequency";        // Hz
constexpr const char* kLnaState = "lna_state";
constexpr const char* kLnaStateMin = "lna_state_min";  // read-only
constexpr const char* kLnaStateMax = "lna_state_max";  // read-only
constexpr const char* kFilterBandwidth = "filter_bandwidth";
constexpr const char* kDemodulator = "demodulator";  // AM/USB/LSB/CW/SAM/NFM/WFM
constexpr const char* kDemodMaxBandwidth = "demod_max_bandwidth";
constexpr const char* kStarted = "started";
constexpr const char* kOverload = "overload";
constexpr const char* kCanControl = "can_control";
constexpr const char* kAudioMute = "audio_mute";
constexpr const char* kAgcEnable = "agc_enable";
constexpr const char* kSignalPower = "signal_power";
constexpr const char* kSignalSnr = "signal_snr";
constexpr const char* kValidAntennas = "valid_antennas";  // comma separated
constexpr const char* kActiveAntenna = "active_antenna";
constexpr const char* kValidDevices = "valid_devices";  // comma separated display names
constexpr const char* kActiveDevice = "active_device";
constexpr const char* kApiVersion = "api_version";  // since 1.0.9 (API 1.0.3)
}  // namespace prop

// Binary messages: a little-endian uint16 type, then the payload.
enum class BinaryType : uint16_t {
  AudioPrimary = 1,      // S16 stereo 48 kHz, LRLR
  IqPrimary = 2,         // S16 interleaved IQ
  SpectrumPrimary = 3,   // U8 FFT bins
  AudioSecondary = 4,
  IqSecondary = 5,
  SpectrumSecondary = 6,
};

struct Message {
  std::string eventType;
  std::string property;
  std::string value;
  std::string device;  // "" leaves the field out
};

std::string encodeMessage(const Message& m);
bool decodeMessage(const std::string& json, Message& m, std::string* error);

}  // namespace hfc::sdrconnect
