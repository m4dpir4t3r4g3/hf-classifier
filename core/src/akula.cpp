#include "hfc/akula.hpp"

#include "navtex/fsk_receiver.hpp"

namespace hfc {

namespace {

// First bit sent in the most significant position.
constexpr uint64_t kSync = 0b100101'100101'100101'100101'110001'110001ull;   // 36 bits
constexpr uint64_t kEnd = 0b101111'100010'100010'101111'011110ull;           // 30 bits
constexpr uint64_t kSyncMask = (1ull << 36) - 1, kEndMask = (1ull << 30) - 1;
constexpr uint64_t kMaxBurstBits = 6000;  // 12 s at 500 Bd

int popcount(uint64_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

std::vector<bool> bitsOf(uint64_t v, int n) {
  std::vector<bool> out;
  for (int i = n - 1; i >= 0; --i) out.push_back((v >> i) & 1);
  return out;
}

}  // namespace

std::vector<bool> akulaSync() { return bitsOf(kSync, 36); }
std::vector<bool> akulaEnd() { return bitsOf(kEnd, 30); }

bool AkulaParser::bit(bool one) {
  shift_ = (shift_ << 1) | (one ? 1 : 0);
  ++count_;
  if (count_ >= 36) {
    const int e = popcount((shift_ ^ kSync) & kSyncMask);
    // The better of two finds within the group's length stands: one word
    // later the group still fits with a few bits wrong (its first four
    // words are alike).
    if (e <= 3 && (syncAt_ == 0 || count_ - syncAt_ > 36 || e < syncErrors_)) syncAt_ = count_, syncErrors_ = e;
  }
  if (count_ < 30) return false;
  const int e = popcount((shift_ ^ kEnd) & kEndMask);
  if (e > 2) return false;
  const bool withSync = syncAt_ != 0 && count_ - syncAt_ <= kMaxBurstBits && count_ > syncAt_ && (count_ - syncAt_) % 6 == 0;
  if (!withSync) return false;  // (the end group alone: see the header)
  burst_.sync = withSync;
  burst_.words = withSync ? static_cast<int>((count_ - syncAt_) / 6) : 0;
  burst_.syncErrors = withSync ? syncErrors_ : 0;
  burst_.endErrors = e;
  syncAt_ = 0;
  return true;
}

std::string AkulaParser::describe(const Burst& b) {
  std::string out = b.sync ? "burst with Akula's sync and end groups, " + std::to_string(b.words) + " six-bit words between them"
                           : "burst ending in Akula's end group (no sync group: a data-only burst, or its start was missed)";
  const int wrong = b.syncErrors + b.endErrors;
  out += wrong ? " (" + std::to_string(wrong) + (wrong == 1 ? " bit" : " bits") + " of the groups wrong)" : "";
  return out + "; content encrypted, not shown";
}

struct AkulaDetector::Impl : navtex::FskReceiver::Client {
  explicit Impl(const navtex::SitorBConfig& c) : rx(c) {}
  navtex::FskReceiver rx;
  AkulaParser parser[2];  // as received, and inverted
  std::vector<std::string> bursts;
  void onBit(float soft, double) override {
    for (int p = 0; p < 2; ++p)
      if (parser[p].bit((soft > 0) != (p == 1))) bursts.push_back(AkulaParser::describe(parser[p].burst()));
  }
  bool synced() const override { return true; }
};

AkulaDetector::AkulaDetector(double rate, double centreHz) {
  navtex::SitorBConfig c;
  c.sampleRate = rate;
  c.baud = 500;
  c.shiftHz = 1000;
  c.minCentreHz = centreHz - 300;
  c.maxCentreHz = centreHz + 300;
  c.fixedCentreHz = centreHz;
  impl_ = std::make_unique<Impl>(c);
}

AkulaDetector::~AkulaDetector() = default;

void AkulaDetector::process(const float* audio, std::size_t n) { impl_->rx.process(audio, n, *impl_); }

std::vector<std::string> AkulaDetector::take() {
  std::vector<std::string> out;
  out.swap(impl_->bursts);
  return out;
}

}  // namespace hfc
