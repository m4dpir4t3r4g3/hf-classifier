// What the decoders spend the processing on (DecoderHost::Config's policy):
// decoder families switched off, emissions no decoder fits not measured,
// decodable emissions taking the place of measured ones, giving up, leaving
// an emission on request, and the account of where the time went.
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "PskModulator.h"
#include "hfc/pipeline.hpp"
#include "iq_scene.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

constexpr double kT0 = 1700000000.0, kCentre = 7000000.0, kRate = 62500, kSig = 0.003;

std::vector<float> scaled(std::vector<float> a, double rms) {
  const double p = testing::meanPower(a);
  for (auto& v : a) v *= static_cast<float>(rms / std::sqrt(p));
  return a;
}

std::vector<float> psk31(double seconds) {
  psk::Modulator m(12000, psk::Mode::Bpsk31, 1500);
  std::vector<float> a;
  while (a.size() < seconds * 12000) {
    auto t = m.Transmission("CQ CQ de G4XYZ G4XYZ testing the resource policy pse k ");
    a.insert(a.end(), t.begin(), t.end());
  }
  return a;
}

// 2-FSK, `shift` Hz, random data at 75 Bd: nothing decodes it.
std::vector<float> fskNoise(double seconds, unsigned seed, double shift = 850) {
  std::mt19937 rng(seed);
  std::vector<float> a;
  double ph = 0;
  while (a.size() < seconds * 12000) {
    const double f = 1500 + ((rng() & 1) ? shift / 2 : -shift / 2);
    for (int i = 0; i < 160; ++i) {
      ph += 2 * 3.14159265358979 * f / 12000.0;
      a.push_back(static_cast<float>(std::sin(ph)));
    }
  }
  return a;
}

// A 16-tone modem, 100 Hz apart from 750 Hz, each tone's phase random
// every 20 ms: 1.6 kHz wide, nothing decodes it.
std::vector<float> multiTone(double seconds, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<float> a;
  double ph[16] = {0};
  while (a.size() < seconds * 12000) {
    double off[16];
    for (double& o : off) o = 3.14159265358979 / 2 * static_cast<double>(rng() % 4);
    for (int i = 0; i < 240; ++i) {
      double v = 0;
      for (int k = 0; k < 16; ++k) {
        ph[k] += 2 * 3.14159265358979 * (750 + 100 * k) / 12000.0;
        v += std::sin(ph[k] + off[k]);
      }
      a.push_back(static_cast<float>(v / 4));
    }
  }
  return a;
}

struct Run {
  std::vector<DecodeEvent> decodes;
  std::map<uint64_t, EmissionFeatures> features;
  std::map<uint64_t, double> centre;  // of every emission
  DecoderHost::Usage usage;
};

// Plays the scene; `during` is called once, `at` seconds in, with the pipeline.
template <typename F>
Run play(const testing::IqScene& sc, const DecoderHost::Config& policy, double at, F during) {
  Pipeline::Config cfg;
  cfg.decoders = policy;
  Pipeline p(cfg);
  Run r;
  p.setDecodeCallback([&](const DecodeEvent& d) { r.decodes.push_back(d); });
  p.setFeaturesCallback([&](uint64_t id, const EmissionFeatures& f) { r.features[id] = f; });
  p.setEmissionCallback([&](Pipeline::EmissionEvent, const Emission& e) { r.centre[e.id] = e.centreHz; });
  p.configure(kRate, kCentre, kT0);
  const auto iq = sc.render();
  const std::size_t block = 3125;
  bool done = false;
  for (std::size_t i = 0; i < iq.size() / 2; i += block) {
    const double t = static_cast<double>(i) / kRate;
    if (!done && t >= at) done = true, during(p);
    p.process(iq.data() + 2 * i, std::min(block, iq.size() / 2 - i), kT0 + t);
    p.poll();
  }
  p.flush();
  r.usage = p.usage();
  return r;
}

Run play(const testing::IqScene& sc, const DecoderHost::Config& policy) {
  return play(sc, policy, 1e9, [](Pipeline&) {});
}

int count(const Run& r, const std::string& decoder) {
  int n = 0;
  for (const auto& d : r.decodes) n += d.decoder == decoder;
  return n;
}

std::set<std::string> allBut(const std::string& keep) {
  std::set<std::string> s = {"CW", "PSK", "RTTY", "SITOR-B", "DSC", "ALE", "Akula", "4:3 code", "FAX", "SSTV", "TIME", "DRM",
                             "FT8", "FT4", "WSPR", "HFDL"};
  s.erase(keep);
  return s;
}

}  // namespace

TEST(policy_decoders_switched_off_and_accounted) {
  testing::IqScene sc(kRate, 20);
  sc.addUsbAudio(scaled(psk31(15), kSig), 12000, 10000 - 1500, 1);
  sc.addNoiseInBand(20 * std::log10(kSig) - 15, 2500, 1);
  DecoderHost::Config on;
  const Run a = play(sc, on);
  CHECK(count(a, "PSK31") > 0);
  // Where the time went: the spectrum, PSK's decoders, measuring.
  CHECK(a.usage.seconds.count("spectrum and detector") && a.usage.seconds.at("spectrum and detector") > 0);
  CHECK(a.usage.seconds.count("decoder PSK") && a.usage.seconds.at("decoder PSK") > 0);
  DecoderHost::Config off;
  off.disabled = {"PSK"};
  const Run b = play(sc, off);
  CHECK(count(b, "PSK31") == 0 && !b.usage.seconds.count("decoder PSK"));
  // Switched off while running: from then on nothing more.
  for (const auto& d : a.decodes) std::printf("    PSK31 text at %.1f s\n", d.utc - kT0);
  const Run c = play(sc, on, 12.0, [](Pipeline& p) {
    DecoderHost::Config q;
    q.disabled = {"PSK"};
    p.setDecoderPolicy(q);
  });
  double lastC = 0, lastA = 0;
  for (const auto& d : c.decodes)
    if (d.decoder == "PSK31") lastC = std::max(lastC, d.utc - kT0);
  for (const auto& d : a.decodes)
    if (d.decoder == "PSK31") lastA = std::max(lastA, d.utc - kT0);
  std::printf("    PSK31: %d texts to %.1f s; switched off at 12 s: %d to %.1f s\n", count(a, "PSK31"), lastA, count(c, "PSK31"), lastC);
  CHECK(lastA > 13.0 && lastC < 13.0);
}

TEST(policy_unknown_signals_measured_or_not_and_room_for_decodable) {
  // A modem no enabled decoder fits (all but PSK switched off; 1.6 kHz
  // wide, beyond PSK500's), and PSK31.
  testing::IqScene sc(kRate, 24);
  sc.addUsbAudio(scaled(multiTone(20, 3), kSig), 12000, -10000 - 1500, 0.5);
  sc.addUsbAudio(scaled(psk31(14), kSig), 12000, 10000 - 1500, 4);
  sc.addNoiseInBand(20 * std::log10(kSig) - 15, 2500, 2);
  DecoderHost::Config p;
  p.disabled = allBut("PSK");
  // (Its emission is the one below the centre.)
  auto fskMeasured = [](const Run& r) {
    for (const auto& [id, f] : r.features)
      if (f.valid && r.centre.count(id) && r.centre.at(id) < kCentre) return true;
    return false;
  };
  const Run measured = play(sc, p);
  CHECK(fskMeasured(measured) && count(measured, "PSK31") > 0);
  // Measured, then left: it was the only thing to do with it.
  CHECK(measured.usage.measuredOnly >= 1);
  p.measureUnknown = false;
  const Run notMeasured = play(sc, p);
  CHECK(!fskMeasured(notMeasured) && count(notMeasured, "PSK31") > 0);
  // Room for one session: the FSK, measured from 0.5 s, gives way to PSK31
  // at 4 s.
  p.measureUnknown = true;
  p.maxSessions = 1;
  const Run one = play(sc, p);
  CHECK(count(one, "PSK31") > 0);
}

TEST(policy_give_up_and_ignore) {
  // Random FSK at a common shift: the RTTY decoders try it and read nothing.
  testing::IqScene sc(kRate, 40);
  sc.addUsbAudio(scaled(fskNoise(38, 5), kSig), 12000, -10000 - 1500, 0.5);
  sc.addUsbAudio(scaled(psk31(36), kSig), 12000, 10000 - 1500, 1);
  sc.addNoiseInBand(20 * std::log10(kSig) - 15, 2500, 3);
  DecoderHost::Config p;
  p.giveUpSeconds = 15;
  const Run r = play(sc, p);
  CHECK(r.usage.givenUp >= 1);
  bool fskStillTried = false;
  for (const auto& s : r.usage.sessions) fskStillTried = fskStillTried || (s.state == "trying" && s.centreHz < kCentre);
  CHECK(!fskStillTried);
  CHECK(count(r, "PSK31") > 0);  // decoding goes on
  // Leaving the PSK31 emission at 12 s: no text after it.
  uint64_t pskId = 0;
  for (const auto& d : r.decodes)
    if (d.decoder == "PSK31") pskId = d.emissionId;
  CHECK(pskId != 0);
  const Run left = play(sc, p, 12.0, [&](Pipeline& pl) { pl.ignore(pskId); });
  double lastPsk = 0;
  for (const auto& d : left.decodes)
    if (d.decoder == "PSK31") lastPsk = std::max(lastPsk, d.utc - kT0);
  std::printf("    last PSK31 text at %.1f s\n", lastPsk);
  CHECK(lastPsk > 0 && lastPsk < 13.0);
  CHECK(left.usage.ignored == 1);
  // The emissions left, with why, for the window's table.
  bool stoppedListed = false, givenUpListed = false;
  for (const auto& [id, why] : left.usage.leftRecent) stoppedListed = stoppedListed || (id == pskId && why == 'i');
  for (const auto& [id, why] : r.usage.leftRecent) givenUpListed = givenUpListed || why == 'g';
  CHECK(stoppedListed && givenUpListed);
}

// The user's windows: "none" leaves an emission alone (no decoding, no
// measuring); a decoder named for a window is used on its own, without
// the plausible-text check (PSK31 sending text that reads like nothing).
TEST(policy_user_windows) {
  psk::Modulator m(12000, psk::Mode::Bpsk31, 1500);
  std::vector<float> odd;
  while (odd.size() < 30 * 12000) {
    auto t = m.Transmission("XQZJ VWKP 77 QXJ ZZKV WPQX 9QJ KZXV JQPW XVKZ ");
    odd.insert(odd.end(), t.begin(), t.end());
  }
  testing::IqScene sc(kRate, 32);
  sc.addUsbAudio(scaled(psk31(30), kSig), 12000, 10000 - 1500, 1);   // ordinary: decoded as always
  sc.addUsbAudio(scaled(odd, kSig), 12000, -10000 - 1500, 1);        // odd text: only when chosen
  sc.addUsbAudio(scaled(psk31(30), kSig), 12000, 20000 - 1500, 1);   // in a "none" window
  sc.addNoiseInBand(20 * std::log10(kSig) - 15, 2500, 3);
  auto near = [](const Run& r, double off) {
    std::string t;
    for (const auto& d : r.decodes)
      if (std::fabs(d.freqHz - kCentre - off) < 200) t += d.decoder + ":" + d.text + " ";
    return t;
  };
  auto measured = [](const Run& r, double off) {
    for (const auto& [id, f] : r.features)
      if (f.valid && r.centre.count(id) && std::fabs(r.centre.at(id) - kCentre - off) < 200) return true;
    return false;
  };
  DecoderHost::Config plain;
  const Run a = play(sc, plain);
  CHECK(near(a, 10000).find("G4XYZ") != std::string::npos);
  CHECK(near(a, -10000).empty());  // the plausible-text check keeps it out
  CHECK(near(a, 20000).find("G4XYZ") != std::string::npos);

  DecoderHost::Config ruled;
  ruled.rules = std::make_shared<std::vector<DecodeRule>>(std::vector<DecodeRule>{
      {kCentre - 10500, kCentre - 9500, "PSK31"}, {kCentre + 19000, kCentre + 21000, "none"}});
  const Run b = play(sc, ruled);
  std::printf("    chosen PSK31: [%s]\n", near(b, -10000).c_str());
  CHECK(near(b, -10000).find("VWKP") != std::string::npos);
  CHECK(near(b, 20000).empty() && !measured(b, 20000));
  CHECK(near(b, 10000).find("G4XYZ") != std::string::npos);  // outside the windows: as before

  // Set while running (as from the window): from then on.
  const Run c = play(sc, plain, 10.0, [&](Pipeline& p) { p.setDecoderPolicy(ruled); });
  CHECK(near(c, -10000).find("VWKP") != std::string::npos);
}

TEST(policy_chosen_decoder_names) {
  CHECK(DecoderHost::choosable("CW") && DecoderHost::choosable("PSK63") && DecoderHost::choosable("RTTY 50/85"));
  CHECK(DecoderHost::choosable("RTTY 45.45/170") && !DecoderHost::choosable("RTTY 5/85") && !DecoderHost::choosable("RTTY 50/85x"));
  CHECK(!DecoderHost::choosable("FT8") && !DecoderHost::choosable("none") && !DecoderHost::choosable(""));
}
