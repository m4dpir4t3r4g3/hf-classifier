#include "navtex/ale2g.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace navtex::ale {

namespace {

constexpr double kBankOffsetHz[Ale2gReceiver::kBanks] = {0.0, -50.0, 50.0};
constexpr double kTickSamples = kBasebandRate / 125.0 / Ale2gReceiver::kPhases;  // 9.6
constexpr int kSymbolSamples = 77;  // 8.02 ms matched filter
constexpr double kSyncTolerance = 0.003;
constexpr double kWindow = 0.009;  // one word decodes at timing phases this far apart

// Acceptance thresholds (unanimous votes of 48, Golay corrections).
constexpr int kMinUnanimous = 14;     // not worth decoding below this
constexpr int kSyncedUnanimous = 20;  // at the boundary of an accepted word
constexpr int kSyncedCorrections = 4;
constexpr int kStrongUnanimous = 26;  // alone
constexpr int kStrongCorrections = 2;
constexpr int kWeakUnanimous = 20;    // paired with another word 392 ms away
constexpr int kWeakCorrections = 4;

int popcount(std::uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

// Soft-decision (Chase) Golay decoding: tries the hard decision with every
// combination of its four least reliable bits flipped, and keeps the code
// word closest to the soft values. rel[b]: reliability of code word bit b.
bool chaseGolay(std::uint32_t hard, const float rel[24], int& data, int& distance) {
  int weakest[4] = {-1, -1, -1, -1};
  for (int b = 0; b < 24; ++b) {
    for (int k = 0; k < 4; ++k) {
      if (weakest[k] < 0 || rel[b] < rel[weakest[k]]) {
        for (int m = 3; m > k; --m) weakest[m] = weakest[m - 1];
        weakest[k] = b;
        break;
      }
    }
  }
  float best = 1e30f;
  bool found = false;
  for (int pattern = 0; pattern < 16; ++pattern) {
    std::uint32_t r = hard;
    for (int k = 0; k < 4; ++k)
      if (pattern & (1 << k)) r ^= 1u << weakest[k];
    int errors;
    int d = golayDecode(r, errors);
    if (d < 0) continue;
    std::uint32_t c = golayEncode(static_cast<std::uint32_t>(d));
    std::uint32_t diff = c ^ hard;
    float cost = 0.0f;
    for (int b = 0; b < 24; ++b)
      if (diff & (1u << b)) cost += rel[b];
    if (cost < best) {
      best = cost;
      data = d;
      distance = popcount(diff);
      found = true;
    }
  }
  return found;
}

bool isAddressPreamble(int p) { return p == kTo || p == kThru || p == kFrom || p == kTis || p == kTwas; }

// A word that may follow another: an address in the basic 38 set, a CMD
// (anything), DATA and REP in the expanded 64 set (addresses and AMD text;
// DTM and DBM data, which may be anything, are not decoded).
bool plausible(std::uint32_t w) {
  const int p = wordPreamble(w);
  if (p == kCmd) return true;
  for (int i = 0; i < 3; ++i) {
    char c = wordChar(w, i);
    if (isAddressPreamble(p) ? !isBasic38(c) : !isExpanded64(c)) return false;
  }
  return true;
}

// A word that may start a frame on its own: an address in the basic 38 set.
bool acquirable(std::uint32_t w) {
  if (!isAddressPreamble(wordPreamble(w))) return false;
  for (int i = 0; i < 3; ++i)
    if (!isBasic38(wordChar(w, i))) return false;
  return true;
}

}  // namespace

// ------------------------------------------------------------- receiver

Ale2gReceiver::Ale2gReceiver() {
  for (int b = 0; b < kBanks; ++b)
    for (int k = 0; k < 8; ++k) {
      double f = kToneHz0 + kToneStepHz * k - kCarrierHz + kBankOffsetHz[b];
      rot_[b][k] = std::polar(1.0, -2.0 * kPi * f / kBasebandRate);
      sums_[b][k].setLength(kSymbolSamples);
    }
  reset();
}

void Ale2gReceiver::reset() {
  for (int b = 0; b < kBanks; ++b)
    for (int k = 0; k < 8; ++k) {
      sums_[b][k].reset();
      phasor_[b][k] = {1.0, 0.0};
    }
  sample_ = 0;
  nextTick_ = kSymbolSamples;
  tick_ = 0;
  renorm_ = 0;
  for (auto& row : hist_)
    for (auto& h : row) h = History();
  poolSize_ = 0;
  lastCommit_ = -1e9;
  for (auto& w : weak_) w = Candidate();
  weakPos_ = 0;
}

void Ale2gReceiver::process(cf x, double time) {
  const std::complex<double> z(x.real(), x.imag());
  for (int b = 0; b < kBanks; ++b)
    for (int k = 0; k < 8; ++k) {
      last_[b][k] = sums_[b][k].process(z * phasor_[b][k]);
      phasor_[b][k] *= rot_[b][k];
    }
  if (++renorm_ >= 4096) {
    renorm_ = 0;
    for (auto& row : phasor_)
      for (auto& p : row) p /= std::abs(p);
  }
  ++sample_;
  if (static_cast<double>(sample_) >= nextTick_) {
    nextTick_ += kTickSamples;
    const int phase = static_cast<int>(tick_++ % kPhases);
    for (int b = 0; b < kBanks; ++b) {
      float e[8];
      for (int k = 0; k < 8; ++k) {
        e[k] = static_cast<float>(std::norm(last_[b][k]));
      }
      symbol(phase, b, e, time);
    }
    if (poolSize_ > 0 && time > poolStart_ + kWindow + 0.002) commit();
  }
}

void Ale2gReceiver::symbol(int phase, int bank, const float energy[8], double time) {
  History& h = hist_[phase][bank];
  Symbol& s = h.sym[h.head];
  float total = 1e-30f;
  for (int k = 0; k < 8; ++k) total += energy[k];
  for (int j = 0; j < 3; ++j) {
    float max1 = 0.0f, max0 = 0.0f;
    for (int k = 0; k < 8; ++k) {
      bool one = (tribitOfTone(k) >> (2 - j)) & 1;
      float& m = one ? max1 : max0;
      m = std::max(m, energy[k]);
    }
    s.llr[j] = (max1 - max0) / total;
  }
  h.head = (h.head + 1) % 49;
  if (h.count < 49) ++h.count;
  if (h.count == 49) evaluate(phase, bank, time);
}

void Ale2gReceiver::evaluate(int phase, int bank, double time) {
  const History& h = hist_[phase][bank];
  // The 147 bits, oldest first: three copies of the 49-bit word.
  float copy[3][49];
  for (int i = 0; i < 147; ++i) {
    const Symbol& s = h.sym[(h.head + i / 3) % 49];
    copy[i / 49][i % 49] = s.llr[i % 3];
  }
  int unanimous = 0;
  float combined[49];
  for (int q = 0; q < 49; ++q) {
    combined[q] = copy[0][q] + copy[1][q] + copy[2][q];
    bool a = copy[0][q] > 0, b = copy[1][q] > 0, c = copy[2][q] > 0;
    if (q < 48 && a == b && b == c) ++unanimous;
  }
  if (unanimous < kMinUnanimous) return;

  std::uint8_t bits[48];
  for (int q = 0; q < 48; ++q) bits[q] = combined[q] > 0 ? 1 : 0;
  std::uint32_t a, b;
  deinterleaveWord(bits, a, b);
  float relA[24], relB[24];
  for (int i = 0; i < 24; ++i) {
    relA[23 - i] = std::fabs(combined[2 * i]);
    relB[23 - i] = std::fabs(combined[2 * i + 1]);
  }
  int dA, dB, eA, eB;
  if (!chaseGolay(a, relA, dA, eA) || !chaseGolay(b, relB, dB, eB)) return;

  Candidate c;
  c.w.word = (static_cast<std::uint32_t>(dA) << 12) | static_cast<std::uint32_t>(dB);
  c.w.time = time;
  c.w.unanimous = unanimous;
  c.w.corrected = eA + eB;
  c.w.offsetHz = kBankOffsetHz[bank];
  c.w.quality = std::clamp((unanimous - 12) / 36.0f - 0.05f * c.w.corrected, 0.0f, 1.0f);
  c.phase = phase;
  c.bank = bank;
  c.score = 4 * unanimous - 6 * c.w.corrected - (bank == 0 ? 0 : 2);
  c.valid = true;

  // At the boundary of an accepted word.
  bool synced = false;
  for (int k = 1; k <= 2; ++k)
    if (std::fabs(time - (lastCommit_ + k * kRedundantWord)) <= kSyncTolerance) synced = true;
  if (synced && unanimous >= kSyncedUnanimous && c.w.corrected <= kSyncedCorrections && plausible(c.w.word)) {
    accept(c);
    return;
  }
  if (!acquirable(c.w.word)) return;
  if (unanimous >= kWeakUnanimous && c.w.corrected <= kWeakCorrections) {
    // Paired with a weak word 392 ms earlier? The best of those is that word.
    Candidate* first = nullptr;
    for (Candidate& old : weak_)
      if (old.valid && std::fabs(time - old.w.time - kRedundantWord) <= kSyncTolerance &&
          (!first || old.score > first->score))
        first = &old;
    if (first) {
      Candidate earlier = *first;
      for (Candidate& old : weak_)
        if (old.valid && std::fabs(old.w.time - earlier.w.time) < kWindow) old.valid = false;
      if (poolSize_ > 0) commit();
      poolSize_ = 0;
      accept(earlier);
      commit();
      accept(c);
      return;
    }
  }
  if (unanimous >= kStrongUnanimous && c.w.corrected <= kStrongCorrections) {
    accept(c);
    return;
  }
  if (unanimous >= kWeakUnanimous && c.w.corrected <= kWeakCorrections) {
    weak_[weakPos_] = c;
    weakPos_ = (weakPos_ + 1) % 32;
  }
}

void Ale2gReceiver::accept(const Candidate& c) {
  if (c.w.time < lastCommit_ + kRedundantWord - 0.02) return;  // within a word already reported
  if (poolSize_ > 0 && c.w.time - poolStart_ > kWindow) commit();
  if (poolSize_ == 0) poolStart_ = c.w.time;
  if (poolSize_ < kPool) pool_[poolSize_++] = c;
}

void Ale2gReceiver::commit() {
  if (poolSize_ == 0) return;
  int best = 0;
  for (int i = 1; i < poolSize_; ++i)
    if (pool_[i].score > pool_[best].score) best = i;
  Word2g w = pool_[best].w;
  // The word's time: the middle of the candidates that agree with the best.
  double sum = 0.0;
  int n = 0;
  for (int i = 0; i < poolSize_; ++i)
    if (pool_[i].w.word == w.word && pool_[i].score >= pool_[best].score - 8) {
      sum += pool_[i].w.time;
      ++n;
    }
  w.time = sum / n;
  poolSize_ = 0;
  lastCommit_ = w.time;
  // Weak candidates within this word are not other words.
  for (Candidate& c : weak_)
    if (c.valid && std::fabs(c.w.time - lastCommit_) < kRedundantWord - 0.01) c.valid = false;
  if (listener_) listener_->onWord(w);
}

// ------------------------------------------------------------- assembler

namespace {

std::string chars(std::uint32_t w) {
  std::string s;
  for (int i = 0; i < 3; ++i) {
    char c = wordChar(w, i);
    s += (c >= 0x20 && c < 0x7f) ? c : '.';
  }
  return s;
}

std::string commandName(char c1, char c2) {
  switch (c1) {
    case '`': return "Advanced LQA";
    case 'b': return "DBM";
    case 'c': return "Channels";
    case 'd': return "DTM";
    case 'f': return "Frequency";
    case 'm':
      switch (c2) {
        case 'a': return "Analog port selection";
        case 'c': return "Crypto negotiation";
        case 'd': return "Data port selection";
        case 'n': return "Modem negotiation";
        case 'q': return "Digital squelch";
        default: return "Mode selection";
      }
    case 'p': return "Power control";
    case 'r': return "LQA report";
    case 't':
      switch (c2) {
        case 'a': return "Adjust slot width";
        case 'b': return "Station busy";
        case 'c': return "Channel busy";
        case 'd': return "Set dwell time";
        case 'h': return "Halt and wait";
        case 'l': return "Contact later";
        case 'm': return "Meet me";
        case 'n': return "Poll operator";
        case 'o': return "Request operator ACK";
        case 'p': return "Schedule periodic function";
        case 'q': return "Quiet contact";
        case 'r': return "Respond and wait";
        case 's': return "Set sounding interval";
        case 't': return "Tune and wait";
        case 'w': return "Set slot width";
        case 'x': return "Do not respond";
        case 'y': return "Year and date";
        case 'z': return "Zulu time";
        default: return "Scheduling";
      }
    case 'v': return c2 == 'c' ? "Capabilities" : c2 == 's' ? "Version" : "Capabilities/version";
    case 'x':
    case 'y':
    case 'z':
    case '{': return "CRC";
    case '|': return "User-unique function";
    case '~': return "Time exchange";
    default: return "";
  }
}

}  // namespace

std::string Ale2gAssembler::describeWord(std::uint32_t w) {
  return std::string(preambleName(wordPreamble(w))) + " " + chars(w);
}

void Ale2gAssembler::reset() {
  open_ = false;
  text_.clear();
  item_.clear();
  itemType_ = Item::None;
  itemPreamble_ = -1;
  section_ = Section::None;
  lastPreamble_ = -1;
  lastWasExtension_ = false;
  amd_ = false;
  concluded_ = false;
  lastWord_ = 0xffffffffu;
  lastTime_ = -1e9;
  qualitySum_ = 0;
  words_ = 0;
}

void Ale2gAssembler::startFrame() {
  open_ = true;
  text_.clear();
  item_.clear();
  itemType_ = Item::None;
  section_ = Section::None;
  lastPreamble_ = -1;
  lastWasExtension_ = false;
  concluded_ = false;
  qualitySum_ = 0;
  words_ = 0;
}

void Ale2gAssembler::closeItem() {
  switch (itemType_) {
    case Item::Address: {
      std::string a = item_;
      std::string label;
      if (a == "@?@") label = " (AllCall)";
      else if (a == "@@?") label = " (AnyCall)";
      else if (a == "@@@") label = " (null)";
      else if (a.size() == 3 && a[0] == '@' && a[2] == '@') label = " (selective AllCall)";
      else if (a.size() == 3 && a[0] == '@' && a[1] == '@') label = " (selective AnyCall)";
      else
        while (a.size() > 1 && a.back() == '@') a.pop_back();  // address stuffing
      text_ += a + label;
      break;
    }
    case Item::Amd: {
      std::string t = item_;
      while (!t.empty() && t.back() == ' ') t.pop_back();  // stuffing
      text_ += "AMD \"" + t + "\"";
      break;
    }
    case Item::Command:
      text_ += item_;
      break;
    case Item::None:
      break;
  }
  item_.clear();
  itemType_ = Item::None;
  itemBase_ = 0xffffffffu;
}

void Ale2gAssembler::startAddress(int p) {
  Section s = (p == kTo || p == kThru) ? Section::Call : p == kFrom ? Section::From : Section::Conclusion;
  const bool sameList = itemType_ == Item::Address && itemPreamble_ == p;
  closeItem();
  if (sameList) {
    text_ += ", ";
  } else {
    if (!text_.empty()) text_ += "  ";
    text_ += std::string(preambleName(p)) + " ";
  }
  section_ = s;
  itemType_ = Item::Address;
  itemPreamble_ = p;
  if (s == Section::Conclusion) concluded_ = true;
}

void Ale2gAssembler::addChars(std::uint32_t w, bool address) {
  for (int i = 0; i < 3; ++i) {
    char c = wordChar(w, i);
    if (address) item_ += isBasic38(c) ? c : '?';
    else if (itemType_ == Item::Amd) item_ += isExpanded64(c) ? c : '?';
  }
  if (itemType_ == Item::Command) {
    char buf[16];
    std::snprintf(buf, sizeof buf, " %06X", static_cast<unsigned>(w & 0x1fffff));
    item_ += buf;
  }
}

void Ale2gAssembler::startCommand(std::uint32_t w) {
  closeItem();
  if (!text_.empty()) text_ += "  ";
  section_ = Section::Message;
  char c1 = wordChar(w, 0), c2 = wordChar(w, 1), c3 = wordChar(w, 2);
  if (isExpanded64(c1) && isExpanded64(c2) && isExpanded64(c3)) {
    itemType_ = Item::Amd;
    item_ = std::string{c1, c2, c3};
    return;
  }
  itemType_ = Item::Command;
  char buf[96];
  if (c1 == 'a') {
    // LQA (table A-XIV): KA1 W11, MP W12-14, SINAD W15-19, BER W20-24.
    int ka = (w >> 13) & 1, mp = (w >> 10) & 7, sinad = (w >> 5) & 31, ber = w & 31;
    std::string s = sinad == 31 ? std::string("-") : std::to_string(sinad) + " dB";
    std::string b = ber == 31 ? std::string("-") : std::to_string(ber);
    std::snprintf(buf, sizeof buf, "CMD LQA: SINAD %s, BER %s, MP %d%s", s.c_str(), b.c_str(), mp,
                  ka ? ", LQA requested" : "");
    item_ = buf;
    return;
  }
  if (c1 == 'n') {
    int mx = (w >> 7) & 127, mean = w & 127;
    std::snprintf(buf, sizeof buf, "CMD Noise report: max %d dB, mean %d dB", mx, mean);
    item_ = buf;
    return;
  }
  std::string name = commandName(c1, c2);
  std::snprintf(buf, sizeof buf, "CMD %s [%06X]", name.empty() ? "?" : name.c_str(),
                static_cast<unsigned>(w & 0x1fffff));
  item_ = buf;
}

void Ale2gAssembler::endFrame() {
  if (!open_) return;
  closeItem();
  if (words_ > 0 && listener_) {
    Frame f;
    f.text = text_;
    if (concluded_ && text_.find("TO ") == std::string::npos && text_.find("THRU ") == std::string::npos &&
        text_.find("CMD") == std::string::npos && text_.find("AMD") == std::string::npos)
      f.text += "  (sounding)";
    if (!concluded_) f.text += "  (no conclusion)";
    f.time = lastTime_;
    f.quality = static_cast<float>(qualitySum_ / words_);
    f.words = words_;
    f.complete = concluded_;
    listener_->onFrame(f);
  }
  open_ = false;
  text_.clear();
}

void Ale2gAssembler::onWord(const Word2g& w) {
  if (w.word == lastWord_ && w.time - lastTime_ < 1.0) {
    lastTime_ = w.time;  // the same word again: redundant (scanning call)
    return;
  }
  if (open_ && w.time - lastTime_ > 1.2) endFrame();
  const int p = wordPreamble(w.word);
  const bool extension = p == kData || p == kRep;
  // Words lost since the last one (an extension then does not extend what it seems to).
  const bool gap = open_ && w.time - lastTime_ > 1.5 * Ale2gReceiver::kRedundantWord;
  if (open_ && concluded_ && !extension) endFrame();  // a new frame after a conclusion

  if (isAddressPreamble(p)) {
    if (open_ && itemType_ == Item::Address && w.word == itemBase_) {
      lastWord_ = w.word;  // the address's first word again (scanning call, after a lost word)
      lastTime_ = w.time;
      return;
    }
    if (!open_) startFrame();
    startAddress(p);
    addChars(w.word, true);
    itemBase_ = w.word;
    lastWasExtension_ = false;
  } else if (p == kCmd) {
    if (!open_) startFrame();
    startCommand(w.word);
    lastWasExtension_ = false;
  } else {
    if (!open_) {
      lastWord_ = w.word;
      lastTime_ = w.time;
      return;  // the rest of a frame we did not see start
    }
    if (gap) {
      if (itemType_ == Item::Address)
        while (!item_.empty() && item_.back() == '@') item_.pop_back();
      if (itemType_ == Item::Address || itemType_ == Item::Amd) item_ += "...";
      else if (itemType_ == Item::Command) item_ += " ...";
    }
    if (itemType_ == Item::Address) {
      // REP right after an address word: another address of the same list;
      // otherwise DATA and REP extend the address.
      if (p == kRep && !lastWasExtension_ && !gap && section_ == Section::Call) {
        startAddress(itemPreamble_);
        addChars(w.word, true);
        itemBase_ = w.word;
        lastWasExtension_ = false;
      } else {
        addChars(w.word, true);
        lastWasExtension_ = true;
      }
    } else if (itemType_ == Item::Amd || itemType_ == Item::Command) {
      addChars(w.word, false);
    }
  }
  lastPreamble_ = p;
  lastWord_ = w.word;
  lastTime_ = w.time;
  qualitySum_ += w.quality;
  ++words_;
}

void Ale2gAssembler::tick(double now) {
  if (open_ && now - lastTime_ > (concluded_ ? 0.9 : 1.2)) endFrame();
}

void Ale2gAssembler::flush() { endFrame(); }

}  // namespace navtex::ale
