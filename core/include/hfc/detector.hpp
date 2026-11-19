// Finds emissions in the wideband spectrum and follows them over time.
//
//   hop spectra (Channelizer) -> average over ~0.13 s -> frame (dB)
//     -> noise floor per segment of ~8 kHz: P30 + (P30 - P10) of the bins,
//        interpolated, smoothed over time (falls fast, rises slowly; a rise
//        across most of the band, e.g. a gain change, is taken at once)
//     -> occupied bins: more than `thresholdDb` above the floor (CFAR)
//     -> segments: runs of occupied bins, gaps up to ~60 Hz bridged; and
//        wide regions (>= 600 Hz) whose running median over ~180 Hz stands
//        3 dB above the floor, made one segment each: a modem a few dB
//        above the noise, most of whose bins miss the threshold and whose
//        fragments would otherwise come and go as separate emissions (a
//        median, unlike a mean, stays low between separate narrow signals;
//        not inside setNoGroupRanges(), nor where a segment peaks 15 dB above
//        the floor: a strong signal's segments are right, and the keying
//        sidebands of a strong wide-shift FSK signal would otherwise join
//        one tone and not the other)
//     -> tracker: segments matched to emissions by frequency overlap;
//        emissions that overlap, or that start together (within
//        `groupOnsetSeconds`) less than `groupGapHz` apart, are one
//        emission (the tones of an FSK or MFSK signal, the carriers of a
//        multi-carrier modem), except inside setNoGroupRanges() (e.g. the
//        FT8 sub-bands, where unrelated signals start together every slot)
//
// An emission is reported once it has been seen in `confirmFrames` frames
// (bursts and keying are bridged for `holdSeconds`), and ends when it has
// not been seen for that long. Measured per emission: start and end time,
// detected band (edges of the bins above the threshold, smoothed over
// frames), occupied bandwidth (the band holding 99 % of the power above the
// floor; narrower than the detected band for strong signals, whose window
// skirts and key clicks rise above the threshold), peak and mean SNR, duty
// cycle (frames seen / frames alive), drift (slope of its centre frequency).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace hfc {

struct Segment {
  std::size_t lo = 0, hi = 0;  // bins, inclusive, in the frame's ascending order
  float peakDb = 0;            // above the noise floor
  float snrDb = 0;             // total power above the floor, relative to the floor in the segment's band
  double centreBin = 0;        // power-weighted
};

struct Emission {
  uint64_t id = 0;
  double startUtc = 0, endUtc = 0;   // endUtc: last time seen
  double loHz = 0, hiHz = 0;         // RF, edges of the detected band (smoothed)
  double occLoHz = 0, occHiHz = 0;   // RF, edges of the 99 % power band (smoothed)
  double centreHz = 0;               // RF, power-weighted, smoothed
  double bandwidthHz() const { return hiHz - loHz; }
  double occupiedHz() const { return occHiHz - occLoHz; }
  float peakSnrDb = 0, meanSnrDb = 0;
  double duty = 0;                   // 0..1
  double driftHzPerSecond = 0;
  uint32_t framesSeen = 0;
  bool confirmed = false, ended = false;
};

class Detector {
 public:
  struct Config {
    double frameSeconds = 0.128;   // spectra averaged into one frame
    double thresholdDb = 7.0;      // above the noise floor
    double segmentHz = 8000.0;     // noise floor segments
    double bridgeHz = 60.0;        // gaps bridged inside a segment
    double edgeGuard = 0.04;       // fraction of the band ignored at each edge (anti-alias roll-off)
    int confirmFrames = 3;
    double holdSeconds = 3.0;
    double minBandwidthHz = 0.0;   // narrower segments are ignored (0: 2 bins)
    double groupOnsetSeconds = 0.4; // emissions starting this close together...
    double groupGapHz = 1100.0;     // ...and at most this far apart are one
    double groupMaxSnrDiffDb = 10.0; // ...if their SNRs are this close
    double wideSmoothHz = 180.0;    // running median for wide emissions...
    double wideThresholdDb = 3.0;   // ...this far above the floor...
    double wideMinHz = 600.0;       // ...over at least this (0: off)...
    double wideMaxPeakDb = 15.0;    // ...where no segment peaks higher (weak)
  };
  // Frame callback for displays: power in dB per bin (ascending), the
  // noise floor in dB, and the frame's UTC time.
  using FrameFn = std::function<void(const std::vector<float>& db, const std::vector<float>& floorDb, double utc)>;
  using EmissionFn = std::function<void(const Emission&)>;

  Detector(double sampleRate, std::size_t bins, double hopSeconds);
  Detector(double sampleRate, std::size_t bins, double hopSeconds, const Config& cfg);

  // Centre frequency of the receiver (RF Hz); a change ends every emission.
  void setCentre(double hz, double utc);
  double centre() const { return centre_; }

  // One hop's power spectrum (Channelizer::SpectrumFn) taken at `utc`.
  void addSpectrum(const float* power, std::size_t n, double utc);

  void setFrameCallback(FrameFn fn) { frameFn_ = std::move(fn); }
  // Called when an emission is confirmed, about once a second while it
  // lasts (updated measurements), and when it ends.
  void setEmissionCallbacks(EmissionFn started, EmissionFn updated, EmissionFn ended);

  // RF ranges where emissions are never grouped by onset.
  void setNoGroupRanges(std::vector<std::pair<double, double>> ranges) { noGroup_ = std::move(ranges); }
  // Ends every open emission (end of input, retune).
  void endAll(double utc);
  const std::vector<Emission>& active() const { return active_; }
  double binHz() const { return rate_ / static_cast<double>(bins_); }
  double binToHz(double bin) const;  // RF
  uint64_t frames() const { return frames_; }
  // Ids are given in order from this one (default 1): a new detector can
  // carry on the numbering of the one it replaces.
  void setNextId(uint64_t id) { nextId_ = id; }
  uint64_t nextId() const { return nextId_; }

 private:
  struct Track {
    Emission e;
    double lastSeen = 0, lastReport = 0;
    uint32_t framesAlive = 0, aliveWhenSeen = 0;
    double snrSum = 0;
    std::map<long, double> power;  // bin -> power above the floor, summed over frames
    // Drift regression on (time since start, centre - f0).
    double f0 = 0;
    double st = 0, sf = 0, stt = 0, stf = 0, n = 0;
  };
  void runFrame(double utc);
  void noiseFloor();
  void findSegments();
  void findWideSegments();
  Segment makeSegment(std::size_t lo, std::size_t hi) const;
  bool inNoGroup(double loHz, double hiHz) const;
  void track(double utc);
  void addPower(Track& tr, const Segment& g) const;
  void updateOccupied(Track& tr) const;

  Config cfg_;
  double rate_;
  std::size_t bins_;
  int hopsPerFrame_;
  double centre_ = 0;
  std::vector<double> acc_;
  int accHops_ = 0;
  std::vector<float> db_, floor_, floorSmooth_;
  bool floorInit_ = false;
  std::vector<Segment> segments_;
  std::vector<Track> tracks_;
  std::vector<Emission> active_;
  std::vector<std::pair<double, double>> noGroup_;
  int skipHops_ = 0;  // hops ignored after a retune (the FFT still holds old samples)
  uint64_t nextId_ = 1, frames_ = 0;
  FrameFn frameFn_;
  EmissionFn started_, updated_, ended_;
};

}  // namespace hfc
