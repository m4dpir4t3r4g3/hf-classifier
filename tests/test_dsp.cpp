// Tests of the channelizer and the detector on synthetic IQ (IqScene).
#include <algorithm>
#include <cmath>
#include <vector>

#include "PskModulator.h"
#include "hfc/channelizer.hpp"
#include "hfc/detector.hpp"
#include "iq_scene.hpp"
#include "morse/synth.hpp"
#include "navtex/synth.hpp"
#include "test_framework.hpp"

using namespace hfc;

namespace {

// A tone through one channel: frequency, amplitude and phase of the
// channel output against what the tone and the channel offset predict.
// `offset` 12000 Hz is an even FFT bin with no fine-tuning residual; other
// offsets exercise the odd-bin sign correction and the fine oscillator.
void checkChannel(double rate, bool exact12k = true, double offset = 12000.0) {
  Channelizer ch(rate);
  if (exact12k) CHECK_NEAR(ch.channelRate(), 12000.0, 1e-9);
  else CHECK_NEAR(ch.channelRate(), 12000.0, 120.0);
  const double tone = offset + 345.678;  // the tone at +345.678 Hz in the channel
  ch.addChannel(offset, 3000);
  std::vector<cfloat> out;
  uint64_t first = 0;
  ch.setChannelCallback([&](int, const cfloat* s, std::size_t n, uint64_t f) {
    if (out.empty()) first = f;
    out.insert(out.end(), s, s + n);
  });
  const auto total = static_cast<std::size_t>(rate * 1.0);
  std::vector<cfloat> x(65536);
  const double w = 2 * kPi * tone / rate;
  std::size_t done = 0;
  while (done < total) {
    const std::size_t n = std::min(x.size(), total - done);
    for (std::size_t i = 0; i < n; ++i) {
      const double ph = std::fmod(w * static_cast<double>(done + i), 2 * kPi);
      x[i] = cfloat(static_cast<float>(0.5 * std::cos(ph)), static_cast<float>(0.5 * std::sin(ph)));
    }
    ch.process(x.data(), n);
    done += n;
  }
  CHECK(out.size() > 10000);
  double maxDev = 0, aMin = 9, aMax = 0;
  for (std::size_t i = 1; i < out.size(); ++i) {
    const double f = std::arg(out[i] * std::conj(out[i - 1])) * ch.channelRate() / (2 * kPi);
    maxDev = std::max(maxDev, std::fabs(f - (tone - offset)));
    aMin = std::min(aMin, static_cast<double>(std::abs(out[i])));
    aMax = std::max(aMax, static_cast<double>(std::abs(out[i])));
  }
  // No phase jumps between blocks: 0.5 Hz of instantaneous frequency at
  // 12 kHz is a phase step of 0.0003 rad. (Measured: < 0.1 Hz below 2.6 MS/s;
  // 0.18 Hz at 10 MS/s between bins, where the shorter blocks let a little
  // of the filter's tails wrap around.)
  if (maxDev >= 0.5) std::printf("    rate %.0f offset %.3f: max deviation %.4f Hz\n", rate, offset, maxDev);
  CHECK(maxDev < 0.5);
  CHECK(aMin > 0.499 && aMax < 0.501);
  // Phase of the first output sample: the tone's phase at that input
  // sample, less the channel's. (Only where the channel is an even bin with
  // no residual: otherwise a constant phase offset, harmless, remains.)
  if (offset == 12000.0 && exact12k) {
    const double expect = 2 * kPi * (tone - offset) / rate * static_cast<double>(first);
    CHECK(std::fabs(std::remainder(std::arg(out[0]) - expect, 2 * kPi)) < 0.01);
  }
}

struct Found {
  std::vector<Emission> ended;
  int started = 0;
  uint64_t frames = 0;
};

Found detect(const testing::IqScene& sc, double centre, int* startedOut = nullptr) {
  const auto iq = sc.render();
  const double rate = sc.sampleRate();
  Channelizer ch(rate);
  Detector det(rate, ch.fftSize(), static_cast<double>(ch.hop()) / rate);
  det.setCentre(centre, 0);
  Found f;
  det.setEmissionCallbacks([&](const Emission&) { ++f.started; }, nullptr,
                           [&](const Emission& e) { f.ended.push_back(e); });
  ch.setSpectrumCallback([&](const float* p, std::size_t n, uint64_t c) {
    det.addSpectrum(p, n, static_cast<double>(c) / rate);
  });
  ch.process(iq.data(), iq.size() / 2);
  det.endAll(sc.seconds());
  f.frames = det.frames();
  if (startedOut) *startedOut = f.started;
  return f;
}

const Emission* near(const Found& f, double hz, double tol) {
  for (const auto& e : f.ended)
    if (std::fabs(e.centreHz - hz) < tol) return &e;
  return nullptr;
}

}  // namespace

TEST(channelizer_is_exact_at_every_sdrconnect_rate) {
  // The rates the fake SDRconnect offers, and 2.048 MS/s (new in SDRconnect
  // 1.0.10). The real list is what sdrc_probe reports.
  for (double rate : {62500.0, 125000.0, 250000.0, 500000.0, 1000000.0, 2000000.0, 2048000.0, 6000000.0, 8000000.0,
                      10000000.0})
    checkChannel(rate);
}

TEST(channelizer_is_exact_on_odd_bins_and_between_bins) {
  for (double rate : {62500.0, 250000.0, 2000000.0, 10000000.0}) {
    Channelizer probe(rate);
    checkChannel(rate, true, 12000.0 + probe.binHz());  // an odd bin
    checkChannel(rate, true, 12007.3);                  // between bins
  }
}

TEST(channelizer_handles_an_awkward_rate) {
  // Not one of SDRconnect's rates (1999999 = 17 * 71 * 1657: no smooth FFT
  // size gives exactly 12 kHz); the channel rate comes out close to it and
  // the channel is still exact at that rate.
  checkChannel(1999999.0, false);
}

TEST(channelizer_spectrum_puts_a_tone_in_its_bin) {
  const double rate = 250000;
  Channelizer ch(rate);
  std::vector<float> last;
  ch.setSpectrumCallback([&](const float* p, std::size_t n, uint64_t) { last.assign(p, p + n); });
  std::vector<cfloat> x(static_cast<std::size_t>(rate / 4));
  const double f = -40000.0;
  for (std::size_t i = 0; i < x.size(); ++i)
    x[i] = std::polar(0.5f, static_cast<float>(std::fmod(2 * kPi * f / rate * static_cast<double>(i), 2 * kPi)));
  ch.process(x.data(), x.size());
  CHECK(!last.empty());
  const auto peak = static_cast<std::size_t>(std::max_element(last.begin(), last.end()) - last.begin());
  const double hz = (static_cast<double>(peak) - static_cast<double>(last.size()) / 2) * ch.binHz();
  CHECK_NEAR(hz, f, ch.binHz());
}

TEST(detector_reports_nothing_in_noise) {
  testing::IqScene sc(250000, 20);
  sc.addNoiseInBand(-60, 3000, 1);
  const Found f = detect(sc, 7e6);
  CHECK(f.started == 0);
  CHECK(f.frames > 100);
}

TEST(detector_finds_and_measures_emissions) {
  testing::IqScene sc(250000, 30);
  sc.addNoiseInBand(-60, 3000, 2);
  sc.addCarrier(-50000, 0.01, 2, 25);
  navtex::SynthConfig nc;
  nc.sampleRate = 12000;
  nc.centreHz = 1500;
  nc.amplitude = 0.01;
  nc.phasingSeconds = 3;
  const auto nv = navtex::synthesize(navtex::navtexMessage("OA23", "GALE WARNING SW 8"), nc);
  sc.addUsbAudio(nv.samples, 12000, 20000 - 1500, 1);
  morse::SynthConfig mc;
  mc.sampleRate = 12000;
  mc.toneHz = 1500;
  mc.amplitude = 0.01;
  const auto cw = morse::synthesize("CQ CQ DE TEST TEST K", mc);
  sc.addUsbAudio(cw.samples, 12000, -20000 - 1500, 3);
  psk::Modulator pm(12000, psk::Mode::Bpsk31, 1500);
  auto ps = pm.Transmission("CQ CQ DE TEST TEST PSK31 K");
  for (auto& v : ps) v *= 0.01f;
  sc.addUsbAudio(ps, 12000, 60000 - 1500, 5);

  const double c = 7e6;
  const Found f = detect(sc, c);
  CHECK(f.ended.size() == 4);
  const Emission* carrier = near(f, c - 50000, 20);
  const Emission* nav = near(f, c + 20000, 40);
  const Emission* morse = near(f, c - 20000, 20);
  const Emission* psk31 = near(f, c + 60000, 20);
  CHECK(carrier && nav && morse && psk31);
  if (carrier) {
    CHECK_NEAR(carrier->startUtc, 2.0, 0.4);
    CHECK_NEAR(carrier->endUtc, 25.0, 0.4);
    CHECK_NEAR(carrier->duty, 1.0, 0.02);
    CHECK_NEAR(carrier->driftHzPerSecond, 0.0, 0.1);
  }
  if (nav) {
    // 170 Hz shift, 100 Bd: about 300 Hz occupied. The detected band of a
    // strong signal is wider (window skirts above the threshold).
    CHECK(nav->occupiedHz() > 200 && nav->occupiedHz() < 400);
    CHECK(nav->bandwidthHz() >= nav->occupiedHz());
    CHECK_NEAR(nav->startUtc, 1.0 + nc.leadSeconds, 0.5);
  }
  if (morse) {
    CHECK(morse->duty < 0.95);  // keyed
    CHECK(morse->occupiedHz() < 150);
  }
}

TEST(detector_merges_tracks_that_grow_together) {
  // A carrier at 1000 Hz, another starting 2 s later at 1600 Hz (separate
  // emissions: they did not start together), then a wideband signal
  // covering both: the two grow into one emission.
  testing::IqScene sc(250000, 20);
  sc.addNoiseInBand(-60, 3000, 3);
  sc.addCarrier(1000, 0.01, 1, 15);
  sc.addCarrier(1600, 0.01, 3, 15);
  std::vector<float> wide(12000 * 6);
  uint32_t seed = 9;
  for (auto& v : wide) {
    seed = seed * 1664525u + 1013904223u;
    v = 0.02f * (static_cast<float>(seed >> 8) / 16777216.0f - 0.5f);
  }
  // Band-limit the noise burst to 600..2200 Hz audio by brute force (FFT).
  {
    std::vector<cfloat> a(wide.size()), b(wide.size());
    for (std::size_t i = 0; i < wide.size(); ++i) a[i] = wide[i];
    Fft fwd(static_cast<int>(a.size()), false), inv(static_cast<int>(a.size()), true);
    fwd.run(a.data(), b.data());
    for (std::size_t k = 0; k < b.size(); ++k) {
      const double f = static_cast<double>(std::min(k, b.size() - k)) * 12000.0 / static_cast<double>(b.size());
      if (f < 600 || f > 2200) b[k] = 0;
    }
    inv.run(b.data(), a.data());
    for (std::size_t i = 0; i < wide.size(); ++i) wide[i] = a[i].real() / static_cast<float>(a.size());
  }
  sc.addUsbAudio(wide, 12000, 0, 6);
  int started = 0;
  const Found f = detect(sc, 7e6, &started);
  CHECK(started == 2);  // the second carrier was reported on its own first
  CHECK(f.ended.size() == 2);
  int lasting = 0;
  for (const auto& e : f.ended)
    if (e.endUtc > 14) {
      ++lasting;
      CHECK(e.loHz < 7e6 + 1000 && e.hiHz > 7e6 + 1600);
      CHECK_NEAR(e.startUtc, 1.0, 0.5);
    }
  CHECK(lasting == 1);
}

// Audio (12 kHz) of a serial-tone modem: random 8PSK at 2400 Bd, 35 %
// root-raised cosine, on 1800 Hz (the shape of STANAG 4285 and
// MIL-STD-188-110), `seconds` long, RMS 1.
std::vector<float> serialModemAudio(double seconds, uint32_t seed) {
  const int sps = 5;  // 12000 / 2400
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
  const std::size_t n = static_cast<std::size_t>(seconds * 12000);
  std::vector<std::complex<double>> up(n + rrc.size());
  for (std::size_t k = 0; k * sps < n; ++k) {
    seed = seed * 1664525u + 1013904223u;
    up[k * sps] = std::polar(1.0, kPi / 4 * static_cast<double>(seed >> 29));
  }
  std::vector<float> out(n);
  double power = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::complex<double> acc = 0;
    for (std::size_t j = 0; j < rrc.size() && j <= i; ++j) acc += rrc[j] * up[i - j];
    out[i] = static_cast<float>((acc * std::polar(1.0, 2 * kPi * 1800.0 * static_cast<double>(i) / 12000.0)).real());
    power += static_cast<double>(out[i]) * out[i];
  }
  const float scale = static_cast<float>(1.0 / std::sqrt(power / static_cast<double>(n)));
  for (auto& v : out) v *= scale;
  return out;
}

TEST(detector_sees_a_weak_wide_modem_as_one_emission) {
  // A serial-tone modem a few dB above the noise: in any one frame most of
  // its bins miss the threshold, and it used to break into fragments that
  // came and went as separate emissions. Now one emission, about as wide as
  // the modem. Six carriers 150 Hz apart, near it in strength, stay six (they
  // start a second apart: started together they would be grouped by onset,
  // as the tones of an MFSK signal are).
  testing::IqScene sc(62500, 16);
  sc.addNoiseInBand(-60, 3000, 7);
  sc.addUsbAudio(serialModemAudio(10, 5), 12000, 10000 - 1800, 2.0, 0.001);
  for (int k = 0; k < 6; ++k) sc.addCarrier(-10000 + 150.0 * k, 0.0004, 2.0 + k, 14.0);
  const Found f = detect(sc, 7e6);
  int modem = 0;
  const Emission* wide = nullptr;
  for (const auto& e : f.ended)
    if (e.hiHz > 7e6 + 10000 - 1200 && e.loHz < 7e6 + 10000 + 1200) {
      ++modem;
      if (!wide || e.occupiedHz() > wide->occupiedHz()) wide = &e;
    }
  // (At this level the threshold alone made 5 overlapping fragments, 33 s
  // of emissions for the 10 s modem.)
  CHECK(modem == 1);
  CHECK(wide && wide->occupiedHz() > 1800 && wide->occupiedHz() < 3600);
  CHECK(wide && wide->endUtc - wide->startUtc > 9.0);
  int carriers = 0;
  for (int k = 0; k < 6; ++k) carriers += near(f, 7e6 - 10000 + 150.0 * k, 30) != nullptr;
  CHECK(carriers == 6);
}

TEST(detector_groups_tones_that_start_together) {
  // An 850 Hz shift FSK (random bits, 75 Bd): its two tones are one emission
  // from the start, centred between them, with the shift as its width. A
  // carrier 900 Hz away that starts 3 s later stays separate.
  testing::IqScene sc(250000, 16);
  sc.addNoiseInBand(-60, 3000, 4);
  std::vector<float> a(12000 * 10);
  uint32_t seed = 3;
  double ph = 0;
  bool mark = false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (i % 160 == 0) {
      seed = seed * 1664525u + 1013904223u;
      mark = (seed >> 31) != 0;
    }
    ph += 2 * kPi * (mark ? 1925.0 : 1075.0) / 12000.0;
    a[i] = 0.01f * static_cast<float>(std::sin(ph));
  }
  sc.addUsbAudio(a, 12000, 10000 - 1500, 2);
  sc.addCarrier(11400, 0.007, 5, 14);
  int started = 0;
  const Found f = detect(sc, 7e6, &started);
  CHECK(started == 2);
  // The power-weighted centre leans towards whichever tone was sent more;
  // the middle of the occupied band is the FSK's centre.
  const Emission* fsk = nullptr;
  for (const auto& e : f.ended)
    if (std::fabs(0.5 * (e.occLoHz + e.occHiHz) - (7e6 + 10000)) < 60) fsk = &e;
  const Emission* carrier = near(f, 7e6 + 11400, 30);
  CHECK(fsk && carrier);
  if (fsk) {
    CHECK(fsk->occupiedHz() > 850 && fsk->occupiedHz() < 1100);
    CHECK_NEAR(fsk->startUtc, 2.0, 0.4);
  }
}
