// Measuring how SDRconnect delivers IQ on this PC and receiver - the things
// the API document does not say (docs/RESEARCH.md, section 1.3) - as a
// report in JSON. Used by tools/sdrc_probe and by the program's diagnostic
// report.
//
// It changes settings while it runs (the sample rate; the antenna with
// `antennas`; the device mode with `dualSerial`) and restores them at the end.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "hfc/client.hpp"

namespace hfc {

// Level statistics of the IQ received while measuring: whoever owns the
// client passes every IQ block to add() (from the client's reader thread).
class ProbeLevels {
 public:
  void add(sdrconnect::Device d, const sdrconnect::IqBlock& b);
  void reset();
  struct Snapshot {
    double sumSq = 0, sumI = 0, sumQ = 0;
    uint64_t n = 0, discontinuities = 0;
    int peak = 0;
  };
  Snapshot get(sdrconnect::Device d) const;

 private:
  mutable std::mutex mu_;
  Snapshot s_[2];
};

struct ProbeOptions {
  double seconds = 5;      // measuring time per setting
  bool tryRates = true;    // each of `rates` in turn (when SDRconnect accepts control)
  std::vector<std::string> rates = {"62500",   "125000",  "250000",  "500000",  "1000000",
                                    "2000000", "3000000", "6000000", "8000000", "10000000"};
  bool antennas = false;   // each antenna in turn, its noise level
  std::string dualSerial;  // also RSPduo dual-tuner mode ("SERIAL:Dual")
  std::function<bool()> stop;                     // asked often; true ends the measuring early
  std::function<void(const std::string&)> say;   // one line of progress at a time
};

// Measures on a connected client and restores what it changed. Returns a
// JSON object: "properties", "iq_current", "sample_rates", "antennas",
// "dual" (if asked for), "restored". IQ is switched off at the end.
std::string runProbe(sdrconnect::Client& c, ProbeLevels& levels, const ProbeOptions& o);

}  // namespace hfc
