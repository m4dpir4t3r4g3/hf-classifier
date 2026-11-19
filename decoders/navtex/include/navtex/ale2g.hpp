// 2G ALE receiver (MIL-STD-188-141B appendix A) on the 9600 Hz baseband of
// AleFrontEnd, and the frame assembler that turns its words into calls,
// soundings and messages.
//
// Receiver: eight tone detectors (8 ms matched filters) in three banks 50 Hz
// apart, so the signal may be mistuned by up to about +-100 Hz. Rather than
// recover a symbol clock, the detectors are sampled at eight timing phases
// (1 ms apart); each phase and bank keeps the last 49 symbols. After every
// symbol, the last 147 bits are tried as a redundant word ending there: the
// three copies are combined (soft majority vote), deinterleaved and Golay
// decoded (with Chase soft-decision decoding). A word is accepted:
//  - at a word boundary of a word already accepted (392 ms on), if it decodes;
//  - alone, if its three copies agree well, it needs at most two corrections,
//    and it is an address word (TO, TIS, TWAS, THRU, FROM in the basic 38
//    characters) — every ALE frame has some;
//  - with a weaker but plausible word 392 ms before or after it.
// Candidates from neighbouring phases and banks are the same word: the best
// is kept.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "navtex/ale_codes.hpp"
#include "navtex/ale_frontend.hpp"
#include "navtex/dsp.hpp"

namespace navtex::ale {

struct Word2g {
  std::uint32_t word = 0;
  double time = 0;      // end of the redundant word
  int unanimous = 0;    // bits (of 48) whose three copies agreed
  int corrected = 0;    // Golay corrections, both halves
  double offsetHz = 0;  // tone offset of the detector bank
  float quality = 0;    // 0..1
};

class Ale2gReceiver {
 public:
  class Listener {
   public:
    virtual ~Listener() = default;
    virtual void onWord(const Word2g& w) = 0;
  };

  Ale2gReceiver();
  void reset();
  void setListener(Listener* l) { listener_ = l; }
  // One baseband sample, at stream time `time`.
  void process(cf x, double time);
  double lastWordTime() const { return lastCommit_; }

  static constexpr int kPhases = 8;
  static constexpr int kBanks = 3;
  static constexpr double kRedundantWord = 0.392;  // 49 symbols

 private:
  struct Symbol {
    float llr[3];  // MSB first; > 0: bit 1
  };
  struct History {
    Symbol sym[49];
    int head = 0;    // next write position
    int count = 0;
  };
  struct Candidate {
    Word2g w;
    int phase = 0, bank = 0;
    int score = 0;
    bool valid = false;
  };

  void symbol(int phase, int bank, const float energy[8], double time);
  void evaluate(int phase, int bank, double time);
  void accept(const Candidate& c);
  void commit();
  static constexpr int kPool = 64;

  Listener* listener_ = nullptr;
  SlidingSum sums_[kBanks][8];
  std::complex<double> last_[kBanks][8];
  std::complex<double> phasor_[kBanks][8], rot_[kBanks][8];
  long long sample_ = 0;
  double nextTick_ = 0.0;
  long long tick_ = 0;
  int renorm_ = 0;
  History hist_[kPhases][kBanks];

  // Candidates for one word (all phases and banks within kWindow).
  Candidate pool_[kPool];
  int poolSize_ = 0;
  double poolStart_ = 0.0;
  double lastCommit_ = -1e9;
  std::uint32_t lastWord_ = 0;
  Candidate weak_[32];
  int weakPos_ = 0;
};

// Builds frames from words: the calling cycle (TO / THRU addresses), the
// message section (CMD: AMD text, LQA and other orderwire commands) and the
// conclusion (TIS / TWAS address), extended addresses (DATA, REP) and
// address stuffing ('@') resolved, and repeated words (scanning calls)
// collapsed. A frame ends when another one starts, or 1.2 s after its last
// word.
class Ale2gAssembler {
 public:
  struct Frame {
    std::string text;
    double time = 0;
    float quality = 0;
    int words = 0;
    bool complete = false;  // ended with a conclusion (TIS / TWAS)
  };
  class Listener {
   public:
    virtual ~Listener() = default;
    virtual void onFrame(const Frame& f) = 0;
  };

  void setListener(Listener* l) { listener_ = l; }
  void reset();
  void onWord(const Word2g& w);
  // Ends a frame left open for too long.
  void tick(double now);
  void flush();

  // "TO ABC" for one word, as shown with AleConfig::showWords.
  static std::string describeWord(std::uint32_t word);

 private:
  enum class Section { None, Call, From, Message, Conclusion };
  void startFrame();
  void endFrame();
  void startAddress(int preamble);
  void addChars(std::uint32_t word, bool address);
  void closeItem();
  void startCommand(std::uint32_t word);
  enum class Item { None, Address, Amd, Command };
  Item itemType_ = Item::None;

  Listener* listener_ = nullptr;
  bool open_ = false;
  std::string text_;
  std::string item_;       // address or message being assembled
  int itemPreamble_ = -1;  // the preamble that started it
  Section section_ = Section::None;
  int lastPreamble_ = -1;
  bool lastWasExtension_ = false;
  bool amd_ = false;       // the message is AMD text
  bool concluded_ = false;
  std::uint32_t lastWord_ = 0xffffffffu;
  std::uint32_t itemBase_ = 0xffffffffu;  // the word that started the current address
  double lastTime_ = -1e9;
  double qualitySum_ = 0;
  int words_ = 0;
};

}  // namespace navtex::ale
