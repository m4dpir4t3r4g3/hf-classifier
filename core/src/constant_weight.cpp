#include "hfc/constant_weight.hpp"

#include <cstdio>

#include "navtex/fsk_receiver.hpp"

namespace hfc {

bool ConstantWeightParser::bit(bool one) {
  word_ = static_cast<uint8_t>(((word_ << 1) | (one ? 1 : 0)) & 0x7F);
  ++count_;
  if (count_ < 7 || found_) return false;
  int weight = 0;
  for (int b = 0; b < 7; ++b) weight += (word_ >> b) & 1;
  const auto al = static_cast<std::size_t>(count_ % 7);
  for (int k = 0; k < 2; ++k) {
    const bool hit = weight == (k == 0 ? 4 : 3);
    auto& f = flags_[al][k];
    f.push_back(hit);
    hits_[al][k] += hit ? 1 : 0;
    if (f.size() > static_cast<std::size_t>(kWords)) {
      hits_[al][k] -= f.front() ? 1 : 0;
      f.pop_front();
    }
    if (f.size() == static_cast<std::size_t>(kWords) && hits_[al][k] * 4 >= kWords * 3) {
      found_ = true;
      share_ = static_cast<double>(hits_[al][k]) / kWords;
      ones_ = k == 0 ? 4 : 3;
      return true;
    }
  }
  return false;
}

struct ConstantWeightDetector::Impl : navtex::FskReceiver::Client {
  explicit Impl(const navtex::SitorBConfig& c) : rx(c) {}
  navtex::FskReceiver rx;
  ConstantWeightParser parser;
  std::string found;
  void onBit(float soft, double) override {
    if (!parser.bit(soft > 0)) return;
    char b[240];
    std::snprintf(b, sizeof b,
                  "7-bit words of constant weight 4:3 (%.0f %% of %d in a row): the kind of code CIS-36-50 and CIS-50-50 "
                  "(T-600), SITOR and several ARQ systems use; content not read",
                  100 * parser.share(), ConstantWeightParser::kWords);
    found = b;
  }
  bool synced() const override { return true; }
};

ConstantWeightDetector::ConstantWeightDetector(double rate, double centreHz, double baud, double shiftHz) {
  navtex::SitorBConfig c;
  c.sampleRate = rate;
  c.baud = baud;
  c.shiftHz = shiftHz;
  c.minCentreHz = centreHz - 150;
  c.maxCentreHz = centreHz + 150;
  c.fixedCentreHz = centreHz;
  impl_ = std::make_unique<Impl>(c);
}

ConstantWeightDetector::~ConstantWeightDetector() = default;

void ConstantWeightDetector::process(const float* audio, std::size_t n) { impl_->rx.process(audio, n, *impl_); }

std::string ConstantWeightDetector::take() {
  std::string out;
  out.swap(impl_->found);
  return out;
}

}  // namespace hfc
