// Akula (CIS Navy burst FSK, 500 Bd, 1000 Hz shift): recognised by its
// published structure, never decoded: the traffic is encrypted, and nothing
// of it is printed or kept. The groups searched for are those analysts
// published and docs/RESEARCH.md section 3.6 found, with no errors, on the
// user's recordings:
//
//   sync          4 x 100101, 2 x 110001              (36 bits)
//   EOM + EOT     101111 100010 100010 101111 011110  (30 bits)
//
// A burst is reported when its end group is found with at most 2 bits
// wrong and, before it, the sync group with at most 3 wrong, a whole number
// of 6-bit words apart (random bits: about once in 10^12 positions). The
// end group alone is not enough: "data-only" bursts, which have no sync
// group, are therefore missed, because on the Signal Identification Wiki's
// samples the lone group, even with at most one bit wrong, turned up in
// another system's data (KG-STV). Both polarities are tried.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hfc {

// From bits (one polarity) to bursts.
class AkulaParser {
 public:
  struct Burst {
    bool sync = false;      // the sync group was found before the end group
    int words = 0;          // 6-bit words from the end of sync to the end of EOT
    int syncErrors = 0, endErrors = 0;
  };
  // Returns true when an end group completed a burst (then in burst()).
  bool bit(bool one);
  const Burst& burst() const { return burst_; }
  static std::string describe(const Burst& b);

 private:
  uint64_t shift_ = 0;
  uint64_t count_ = 0;
  uint64_t syncAt_ = 0;  // bit count at the end of the last sync group (0: none)
  int syncErrors_ = 0;
  Burst burst_;
};

// The two groups as bits, first sent first (for tests and generators).
std::vector<bool> akulaSync();
std::vector<bool> akulaEnd();

// From audio: `rate` in Hz, the two tones about `centreHz` (within 300 Hz).
class AkulaDetector {
 public:
  AkulaDetector(double rate, double centreHz);
  ~AkulaDetector();
  void process(const float* audio, std::size_t n);
  std::vector<std::string> take();  // bursts since the last take(), in words

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hfc
