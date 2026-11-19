#include "demo_scene.hpp"

#include <cmath>
#include <random>

#include "PskModulator.h"
#include "hfc/akula.hpp"
#include "hfc/dsc.hpp"
#include "hfc/fax.hpp"
#include "hfc/hfdl.hpp"
#include "hfc/sstv.hpp"
#include "iq_scene.hpp"
#include "morse/synth.hpp"
#include "navtex/ale_synth.hpp"
#include "navtex/synth.hpp"

namespace hfc {
namespace testing {

namespace {

constexpr double kPi = 3.14159265358979323846;

constexpr double kTwoPi = 6.283185307179586;
constexpr double kRate = 62500, kAudio = 12000, kSig = 0.003, kSnr = 15;

std::vector<float> scaled(std::vector<float> a, double rms) {
  const double p = meanPower(a);
  for (auto& v : a) v *= static_cast<float>(rms / std::sqrt(p));
  return a;
}

// Continuous-phase FSK of `bits` on `centre` in 12 kHz audio.
std::vector<float> fsk(const std::vector<bool>& bits, double baud, double shift, double centre) {
  std::vector<float> a;
  double ph = 0, due = 0;
  for (bool b : bits) {
    due += kAudio / baud;
    while (static_cast<double>(a.size()) < due) {
      ph += kTwoPi * (centre + (b ? shift / 2 : -shift / 2)) / kAudio;
      a.push_back(static_cast<float>(std::sin(ph)));
    }
  }
  return a;
}

// Serial 8PSK at 2400 Bd on 1800 Hz, 35 % root-raised cosine, frames of 256
// symbols each starting with the same 80 (STANAG 4285's layout).
std::vector<float> framedModem(double seconds, uint32_t seed) {
  const int sps = 5;
  const double beta = 0.35;
  std::vector<double> rrc;
  for (int i = -4 * sps; i <= 4 * sps; ++i) {
    const double t = static_cast<double>(i) / sps;
    double h;
    if (i == 0) h = 1.0 - beta + 4.0 * beta / kPi;
    else if (std::fabs(std::fabs(4.0 * beta * t) - 1.0) < 1e-9)
      h = beta / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * beta)) + (1 - 2 / kPi) * std::cos(kPi / (4 * beta)));
    else
      h = (std::sin(kPi * t * (1 - beta)) + 4 * beta * t * std::cos(kPi * t * (1 + beta))) / (kPi * t * (1 - 16 * beta * beta * t * t));
    rrc.push_back(h);
  }
  std::mt19937 rng(seed);
  std::vector<int> preamble(80);
  for (int& v : preamble) v = static_cast<int>(rng() % 8);
  const auto n = static_cast<std::size_t>(seconds * kAudio);
  std::vector<std::complex<double>> up(n + rrc.size());
  for (std::size_t k = 0; k * sps < n; ++k) {
    const auto pos = static_cast<int>(k % 256);
    const int s = pos < 80 ? preamble[static_cast<std::size_t>(pos)] : static_cast<int>(rng() % 8);
    up[k * sps] = std::polar(1.0, kTwoPi / 8 * s);
  }
  std::vector<float> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::complex<double> acc = 0;
    for (std::size_t j = 0; j < rrc.size() && j <= i; ++j) acc += rrc[j] * up[i - j];
    out[i] = static_cast<float>((acc * std::polar(1.0, kTwoPi * 1800.0 * static_cast<double>(i) / kAudio)).real());
  }
  return out;
}

// A burst built like Akula's: reversals, the sync group, `words` random
// 6-bit words, the end group; 500 Bd, 1000 Hz shift.
std::vector<float> akulaLike(int words, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<bool> bits;
  for (int i = 0; i < 120; ++i) bits.push_back(i % 2 == 0);
  for (int i = 0; i < 6; ++i) bits.push_back(false);
  for (bool b : akulaSync()) bits.push_back(b);
  for (int i = 0; i < 6 * words; ++i) bits.push_back(rng() & 1);
  for (bool b : akulaEnd()) bits.push_back(b);
  return fsk(bits, 500, 1000, 1500);
}

// 50 Bd, 200 Hz FSK in 7-bit words of four ones.
std::vector<float> constantWeight(double seconds, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<bool> bits;
  while (static_cast<double>(bits.size()) < seconds * 50) {
    std::vector<bool> w(7, false);
    for (int k = 0; k < 4;) {
      const std::size_t at = rng() % 7;
      if (!w[at]) w[at] = true, ++k;
    }
    bits.insert(bits.end(), w.begin(), w.end());
  }
  return fsk(bits, 50, 200, 1500);
}

// Tones 0..tones-1 `spacing` apart around `centre`, one at a time, `baud`.
std::vector<float> mfsk(int tones, double spacing, double baud, double seconds, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kAudio));
  double ph = 0;
  int sym = 0;
  long index = -1;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto k = static_cast<long>(std::floor(static_cast<double>(i) * baud / kAudio));
    if (k != index) index = k, sym = static_cast<int>(rng() % static_cast<unsigned>(tones));
    ph += kTwoPi * (1500 + (sym - (tones - 1) / 2.0) * spacing) / kAudio;
    a[i] = static_cast<float>(std::sin(ph));
  }
  return a;
}

// 16 carriers 112.5 Hz apart, DQPSK at 75 Bd each (a parallel-tone modem).
std::vector<float> parallelTone(double seconds, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<float> a(static_cast<std::size_t>(seconds * kAudio), 0.0f);
  std::vector<double> ph(16, 0.0);
  const auto sym = static_cast<std::size_t>(kAudio / 75);
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (i % sym == 0)
      for (auto& p : ph) p += (rng() % 4) * kTwoPi / 4;
    double v = 0;
    for (int c = 0; c < 16; ++c)
      v += std::sin(kTwoPi * (1500 + (c - 7.5) * 112.5) * static_cast<double>(i) / kAudio + ph[static_cast<std::size_t>(c)]);
    a[i] = static_cast<float>(v / 4);
  }
  return a;
}

DemoPart amateurPart() {
  const double centre = 14080000;
  IqScene sc(kRate, 45);  // (parts of whole FT8 slots: the loop keeps UTC on 15 s boundaries)
  navtex::SynthConfig nc;
  nc.sampleRate = kAudio;
  nc.centreHz = 1500;
  nc.phasingSeconds = 4;
  nc.leadSeconds = nc.tailSeconds = 0.5;
  sc.addUsbAudio(scaled(navtex::synthesize(navtex::navtexMessage("OA23", "GALE WARNING NUMBER 12 SOUTHERN NORTH SEA"), nc).samples, kSig),
                 kAudio, 8000 - 1500, 2.0);
  navtex::SynthConfig rc = nc;
  rc.baud = 45.45;
  rc.phasingSeconds = 1;
  sc.addUsbAudio(scaled(navtex::synthesizeRtty("CQ CQ DE W1AW W1AW K RYRYRY THE QUICK BROWN FOX", rc).samples, kSig), kAudio,
                 -15000 - 1500, 3.0);
  morse::SynthConfig mc;
  mc.sampleRate = kAudio;
  mc.toneHz = 700;
  mc.wpm = 22;
  sc.addUsbAudio(scaled(morse::synthesize("CQ CQ DE DL1ABC DL1ABC K   TEST 599 73", mc).samples, kSig), kAudio, 12000 - 700, 2.0);
  psk::Modulator pm(kAudio, psk::Mode::Bpsk31, 1000);
  sc.addUsbAudio(scaled(pm.Transmission("CQ CQ CQ de G4XYZ G4XYZ pse k  hello from psk31 test "), kSig), kAudio, 16000 - 1000, 2.0);
  navtex::ale::AleChannel ac;
  ac.sampleRate = kAudio;
  ac.leadSeconds = ac.tailSeconds = 0.1;
  sc.addUsbAudio(scaled(navtex::ale::synth2g(navtex::ale::ale2gCall("K1ABC", "W2XYZ", 12), ac).samples, kSig), kAudio, -24000, 3.0);
  // SSTV 21 kHz above the centre: a Robot 36 test picture with its header (37 s).
  sc.addUsbAudio(scaled(sstvModulate(sstvTestPicture("Robot 36"), "Robot 36", kAudio), kSig), kAudio, 21000 - 1750, 3.0);
  sc.addUsbAudio(scaled(ftxAudio("CQ K1ABC FN42", 1200, false), kSig * 0.3), kAudio, 14074000 - centre, 15.5);
  sc.addUsbAudio(scaled(ftxAudio("W2XYZ K1ABC -12", 1800, false), kSig * 0.1), kAudio, 14074000 - centre, 15.6);
  sc.addUsbAudio(scaled(ftxAudio("CQ DL1ABC JO62", 1000, true), kSig * 0.3), kAudio, 14080000 - centre, 8.0);
  sc.addNoiseInBand(20 * std::log10(kSig) - kSnr, 2500, 7);
  DemoPart p;
  p.centreHz = centre;
  p.sampleRate = kRate;
  p.iq = sc.render();
  p.description = "14.080 MHz: NAVTEX, RTTY, CW, PSK31, ALE 2G, FT8, FT4 (synthetic)";
  return p;
}

DemoPart utilityPart() {
  const double centre = 8400000;
  IqScene sc(kRate, 60);
  // GMDSS DSC test call on 8414.5 kHz, twice.
  const std::vector<int> call = {120, 23, 1, 45, 0, 0, 108, 0, 21, 91, 0, 0, 118, 126, 126, 126, 126, 126, 126, 126, 122};
  const std::vector<float> dsc = scaled(fsk(dscSequence(call), 100, 170, 1500), kSig);
  sc.addUsbAudio(dsc, kAudio, 14500 - 1500, 3.0);
  sc.addUsbAudio(dsc, kAudio, 14500 - 1500, 40.0);
  // HFDL on a listed channel 15 kHz below the centre (dial 8385.0 kHz, the
  // subcarrier 1440 Hz above): a squitter and an aircraft's log-on request.
  sc.addUsbAudio(scaled(hfdlModulate(hfdlSquitter(7, 902, 10), 0, kAudio, 1440), kSig), kAudio, -15000, 5.0);
  sc.addUsbAudio(scaled(hfdlModulate(hfdlLogon(7, 98, 0x4CA1F3), 2, kAudio, 1440), kSig), kAudio, -15000, 20.0);
  sc.addUsbAudio(scaled(hfdlModulate(hfdlSquitter(7, 903, 10), 0, kAudio, 1440), kSig), kAudio, -15000, 37.0);
  // A radiofax 19 kHz above the centre: start tone, phasing lines, a test
  // chart of 60 lines (30 s), stop tone.
  sc.addUsbAudio(scaled(faxModulate(faxTestChart(60), kAudio, true), kSig), kAudio, 19000 - 1900, 3.0);
  // SITOR-B (NAVTEX format) at +4 kHz.
  navtex::SynthConfig nc;
  nc.sampleRate = kAudio;
  nc.centreHz = 1500;
  nc.phasingSeconds = 4;
  nc.leadSeconds = nc.tailSeconds = 0.5;
  sc.addUsbAudio(scaled(navtex::synthesize(navtex::navtexMessage("WZ41", "NAVIGATIONAL WARNING BUOY ADRIFT IN POSITION 52 30N 003 15E"), nc).samples,
                        kSig),
                 kAudio, 4000 - 1500, 6.0);
  // Serial 8PSK like STANAG 4285, centred on 8390.0 kHz (listed in the demo list).
  sc.addUsbAudio(scaled(framedModem(50, 11), kSig), kAudio, -10000 - 1800, 4.0);
  // Akula-like bursts at +24 kHz.
  for (double t : {8.0, 30.0, 52.0}) sc.addUsbAudio(scaled(akulaLike(150, static_cast<uint32_t>(t)), kSig), kAudio, 24000 - 1500, t);
  // T-600-like code at -20 kHz.
  sc.addUsbAudio(scaled(constantWeight(40, 3), kSig), kAudio, -20000 - 1500, 10.0);
  // A 16-tone parallel modem at +8.5 kHz, a 16-tone MFSK at -26 kHz.
  sc.addUsbAudio(scaled(parallelTone(30, 5), kSig), kAudio, 8500 - 1500, 20.0);
  sc.addUsbAudio(scaled(mfsk(16, 15.625, 15.625, 30, 6), kSig), kAudio, -26000 - 1500, 25.0);
  sc.addNoiseInBand(20 * std::log10(kSig) - kSnr, 2500, 8);
  DemoPart p;
  p.centreHz = centre;
  p.sampleRate = kRate;
  p.iq = sc.render();
  p.description = "8.400 MHz: DSC, SITOR-B, and signals built like STANAG 4285, Akula, T-600, a 16-tone modem and MFSK (synthetic)";
  return p;
}

}  // namespace

std::vector<DemoPart> demoScene() { return {amateurPart(), utilityPart()}; }

std::string demoFrequencyList() {
  return "8385.0 HFDL channel (demo list entry, synthetic signal)\n"
         "8390.0 STANAG 4285 net (demo list entry, synthetic signal)\n";
}

}  // namespace testing
}  // namespace hfc
