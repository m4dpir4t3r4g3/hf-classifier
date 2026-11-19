#include "iq_scene.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

#include "hfc/dsp.hpp"

extern "C" {
#include "ft8/constants.h"
#include "ft8/encode.h"
#include "ft8/message.h"
}

namespace hfc::testing {

namespace {

void fftDouble(std::vector<std::complex<double>>& v, bool inverse) {
  // KISS FFT in float is plenty for test signals.
  std::vector<cfloat> in(v.size()), out(v.size());
  for (std::size_t i = 0; i < v.size(); ++i) in[i] = cfloat(static_cast<float>(v[i].real()), static_cast<float>(v[i].imag()));
  Fft f(static_cast<int>(v.size()), inverse);
  f.run(in.data(), out.data());
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = std::complex<double>(out[i].real(), out[i].imag());
}

std::size_t checkedSize(double rate, double seconds) {
  const double n = rate * seconds;
  const auto r = static_cast<long>(std::llround(n));
  if (std::fabs(n - static_cast<double>(r)) > 1e-6 || !isSmooth235(r))
    throw std::invalid_argument("IqScene: rate * seconds must be an integer with factors 2, 3 and 5 only");
  return static_cast<std::size_t>(r);
}

}  // namespace

IqScene::IqScene(double sampleRate, double seconds)
    : rate_(sampleRate), seconds_(seconds), n_(checkedSize(sampleRate, seconds)) {
  spec_.assign(n_, {0, 0});
  time_.assign(n_, {0, 0});
}

void IqScene::addUsbAudio(const std::vector<float>& audio, double audioRate, double dialOffsetHz,
                          double startSeconds, double gain) {
  const std::size_t na = checkedSize(audioRate, seconds_);
  std::vector<std::complex<double>> a(na, {0, 0});
  const auto start = static_cast<std::size_t>(std::llround(startSeconds * audioRate));
  for (std::size_t i = 0; i < audio.size() && start + i < na; ++i) a[start + i] = gain * audio[i];
  fftDouble(a, false);
  // Bins are 1/seconds apart in both; the dial offset is rounded to a bin.
  const long dialBin = std::lround(dialOffsetHz * seconds_);
  const double scale = 2.0 * static_cast<double>(n_) / static_cast<double>(na);  // analytic, size change
  const long n = static_cast<long>(n_);
  for (std::size_t k = 1; k < na / 2; ++k) {
    long dst = (dialBin + static_cast<long>(k)) % n;
    if (dst < 0) dst += n;
    spec_[static_cast<std::size_t>(dst)] += a[k] * scale;
  }
}

void IqScene::addCarrier(double offsetHz, double amplitude, double startSeconds, double stopSeconds) {
  const auto a = static_cast<std::size_t>(std::max(0.0, startSeconds * rate_));
  const auto b = std::min(n_, static_cast<std::size_t>(stopSeconds * rate_));
  const double w = 2 * kPi * offsetHz / rate_;
  for (std::size_t i = a; i < b; ++i) time_[i] += amplitude * std::polar(1.0, w * static_cast<double>(i));
}

void IqScene::addNoise(double dbfs, uint32_t seed) {
  std::mt19937 g(seed);
  std::normal_distribution<double> nd(0.0, 1.0);
  const double sigma = std::sqrt(std::pow(10.0, dbfs / 10.0) / 2.0);
  for (auto& v : time_) v += std::complex<double>(sigma * nd(g), sigma * nd(g));
}

void IqScene::addNoiseInBand(double dbfs, double bandHz, uint32_t seed) {
  addNoise(dbfs + 10.0 * std::log10(rate_ / bandHz), seed);
}

std::vector<std::complex<float>> IqScene::renderFloat() const {
  std::vector<std::complex<double>> t = spec_;
  fftDouble(t, true);
  std::vector<std::complex<float>> out(n_);
  for (std::size_t i = 0; i < n_; ++i) {
    const auto v = t[i] / static_cast<double>(n_) + time_[i];
    out[i] = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
  }
  return out;
}

std::vector<int16_t> IqScene::render() const {
  const auto f = renderFloat();
  std::vector<int16_t> out(2 * n_);
  for (std::size_t i = 0; i < n_; ++i) {
    out[2 * i] = static_cast<int16_t>(std::lround(std::clamp(f[i].real() * 32768.0, -32768.0, 32767.0)));
    out[2 * i + 1] = static_cast<int16_t>(std::lround(std::clamp(f[i].imag() * 32768.0, -32768.0, 32767.0)));
  }
  return out;
}

std::vector<float> ftxAudio(const std::string& message, double toneHz, bool ft4, double sampleRate) {
  ftx_message_t msg;
  if (ftx_message_encode(&msg, nullptr, message.c_str()) != FTX_MESSAGE_RC_OK)
    throw std::invalid_argument("cannot pack FT8/FT4 message: " + message);
  const int nn = ft4 ? FT4_NN : FT8_NN;
  std::vector<uint8_t> tones(static_cast<std::size_t>(nn));
  if (ft4) ft4_encode(msg.payload, tones.data());
  else ft8_encode(msg.payload, tones.data());
  const double period = ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
  const double bt = ft4 ? 1.0 : 2.0;
  const int sps = static_cast<int>(0.5 + sampleRate * period);
  const int nwave = nn * sps;
  // Gaussian-smoothed frequency pulse, 3 symbols long.
  std::vector<double> pulse(static_cast<std::size_t>(3 * sps));
  const double k = 5.336446;  // pi * sqrt(2 / ln 2)
  for (int i = 0; i < 3 * sps; ++i) {
    const double t = i / static_cast<double>(sps) - 1.5;
    pulse[static_cast<std::size_t>(i)] = (std::erf(k * bt * (t + 0.5)) - std::erf(k * bt * (t - 0.5))) / 2;
  }
  std::vector<double> dphi(static_cast<std::size_t>(nwave + 2 * sps), 2 * kPi * toneHz / sampleRate);
  const double peak = 2 * kPi / sps;
  for (int i = 0; i < nn; ++i)
    for (int j = 0; j < 3 * sps; ++j) dphi[static_cast<std::size_t>(i * sps + j)] += peak * tones[static_cast<std::size_t>(i)] * pulse[static_cast<std::size_t>(j)];
  for (int j = 0; j < 2 * sps; ++j) {
    dphi[static_cast<std::size_t>(j)] += peak * pulse[static_cast<std::size_t>(j + sps)] * tones[0];
    dphi[static_cast<std::size_t>(j + nn * sps)] += peak * pulse[static_cast<std::size_t>(j)] * tones[static_cast<std::size_t>(nn - 1)];
  }
  std::vector<float> out(static_cast<std::size_t>(nwave));
  double phi = 0;
  for (int i = 0; i < nwave; ++i) {
    out[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(phi));
    phi = std::fmod(phi + dphi[static_cast<std::size_t>(i + sps)], 2 * kPi);
  }
  const int ramp = sps / 8;
  for (int i = 0; i < ramp; ++i) {
    const float env = static_cast<float>((1 - std::cos(2 * kPi * i / (2 * ramp))) / 2);
    out[static_cast<std::size_t>(i)] *= env;
    out[static_cast<std::size_t>(nwave - 1 - i)] *= env;
  }
  return out;
}

double meanPower(const std::vector<float>& audio) {
  double s = 0;
  for (float v : audio) s += double(v) * v;
  return audio.empty() ? 0.0 : s / static_cast<double>(audio.size());
}

}  // namespace hfc::testing
