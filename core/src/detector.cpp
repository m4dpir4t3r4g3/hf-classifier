#include "hfc/detector.hpp"

#include <algorithm>
#include <cmath>

namespace hfc {

namespace {

float toDb(double p) { return static_cast<float>(10.0 * std::log10(p + 1e-30)); }
double fromDb(double db) { return std::pow(10.0, db / 10.0); }

}  // namespace

Detector::Detector(double sampleRate, std::size_t bins, double hopSeconds)
    : Detector(sampleRate, bins, hopSeconds, Config()) {}

Detector::Detector(double sampleRate, std::size_t bins, double hopSeconds, const Config& cfg)
    : cfg_(cfg), rate_(sampleRate), bins_(bins) {
  hopsPerFrame_ = std::max(1, static_cast<int>(std::lround(cfg_.frameSeconds / hopSeconds)));
  acc_.assign(bins_, 0.0);
  db_.resize(bins_);
  floor_.resize(bins_);
  floorSmooth_.resize(bins_);
}

void Detector::setEmissionCallbacks(EmissionFn started, EmissionFn updated, EmissionFn ended) {
  started_ = std::move(started);
  updated_ = std::move(updated);
  ended_ = std::move(ended);
}

double Detector::binToHz(double bin) const {
  return centre_ + (bin - static_cast<double>(bins_) / 2.0) * binHz();
}

void Detector::setCentre(double hz, double utc) {
  if (hz == centre_) return;
  endAll(utc);
  centre_ = hz;
  std::fill(acc_.begin(), acc_.end(), 0.0);
  accHops_ = 0;
  floorInit_ = false;
  // The next hops' FFT blocks still hold samples from before the retune.
  skipHops_ = 2;
}

void Detector::addSpectrum(const float* power, std::size_t n, double utc) {
  if (n != bins_) return;
  if (skipHops_ > 0) {
    --skipHops_;
    return;
  }
  for (std::size_t i = 0; i < n; ++i) acc_[i] += power[i];
  if (++accHops_ < hopsPerFrame_) return;
  for (std::size_t i = 0; i < n; ++i) db_[i] = toDb(acc_[i] / accHops_);
  std::fill(acc_.begin(), acc_.end(), 0.0);
  accHops_ = 0;
  runFrame(utc);
}

void Detector::runFrame(double utc) {
  ++frames_;
  noiseFloor();
  findSegments();
  track(utc);
  if (frameFn_) frameFn_(db_, floorSmooth_, utc);
}

// The level of the noise, per bin. In each segment the 10th and 30th
// percentiles are taken: for noise alone their distance measures the spread
// of the averaged power, and the mean lies about that far above the 30th.
// Using low percentiles keeps the estimate right when signals occupy most
// of a segment.
void Detector::noiseFloor() {
  const std::size_t seg = std::max<std::size_t>(64, static_cast<std::size_t>(cfg_.segmentHz / binHz()));
  const std::size_t guard = static_cast<std::size_t>(cfg_.edgeGuard * static_cast<double>(bins_));
  const std::size_t lo = guard, hi = bins_ - guard;
  std::vector<double> centres, levels;
  std::vector<float> tmp;
  for (std::size_t s = lo; s < hi; s += seg) {
    const std::size_t e = std::min(hi, s + seg);
    if (e - s < seg / 2 && !levels.empty()) break;
    tmp.assign(db_.begin() + static_cast<std::ptrdiff_t>(s), db_.begin() + static_cast<std::ptrdiff_t>(e));
    const std::size_t i10 = tmp.size() / 10, i30 = tmp.size() * 3 / 10;
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(i30), tmp.end());
    const float p30 = tmp[i30];
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(i10), tmp.begin() + static_cast<std::ptrdiff_t>(i30));
    const float p10 = tmp[i10];
    centres.push_back(0.5 * static_cast<double>(s + e));
    levels.push_back(p30 + (p30 - p10));
  }
  if (levels.empty()) return;
  for (std::size_t i = 0; i < bins_; ++i) {
    const double x = static_cast<double>(i);
    double v;
    if (x <= centres.front()) v = levels.front();
    else if (x >= centres.back()) v = levels.back();
    else {
      const std::size_t j = static_cast<std::size_t>(std::upper_bound(centres.begin(), centres.end(), x) - centres.begin());
      const double t = (x - centres[j - 1]) / (centres[j] - centres[j - 1]);
      v = levels[j - 1] + t * (levels[j] - levels[j - 1]);
    }
    floor_[i] = static_cast<float>(v);
  }
  if (!floorInit_) {
    floorSmooth_ = floor_;
    floorInit_ = true;
    return;
  }
  // A rise across most of the band is the noise itself getting stronger
  // (gain or antenna change, band opening), not signals: signals cannot lift
  // the low percentiles of most segments at once. Taken at once; otherwise
  // the whole band would sit above a stale floor and turn into emissions.
  std::vector<float> diff;
  const std::size_t stride = std::max<std::size_t>(1, bins_ / 512);
  for (std::size_t i = 0; i < bins_; i += stride) diff.push_back(floor_[i] - floorSmooth_[i]);
  std::nth_element(diff.begin(), diff.begin() + static_cast<std::ptrdiff_t>(diff.size() / 2), diff.end());
  const float rise = diff[diff.size() / 2];
  if (rise > 1.0f)
    for (auto& v : floorSmooth_) v += rise;
  for (std::size_t i = 0; i < bins_; ++i) {
    const float d = floor_[i] - floorSmooth_[i];
    floorSmooth_[i] += d < 0 ? 0.5f * d : 0.05f * d;
  }
}

void Detector::findSegments() {
  segments_.clear();
  const std::size_t guard = static_cast<std::size_t>(cfg_.edgeGuard * static_cast<double>(bins_));
  const std::size_t bridge = std::max<std::size_t>(1, static_cast<std::size_t>(cfg_.bridgeHz / binHz()));
  const std::size_t minBins = std::max<std::size_t>(2, static_cast<std::size_t>(cfg_.minBandwidthHz / binHz()));
  const float thr = static_cast<float>(cfg_.thresholdDb);
  std::size_t i = guard;
  while (i < bins_ - guard) {
    if (db_[i] <= floorSmooth_[i] + thr) {
      ++i;
      continue;
    }
    std::size_t lo = i, hi = i, gap = 0;
    for (std::size_t j = i + 1; j < bins_ - guard; ++j) {
      if (db_[j] > floorSmooth_[j] + thr) {
        hi = j;
        gap = 0;
      } else if (++gap > bridge) {
        break;
      }
    }
    i = hi + 1;
    if (hi - lo + 1 < minBins) continue;
    segments_.push_back(makeSegment(lo, hi));
  }
  findWideSegments();
}

Segment Detector::makeSegment(std::size_t lo, std::size_t hi) const {
  Segment s;
  s.lo = lo;
  s.hi = hi;
  double sig = 0, noise = 0, wsum = 0, wbin = 0, peak = -1e9;
  for (std::size_t k = lo; k <= hi; ++k) {
    const double p = fromDb(db_[k]), nf = fromDb(floorSmooth_[k]);
    const double ex = std::max(0.0, p - nf);
    sig += ex;
    noise += nf;
    wsum += ex;
    wbin += ex * static_cast<double>(k);
    peak = std::max(peak, static_cast<double>(db_[k] - floorSmooth_[k]));
  }
  s.peakDb = static_cast<float>(peak);
  s.snrDb = toDb(sig / std::max(noise, 1e-30));
  s.centreBin = wsum > 0 ? wbin / wsum : 0.5 * static_cast<double>(lo + hi);
  return s;
}

bool Detector::inNoGroup(double loHz, double hiHz) const {
  for (const auto& r : noGroup_)
    if (hiHz > r.first && loHz < r.second) return true;
  return false;
}

// Wide regions: the running median of the frame over wideSmoothHz, at least
// wideThresholdDb above the floor over at least wideMinHz. Each becomes one
// segment, taking in the threshold's segments inside it.
void Detector::findWideSegments() {
  if (cfg_.wideMinHz <= 0) return;
  const std::size_t guard = static_cast<std::size_t>(cfg_.edgeGuard * static_cast<double>(bins_));
  const std::size_t half = std::max<std::size_t>(1, static_cast<std::size_t>(0.5 * cfg_.wideSmoothHz / binHz()));
  const std::size_t minBins = static_cast<std::size_t>(cfg_.wideMinHz / binHz());
  const std::size_t bridge = std::max<std::size_t>(1, static_cast<std::size_t>(cfg_.bridgeHz / binHz()));
  const float thr = static_cast<float>(cfg_.wideThresholdDb);
  if (bins_ < 2 * (guard + half) + minBins) return;
  // The median is taken every half window (the regions sought are many
  // windows wide), gaps counted in those steps.
  const std::size_t step = std::max<std::size_t>(1, half / 2);
  const std::size_t bridgeSteps = std::max<std::size_t>(1, bridge / step);
  std::vector<std::pair<std::size_t, std::size_t>> runs;
  std::vector<float> win(2 * half + 1);
  std::size_t lo = 0, hi = 0, gap = 0;
  bool open = false;
  auto close = [&] {
    if (open && hi - lo + 1 >= minBins) runs.emplace_back(lo, hi);
    open = false;
  };
  for (std::size_t i = guard + half; i + half < bins_ - guard; i += step) {
    std::copy(db_.begin() + static_cast<std::ptrdiff_t>(i - half), db_.begin() + static_cast<std::ptrdiff_t>(i + half + 1), win.begin());
    std::nth_element(win.begin(), win.begin() + static_cast<std::ptrdiff_t>(half), win.end());
    if (win[half] > floorSmooth_[i] + thr) {
      if (!open) {
        open = true;
        lo = i;
      }
      hi = std::min(i + step - 1, bins_ - guard - 1);
      gap = 0;
    } else if (open && ++gap > bridgeSteps) {
      close();
    }
  }
  close();
  for (const auto& [a, b] : runs) {
    if (inNoGroup(binToHz(static_cast<double>(a)), binToHz(static_cast<double>(b)))) continue;
    auto inside = [&](const Segment& s) { return s.hi + bridge >= a && s.lo <= b + bridge; };
    bool strong = false;
    for (const Segment& s : segments_) strong = strong || (inside(s) && s.peakDb >= cfg_.wideMaxPeakDb);
    if (strong) continue;
    std::size_t from = a, to = b;
    std::vector<Segment> keep;
    for (const Segment& s : segments_) {
      if (inside(s)) {
        from = std::min(from, s.lo);
        to = std::max(to, s.hi);
      } else {
        keep.push_back(s);
      }
    }
    keep.push_back(makeSegment(from, to));
    segments_.swap(keep);
  }
  std::sort(segments_.begin(), segments_.end(), [](const Segment& x, const Segment& y) { return x.lo < y.lo; });
}

// The power above the floor in each bin of a segment, added to the
// emission's spectrum (summed over its life).
void Detector::addPower(Track& tr, const Segment& g) const {
  for (std::size_t k = g.lo; k <= g.hi; ++k) {
    const double ex = fromDb(db_[k]) - fromDb(floorSmooth_[k]);
    if (ex > 0) tr.power[static_cast<long>(k)] += ex;
  }
}

// Occupied band: from the emission's summed spectrum, the band with 0.5 %
// of the power below it and 0.5 % above (the ITU-R SM.328 definition).
void Detector::updateOccupied(Track& tr) const {
  double total = 0;
  for (const auto& [k, v] : tr.power) total += v;
  if (total <= 0) return;
  double acc = 0;
  bool loSet = false;
  for (const auto& [k, v] : tr.power) {
    if (!loSet && acc + v >= 0.005 * total) {
      tr.e.occLoHz = binToHz(static_cast<double>(k) - 0.5);
      loSet = true;
    }
    acc += v;
    if (acc >= 0.995 * total) {
      tr.e.occHiHz = binToHz(static_cast<double>(k) + 0.5);
      break;
    }
  }
}

void Detector::track(double utc) {
  const double bin = binHz();
  const double margin = 2 * bin;
  std::vector<int> owner(segments_.size(), -1);
  // Each segment goes to the track it overlaps most. Only tracks that can
  // overlap it are tried (found by their lower edges, sorted), in the order
  // of tracks_ (so that ties go as they always did): on a busy band there
  // are a thousand of each, and trying every pair took most of the
  // detector's time.
  {
    std::vector<std::pair<double, int>> byLo(tracks_.size());
    double maxWidth = 0;
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
      byLo[t] = {tracks_[t].e.loHz - margin, static_cast<int>(t)};
      maxWidth = std::max(maxWidth, tracks_[t].e.hiHz - tracks_[t].e.loHz + 2 * margin);
    }
    std::sort(byLo.begin(), byLo.end());
    std::vector<int> near;
    for (std::size_t s = 0; s < segments_.size(); ++s) {
      const double lo = binToHz(static_cast<double>(segments_[s].lo) - 0.5);
      const double hi = binToHz(static_cast<double>(segments_[s].hi) + 0.5);
      // An overlap needs the track's lower edge below `hi` and its upper
      // edge, at most maxWidth above its lower one, above `lo`.
      auto first = std::upper_bound(byLo.begin(), byLo.end(), std::make_pair(lo - maxWidth, -1));
      near.clear();
      for (auto it = first; it != byLo.end() && it->first < hi; ++it) near.push_back(it->second);
      std::sort(near.begin(), near.end());
      double best = 0;
      for (int t : near) {
        const double ov = std::min(hi, tracks_[static_cast<std::size_t>(t)].e.hiHz + margin) -
                          std::max(lo, tracks_[static_cast<std::size_t>(t)].e.loHz - margin);
        if (ov > best) {
          best = ov;
          owner[s] = t;
        }
      }
    }
  }
  // Each track's segments, in their order.
  std::vector<std::vector<std::size_t>> segmentsOf(tracks_.size());
  for (std::size_t s = 0; s < segments_.size(); ++s)
    if (owner[s] >= 0) segmentsOf[static_cast<std::size_t>(owner[s])].push_back(s);
  // Combine the segments of each track (an FSK signal can show as several).
  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    double lo = 1e300, hi = -1e300, wsum = 0, wf = 0;
    float peak = -1e9f, snrLin = 0;
    for (std::size_t s : segmentsOf[t]) {
      const Segment& g = segments_[s];
      lo = std::min(lo, binToHz(static_cast<double>(g.lo) - 0.5));
      hi = std::max(hi, binToHz(static_cast<double>(g.hi) + 0.5));
      addPower(tracks_[t], g);
      const double w = fromDb(g.snrDb) * static_cast<double>(g.hi - g.lo + 1);
      wsum += w;
      wf += w * binToHz(g.centreBin);
      peak = std::max(peak, g.peakDb);
      snrLin += static_cast<float>(fromDb(g.snrDb));
    }
    if (wsum <= 0) continue;
    Track& tr = tracks_[t];
    const double centre = wf / wsum;
    const double a = 0.25;
    tr.e.loHz += a * (lo - tr.e.loHz);
    tr.e.hiHz += a * (hi - tr.e.hiHz);
    updateOccupied(tr);
    tr.e.centreHz += a * (centre - tr.e.centreHz);
    tr.e.peakSnrDb = std::max(tr.e.peakSnrDb, peak);
    tr.snrSum += 10.0 * std::log10(snrLin + 1e-30);
    ++tr.e.framesSeen;
    tr.lastSeen = utc;
    tr.aliveWhenSeen = tr.framesAlive + 1;
    const double x = utc - tr.e.startUtc, y = centre - tr.f0;
    tr.st += x;
    tr.sf += y;
    tr.stt += x * x;
    tr.stf += x * y;
    tr.n += 1;
  }
  // Tracks that are one emission are merged; the older (or the reported
  // one) absorbs the younger, which ends if it had been reported:
  //  - tracks that have grown over each other (a wide signal first seen as
  //    separate tones);
  //  - tracks that started together, close in frequency and level (the
  //    tones of an FSK or MFSK signal, the carriers of a multi-carrier
  //    modem), unless one of them has been reported for longer than the
  //    onset window or they lie in a no-group range.
  auto inNoGroup = [&](double hz) {
    for (const auto& r : noGroup_)
      if (hz >= r.first && hz <= r.second) return true;
    return false;
  };
  auto belongTogether = [&](const Track& x, const Track& y) {
    const Emission& a = x.e;
    const Emission& b = y.e;
    const double ov = std::min(a.hiHz, b.hiHz) - std::max(a.loHz, b.loHz);
    if (ov > 0.5 * std::min(a.bandwidthHz(), b.bandwidthHz())) return true;
    if (a.confirmed && b.confirmed) return false;
    if (std::fabs(a.startUtc - b.startUtc) > cfg_.groupOnsetSeconds) return false;
    if (-ov > cfg_.groupGapHz) return false;  // the gap between them
    const double snrA = x.snrSum / std::max<uint32_t>(1, a.framesSeen);
    const double snrB = y.snrSum / std::max<uint32_t>(1, b.framesSeen);
    if (std::fabs(snrA - snrB) > cfg_.groupMaxSnrDiffDb) return false;
    return !inNoGroup(a.centreHz) && !inNoGroup(b.centreHz);
  };
  std::vector<bool> absorbed(tracks_.size(), false);
  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    if (absorbed[t]) continue;
    for (std::size_t u = t + 1; u < tracks_.size(); ++u) {
      if (absorbed[u] || absorbed[t]) continue;
      if (!belongTogether(tracks_[t], tracks_[u])) continue;
      const Emission& a = tracks_[t].e;
      const Emission& b = tracks_[u].e;
      // The reported one survives; else the older.
      const bool keepT = a.confirmed != b.confirmed ? a.confirmed : a.startUtc <= b.startUtc;
      Track& keep = keepT ? tracks_[t] : tracks_[u];
      Track& gone = keepT ? tracks_[u] : tracks_[t];
      const bool grew = gone.e.loHz < keep.e.loHz || gone.e.hiHz > keep.e.hiHz;
      keep.e.loHz = std::min(keep.e.loHz, gone.e.loHz);
      keep.e.hiHz = std::max(keep.e.hiHz, gone.e.hiHz);
      for (const auto& [k, v] : gone.power) keep.power[k] += v;
      updateOccupied(keep);
      keep.e.peakSnrDb = std::max(keep.e.peakSnrDb, gone.e.peakSnrDb);
      keep.lastSeen = std::max(keep.lastSeen, gone.lastSeen);
      keep.e.startUtc = std::min(keep.e.startUtc, gone.e.startUtc);
      if (grew) {
        // The centre moved: it is the band's middle now, and the drift
        // measurement starts again from it.
        keep.e.centreHz = 0.5 * (keep.e.occLoHz + keep.e.occHiHz);
        keep.f0 = keep.e.centreHz;
        keep.st = keep.sf = keep.stt = keep.stf = keep.n = 0;
      }
      if (gone.e.confirmed) {
        gone.e.ended = true;
        gone.e.endUtc = gone.lastSeen;
        if (ended_) ended_(gone.e);
      }
      absorbed[keepT ? u : t] = true;
    }
  }
  for (std::size_t t = tracks_.size(); t-- > 0;)
    if (absorbed[t]) tracks_.erase(tracks_.begin() + static_cast<std::ptrdiff_t>(t));
  // New tracks for segments nobody owns.
  for (std::size_t s = 0; s < segments_.size(); ++s) {
    if (owner[s] >= 0) continue;
    const Segment& g = segments_[s];
    Track tr;
    tr.e.loHz = binToHz(static_cast<double>(g.lo) - 0.5);
    tr.e.hiHz = binToHz(static_cast<double>(g.hi) + 0.5);
    addPower(tr, g);
    updateOccupied(tr);
    tr.e.centreHz = binToHz(g.centreBin);
    tr.f0 = tr.e.centreHz;
    tr.e.startUtc = utc;
    tr.e.peakSnrDb = g.peakDb;
    tr.e.framesSeen = 1;
    tr.snrSum = g.snrDb;
    tr.lastSeen = utc;
    tr.framesAlive = 0;
    tracks_.push_back(tr);
  }
  // Confirm, report, end.
  std::vector<Track> keep;
  active_.clear();
  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    Track& tr = tracks_[t];
    ++tr.framesAlive;
    Emission& e = tr.e;
    e.endUtc = tr.lastSeen;
    e.meanSnrDb = static_cast<float>(tr.snrSum / std::max<uint32_t>(1, e.framesSeen));
    // Over the time it was present: up to its last frame.
    e.duty = static_cast<double>(e.framesSeen) / std::max<uint32_t>(1, std::max(tr.aliveWhenSeen, e.framesSeen));
    const double den = tr.n * tr.stt - tr.st * tr.st;
    e.driftHzPerSecond = tr.n >= 5 && den > 1e-9 ? (tr.n * tr.stf - tr.st * tr.sf) / den : 0.0;
    const double missing = utc - tr.lastSeen;
    if (!e.confirmed) {
      if (missing > 0.5 + cfg_.frameSeconds) continue;  // never confirmed: forget it
      if (static_cast<int>(e.framesSeen) >= cfg_.confirmFrames) {
        e.confirmed = true;
        e.id = nextId_++;
        tr.lastReport = utc;
        if (started_) started_(e);
      }
    } else if (missing > cfg_.holdSeconds) {
      e.ended = true;
      if (ended_) ended_(e);
      continue;
    } else if (utc - tr.lastReport >= 1.0) {
      tr.lastReport = utc;
      if (updated_) updated_(e);
    }
    if (e.confirmed) active_.push_back(e);
    keep.push_back(tr);
  }
  tracks_.swap(keep);
}

void Detector::endAll(double utc) {
  for (auto& tr : tracks_) {
    if (!tr.e.confirmed) continue;
    tr.e.ended = true;
    tr.e.endUtc = std::min(utc, tr.lastSeen);
    if (ended_) ended_(tr.e);
  }
  tracks_.clear();
  active_.clear();
}

}  // namespace hfc
