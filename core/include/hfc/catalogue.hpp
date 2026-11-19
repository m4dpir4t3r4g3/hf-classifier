// The mode catalogue (catalogue/modes.json, embedded at build time) and
// the first, parametric layer of classification: which catalogued modes an
// emission's measured features are consistent with.
//
// A mode's "match" blocks give what the feature meter can measure: the
// modulation family (FSK, MFSK, PSK, multi-carrier, OOK), and per family
// the shift, symbol rate, number of tones and their spacing, each a list of
// allowed values, with the provenance of those numbers. An emission is
// consistent with a block when its family is the same and every parameter
// both sides have agrees:
//
//   shift          within 7 % (at least 10 Hz)
//   symbol rate    within 3 %
//   tone spacing   within 6 %
//   tones          within the block's range, or within 2 of a listed count
//                  (an 8 s measurement may not see every tone)
//   frame          within 2 %: evidence for the mode when it agrees, never
//                  against it when it does not or was not measured (a modem
//                  repeats itself at other periods too: in its preamble, its
//                  idle pattern, or over several frames when its probes
//                  vary). When it agrees, a symbol rate that does not counts
//                  as unmeasured: a frame puts lines at every multiple of
//                  1 / frame in the spectrum the rate is measured on, and the
//                  meter can take one of them for the rate. An agreeing frame
//                  lists a mode even when the family measured is
//                  multi-carrier or undecided (the frame's lines can make a
//                  single carrier look like either), as partial
//
// A parameter the block lists but the meter did not measure (no symbol rate
// found) is not held against it, but the match is marked partial (not so
// the frame). PSK needs its symbol rate or its frame; MFSK and
// multi-carrier need two matching parameters.
// Ranked by the parameters matched, then frame matches first (a frame is
// rarer than a rate), complete before partial, then by provenance
// (documented, measured, vendor, observed, guess; a match's provenance is
// the weakest of the parameters that agreed). This lists candidates, it does not identify:
// several modes often share every measured parameter (SITOR-A, DSC-HF and
// CODAN Selcal are all 100 Bd / 170 Hz FSK). A frame period tells some of
// the serial modems apart (STANAG 4285, 4529, 4539, MIL-STD-188-110,
// CIS BPSK 1200), which share their symbol rate with others.
#pragma once

#include <string>
#include <vector>

#include "hfc/features.hpp"

namespace hfc {

struct CatalogueMatch {
  std::string mode;        // the catalogue's name
  std::string provenance;  // of the parameters that matched
  std::string source;      // key into the catalogue's sources
  int matched = 0;         // parameters that agreed
  bool partial = false;    // a listed parameter was not measured
  bool frame = false;      // the frame period agreed
};

class Catalogue {
 public:
  // The embedded catalogue, parsed once.
  static const Catalogue& builtin();

  bool load(const std::string& json, std::string* error);
  std::size_t modes() const { return modes_.size(); }
  // The modes the features are consistent with, best first (at most `max`).
  std::vector<CatalogueMatch> match(const EmissionFeatures& f, std::size_t max = 5) const;

 private:
  struct Block {
    std::string modulation, provenance, source;
    std::vector<double> shift, baud, tones, spacing, frame;
    double tonesMin = 0, tonesMax = 0;  // 0: no range
    std::string frameProvenance, frameSource;  // when not the block's
  };
  struct Mode {
    std::string name;
    std::vector<Block> blocks;
  };
  std::vector<Mode> modes_;
};

// catalogue/modes.json as embedded (cmake/embed.cmake).
const std::string& catalogueJson();

}  // namespace hfc
