// Test signals as SDRconnect would deliver them: IQ at a given rate, built
// from audio that the decoders' own generators make (NAVTEX, RTTY, ALE,
// Morse, PSK31, FT8...), each placed as an upper-sideband signal at an RF
// offset, plus carriers and white noise.
//
// The scene is built in the frequency domain over its whole duration: each
// audio signal's spectrum (positive half, i.e. its analytic signal) is copied
// to the bins at its dial offset, then one inverse FFT gives the IQ. This is
// exact; the duration must make the FFT sizes factor into 2, 3 and 5 (whole
// seconds with only those factors, e.g. 16, 20, 24, 30, 32, 40, 45, 48, 60).
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace hfc::testing {

class IqScene {
 public:
  IqScene(double sampleRate, double seconds);

  // Real audio at `audioRate`, starting `startSeconds` into the scene,
  // received in USB with the dial at `dialOffsetHz` from the centre: audio
  // frequency f appears at dialOffsetHz + f. `gain` scales the audio.
  void addUsbAudio(const std::vector<float>& audio, double audioRate, double dialOffsetHz, double startSeconds,
                   double gain = 1.0);
  // A carrier of amplitude `amplitude` (full scale = 1) from start to stop.
  void addCarrier(double offsetHz, double amplitude, double startSeconds, double stopSeconds);
  // Complex white Gaussian noise: `dbfs` total power over the whole band.
  void addNoise(double dbfs, uint32_t seed);
  // Noise density as SNR references use it: noise power in `bandHz` = dbfs.
  void addNoiseInBand(double dbfs, double bandHz, uint32_t seed);

  // Interleaved int16 IQ (clipped at full scale).
  std::vector<int16_t> render() const;
  std::vector<std::complex<float>> renderFloat() const;
  double sampleRate() const { return rate_; }
  double seconds() const { return seconds_; }

 private:
  double rate_, seconds_;
  std::size_t n_;
  std::vector<std::complex<double>> spec_;  // the scene's spectrum, FFT order
  std::vector<std::complex<double>> time_;  // time-domain additions (noise, carriers)
};

// FT8 / FT4 audio of one message at `toneHz` (tone 0), 12 kHz, as WSJT-X
// sends it: GFSK, the message alone (no padding). Ported from ft8_lib's
// demo/gen_ft8.c (MIT, Karlis Goba).
std::vector<float> ftxAudio(const std::string& message, double toneHz, bool ft4, double sampleRate = 12000.0);

// The audio's power, for setting SNRs.
double meanPower(const std::vector<float>& audio);

}  // namespace hfc::testing
