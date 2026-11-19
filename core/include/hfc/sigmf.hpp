// Writes IQ recordings in SigMF (https://github.com/sigmf/SigMF, core
// namespace v1.2): <base>.sigmf-data holds the samples as ci16_le, exactly as
// SDRconnect sends them; <base>.sigmf-meta holds the sample rate and one
// capture segment per stretch of contiguous samples (a new one after every
// retune and every detected gap, with its centre frequency and UTC time).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hfc {

// "2026-10-19T12:34:56.123456Z"
std::string isoUtc(double unixSeconds);
// The reverse, for ISO 8601 UTC times ("...Z", any number of decimals);
// a negative value if `text` is not one.
double parseIsoUtc(const std::string& text);

class SigmfWriter {
 public:
  struct Global {
    double sampleRate = 0.0;
    std::string hardware;     // core:hw
    std::string recorder;     // core:recorder
    std::string description;  // core:description
  };

  ~SigmfWriter();
  bool open(const std::string& basePath, const Global& g, std::string* error);
  bool isOpen() const { return data_ != nullptr; }
  // Appends `pairs` IQ pairs. `newCapture` starts a capture segment (forced
  // on the first call) at `centerHz` and `utc`.
  bool write(const int16_t* iq, std::size_t pairs, bool newCapture, double centerHz, double utc);
  // A capture segment starting at an already written sample (e.g. where a
  // gap turned out to be); kept in order, replacing one at the same sample.
  void addCapture(uint64_t start, double centerHz, double utc);
  // An annotation over [start, start + count) of the written samples.
  void annotate(uint64_t start, uint64_t count, const std::string& label, const std::string& comment);
  // Writes the metadata file and closes both files.
  bool close(std::string* error);
  uint64_t samplesWritten() const { return written_; }
  const std::string& basePath() const { return base_; }

 private:
  struct Capture {
    uint64_t start;
    double centerHz;
    double utc;
  };
  struct Annotation {
    uint64_t start, count;
    std::string label, comment;
  };
  std::string base_;
  Global global_;
  std::FILE* data_ = nullptr;
  uint64_t written_ = 0;
  std::vector<Capture> captures_;
  std::vector<Annotation> annotations_;
};

// Reads SigMF recordings (sdrc_record's, and other programs' in ci16_le,
// cf32_le, ci8 or cu8) as int16 IQ, a capture segment at a time, for
// playback through the classifier.
class SigmfReader {
 public:
  struct Capture {
    uint64_t start = 0;
    double centerHz = 0;  // 0: not given
    double utc = 0;       // 0: not given
  };

  ~SigmfReader();
  // `path`: the base name, or either file's name.
  bool open(const std::string& path, std::string* error);
  void close();
  double sampleRate() const { return rate_; }
  const std::string& datatype() const { return type_; }
  const std::string& description() const { return description_; }
  const std::vector<Capture>& captures() const { return captures_; }
  uint64_t samples() const { return total_; }
  uint64_t position() const { return pos_; }

  // Reads up to `pairs` samples into `iq` (interleaved int16), never across
  // the start of a capture segment. `cap` is the segment they belong to and
  // `utc` the time of the first (continuing the segment's datetime, or the
  // previous segment's time if it has none). 0 at the end.
  std::size_t read(int16_t* iq, std::size_t pairs, Capture& cap, double& utc);

 private:
  std::FILE* data_ = nullptr;
  std::string type_, description_;
  double rate_ = 0;
  int bytesPerSample_ = 4;
  uint64_t total_ = 0, pos_ = 0;
  std::vector<Capture> captures_;
  double lastUtc_ = 0;
  uint64_t lastUtcAt_ = 0;
  std::vector<uint8_t> raw_;
};

}  // namespace hfc
