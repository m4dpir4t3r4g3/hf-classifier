#include <atomic>
// End-to-end decoding: IQ scenes built from the decoders' own signal
// generators go through the Pipeline (channelizer, detector, decoder host);
// each signal must come out decoded, by the right decoder, tied to its
// emission, and noise must decode as nothing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "PskModulator.h"
#include "hfc/frequency_list.hpp"
#include "hfc/decoder_host.hpp"
#include "hfc/ftx_decoder.hpp"
#include "demo_scene.hpp"
#include "hfc/akula.hpp"
#include "hfc/dsc.hpp"
#include "hfc/pipeline.hpp"
#include "iq_scene.hpp"
#include "morse/synth.hpp"
#include "navtex/ale_pdu.hpp"
#include "navtex/ale_synth.hpp"
#include "navtex/synth.hpp"
#include "hfc/worker_pool.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

constexpr double kT0 = 1699999995.0;  // a multiple of 15 s: FT8 slots start with the scene
constexpr double kCentre = 14080000.0;

std::vector<float> scaled(std::vector<float> a, double rms) {
  const double p = testing::meanPower(a);
  for (auto& v : a) v *= static_cast<float>(rms / std::sqrt(p));
  return a;
}

struct Run {
  std::vector<DecodeEvent> decodes;
  std::map<uint64_t, EmissionFeatures> features;
  std::map<uint64_t, Emission> emissions;  // as ended
  std::map<uint64_t, std::string> labels;  // label at the end
};

Run runPipeline(const testing::IqScene& sc, double centre, double blockSeconds = 0.05, const Pipeline::Config& cfg = {}) {
  const auto iq = sc.render();
  const double rate = sc.sampleRate();
  Pipeline p(cfg);
  Run r;
  p.setDecodeCallback([&](const DecodeEvent& d) { r.decodes.push_back(d); });
  p.setFeaturesCallback([&](uint64_t id, const EmissionFeatures& f) { r.features[id] = f; });
  p.setEmissionCallback([&](Pipeline::EmissionEvent k, const Emission& e) {
    if (k != Pipeline::EmissionEvent::Ended) return;
    r.emissions[e.id] = e;
    r.labels[e.id] = p.label(e.id);
  });
  p.configure(rate, centre, kT0);
  const auto block = static_cast<std::size_t>(rate * blockSeconds);
  for (std::size_t i = 0; i < iq.size() / 2; i += block) {
    const std::size_t n = std::min(block, iq.size() / 2 - i);
    p.process(iq.data() + 2 * i, n, kT0 + static_cast<double>(i) / rate);
    p.poll();
  }
  p.flush();
  return r;
}

// All text one decoder produced near `hz`, in order.
std::string textOf(const Run& r, const std::string& decoder, double hz, double tol = 100) {
  std::string s;
  for (const auto& d : r.decodes)
    if (d.decoder == decoder && std::fabs(d.freqHz - hz) < tol) s += d.text + " ";
  return s;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

}  // namespace

TEST(pipeline_decodes_every_mode_in_one_band) {
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 48);

  navtex::SynthConfig nc;
  nc.sampleRate = 12000;
  nc.centreHz = 1500;
  nc.phasingSeconds = 4;
  nc.leadSeconds = nc.tailSeconds = 0.5;
  const auto nav = navtex::synthesize(navtex::navtexMessage("OA23", "GALE WARNING NUMBER 12 SOUTHERN NORTH SEA"), nc);
  sc.addUsbAudio(scaled(nav.samples, sig), 12000, 8000 - 1500, 2.0);

  navtex::SynthConfig rc = nc;
  rc.baud = 45.45;
  rc.phasingSeconds = 1;
  const auto rtty = navtex::synthesizeRtty("CQ CQ DE W1AW W1AW K RYRYRY THE QUICK BROWN FOX", rc);
  sc.addUsbAudio(scaled(rtty.samples, sig), 12000, -15000 - 1500, 3.0);

  morse::SynthConfig mc;
  mc.sampleRate = 12000;
  mc.toneHz = 700;
  mc.wpm = 22;
  const auto cw = morse::synthesize("CQ CQ DE DL1ABC DL1ABC K   TEST 599 73", mc);
  sc.addUsbAudio(scaled(cw.samples, sig), 12000, 12000 - 700, 2.0);

  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
  const auto psk31 = pm.Transmission("CQ CQ CQ de G4XYZ G4XYZ pse k  hello from psk31 test ");
  sc.addUsbAudio(scaled(psk31, sig), 12000, 16000 - 1000, 2.0);

  navtex::ale::AleChannel ac;
  ac.sampleRate = 12000;
  ac.leadSeconds = ac.tailSeconds = 0.1;
  const auto ale = navtex::ale::synth2g(navtex::ale::ale2gCall("K1ABC", "W2XYZ", 12), ac);
  sc.addUsbAudio(scaled(ale.samples, sig), 12000, -24000, 3.0);

  // FT8 in the 14074 kHz sub-band, second slot; FT4 at 14080 kHz, second slot.
  sc.addUsbAudio(scaled(testing::ftxAudio("CQ K1ABC FN42", 1200, false), sig * 0.3), 12000, 14074000 - kCentre, 15.5);
  sc.addUsbAudio(scaled(testing::ftxAudio("W2XYZ K1ABC -12", 1800, false), sig * 0.1), 12000, 14074000 - kCentre, 15.6);
  sc.addUsbAudio(scaled(testing::ftxAudio("CQ DL1ABC JO62", 1000, true), sig * 0.3), 12000, 14080000 - kCentre, 8.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 7);

  const Run r = runPipeline(sc, kCentre);

  CHECK(has(textOf(r, "NAVTEX", kCentre + 8000), "ZCZC OA23"));
  CHECK(has(textOf(r, "NAVTEX", kCentre + 8000), "GALE WARNING NUMBER 12"));
  CHECK(has(textOf(r, "NAVTEX", kCentre + 8000), "NNNN"));
  CHECK(has(textOf(r, "RTTY 45.45/170", kCentre - 15000), "W1AW"));
  CHECK(has(textOf(r, "RTTY 45.45/170", kCentre - 15000), "QUICK BROWN FOX"));
  CHECK(has(textOf(r, "CW", kCentre + 12000), "DL1ABC"));
  CHECK(has(textOf(r, "PSK31", kCentre + 16000), "G4XYZ"));
  CHECK(has(textOf(r, "ALE 2G", kCentre - 22375, 300), "TO K1ABC"));
  CHECK(has(textOf(r, "FT8", kCentre - 4778, 30), "CQ K1ABC FN42"));
  CHECK(has(textOf(r, "FT8", kCentre - 4178, 30), "W2XYZ K1ABC -12"));
  CHECK(has(textOf(r, "FT4", kCentre + 1031, 30), "CQ DL1ABC JO62"));

  // Nothing else: every decode is one of the above.
  for (const auto& d : r.decodes) {
    const double off = d.freqHz - kCentre;
    const bool known = std::fabs(off - 8000) < 100 || std::fabs(off + 15000) < 100 || std::fabs(off - 12000) < 100 ||
                       std::fabs(off - 16000) < 100 || std::fabs(off + 22375) < 400 || std::fabs(off + 4500) < 400 ||
                       std::fabs(off - 1031) < 50;
    if (!known) std::printf("    unexpected decode %s at %+.0f Hz: %s\n", d.decoder.c_str(), off, d.text.c_str());
    CHECK(known);
  }
  // Decodes are tied to emissions, which are labelled with the decoder.
  int labelled = 0;
  for (const auto& d : r.decodes) {
    if (!d.first) continue;
    CHECK(d.emissionId != 0);
    // FT8/FT4 decodes arrive from the worker thread, after the emission
    // ended or not depending on timing; the label check leaves them out.
    if (d.decoder == "FT8" || d.decoder == "FT4") continue;
    auto it = r.labels.find(d.emissionId);
    if (it != r.labels.end() && it->second == d.decoder) ++labelled;
  }
  CHECK(labelled == 5);  // NAVTEX, RTTY, CW, PSK31, ALE
  // UTC: the FT8 decode carries the slot time plus its offset.
  for (const auto& d : r.decodes)
    if (d.decoder == "FT8" && has(d.text, "CQ K1ABC")) CHECK_NEAR(d.utc, kT0 + 15.5, 0.3);
}

// The sessions run on several threads; what comes out, and in what order,
// is the same as on one.
TEST(pipeline_decodes_the_same_on_one_thread_and_on_several) {
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 24);
  navtex::SynthConfig nc;
  nc.sampleRate = 12000;
  nc.centreHz = 1500;
  nc.phasingSeconds = 2;
  nc.leadSeconds = nc.tailSeconds = 0.5;
  sc.addUsbAudio(scaled(navtex::synthesize(navtex::navtexMessage("OA23", "GALE WARNING"), nc).samples, sig), 12000, 8000 - 1500, 1.0);
  navtex::SynthConfig rc = nc;
  rc.baud = 45.45;
  rc.phasingSeconds = 1;
  sc.addUsbAudio(scaled(navtex::synthesizeRtty("CQ CQ DE W1AW W1AW K RYRYRY", rc).samples, sig), 12000, -15000 - 1500, 1.5);
  morse::SynthConfig mc;
  mc.sampleRate = 12000;
  mc.toneHz = 700;
  mc.wpm = 22;
  for (int i = 0; i < 4; ++i)
    sc.addUsbAudio(scaled(morse::synthesize("CQ CQ DE DL1ABC DL1ABC K   TEST 599 73", mc).samples, sig), 12000,
                   20000 + 3000 * i - 700, 1.0 + i);
  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
  sc.addUsbAudio(scaled(pm.Transmission("CQ CQ CQ de G4XYZ G4XYZ pse k "), sig), 12000, 16000 - 1000, 1.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 7);
  Pipeline::Config one, many;
  one.decoders.threads = 1;
  many.decoders.threads = 4;
  const Run a = runPipeline(sc, kCentre, 0.05, one), b = runPipeline(sc, kCentre, 0.05, many);
  CHECK(a.decodes.size() >= 6);
  CHECK(a.decodes.size() == b.decodes.size());
  for (std::size_t i = 0; i < a.decodes.size() && i < b.decodes.size(); ++i)
    CHECK(a.decodes[i].decoder == b.decodes[i].decoder && a.decodes[i].text == b.decodes[i].text &&
          a.decodes[i].emissionId == b.decodes[i].emissionId && a.decodes[i].utc == b.decodes[i].utc);
  CHECK(has(textOf(b, "CW", kCentre + 20000), "DL1ABC") && has(textOf(b, "PSK31", kCentre + 16000), "G4XYZ"));
}

// Morse of a net, as on a user's recording (short codes, many single E's,
// no callsigns) passes once its emission is measured as a keyed tone; a
// carrier in deep fading and random on-off keying at a fixed rate, which a
// Morse decoder also makes letters of, still give nothing.
TEST(pipeline_decodes_net_morse_but_not_fading_or_random_keying) {
  const double rate = 62500, sig = 0.003, fs = 12000;
  const int secs = 40;
  testing::IqScene sc(rate, secs);
  morse::SynthConfig mc;
  mc.sampleRate = fs;
  mc.toneHz = 700;
  mc.wpm = 20;
  sc.addUsbAudio(scaled(morse::synthesize("E RPT K   E E E E E   R 853 K   QSL 22 K   E E   R 471 K", mc).samples, sig), fs,
                 -12000 - 700, 1.0);
  const std::size_t n = static_cast<std::size_t>(fs * (secs - 2));
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / 16777216.0;
  };
  // A carrier fading in and out (a complex Gaussian, low-passed to about
  // 0.7 Hz: fades of 20 dB and more, several a second... and slower).
  std::vector<float> fade(n);
  double re = 0, im = 0;
  for (std::size_t i = 0; i < n; ++i) {
    re += 0.0004 * ((rnd() - 0.5) * 2 * 3.4641 - re * 0.4);
    im += 0.0004 * ((rnd() - 0.5) * 2 * 3.4641 - im * 0.4);
    fade[i] = static_cast<float>(std::hypot(re, im) * std::sin(2 * kPi * 700.0 * static_cast<double>(i) / fs));
  }
  sc.addUsbAudio(scaled(fade, sig), fs, 6000 - 700, 1.0);
  // Random on-off keying at 8, 12 and 20 symbols a second, 5 ms edges.
  const double bauds[] = {8, 12, 20};
  for (int k = 0; k < 3; ++k) {
    std::vector<float> ook(n);
    const std::size_t sym = static_cast<std::size_t>(fs / bauds[k]);
    double level = 0;
    bool on = false;
    for (std::size_t i = 0; i < n; ++i) {
      if (i % sym == 0) on = rnd() < 0.5;
      level += ((on ? 1.0 : 0.0) - level) / 60.0;
      ook[i] = static_cast<float>(level * std::sin(2 * kPi * 700.0 * static_cast<double>(i) / fs));
    }
    sc.addUsbAudio(scaled(ook, sig), fs, 20000 + 4000 * k - 700, 1.0);
  }
  sc.addNoiseInBand(20 * std::log10(sig) - 18, 2500, 5);
  const Run r = runPipeline(sc, kCentre);
  const std::string net = textOf(r, "CW", kCentre - 12000);
  std::printf("    net Morse: [%s]\n", net.c_str());
  CHECK(has(net, "853") && has(net, "RPT"));
  CHECK(textOf(r, "CW", kCentre + 6000, 300).empty());
  for (int k = 0; k < 3; ++k) CHECK(textOf(r, "CW", kCentre + 20000 + 4000 * k, 300).empty());
  for (const auto& d : r.decodes)
    if (d.freqHz - kCentre > 5000)
      std::printf("    unexpected: %s %.0f [%s]\n", d.decoder.c_str(), d.freqHz - kCentre, d.text.c_str());
}

TEST(worker_pool_runs_every_job_once) {
  for (int threads : {0, 1, 3}) {
    WorkerPool pool(threads);
    for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(7), std::size_t(500)}) {
      std::vector<std::atomic<int>> hits(n);
      for (auto& h : hits) h = 0;
      for (int round = 0; round < 20; ++round) pool.run(n, [&](std::size_t i) { hits[i]++; });
      bool all = true;
      for (auto& h : hits) all = all && h == 20;
      CHECK(all);
    }
  }
  CHECK(WorkerPool::threadsFor(1) == 0 && WorkerPool::threadsFor(5) == 4 && WorkerPool::threadsFor(0) <= 16);
}

TEST(pipeline_decodes_nothing_in_noise_and_carriers) {
  const double rate = 250000;
  testing::IqScene sc(rate, 30);
  sc.addCarrier(5000, 0.01, 1, 29);
  sc.addCarrier(-9000, 0.003, 2, 9);
  sc.addNoiseInBand(-60, 2500, 3);
  // Centre chosen so that the 7074 kHz FT8 sub-band is decoded too.
  const Run r = runPipeline(sc, 7094000);
  for (const auto& d : r.decodes) std::printf("    unexpected decode %s: %s\n", d.decoder.c_str(), d.text.c_str());
  CHECK(r.decodes.empty());
  CHECK(r.emissions.size() == 2);
}

TEST(decoder_host_plans_ft8_bands_inside_the_bandwidth) {
  Channelizer ch(250000);
  DecoderHost::Config cfg;
  DecoderHost host(ch, cfg);
  host.setCentre(7074000 + 20000);
  const auto bands = host.ftxBands();
  // Within +-0.45 * 250 kHz of 7094 kHz: 7074 (FT8), 7047.5 (FT4) and
  // 7038.6 (WSPR).
  bool ft8 = false, ft4 = false, wspr = false;
  for (const auto& b : bands) {
    if (!b.ft4 && !b.wspr && b.dialHz == 7074000) ft8 = true;
    if (b.ft4 && b.dialHz == 7047500) ft4 = true;
    if (b.wspr && std::fabs(b.dialHz - 7038600) < 1) wspr = true;
  }
  CHECK(ft8 && ft4 && wspr);
  CHECK(bands.size() == 3);
  host.setCentre(9000000);
  CHECK(host.ftxBands().empty());
}

TEST(decoder_host_candidates_follow_bandwidth) {
  auto has = [](const std::vector<std::string>& v, const char* n) {
    return std::find(v.begin(), v.end(), n) != v.end();
  };
  CHECK(has(DecoderHost::candidatesFor(40), "CW"));
  CHECK(has(DecoderHost::candidatesFor(40), "PSK31"));
  CHECK(has(DecoderHost::candidatesFor(40), "QPSK31"));
  CHECK(!has(DecoderHost::candidatesFor(40), "PSK125"));
  CHECK(has(DecoderHost::candidatesFor(150), "QPSK125"));
  CHECK(has(DecoderHost::candidatesFor(500), "PSK250"));
  CHECK(has(DecoderHost::candidatesFor(500), "QPSK500"));
  CHECK(has(DecoderHost::candidatesFor(1100), "PSK500"));
  // A session is ALE (two audio views) or not: no PSK where ALE starts.
  // (Akula's detector listens to the first view of either kind of session.
  // The radiofax decoder listens to the second view of an ALE session.)
  for (const auto& n : DecoderHost::candidatesFor(1300)) CHECK(n.rfind("ALE", 0) == 0 || n == "Akula" || n == "FAX" || n == "SSTV" || n == "TIME" || n == "DRM");
  // Radiofax from 500 to 1900 Hz, HFDL from 1800 Hz.
  CHECK(has(DecoderHost::candidatesFor(900), "FAX") && has(DecoderHost::candidatesFor(1700), "FAX"));
  CHECK(!has(DecoderHost::candidatesFor(400), "FAX") && !has(DecoderHost::candidatesFor(2400), "FAX"));
  // DRM from 1300 Hz up, and alone beyond every other decoder.
  CHECK(has(DecoderHost::candidatesFor(2400), "DRM") && DecoderHost::candidatesFor(9000) == std::vector<std::string>{"DRM"});
  CHECK(has(DecoderHost::candidatesFor(2400), "HFDL") && !has(DecoderHost::candidatesFor(1500), "HFDL"));
  CHECK(DecoderHost::candidatesFor(1300).front() == "ALE 2G");
  CHECK(has(DecoderHost::candidatesFor(1150), "Akula") && has(DecoderHost::candidatesFor(1600), "Akula"));
  CHECK(!has(DecoderHost::candidatesFor(900), "Akula") && !has(DecoderHost::candidatesFor(2500), "Akula"));
  CHECK(has(DecoderHost::candidatesFor(300), "SITOR-B"));
  CHECK(has(DecoderHost::candidatesFor(600), "RTTY 50/450"));
  CHECK(has(DecoderHost::candidatesFor(2400), "ALE 2G"));
  CHECK(has(DecoderHost::candidatesFor(600), "CW"));  // a strong keyed tone measures wide
  CHECK(!has(DecoderHost::candidatesFor(900), "CW"));
  CHECK(DecoderHost::candidatesFor(8000) == std::vector<std::string>{"DRM"});
}

TEST(pipeline_retune_ends_emissions_and_keeps_decoding) {
  // PSK31 at +10 kHz; halfway through, the receiver retunes by 1 kHz, so
  // the signal moves in the IQ to +9 kHz. Text before and after the
  // retune comes out, from two emissions.
  const double rate = 62500;
  testing::IqScene a(rate, 16), b(rate, 16);
  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
  const auto t1 = pm.Transmission("CQ CQ de G4XYZ G4XYZ first part of the over k ");
  psk::Modulator pm2(12000, psk::Mode::Bpsk31, 1000);
  const auto t2 = pm2.Transmission("CQ CQ de G4XYZ G4XYZ second part of the over k ");
  a.addUsbAudio(scaled(t1, 0.003), 12000, 10000 - 1000, 1);
  b.addUsbAudio(scaled(t2, 0.003), 12000, 9000 - 1000, 1);
  a.addNoiseInBand(20 * std::log10(0.003) - 15, 2500, 1);
  b.addNoiseInBand(20 * std::log10(0.003) - 15, 2500, 2);
  const auto ia = a.render(), ib = b.render();

  Pipeline p;
  std::vector<DecodeEvent> d;
  int ended = 0;
  p.setDecodeCallback([&](const DecodeEvent& e) { d.push_back(e); });
  p.setEmissionCallback([&](Pipeline::EmissionEvent k, const Emission&) { ended += k == Pipeline::EmissionEvent::Ended; });
  p.configure(rate, 7000000, kT0);
  const std::size_t block = 3125;
  for (std::size_t i = 0; i < ia.size() / 2; i += block) p.process(ia.data() + 2 * i, block, kT0 + static_cast<double>(i) / rate);
  p.configure(rate, 7001000, kT0 + 16);
  CHECK(ended == 1);
  for (std::size_t i = 0; i < ib.size() / 2; i += block)
    p.process(ib.data() + 2 * i, block, kT0 + 16 + static_cast<double>(i) / rate);
  p.flush();
  // The signal stays at 7010 kHz RF throughout.
  std::string before, after;
  for (const auto& e : d) {
    CHECK(std::fabs(e.freqHz - 7010000) < 50);
    (e.utc < kT0 + 16 ? before : after) += e.text + " ";
  }
  CHECK(has(before, "first part"));
  CHECK(has(after, "second part"));
}

TEST(pipeline_decodes_psk63_psk125_and_rtty_50_450) {
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 24);
  psk::Modulator p63(12000, psk::Mode::Bpsk63, 1000);
  sc.addUsbAudio(scaled(p63.Transmission("CQ CQ de PA3ABC PA3ABC psk63 test test k "), sig), 12000, 10000 - 1000, 1.0);
  psk::Modulator p125(12000, psk::Mode::Bpsk125, 1000);
  sc.addUsbAudio(scaled(p125.Transmission("CQ CQ de OH2XYZ OH2XYZ psk125 test test test k "), sig), 12000, -10000 - 1000, 1.0);
  navtex::SynthConfig rc;
  rc.sampleRate = 12000;
  rc.centreHz = 1500;
  rc.baud = 50;
  rc.shiftHz = 450;
  rc.phasingSeconds = 1;
  rc.leadSeconds = rc.tailSeconds = 0.5;
  const auto rtty = navtex::synthesizeRtty("WEATHER REPORT GERMAN BIGHT WEST 5 TO 6 SEA STATE 3", rc);
  sc.addUsbAudio(scaled(rtty.samples, sig), 12000, 20000 - 1500, 1.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 11);
  const Run r = runPipeline(sc, 7000000);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - 7000000, d.text.c_str());
  CHECK(has(textOf(r, "PSK63", 7010000), "PA3ABC"));
  CHECK(has(textOf(r, "PSK125", 6990000), "OH2XYZ"));
  CHECK(has(textOf(r, "RTTY 50/450", 7020000, 300), "GERMAN BIGHT"));
}

TEST(pipeline_decodes_qpsk_and_psk250_500) {
  // fldigi's other PSK modes, with its preamble and postamble (32 symbols
  // at 31.25 Bd, as long in time at the faster rates). The QPSK63 station
  // sends in the reverse sense of rotation (fldigi's "Reverse", or the
  // other sideband): the decoder finds it.
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 24);
  auto add = [&](psk::Mode mode, bool reversed, const std::string& text, double offsetHz) {
    psk::Modulator m(12000, mode, 1000, 0.0, reversed);
    const int fldigi = static_cast<int>(std::lround(32 * psk::BaudRate(mode) / 31.25));
    sc.addUsbAudio(scaled(m.Transmission(text, std::max(64, fldigi), std::max(32, fldigi)), sig), 12000, offsetHz - 1000, 1.0);
  };
  add(psk::Mode::Qpsk31, false, "CQ CQ de LZ1ABC LZ1ABC qpsk31 test k ", 8000);
  add(psk::Mode::Qpsk63, true, "CQ CQ de SP5XYZ SP5XYZ qpsk63 reverse test k ", -8000);
  add(psk::Mode::Bpsk250, false, "CQ CQ CQ de G4ABC G4ABC G4ABC psk250 test test test test k ", 16000);
  std::string long500;
  for (int i = 0; i < 4; ++i) long500 += "CQ CQ CQ de DL2XYZ DL2XYZ DL2XYZ qpsk500 test test test ";
  add(psk::Mode::Qpsk500, false, long500 + "k ", -16000);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  const Run r = runPipeline(sc, 7000000);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - 7000000, d.text.c_str());
  CHECK(has(textOf(r, "QPSK31", 7008000), "LZ1ABC"));
  CHECK(has(textOf(r, "QPSK63", 6992000), "SP5XYZ"));
  CHECK(has(textOf(r, "PSK250", 7016000, 200), "G4ABC"));
  CHECK(has(textOf(r, "QPSK500", 6984000, 300), "DL2XYZ"));
  // Each by its own decoder only.
  for (const auto& d : r.decodes) CHECK(d.decoder == "QPSK31" || d.decoder == "QPSK63" || d.decoder == "PSK250" || d.decoder == "QPSK500");
}

TEST(pipeline_decodes_3g_and_4g_ale) {
  // The NAVTEX plugin's 3G and 4G bursts through the IQ chain: a Deep WALE
  // link setup request, a 3G BW0 call, and a Fast WALE PDU sealed like
  // linking protection seals it (its CRC no longer matches), which must
  // come out as received intact, as sigidwiki's real samples do.
  namespace ale = navtex::ale;
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 12);
  ale::AleChannel ch;
  ch.sampleRate = 12000;
  const ale::WalePdu req = ale::waleLinkSetup(0, false, false, 1, 1, 0x1234, 0x5678, 0x00ff, 0);
  sc.addUsbAudio(scaled(ale::synthWale(true, {req}, ch).samples, sig), 12000, 8000 - 1800, 1.0);
  sc.addUsbAudio(scaled(ale::synthBw0(ale::leCall(12, 2, 5, 3), ch).samples, sig), 12000, -8000 - 1800, 3.0);
  ale::WalePdu sealed = req;
  sealed[10] ^= 0x5a;
  sc.addUsbAudio(scaled(ale::synthWale(false, {sealed}, ch).samples, sig), 12000, 16000 - 1800, 5.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 13);
  const Run r = runPipeline(sc, 7000000);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - 7000000, d.text.c_str());
  CHECK(has(textOf(r, "ALE 4G", 7008000, 300), "LSU_Req  1234 -> 5678"));
  CHECK(has(textOf(r, "ALE 3G", 6992000, 300), "LE_Call"));
  CHECK(has(textOf(r, "ALE 4G", 7016000, 300), "received intact"));
}

TEST(ftx_decoder_stops_at_50_messages_per_slot) {
  // A busy band: 60 FT8 signals in one slot. ft8_lib's demo loop, which the
  // decoder follows, used to spin forever on the 51st different message.
  std::vector<float> slot(15 * 12000, 0.0f);
  for (int i = 0; i < 60; ++i) {
    char msg[32];
    std::snprintf(msg, sizeof(msg), "CQ K%dA%c%c FN42", i % 10, 'A' + (i / 10) % 26, 'A' + i % 26);
    const auto a = testing::ftxAudio(msg, 100 + 45.0 * i, false);
    for (std::size_t j = 0; j < a.size(); ++j) slot[6000 + j] += 0.02f * a[j];
  }
  FtxDecoder d(false);
  const auto m = d.decodeSlot(slot.data(), slot.size());
  CHECK(m.size() == 50);
}

TEST(pipeline_decodes_nothing_from_signals_of_other_modes) {
  // Signals of modes no decoder here handles, carrying random data: FSK at
  // several baud rates and shifts, BPSK without Varicode, random keying,
  // 8-FSK and 16-FSK. None may come out as text. (Before the text
  // plausibility gate and onset grouping: 40 to 80 false "CW", "PSK63" and
  // "RTTY" decodes in a scene like this.)
  const double rate = 250000, sig = 0.003;
  testing::IqScene sc(rate, 30);
  uint32_t seed = 7;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  double off = -100000;
  const std::pair<double, double> fsks[] = {{50, 1000}, {75, 850}, {100, 170}, {200, 1000}, {45.45, 170}, {50, 450}};
  for (const auto& f : fsks) {
    std::vector<bool> bits(static_cast<std::size_t>(f.first * 24));
    for (std::size_t i = 0; i < bits.size(); ++i) bits[i] = rnd() & 1;
    navtex::SynthConfig c;
    c.sampleRate = 12000;
    c.centreHz = 1500;
    c.baud = f.first;
    c.shiftHz = f.second;
    c.leadSeconds = c.tailSeconds = 0.2;
    sc.addUsbAudio(scaled(navtex::synthesizeBits(bits, c).samples, sig), 12000, off - 1500, 2.0);
    off += 12000;
  }
  for (auto m : {psk::Mode::Bpsk31, psk::Mode::Bpsk63}) {
    psk::Modulator pm(12000, m, 1500);
    std::vector<float> a;
    pm.Idle(32, a);
    for (int i = 0; i < 1500; ++i) pm.Bit(rnd() & 1, a);
    sc.addUsbAudio(scaled(a, sig), 12000, off - 1500, 2.0);
    off += 12000;
  }
  {
    std::vector<float> a(12000 * 24);
    bool on = false;
    std::size_t next = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (i >= next) {
        on = rnd() & 1;
        next = i + 720 * (1 + rnd() % 3);
      }
      a[i] = on ? static_cast<float>(std::sin(2 * kPi * 1500.0 * static_cast<double>(i) / 12000.0)) : 0.0f;
    }
    sc.addUsbAudio(scaled(a, sig), 12000, off - 1500, 2.0);
    off += 12000;
  }
  for (const auto& [baud, spacing, tones] : {std::tuple<double, double, int>{125, 250, 8}, {15.625, 15.625, 16}}) {
    std::vector<float> a(12000 * 24);
    double ph = 0;
    const auto sps = static_cast<std::size_t>(12000 / baud);
    int tone = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (i % sps == 0) tone = static_cast<int>(rnd() % static_cast<uint32_t>(tones));
      ph += 2 * kPi * (1500 - spacing * (tones - 1) / 2 + spacing * tone) / 12000;
      a[i] = static_cast<float>(std::sin(ph));
    }
    sc.addUsbAudio(scaled(a, sig), 12000, off - 1500, 2.0);
    off += 12000;
  }
  sc.addNoiseInBand(20 * std::log10(sig) - 20, 2500, 12);
  const Run r = runPipeline(sc, 7100000);
  for (const auto& d : r.decodes) std::printf("    false decode %s at %+.0f Hz: %s\n", d.decoder.c_str(), d.freqHz - 7100000, d.text.c_str());
  CHECK(r.decodes.empty());
  CHECK(r.emissions.size() >= 10);
}

TEST(pipeline_decodes_weak_ale_and_strong_cw) {
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 24);
  // ALE 2G at 6 dB SNR: its 8 tones show as pieces that must be grouped.
  navtex::ale::AleChannel ac;
  ac.sampleRate = 12000;
  ac.leadSeconds = ac.tailSeconds = 0.1;
  const auto ale = navtex::ale::synth2g(navtex::ale::ale2gCall("K1ABC", "W2XYZ", 12), ac);
  sc.addUsbAudio(scaled(ale.samples, sig), 12000, -10000 - 1625, 2.0);
  // CW 30 dB above the ALE: its detected band is wide, its occupied band not.
  morse::SynthConfig mc;
  mc.sampleRate = 12000;
  mc.toneHz = 700;
  mc.wpm = 22;
  const auto cw = morse::synthesize("CQ CQ DE DL1ABC DL1ABC K   TEST 599 73", mc);
  sc.addUsbAudio(scaled(cw.samples, sig * 31.6), 12000, 12000 - 700, 2.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 6, 2500, 21);
  const Run r = runPipeline(sc, 7000000);
  CHECK(has(textOf(r, "ALE 2G", 6990000, 300), "TO K1ABC"));
  // The whole call from its second word on (the first may be lost while
  // the emission is confirmed).
  CHECK(has(textOf(r, "CW", 7012000), "DE DL1ABC"));
}

TEST(pipeline_keeps_long_runs_without_spaces) {
  // Text is published in pieces of up to 64 characters; a piece cut inside
  // a word must not lose the character it was cut at. PSK125 sends ~30
  // characters a second, so 64 pile up before the 3 s flush.
  testing::IqScene sc(62500, 16);
  std::string run;
  for (int i = 0; i < 12; ++i) run += "0123456789";
  psk::Modulator pm(12000, psk::Mode::Bpsk125, 1500);
  const auto a = pm.Transmission("CQ CQ de OH2XYZ OH2XYZ pse k " + run + " de OH2XYZ k ");
  sc.addUsbAudio(scaled(a, 0.003), 12000, 10000 - 1500, 1.0);
  sc.addNoiseInBand(20 * std::log10(0.003) - 20, 2500, 3);
  const Run r = runPipeline(sc, 7000000);
  std::string joined;
  for (const auto& d : r.decodes)
    for (char c : d.text)
      if (c != ' ') joined += c;
  CHECK(has(joined, run));
}

TEST(pipeline_takes_a_noise_rise_at_once) {
  // The noise gets 6 dB stronger from one block to the next (a gain change):
  // no emissions. (Before: about 30 false ones while the floor caught up.)
  const double rate = 250000;
  testing::IqScene quiet(rate, 8), loud(rate, 8);
  quiet.addNoiseInBand(-66, 2500, 1);
  loud.addNoiseInBand(-60, 2500, 2);
  const auto a = quiet.render(), b = loud.render();
  Pipeline p;
  int emissions = 0;
  p.setEmissionCallback([&](Pipeline::EmissionEvent k, const Emission&) { emissions += k == Pipeline::EmissionEvent::Started; });
  p.configure(rate, 7000000, kT0);
  const std::size_t block = 12500;
  for (std::size_t i = 0; i < a.size() / 2; i += block) p.process(a.data() + 2 * i, block, kT0 + static_cast<double>(i) / rate);
  for (std::size_t i = 0; i < b.size() / 2; i += block)
    p.process(b.data() + 2 * i, block, kT0 + 8 + static_cast<double>(i) / rate);
  p.flush();
  CHECK(emissions == 0);
}

TEST(text_gate_on_real_decodes) {
  // Text decoded from sigidwiki's samples (tools/sigidwiki_bench.py).
  // Morse from real contests and QSOs reads as traffic...
  CHECK(looksLikeTraffic("IQ DK1AX DK1AX TEST RW1A LNT 14 TU DTA1AX SM7GIB * 5NN 15 "));
  CHECK(looksLikeTraffic("CQ OMCQ UA2F UA2F TEST C Q OM6*AAJ E 9A39DD 5NN 1Q 9A3XV Q3XV TEST "));
  CHECK(looksLikeTraffic("CQ TEST DJ0MDR DJ0MDR OK7M @M 5C TOC DL_M SM7GNB TEST "));
  CHECK(looksLikeTraffic("RA8CB RA8CB de RL2D Good morning Victor RST 599 599 name Vladimir "));
  CHECK(looksLikeTraffic("CQ  SARTG  SP2CHY  SP2CHY  CQ  TEST  SP2CHB  SP2CHY  K "));
  CHECK(looksLikeTraffic("HE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 12 "));
  CHECK(looksLikeTraffic("38271 59302 11847 92035 66120 40917 "));  // coded groups
  CHECK(looksLikeTraffic("K7RTA V2BQ4 HZ8XD 5FG2K 0PRT1 AQ9ZM "));  // mixed groups of five
  CHECK(looksLikeTraffic("EA8A TO M 8A TEST HA8A HA8A TEST IA8A HA8A TEST "));  // contest calls, repeated
  // ... a THROB signal through the Morse decoder does not, though "ET1N"
  // and "OR1N" are shaped like callsigns.
  CHECK(!looksLikeTraffic("NO0EO9<AR>ET1N@O*H5HEYN\"O<HH>INT <HH>ENTTLFZ0ND8TOTNOH;OR1N/TNT ETNO1N45MTTTTOHV "));
  CHECK(!looksLikeTraffic("ET1N OR1N AR TNT INT HH O E T "));
}

TEST(pipeline_decodes_rtty_at_measured_rate_and_shift) {
  // RTTY 75 Bd / 850 Hz and 50 Bd / 85 Hz: no fixed candidate covers them;
  // the measured shift and rate start the right decoders.
  const double rate = 62500, sig = 0.003;
  testing::IqScene sc(rate, 30);
  auto rtty = [&](double baud, double shift, const char* text) {
    navtex::SynthConfig rc;
    rc.sampleRate = 12000;
    rc.centreHz = 1500;
    rc.baud = baud;
    rc.shiftHz = shift;
    rc.phasingSeconds = 1;
    rc.leadSeconds = rc.tailSeconds = 0.5;
    return scaled(navtex::synthesizeRtty(text, rc).samples, sig);
  };
  sc.addUsbAudio(rtty(75, 850, "CQ CQ DE DL1ABC DL1ABC TEST RTTY 75 BAUD 850 HZ SHIFT CQ CQ DE DL1ABC DL1ABC K "), 12000,
                 15000 - 1500, 1.0);
  sc.addUsbAudio(rtty(50, 85, "CQ CQ DE OH2XYZ OH2XYZ TEST RTTY 50 BAUD 85 HZ SHIFT CQ CQ DE OH2XYZ OH2XYZ K "), 12000,
                 -15000 - 1500, 1.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  const Run r = runPipeline(sc, 7000000);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - 7000000, d.text.c_str());
  for (const auto& [id, f] : r.features) std::printf("    features %llu: %s\n", static_cast<unsigned long long>(id), f.summary().c_str());
  CHECK(has(textOf(r, "RTTY 75/850", 7015000, 300), "DL1ABC"));
  CHECK(has(textOf(r, "RTTY 50/85", 6985000, 300), "OH2XYZ"));
  int fsk = 0;
  for (const auto& [id, f] : r.features) fsk += f.modulation == "FSK" ? 1 : 0;
  CHECK(fsk >= 2);
}

TEST(pipeline_uses_a_frequency_list) {
  // Two FSK emissions on listed frequencies. On 8414.5 kHz (GMDSS DSC, built
  // in) 100 Bd / 170 Hz that is not NAVTEX: DSC-HF goes first among the
  // modes it is consistent with, marked as listed (without the list,
  // NAVTEX / SITOR-B, the documented one, comes first). On a frequency a
  // user's list gives as "RTTY 75/850", that decoder is in from the start.
  const double rate = 62500, sig = 0.003, centre = 8400000;
  testing::IqScene sc(rate, 30);
  navtex::SynthConfig rc;
  rc.sampleRate = 12000;
  rc.centreHz = 1500;
  rc.baud = 75;
  rc.shiftHz = 850;
  rc.phasingSeconds = 1;
  rc.leadSeconds = rc.tailSeconds = 0.5;
  sc.addUsbAudio(scaled(navtex::synthesizeRtty("CQ CQ DE DL1ABC DL1ABC TEST RTTY 75 BAUD 850 HZ SHIFT CQ CQ DE DL1ABC K ", rc).samples, sig),
                 12000, -12000 - 1500, 1.0);
  // Random 100 Bd / 170 Hz FSK.
  std::vector<float> fsk(12000 * 20);
  uint32_t lfsr = 0xACE1u;
  double ph = 0;
  for (std::size_t i = 0; i < fsk.size(); ++i) {
    if (i % 120 == 0) lfsr = lfsr * 1664525u + 1013904223u;
    ph += 2 * kPi * (1500.0 + ((lfsr >> 16) & 1 ? 85.0 : -85.0)) / 12000.0;
    fsk[i] = static_cast<float>(std::sin(ph));
  }
  sc.addUsbAudio(scaled(fsk, sig), 12000, 14500 - 1500, 2.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  Pipeline::Config cfg;
  auto list = FrequencyList::builtin();
  CHECK(list->addText("8388.0;Somewhere Meteo RTTY 75/850", "user.csv") == 1);
  cfg.decoders.frequencies = list;
  const Run r = runPipeline(sc, centre, 0.05, cfg);
  CHECK(has(textOf(r, "RTTY 75/850", centre - 12000, 300), "DL1ABC"));
  bool dscFirst = false, rttyListed = false;
  for (const auto& [id, f] : r.features) {
    if (!f.valid) continue;  // (listed only: an emission never measured)
    for (const auto& l : f.listed) rttyListed = rttyListed || (l.source == "user.csv" && l.mode == "RTTY (other rates and shifts)");
    if (!f.listed.empty() && f.listed[0].source == "ITU")
      dscFirst = dscFirst || (!f.consistent.empty() && f.consistent[0].mode == "DSC-HF" && f.consistent[0].listed &&
                              f.json().find("\"listed\":[{\"khz\":8414.500,\"label\":\"GMDSS DSC distress and safety\"") != std::string::npos);
  }
  CHECK(dscFirst);
  CHECK(rttyListed);
  // Without a list: nothing listed, NAVTEX / SITOR-B first as before.
  const Run plain = runPipeline(sc, centre);
  bool navtexFirst = false, anyListed = false;
  for (const auto& [id, f] : plain.features) {
    anyListed = anyListed || !f.listed.empty();
    navtexFirst = navtexFirst || (!f.consistent.empty() && f.consistent[0].mode == "NAVTEX / SITOR-B");
  }
  CHECK(navtexFirst && !anyListed);
}

TEST(pipeline_decodes_dsc) {
  // A DSC test call on 8414.5 kHz: decoded, the emission labelled DSC.
  const double rate = 62500, sig = 0.003, centre = 8400000;
  testing::IqScene sc(rate, 16);
  const std::vector<int> call = {120, 23, 1, 45, 0, 0, 108, 0, 21, 91, 0, 0, 118, 126, 126, 126, 126, 126, 126, 126, 122};
  std::vector<float> a;
  double ph = 0;
  for (bool b : dscSequence(call))
    for (int i = 0; i < 120; ++i) {
      ph += 2 * kPi * (1500.0 + (b ? 85.0 : -85.0)) / 12000.0;
      a.push_back(static_cast<float>(std::sin(ph)));
    }
  sc.addUsbAudio(scaled(a, sig), 12000, 14500 - 1500, 2.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  const Run r = runPipeline(sc, centre);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - centre, d.text.c_str());
  CHECK(has(textOf(r, "DSC", centre + 14500, 300), "selective call to 230145000, safety from 002191000, test"));
  bool labelled = false;
  for (const auto& [id, label] : r.labels) labelled = labelled || label == "DSC";
  CHECK(labelled);
}

TEST(pipeline_recognises_an_akula_burst) {
  // A burst built like Akula's (reversals, sync group, random words, end
  // group; 500 Bd on a 1000 Hz shift, about 2 s): recognised by its groups.
  // The same keying with random bits throughout: not.
  const double rate = 62500, sig = 0.003, centre = 9200000;
  auto burst = [&](bool groups, uint32_t seed) {
    std::vector<bool> bits;
    for (int i = 0; i < 120; ++i) bits.push_back(i % 2 == 0);
    for (int i = 0; i < 6; ++i) bits.push_back(false);
    std::mt19937 rng(seed);
    if (groups) {
      for (bool b : akulaSync()) bits.push_back(b);
    } else {
      for (int i = 0; i < 36; ++i) bits.push_back(rng() % 2);
    }
    for (int i = 0; i < 150 * 6; ++i) bits.push_back(rng() % 2);
    if (groups) {
      for (bool b : akulaEnd()) bits.push_back(b);
    } else {
      for (int i = 0; i < 30; ++i) bits.push_back(rng() % 2);
    }
    for (int i = 0; i < 12; ++i) bits.push_back(rng() % 2);
    std::vector<float> a;
    double ph = 0;
    for (bool b : bits)
      for (int i = 0; i < 24; ++i) {
        ph += 2 * kPi * (1500.0 + (b ? 500.0 : -500.0)) / 12000.0;
        a.push_back(static_cast<float>(std::sin(ph)));
      }
    return scaled(a, sig);
  };
  testing::IqScene sc(rate, 16);
  sc.addUsbAudio(burst(true, 1), 12000, 12000 - 1500, 2.0);
  sc.addUsbAudio(burst(true, 2), 12000, 12000 - 1500, 6.0);
  sc.addUsbAudio(burst(false, 3), 12000, -12000 - 1500, 4.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  const Run r = runPipeline(sc, centre);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - centre, d.text.c_str());
  int found = 0;
  for (const auto& d : r.decodes)
    found += d.decoder == "Akula" && std::fabs(d.freqHz - (centre + 12000)) < 400 &&
             has(d.text, "sync and end groups, 155 six-bit words");
  CHECK(found == 2);
  CHECK(textOf(r, "Akula", centre - 12000, 1500).empty());
}

TEST(pipeline_recognises_the_t600_code) {
  // 50 Bd on a 200 Hz shift: once in 7-bit words of four ones (T-600's
  // code), once in random bits. Only the first is recognised.
  const double rate = 62500, sig = 0.003, centre = 9000000;
  auto keyed = [&](bool code, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<bool> bits;
    while (bits.size() < 50 * 24) {
      if (!code) {
        bits.push_back(rng() & 1);
        continue;
      }
      std::vector<bool> word(7, false);
      for (int k = 0; k < 4;) {
        const std::size_t at = rng() % 7;
        if (!word[at]) word[at] = true, ++k;
      }
      bits.insert(bits.end(), word.begin(), word.end());
    }
    std::vector<float> a;
    double ph = 0;
    for (bool b : bits)
      for (int i = 0; i < 240; ++i) {
        ph += 2 * kPi * (1500.0 + (b ? 100.0 : -100.0)) / 12000.0;
        a.push_back(static_cast<float>(std::sin(ph)));
      }
    return scaled(a, sig);
  };
  testing::IqScene sc(rate, 30);
  sc.addUsbAudio(keyed(true, 1), 12000, 12000 - 1500, 2.0);
  sc.addUsbAudio(keyed(false, 2), 12000, -12000 - 1500, 2.0);
  sc.addNoiseInBand(20 * std::log10(sig) - 15, 2500, 12);
  const Run r = runPipeline(sc, centre);
  for (const auto& d : r.decodes) std::printf("    %s %+.0f: %s\n", d.decoder.c_str(), d.freqHz - centre, d.text.c_str());
  CHECK(has(textOf(r, "4:3 code", centre + 12000, 300), "7-bit words of constant weight 4:3"));
  CHECK(textOf(r, "4:3 code", centre - 12000, 300).empty());
}

TEST(demo_band_is_found_and_decoded) {
  // The demo's two parts through the pipeline: every decoded mode decodes,
  // and the signals built like military systems are recognised or measured.
  // (Decoders by the names their decodes carry.)
  const auto parts = testing::demoScene();
  CHECK(parts.size() == 2);
  std::set<std::string> labels;
  bool framed = false;
  std::vector<std::string> hfdl;
  std::vector<std::shared_ptr<const FaxImage>> pictures;
  for (const auto& part : parts) {
    CHECK(part.sampleRate == 62500 && part.iq.size() > 2 * 40 * 62500);
    CHECK(std::fmod(static_cast<double>(part.iq.size() / 2) / part.sampleRate, 15.0) == 0.0);  // whole FT8 slots
    // With the demo's frequency list, as the program runs it: the HFDL
    // channel it names is decoded all the time.
    Pipeline::Config cfg;
    auto list = FrequencyList::builtin();
    list->addText(testing::demoFrequencyList(), "demo");
    cfg.decoders.frequencies = list;
    Pipeline p(cfg);
    p.setDecodeCallback([&](const DecodeEvent& d) {
      labels.insert(d.decoder);
      if (d.decoder == "HFDL") hfdl.push_back(d.text);
      if (d.image) pictures.push_back(d.image);
    });
    p.setFeaturesCallback([&](uint64_t, const EmissionFeatures& f) { framed = framed || std::fabs(f.frameMs - 106.67) < 1; });
    p.configure(part.sampleRate, part.centreHz, kT0);
    const std::size_t block = 3125;
    for (std::size_t i = 0; i < part.iq.size() / 2; i += block) {
      const std::size_t n = std::min(block, part.iq.size() / 2 - i);
      p.process(part.iq.data() + 2 * i, n, kT0 + static_cast<double>(i) / part.sampleRate);
      p.poll();
    }
    p.flush();
  }
  for (const char* want : {"NAVTEX", "RTTY 45.45/170", "CW", "PSK31", "ALE 2G", "FT8", "FT4", "DSC", "HFDL", "FAX", "SSTV", "Akula", "4:3 code"}) {
    if (!labels.count(want)) std::printf("    missing: %s\n", want);
    CHECK(labels.count(want) == 1);
  }
  CHECK(framed);
  // Both squitters and the log-on, once each.
  CHECK(hfdl.size() == 3);
  bool logon = false;
  for (const auto& t : hfdl) logon = logon || t.find("log-on request, aircraft address 4CA1F3 (1200 bit/s)") != std::string::npos;
  CHECK(logon);
  // The SSTV picture and the fax: one each, the fax's line start found
  // from the phasing lines.
  CHECK(pictures.size() == 2);
  if (pictures.size() == 2) {
    const FaxImage& tv = *pictures[0];
    CHECK(tv.colour && tv.what == "Robot 36" && tv.width == 320 && tv.height == 240);
    // The second colour bar is yellow, the sixth red.
    const uint8_t* yellow = tv.grey.data() + (100 * 320 + 60) * 3;
    const uint8_t* red = tv.grey.data() + (100 * 320 + 220) * 3;
    std::printf("    SSTV bars: yellow %d %d %d, red %d %d %d\n", yellow[0], yellow[1], yellow[2], red[0], red[1], red[2]);
    CHECK(yellow[0] > 180 && yellow[1] > 180 && yellow[2] < 80 && red[0] > 180 && red[1] < 80 && red[2] < 80);
    const FaxImage& im = *pictures[1];
    std::printf("    fax picture %dx%d, %d lpm, phased %d\n", im.width, im.height, im.linesPerMinute, im.phased);
    CHECK(im.width == 1810 && im.height >= 60 && im.linesPerMinute == 120 && im.phased);
  }
  CHECK(testing::demoFrequencyList().find("8390.0 STANAG 4285") != std::string::npos);
}
