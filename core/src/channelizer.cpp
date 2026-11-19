#include "hfc/channelizer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace hfc {

namespace {

// FFT size: a multiple of `multiple` (so that K is an integer), made of
// factors 2, 3 and 5, at least `minSize`, and even twice over (N/4 must be
// an integer).
std::size_t pickSize(long multiple, long minSize) {
  for (long n = multiple; n < (1L << 24); n += multiple)
    if (n >= minSize && n % 4 == 0 && isSmooth235(n)) return static_cast<std::size_t>(n);
  return 0;
}

}  // namespace

Channelizer::Channelizer(double inputRate) : rate_(inputRate), outRate_(kChannelRate) {
  if (!(inputRate >= 24000.0 && inputRate <= 100e6))
    throw std::invalid_argument("Channelizer: sample rate out of range (24 kS/s to 100 MS/s)");
  const double targetBin = inputRate <= 2.6e6 ? 15.625 : 31.25;
  const long minSize = static_cast<long>(std::ceil(inputRate / targetBin));
  // 12000 / rate = a / b in lowest terms: K = N a / b is an integer when N
  // is a multiple of b. Also needed: N and K multiples of 4, N smooth.
  const long r = std::lround(inputRate);
  std::size_t n = 0;
  if (std::fabs(inputRate - static_cast<double>(r)) < 1e-6) {
    const long g = gcdLong(12000, r);
    const long a = 12000 / g, b = r / g;
    for (long cand = b; cand < (1L << 24); cand += b) {
      if (cand < minSize || cand % 4 != 0 || !isSmooth235(cand)) continue;
      if ((cand / b * a) % 4 != 0) continue;
      n = static_cast<std::size_t>(cand);
      break;
    }
  }
  if (n) {
    n_ = n;
    k_ = static_cast<std::size_t>(std::lround(static_cast<double>(n_) * kChannelRate / inputRate));
  } else {
    // A rate that does not divide: nearest K, channels slightly off 12 kHz.
    n_ = pickSize(4, minSize);
    k_ = static_cast<std::size_t>(std::lround(static_cast<double>(n_) * kChannelRate / inputRate / 4.0)) * 4;
    outRate_ = inputRate * static_cast<double>(k_) / static_cast<double>(n_);
  }
  fft_ = std::make_unique<Fft>(static_cast<int>(n_), false);
  ifft_ = std::make_unique<Fft>(static_cast<int>(k_), true);
  buf_.assign(n_, cfloat(0, 0));
  spectrum_.resize(n_);
  power_.resize(n_);
}

Channelizer::~Channelizer() = default;

int Channelizer::addChannel(double offsetHz, double bandwidthHz) {
  Channel c;
  const double bin = binHz();
  c.bin = std::lround(offsetHz / bin);
  const double residual = offsetHz - static_cast<double>(c.bin) * bin;
  c.fine.setStep(-2.0 * kPi * residual / outRate_);
  // Flat to +-bandwidth/2, raised-cosine edges 300 Hz wide outside it,
  // never beyond 90 % of the channel's Nyquist frequency.
  const double half = std::min(bandwidthHz / 2.0, 0.45 * outRate_ - 300.0);
  const double edge = 300.0;
  c.filter.resize(k_);
  for (std::size_t i = 0; i < k_; ++i) {
    const double f = (static_cast<double>(i) - static_cast<double>(k_) / 2.0) * bin;
    const double a = std::fabs(f);
    double w = 0.0;
    if (a <= half) w = 1.0;
    else if (a < half + edge) w = 0.5 * (1.0 + std::cos(kPi * (a - half) / edge));
    // FFT scaling: forward N then inverse K gives N times the amplitude.
    c.filter[i] = static_cast<float>(w / static_cast<double>(n_));
  }
  c.spec.resize(k_);
  c.time.resize(k_);
  const int id = nextId_++;
  channels_.emplace(id, std::move(c));
  return id;
}

void Channelizer::removeChannel(int id) { channels_.erase(id); }

void Channelizer::extract(int id, Channel& c, const cfloat* spectrum, uint64_t block, uint64_t blockStart, bool full) {
  // K bins centred on the channel, in IFFT order (bin 0 = channel centre).
  const long n = static_cast<long>(n_);
  const long k = static_cast<long>(k_);
  for (long i = 0; i < k; ++i) {
    const long rel = i < k / 2 ? i : i - k;  // -K/2 .. K/2-1
    long src = (c.bin + rel) % n;
    if (src < 0) src += n;
    c.spec[static_cast<std::size_t>(i)] = spectrum[static_cast<std::size_t>(src)] *
                                           c.filter[static_cast<std::size_t>(rel + k / 2)];
  }
  ifft_->run(c.spec.data(), c.time.data());
  // Moving the band by `bin` bins in a block that starts at sample s0
  // leaves a phase of 2 pi bin s0 / N; with s0 a multiple of N/2 that is
  // (-1)^(bin * blocks).
  const float sign = ((c.bin & 1) && (block & 1)) ? -1.0f : 1.0f;
  cfloat* mid = c.time.data() + k_ / 4;
  for (std::size_t i = 0; i < k_ / 2; ++i) mid[i] *= sign * c.fine.next();
  if (channelFn_ && full) channelFn_(id, mid, k_ / 2, blockStart + n_ / 4);
  ++c.blocks;
}

void Channelizer::setHistorySeconds(double seconds, std::size_t maxBytes) {
  std::size_t hops = seconds > 0 ? static_cast<std::size_t>(std::ceil(seconds * rate_ / static_cast<double>(hop()))) : 0;
  hops = std::min(hops, maxBytes / (n_ * sizeof(cfloat)));
  history_.assign(hops, Kept());
  historyNext_ = historyCount_ = 0;
}

double Channelizer::historySeconds() const {
  return static_cast<double>(history_.size() * hop()) / rate_;
}

void Channelizer::replay(int id, double seconds) {
  auto it = channels_.find(id);
  if (it == channels_.end() || historyCount_ == 0) return;
  const std::size_t want = static_cast<std::size_t>(std::ceil(seconds * rate_ / static_cast<double>(hop())));
  const std::size_t count = std::min(want, historyCount_);
  for (std::size_t i = 0; i < count; ++i) {
    const Kept& k = history_[(historyNext_ + history_.size() - count + i) % history_.size()];
    extract(id, it->second, k.spectrum.data(), k.block, k.blockStart, k.full);
  }
}

void Channelizer::process(const int16_t* iq, std::size_t pairs) {
  // Convert in chunks so that blocks run as soon as they are complete.
  constexpr float scale = 1.0f / 32768.0f;
  std::size_t i = 0;
  while (i < pairs) {
    const std::size_t room = n_ / 2 - fill_;
    const std::size_t take = std::min(room, pairs - i);
    cfloat* dst = buf_.data() + n_ / 2 + fill_;
    for (std::size_t j = 0; j < take; ++j)
      dst[j] = cfloat(iq[2 * (i + j)] * scale, iq[2 * (i + j) + 1] * scale);
    fill_ += take;
    i += take;
    in_ += take;
    if (fill_ == n_ / 2) runBlock();
  }
}

void Channelizer::process(const cfloat* iq, std::size_t pairs) {
  std::size_t i = 0;
  while (i < pairs) {
    const std::size_t take = std::min(n_ / 2 - fill_, pairs - i);
    std::copy(iq + i, iq + i + take, buf_.begin() + static_cast<std::ptrdiff_t>(n_ / 2 + fill_));
    fill_ += take;
    i += take;
    in_ += take;
    if (fill_ == n_ / 2) runBlock();
  }
}

void Channelizer::runBlock() {
  // buf_ holds the previous half and the new half: one block of N samples
  // starting at input sample in_ - N.
  fft_->run(buf_.data(), spectrum_.data());
  const uint64_t blockStart = in_ >= n_ ? in_ - n_ : 0;
  const bool full = in_ >= n_;  // the first block is half zeros

  if (spectrumFn_ && full) {
    // Hann window applied in the frequency domain, bins reordered to
    // ascending frequency.
    const std::size_t half = n_ / 2;
    const cfloat* x = spectrum_.data();
    auto hann = [&](std::size_t k, std::size_t prev, std::size_t next) {
      return std::norm(0.5f * x[k] - 0.25f * (x[prev] + x[next]));
    };
    // Bins 0 .. N/2-1 go to N/2 .. N-1 and the rest to 0 .. N/2-1; the
    // wrap-around neighbours of bins 0 and N-1 are done apart (no modulo
    // in the loops: it cost more than the FFT's butterflies).
    power_[half] = hann(0, n_ - 1, 1);
    for (std::size_t k = 1; k < half; ++k) power_[k + half] = hann(k, k - 1, k + 1);
    for (std::size_t k = half; k + 1 < n_; ++k) power_[k - half] = hann(k, k - 1, k + 1);
    power_[half - 1] = hann(n_ - 1, n_ - 2, 0);
    spectrumFn_(power_.data(), n_, blockStart + n_ / 2);
  }

  for (auto& [id, c] : channels_) extract(id, c, spectrum_.data(), blocks_, blockStart, full);
  if (!history_.empty()) {
    Kept& k = history_[historyNext_];
    k.spectrum.assign(spectrum_.begin(), spectrum_.end());
    k.block = blocks_;
    k.blockStart = blockStart;
    k.full = full;
    historyNext_ = (historyNext_ + 1) % history_.size();
    historyCount_ = std::min(historyCount_ + 1, history_.size());
  }
  ++blocks_;
  // Keep the newest half for the next block.
  std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(n_ / 2), buf_.end(), buf_.begin());
  fill_ = 0;
  if (blockDoneFn_) blockDoneFn_();
}

}  // namespace hfc
