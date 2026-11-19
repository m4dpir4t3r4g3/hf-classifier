// make_test_scene: writes a SigMF recording (ci16_le) of a synthetic HF band
// with one signal of every mode the classifier decodes, made by each
// decoder's own generator: NAVTEX, RTTY 45.45/170, CW, PSK31, ALE 2G, FT8 and
// FT4, in white noise. For trying hf_classifier without a receiver
// (hf_classifier --play scene.sigmf-meta) and for its smoke test.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "PskModulator.h"
#include "hfc/sigmf.hpp"
#include "iq_scene.hpp"
#include "morse/synth.hpp"
#include "navtex/ale_synth.hpp"
#include "navtex/synth.hpp"

using namespace hfc;

namespace {

std::vector<float> scaled(std::vector<float> a, double rms) {
  const double p = testing::meanPower(a);
  for (auto& v : a) v *= static_cast<float>(rms / std::sqrt(p));
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: make_test_scene BASE [snr_db]\n"
        "writes BASE.sigmf-data / BASE.sigmf-meta: 48 s at 62.5 kS/s centred on 14.080 MHz, starting at\n"
        "2023-11-14 22:13:15 UTC (an FT8 slot boundary), every signal snr_db (default 15) over the noise in 2.5 kHz\n");
    return 2;
  }
  const std::string base = argv[1];
  const double snr = argc > 2 ? std::atof(argv[2]) : 15.0;
  const double rate = 62500, centre = 14080000, sig = 0.003, t0 = 1699999995.0;
  testing::IqScene sc(rate, 48);

  navtex::SynthConfig nc;
  nc.sampleRate = 12000;
  nc.centreHz = 1500;
  nc.phasingSeconds = 4;
  nc.leadSeconds = nc.tailSeconds = 0.5;
  sc.addUsbAudio(scaled(navtex::synthesize(navtex::navtexMessage("OA23", "GALE WARNING NUMBER 12 SOUTHERN NORTH SEA"), nc).samples, sig),
                 12000, 8000 - 1500, 2.0);
  navtex::SynthConfig rc = nc;
  rc.baud = 45.45;
  rc.phasingSeconds = 1;
  sc.addUsbAudio(scaled(navtex::synthesizeRtty("CQ CQ DE W1AW W1AW K RYRYRY THE QUICK BROWN FOX", rc).samples, sig), 12000,
                 -15000 - 1500, 3.0);
  morse::SynthConfig mc;
  mc.sampleRate = 12000;
  mc.toneHz = 700;
  mc.wpm = 22;
  sc.addUsbAudio(scaled(morse::synthesize("CQ CQ DE DL1ABC DL1ABC K   TEST 599 73", mc).samples, sig), 12000, 12000 - 700, 2.0);
  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
  sc.addUsbAudio(scaled(pm.Transmission("CQ CQ CQ de G4XYZ G4XYZ pse k  hello from psk31 test "), sig), 12000, 16000 - 1000, 2.0);
  navtex::ale::AleChannel ac;
  ac.sampleRate = 12000;
  ac.leadSeconds = ac.tailSeconds = 0.1;
  sc.addUsbAudio(scaled(navtex::ale::synth2g(navtex::ale::ale2gCall("K1ABC", "W2XYZ", 12), ac).samples, sig), 12000, -24000, 3.0);
  sc.addUsbAudio(scaled(testing::ftxAudio("CQ K1ABC FN42", 1200, false), sig * 0.3), 12000, 14074000 - centre, 15.5);
  sc.addUsbAudio(scaled(testing::ftxAudio("W2XYZ K1ABC -12", 1800, false), sig * 0.1), 12000, 14074000 - centre, 15.6);
  sc.addUsbAudio(scaled(testing::ftxAudio("CQ DL1ABC JO62", 1000, true), sig * 0.3), 12000, 14080000 - centre, 8.0);
  sc.addNoiseInBand(20 * std::log10(sig) - snr, 2500, 7);
  const auto iq = sc.render();

  SigmfWriter w;
  SigmfWriter::Global g;
  g.sampleRate = rate;
  g.recorder = std::string("make_test_scene ") + HFC_VERSION;
  g.description = "Synthetic test scene: NAVTEX +8 kHz, RTTY -15 kHz, CW +12 kHz, PSK31 +16 kHz, ALE 2G -22.4 kHz, "
                  "FT8 at 14074 kHz dial, FT4 at 14080 kHz dial. Not a real recording.";
  std::string err;
  if (!w.open(base, g, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  w.write(iq.data(), iq.size() / 2, true, centre, t0);
  if (!w.close(&err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s.sigmf-data (%zu samples)\n", base.c_str(), iq.size() / 2);
  return 0;
}
