// The catalogue matcher: features of known modes against the embedded
// catalogue/modes.json.
#include <algorithm>
#include <string>

#include "hfc/catalogue.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

EmissionFeatures feat(const char* modulation, int tones, double spacing, double rate) {
  EmissionFeatures f;
  f.valid = true;
  f.modulation = modulation;
  f.tones = tones;
  f.toneSpacingHz = spacing;
  f.symbolRate = rate;
  return f;
}

bool lists(const std::vector<CatalogueMatch>& m, const std::string& mode) {
  return std::any_of(m.begin(), m.end(), [&](const CatalogueMatch& x) { return x.mode == mode; });
}

}  // namespace

TEST(catalogue_loads_with_match_blocks) {
  const Catalogue& c = Catalogue::builtin();
  CHECK(c.modes() >= 40);
  Catalogue bad;
  std::string err;
  CHECK(!bad.load("{\"modes\": [", &err));
}

TEST(catalogue_lists_consistent_modes) {
  const Catalogue& c = Catalogue::builtin();
  // 100 Bd / 170 Hz FSK: SITOR-B (documented) first, and the others that
  // share these parameters.
  auto m = c.match(feat("FSK", 2, 170, 100));
  CHECK(!m.empty() && m[0].mode == "NAVTEX / SITOR-B" && m[0].provenance == "documented");
  CHECK(lists(m, "SITOR-A") && lists(m, "DSC-HF") && lists(m, "CODAN Selcal"));
  CHECK(!lists(m, "Akula") && !lists(m, "RTTY 45.45/170"));
  // 500 Bd / 1000 Hz: Akula.
  m = c.match(feat("FSK", 2, 1000, 500));
  CHECK(!m.empty() && m[0].mode == "Akula" && m[0].provenance == "measured");
  // 1000 Hz shift, rate not measured: partial matches, Akula and CIS-36-50.
  m = c.match(feat("FSK", 2, 995, 0));
  CHECK(lists(m, "Akula") && lists(m, "CIS-36-50 (T-600)"));
  CHECK(std::all_of(m.begin(), m.end(), [](const CatalogueMatch& x) { return x.partial; }));
  // RTTY 75 Bd / 850 Hz.
  m = c.match(feat("FSK", 2, 846, 75.2));
  CHECK(lists(m, "RTTY (other rates and shifts)") && !lists(m, "RTTY 50/450"));
  // 8 tones 250 Hz apart at 125 Bd: 2G ALE.
  m = c.match(feat("MFSK", 8, 249.5, 125));
  CHECK(!m.empty() && m[0].mode == "ALE 2G");
  // PSK at 2400 Bd: the serial modems and 3G/4G ALE, which it cannot tell apart.
  m = c.match(feat("PSK", 1, 0, 2399), 10);
  CHECK(lists(m, "STANAG 4285") && lists(m, "MIL-STD-188-110 serial / STANAG 4415") && lists(m, "ALE 3G / 4G (WALE)"));
  CHECK(m[0].mode == "ALE 3G / 4G (WALE)");  // documented, ranked first
  // PSK31.
  m = c.match(feat("PSK", 1, 0, 31.25));
  CHECK(!m.empty() && m[0].mode == "PSK31");
  // 16 tones 112.5 Hz apart at 75 Bd: CODAN 9001 (and 16-tone modems near it).
  m = c.match(feat("multi-carrier", 16, 112.5, 75));
  CHECK(lists(m, "CODAN 9001") && lists(m, "MIL-STD-188-110 App A (16-tone)"));
  // Nothing measured, nothing listed; a PSK without a rate neither.
  CHECK(c.match(feat("", 0, 0, 0)).empty());
  CHECK(c.match(feat("PSK", 1, 0, 0)).empty());
  // A rate no catalogued FSK mode has.
  CHECK(c.match(feat("FSK", 2, 333, 61)).empty());
}

TEST(features_json_carries_the_consistent_modes) {
  EmissionFeatures f = feat("FSK", 2, 170, 100);
  for (const auto& m : Catalogue::builtin().match(f)) f.consistent.push_back({m.mode, m.provenance, m.partial});
  const std::string j = f.json();
  CHECK(j.find("\"consistent\":[{\"mode\":\"NAVTEX / SITOR-B\",\"provenance\":\"documented\",\"partial\":false,\"frame\":false,\"listed\":false}") !=
        std::string::npos);
  CHECK(j.back() == '}');
}

TEST(catalogue_tells_serial_modems_apart_by_their_frame) {
  const Catalogue& c = Catalogue::builtin();
  auto withFrame = [](double rate, double frameMs) {
    EmissionFeatures f = feat("PSK", 1, 0, rate);
    f.frameMs = frameMs;
    return f;
  };
  // 2400 Bd with STANAG 4285's 106.67 ms frame: 4285 first, the frame
  // counting for it; the other 2400 Bd modes still listed.
  auto m = c.match(withFrame(2399, 106.6), 10);
  CHECK(!m.empty() && m[0].mode == "STANAG 4285" && m[0].frame && m[0].provenance == "vendor");
  CHECK(lists(m, "MIL-STD-188-110 serial / STANAG 4415") && lists(m, "ALE 3G / 4G (WALE)"));
  CHECK(std::count_if(m.begin(), m.end(), [](const CatalogueMatch& x) { return x.frame; }) == 1);
  // MIL-STD-188-110's probe periods; the frame's provenance is the weaker.
  m = c.match(withFrame(2400, 66.7), 10);
  CHECK(!m.empty() && m[0].mode == "MIL-STD-188-110 serial / STANAG 4415" && m[0].frame && m[0].provenance == "observed");
  m = c.match(withFrame(2400, 200.2), 10);
  CHECK(!m.empty() && m[0].mode == "MIL-STD-188-110 serial / STANAG 4415");
  m = c.match(withFrame(2400, 119.6), 10);
  CHECK(!m.empty() && m[0].mode == "STANAG 4539 / MIL-STD-188-110 App C");
  // 1200 Bd: STANAG 4529 (213.33 ms) and CIS BPSK 1200 (426.67 ms).
  m = c.match(withFrame(1200, 213.3), 10);
  CHECK(!m.empty() && m[0].mode == "STANAG 4529" && lists(m, "CIS BPSK 1200"));
  m = c.match(withFrame(1201, 425.9), 10);
  CHECK(!m.empty() && m[0].mode == "CIS BPSK 1200" && lists(m, "STANAG 4529"));
  // A frame that agrees with none, or none measured: nothing changes.
  for (double fr : {0.0, 76.7}) {
    m = c.match(withFrame(2399, fr), 10);
    CHECK(!m.empty() && m[0].mode == "ALE 3G / 4G (WALE)" && lists(m, "STANAG 4285"));
    CHECK(std::none_of(m.begin(), m.end(), [](const CatalogueMatch& x) { return x.frame || x.partial; }));
  }
  // The frame alone (rate not measured) lists the mode, marked partial.
  m = c.match(withFrame(0, 106.7), 10);
  CHECK(!m.empty() && m[0].mode == "STANAG 4285" && m[0].partial);
  // A rate taken from one of the frame's own lines (795 = 53 x 15 Hz): the
  // agreeing frame keeps the mode, partial; no other 2400 Bd mode is listed.
  m = c.match(withFrame(795, 66.7), 10);
  CHECK(!m.empty() && m[0].mode == "MIL-STD-188-110 serial / STANAG 4415" && m[0].frame && m[0].partial);
  CHECK(!lists(m, "STANAG 4285"));
  // ... or half the rate (1200, another of its lines): the frame match
  // ranks above the 1200 Bd modes that match on the rate alone.
  m = c.match(withFrame(1200, 66.7), 10);
  CHECK(!m.empty() && m[0].mode == "MIL-STD-188-110 serial / STANAG 4415" && lists(m, "STANAG 4529"));
  // A single carrier whose frame's lines made it look multi-carrier, or
  // left its family undecided: the agreeing frame still lists the mode.
  for (const char* family : {"multi-carrier", ""}) {
    EmissionFeatures f = feat(family, 6, 194, 0);
    f.frameMs = 213.3;
    m = c.match(f, 10);
    CHECK(!m.empty() && m[0].mode == "STANAG 4529" && m[0].frame && m[0].partial);
  }
  // Not for FSK, nor without a frame that agrees.
  EmissionFeatures fsk = feat("FSK", 2, 170, 45.45);
  fsk.frameMs = 106.7;
  CHECK(!lists(c.match(fsk, 10), "STANAG 4285"));
  EmissionFeatures mc = feat("multi-carrier", 6, 194, 0);
  mc.frameMs = 150;
  CHECK(!lists(c.match(mc, 10), "STANAG 4529"));
  CHECK(c.match(feat("", 0, 0, 0)).empty());
}
