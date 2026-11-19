// make_busy_scene: writes a SigMF recording (ci16_le) of a busy synthetic HF
// band at an RSPduo's 2.048 MS/s: many CW stations, FT8 and FT4 in two
// slots, RTTY, PSK31, AM broadcasters and wideband modem-like signals, in
// white noise. For measuring whether the processing keeps up with a full
// band and still decodes (hf_classifier --play BASE.sigmf-meta --speed 0),
// and for comparing versions on the same input. Not a real recording.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "PskModulator.h"
#include "hfc/sigmf.hpp"
#include "iq_scene.hpp"
#include "morse/synth.hpp"
#include "navtex/synth.hpp"

using namespace hfc;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> scaled(std::vector<float> a, double rms) {
  const double p = testing::meanPower(a);
  for (auto& v : a) v *= static_cast<float>(rms / std::sqrt(p));
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf(
        "usage: make_busy_scene BASE [rate] [seconds] [cw] [am] [wide]\n"
        "writes BASE.sigmf-data / BASE.sigmf-meta: a busy band centred on 7.074 MHz (default 2048000 S/s,\n"
        "30 s, 40 CW stations, 20 AM broadcasters, 15 wideband signals; also 8 FT8, 4 FT4, 10 RTTY, 6 PSK31)\n");
    return 2;
  }
  const std::string base = argv[1];
  const double rate = argc > 2 ? std::atof(argv[2]) : 2048000;
  const double seconds = argc > 3 ? std::atof(argv[3]) : 30;
  const int nCw = argc > 4 ? std::atoi(argv[4]) : 40, nAm = argc > 5 ? std::atoi(argv[5]) : 20,
            nWide = argc > 6 ? std::atoi(argv[6]) : 15;
  const double centre = 7074000, sig = 0.002, t0 = 1699999995.0;
  const double half = rate * 0.45;
  testing::IqScene sc(rate, seconds);
  std::mt19937 rng(20261116);
  std::uniform_real_distribution<double> u(0, 1);
  // Positions on a 3 kHz grid, each used once, away from the FT8/FT4 dials.
  std::vector<double> used;
  auto place = [&](double width) {
    for (;;) {
      const double f = std::round((-half + u(rng) * 2 * half) / 3000.0) * 3000.0;
      if (std::fabs(f) < 6000 || std::fabs(f - (7047500 - centre)) < 6000) continue;
      bool free = true;
      for (double g : used) free = free && std::fabs(g - f) > width + 3000;
      if (!free) continue;
      used.push_back(f);
      return f;
    }
  };
  auto level = [&](double lo, double hi) { return sig * std::pow(10.0, (lo + u(rng) * (hi - lo)) / 20.0); };
  // Stations call at different times: starts spread over all but the last
  // 25 s (a call lasts up to about 20 s).
  auto start = [&] { return u(rng) * std::max(3.0, seconds - 25.0); };
  const char* calls[] = {"DL1ABC", "G4XYZ", "LZ1AA", "OK2ZZ", "SP5KP", "YU1EV", "HA5KQ", "OE3DX", "I2ABC", "F5NTX",
                         "EA3HV", "PA0RDT", "SM5AQ", "LA9ZX", "OH2BH", "UR5LAM", "YO3FF", "9A2AA", "S51DX", "HB9CQ"};

  // CW: calls and reports at 15 to 30 WPM, 700 Hz, 0 to 20 dB over the noise.
  int cw = 0;
  for (int i = 0; i < nCw; ++i) {
    morse::SynthConfig mc;
    mc.sampleRate = 12000;
    mc.toneHz = 700;
    mc.wpm = 15 + static_cast<int>(u(rng) * 15);
    const std::string c = calls[i % 20];
    const auto a = morse::synthesize("CQ CQ DE " + c + " " + c + " K   TEST 599 73 " + c, mc).samples;
    sc.addUsbAudio(scaled(a, level(0, 20)), 12000, place(500) - 700, start());
    ++cw;
  }
  // FT8 at 7.074 MHz and FT4 at 7.0475 MHz, two slots.
  for (int slot = 0; slot < 2; ++slot)
    for (int i = 0; i < 4; ++i) {
      const std::string msg = std::string("CQ ") + calls[(slot * 4 + i) % 20] + " JN" + std::to_string(10 + i);
      sc.addUsbAudio(scaled(testing::ftxAudio(msg, 600 + 400 * i, false), sig * 0.3), 12000, 0, 0.5 + 15 * slot);
    }
  for (int slot = 0; slot < 4 && 7.5 * slot + 6 < seconds; ++slot) {
    const std::string msg = std::string("CQ ") + calls[(slot + 10) % 20] + " KN22";
    sc.addUsbAudio(scaled(testing::ftxAudio(msg, 1000, true), sig * 0.3), 12000, 7047500 - centre, 0.5 + 7.5 * slot);
  }
  // RTTY 45.45 Bd / 170 Hz.
  for (int i = 0; i < 10; ++i) {
    navtex::SynthConfig rc;
    rc.sampleRate = 12000;
    rc.centreHz = 1500;
    rc.baud = 45.45;
    rc.phasingSeconds = 1;
    rc.leadSeconds = rc.tailSeconds = 0.5;
    const std::string c = calls[(i + 5) % 20];
    sc.addUsbAudio(scaled(navtex::synthesizeRtty("CQ CQ DE " + c + " " + c + " K RYRYRY THE QUICK BROWN FOX", rc).samples,
                          level(5, 20)),
                   12000, place(400) - 1500, start());
  }
  // PSK31.
  for (int i = 0; i < 6; ++i) {
    psk::Modulator pm(12000, psk::Mode::Bpsk31, 1000);
    const std::string c = calls[(i + 12) % 20];
    sc.addUsbAudio(scaled(pm.Transmission("CQ CQ CQ de " + c + " " + c + " pse k  hello from psk31 "), level(5, 20)), 12000,
                   place(200) - 1000, start());
  }
  // AM broadcasters: a carrier and noise-modulated sidebands, 9 kHz wide, strong.
  const double amRate = 24000;
  const std::size_t amN = static_cast<std::size_t>(amRate * seconds);
  for (int i = 0; i < nAm; ++i) {
    std::vector<float> a(amN);
    double m = 0;
    for (std::size_t k = 0; k < amN; ++k) {
      m = 0.85 * m + 0.15 * (u(rng) * 2 - 1);  // low-passed noise as programme
      a[k] = static_cast<float>((1.0 + 1.6 * m) * std::cos(2 * kPi * 6000.0 * static_cast<double>(k) / amRate));
    }
    sc.addUsbAudio(scaled(a, level(20, 40)), amRate, place(10000) - 6000, 0);
  }
  // Wideband modems: 39 tones, 2.4 kHz, a new random phase every 1/75 s.
  for (int i = 0; i < nWide; ++i) {
    const std::size_t n = static_cast<std::size_t>(12000 * seconds);
    std::vector<float> a(n, 0.0f);
    std::vector<double> ph(39);
    for (std::size_t k = 0; k < n; ++k) {
      if (k % 160 == 0)
        for (auto& p : ph) p = std::floor(u(rng) * 4) * kPi / 2;
      double v = 0;
      for (int t = 0; t < 39; ++t) v += std::cos(2 * kPi * (675 + 56.25 * t) * static_cast<double>(k) / 12000.0 + ph[t]);
      a[k] = static_cast<float>(v);
    }
    sc.addUsbAudio(scaled(a, level(5, 25)), 12000, place(3000), 0);
  }
  sc.addNoiseInBand(20 * std::log10(sig) - 10, 2500, 11);
  const auto iq = sc.render();

  SigmfWriter w;
  SigmfWriter::Global g;
  g.sampleRate = rate;
  g.recorder = std::string("make_busy_scene ") + HFC_VERSION;
  g.description = "Synthetic busy band around 7.074 MHz: " + std::to_string(cw) +
                  " CW, 8 FT8, FT4, 10 RTTY, 6 PSK31, " + std::to_string(nAm) + " AM, " + std::to_string(nWide) +
                  " wideband. Not a real recording.";
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
