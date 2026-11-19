#include "hfc/catalogue.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "hfc/json.hpp"

namespace hfc {

namespace {

std::vector<double> numbers(const std::string& arrayJson) {
  std::vector<double> out;
  std::vector<JsonField> el;
  if (arrayJson.empty() || !parseJsonArray(arrayJson, el, nullptr)) return out;
  for (const auto& e : el) out.push_back(std::atof(e.value.c_str()));
  return out;
}

int provenanceRank(const std::string& p) {
  if (p == "documented") return 4;
  if (p == "measured") return 3;
  if (p == "vendor") return 2;
  if (p == "observed") return 1;
  return 0;
}

bool nearAny(double v, const std::vector<double>& list, double rel, double absMin) {
  for (double x : list)
    if (std::fabs(v - x) <= std::max(absMin, rel * x)) return true;
  return false;
}

}  // namespace

const Catalogue& Catalogue::builtin() {
  static const Catalogue c = [] {
    Catalogue k;
    std::string err;
    if (!k.load(catalogueJson(), &err)) std::fprintf(stderr, "catalogue: %s\n", err.c_str());
    return k;
  }();
  return c;
}

bool Catalogue::load(const std::string& json, std::string* error) {
  modes_.clear();
  std::vector<JsonField> top;
  if (!parseJsonObject(json, top, error)) return false;
  std::vector<JsonField> modes;
  if (!parseJsonArray(jsonField(top, "modes"), modes, error)) return false;
  for (const auto& m : modes) {
    std::vector<JsonField> mf;
    if (!parseJsonObject(m.value, mf, error)) return false;
    const std::string matchJson = jsonField(mf, "match");
    if (matchJson.empty()) continue;
    Mode mode;
    mode.name = jsonField(mf, "name");
    std::vector<JsonField> blocks;
    if (!parseJsonArray(matchJson, blocks, error)) return false;
    for (const auto& b : blocks) {
      std::vector<JsonField> bf;
      if (!parseJsonObject(b.value, bf, error)) return false;
      Block k;
      k.modulation = jsonField(bf, "modulation");
      k.provenance = jsonField(bf, "provenance");
      k.source = jsonField(bf, "source");
      k.shift = numbers(jsonField(bf, "shift_hz"));
      k.baud = numbers(jsonField(bf, "baud"));
      k.tones = numbers(jsonField(bf, "tones"));
      k.spacing = numbers(jsonField(bf, "spacing_hz"));
      const auto range = numbers(jsonField(bf, "tones_range"));
      if (range.size() == 2) k.tonesMin = range[0], k.tonesMax = range[1];
      k.frame = numbers(jsonField(bf, "frame_ms"));
      k.frameProvenance = jsonField(bf, "frame_provenance");
      k.frameSource = jsonField(bf, "frame_source");
      if (k.frameProvenance.empty()) k.frameProvenance = k.provenance;
      if (k.frameSource.empty()) k.frameSource = k.source;
      mode.blocks.push_back(std::move(k));
    }
    if (!mode.blocks.empty()) modes_.push_back(std::move(mode));
  }
  return true;
}

std::vector<CatalogueMatch> Catalogue::match(const EmissionFeatures& f, std::size_t max) const {
  std::vector<CatalogueMatch> out;
  if (!f.valid || (f.modulation.empty() && f.frameMs <= 0)) return out;
  for (const auto& mode : modes_) {
    CatalogueMatch best;
    bool any = false;
    for (const auto& b : mode.blocks) {
      // The frame period: for the mode when it agrees, never against it.
      // When it agrees, a symbol rate that does not is taken as unmeasured:
      // a frame puts a line at every multiple of 1 / frame in the spectrum
      // the rate is measured on, and one of those may have been taken for it.
      const bool frame = !b.frame.empty() && f.frameMs > 0 && nearAny(f.frameMs, b.frame, 0.02, 0);
      if (b.modulation != f.modulation) {
        // An agreeing frame also lists a mode whose family the meter did not
        // see: the frame's own spectral lines can make a single carrier look
        // multi-carrier, or leave its family undecided (not FSK: its levels
        // are not taken on a framed signal with a varying envelope).
        if (frame && (f.modulation == "multi-carrier" || f.modulation.empty())) {
          if (!any || best.matched < 1 || (best.matched == 1 && !best.frame)) {
            best.mode = mode.name;
            best.provenance = provenanceRank(b.frameProvenance) < provenanceRank(b.provenance) ? b.frameProvenance : b.provenance;
            best.source = provenanceRank(b.frameProvenance) < provenanceRank(b.provenance) ? b.frameSource : b.source;
            best.matched = 1;
            best.partial = true;
            best.frame = true;
            any = true;
          }
        }
        continue;
      }
      int matched = 0;
      bool partial = false, ok = true;
      if (!b.shift.empty()) {
        if (nearAny(f.toneSpacingHz, b.shift, 0.07, 10)) ++matched;
        else ok = false;
      }
      if (ok && !b.baud.empty()) {
        if (f.symbolRate <= 0) partial = true;
        else if (nearAny(f.symbolRate, b.baud, 0.03, 0)) ++matched;
        else if (frame) partial = true;
        else ok = false;
      }
      if (ok && !b.spacing.empty()) {
        if (f.toneSpacingHz <= 0) partial = true;
        else if (nearAny(f.toneSpacingHz, b.spacing, 0.06, 0)) ++matched;
        else ok = false;
      }
      if (ok && (b.tonesMax > 0 || !b.tones.empty())) {
        const double t = f.tones;
        const bool inRange = b.tonesMax > 0 && t >= b.tonesMin && t <= b.tonesMax;
        if (inRange || nearAny(t, b.tones, 0, 2)) ++matched;
        else ok = false;
      }
      if (!ok) continue;
      std::string provenance = b.provenance, source = b.source;
      if (frame) {
        ++matched;
        if (provenanceRank(b.frameProvenance) < provenanceRank(provenance)) provenance = b.frameProvenance, source = b.frameSource;
      }
      // What each family needs to be worth listing.
      if (b.modulation == "PSK" && matched < 1) continue;
      if ((b.modulation == "MFSK" || b.modulation == "multi-carrier") && matched < 2) continue;
      if (b.modulation == "FSK" && matched < 1) continue;
      const bool better = !any || matched > best.matched || (matched == best.matched && frame > best.frame) ||
                          (matched == best.matched && frame == best.frame && partial < best.partial) ||
                          (matched == best.matched && frame == best.frame && partial == best.partial &&
                           provenanceRank(provenance) > provenanceRank(best.provenance));
      if (better) {
        best.mode = mode.name;
        best.provenance = provenance;
        best.source = source;
        best.matched = matched;
        best.partial = partial;
        best.frame = frame;
        any = true;
      }
    }
    if (any) out.push_back(best);
  }
  std::stable_sort(out.begin(), out.end(), [](const CatalogueMatch& a, const CatalogueMatch& b) {
    if (a.matched != b.matched) return a.matched > b.matched;
    if (a.frame != b.frame) return a.frame;  // a frame is rarer evidence than a rate
    if (a.partial != b.partial) return !a.partial;
    return provenanceRank(a.provenance) > provenanceRank(b.provenance);
  });
  if (out.size() > max) out.resize(max);
  return out;
}

}  // namespace hfc
