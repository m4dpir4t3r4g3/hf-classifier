#include "hfc/decoder_host.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <complex>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "PskDecoder.h"
#include "hfc/akula.hpp"
#include "hfc/catalogue.hpp"
#include "hfc/constant_weight.hpp"
#include "hfc/drm.hpp"
#include "hfc/dsc.hpp"
#include "hfc/fax.hpp"
#include "hfc/hfdl.hpp"
#include "hfc/sstv.hpp"
#include "hfc/timesig.hpp"
#include "hfc/wspr.hpp"
#include "hfc/family_model.hpp"
#include "hfc/ftx_decoder.hpp"
#include "morse/decoder.hpp"
#include "navtex/ale.hpp"
#include "navtex/decoder.hpp"
#include "navtex/fsk_decoder.hpp"

namespace hfc {

namespace {

constexpr std::size_t kMaxHeldText = 512;  // text kept before a decoder passes its gate
constexpr std::size_t kChunk = 64;         // published text: a line, or this many characters
constexpr double kPendingSeconds = 3.0;    // ... or what has waited this long

// What a decoder's text must look like before it is believed. Measured over
// the last kWindow characters (spaces and line ends not counted).
struct Gate {
  int minChars = 10;
  double maxBad = 0.2;     // fraction unrecoverable ('*') or unprintable
  double minConf = 0.0;    // mean confidence (decoders that give one)
  int minDistinct = 4;     // different characters: keeps out "EEEEEEEE"
  // Morse: most characters of one or two elements (E I S H T 5) is what
  // a tone keyed by noise or another mode decodes as, not text.
  double maxShort = 1.0;
  // At most this fraction one character: keeps out "IIIIIIIINIII" (a SITOR
  // alphabet signal of another mode, POL-ARQ, through the SITOR-B decoder);
  // NAVTEX's "RYRYRY" phasing is half R. (Last, so that the candidates'
  // positional initialisers keep their meaning.)
  double maxSame = 0.6;
};
constexpr std::size_t kWindow = 32;
// Morse passing on a keyed-tone measurement (Candidate::keyedEvidence).
constexpr double kKeyedMinConf = 0.65;
constexpr double kKeyedMaxShort = 0.8;
constexpr double kKeyedMaxSame = 0.7;
// A session starts once the detector has confirmed its emission, about 0.4 s
// (three 128 ms frames) after it began: its decoders are also given this much
// of the channel from before (Channelizer::replay), so that a burst's
// preamble (3G/4G ALE: its first 120 to 240 ms) and the first characters of
// a transmission are not lost.
constexpr double kPreRollSeconds = 0.75;
// An undecided session keeps its place this long before a waiting emission
// may take it (evictUndecided).
constexpr double kMinTrySeconds = 20.0;

bool printable(char c) { return c == '\n' || (c >= 32 && c <= 126); }

// A callsign: an optional "XX/" before, a prefix (one or two letters, or a
// digit and a letter, or a letter and a digit), a digit, one to four
// letters, an optional "/X" after. "DL1ABC", "W1AW", "2E0ABC", "OH2XYZ/P".
bool isCallsign(std::string t, std::size_t* suffixLetters = nullptr) {
  const std::size_t slash = t.find('/');
  if (slash != std::string::npos) {
    // Keep the longer side.
    const std::string a = t.substr(0, slash), b = t.substr(slash + 1);
    t = a.size() >= b.size() ? a : b;
  }
  if (t.size() < 4 || t.size() > 8) return false;
  auto alpha = [](char c) { return c >= 'A' && c <= 'Z'; };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  std::size_t i = 0;
  if (alpha(t[0]) && alpha(t[1])) i = 2;
  else if ((digit(t[0]) && alpha(t[1])) || (alpha(t[0]) && digit(t[1]) && t.size() > 4 && digit(t[2]))) i = 2;
  else if (alpha(t[0])) i = 1;
  else return false;
  if (i >= t.size() || !digit(t[i])) return false;
  ++i;
  if (i < t.size() && digit(t[i])) ++i;  // two-digit areas, e.g. "VK10ABC"? rare but valid
  const std::size_t letters = t.size() - i;
  if (letters < 1 || letters > 4) return false;
  for (; i < t.size(); ++i)
    if (!alpha(t[i])) return false;
  if (suffixLetters) *suffixLetters = letters;
  return true;
}

// Words, abbreviations and Q-codes of radio traffic, and common English,
// German and weather words. Upper case.
const std::set<std::string>& vocabulary() {
  static const std::set<std::string> v = {
      // amateur traffic
      "CQ", "DE", "QRZ", "QSL", "QTH", "QSO", "QRM", "QRN", "QSB", "QSY", "QRT", "QRP", "QRO", "QRV", "QRL", "QRX",
      "RST", "TNX", "TKS", "THX", "PSE", "ANT", "RIG", "PWR", "NAME", "TEST", "CONTEST", "5NN", "RPT", "SRI", "ABT",
      "AGN", "CUL", "BCNU", "COPY", "OVER", "INFO", "CALL", "FER", "WID", "HPE", "GUD", "VY", "DX", "UR", "TU", "ES",
      "OM", "GM", "GA", "GE", "GN", "HW", "HR", "OP", "BK", "KN", "SK", "AR", "FB", "YL", "WX", "OK", "HI", "NR", "73",
      "88", "PSK", "PSK31", "PSK63", "RTTY", "SSB", "FT8", "BPSK", "RYRY", "RYRYRY", "QRQ", "QRS", "EL", "ZL",
      // English
      "THE", "AND", "FOR", "YOU", "ARE", "WITH", "THIS", "THAT", "HAVE", "FROM", "HERE", "THERE", "WHAT", "WILL",
      "YOUR", "OUR", "NOT", "BUT", "ALL", "CAN", "WAS", "ONE", "HAS", "HOW", "NOW", "GOOD", "THANKS", "THANK",
      "HELLO", "MORNING", "EVENING", "NIGHT", "DAY", "TODAY", "NICE", "BEST", "REGARDS", "FINE", "VERY", "MUCH",
      "SIGNAL", "STRONG", "WEAK", "BAND", "RADIO", "STATION", "QUICK", "BROWN", "FOX", "JUMPS", "LAZY", "DOG",
      "TESTING", "WORKING", "RUNNING", "WATTS", "INTO", "ABOUT", "BACK", "SEE", "AGAIN", "BYE", "SOON",
      // weather and maritime
      "WEATHER", "REPORT", "FORECAST", "WIND", "WINDS", "SEA", "STATE", "GALE", "WARNING", "STORM", "NORTH", "SOUTH",
      "EAST", "WEST", "NORTHERLY", "SOUTHERLY", "EASTERLY", "WESTERLY", "VISIBILITY", "MODERATE", "POOR", "RAIN",
      "SHOWERS", "FOG", "CLOUD", "PRESSURE", "HIGH", "LOW", "SECURITE", "MAYDAY", "NAVTEX", "UTC", "GMT", "KNOTS",
      "BIGHT", "NUMBER", "AREA", "COAST", "ISSUED", "VALID", "UNTIL", "OCCASIONALLY", "LATER", "BECOMING",
      // German
      "UND", "DER", "DIE", "DAS", "WETTER", "BERICHT", "WETTERBERICHT", "NORD", "SUED", "OST", "SEE", "VORHERSAGE"};
  return v;
}

// Does decoded text read like something people send? Judged on the last 16
// whole words, once there are at least 6: a word counts when it is a
// callsign, a word of vocabulary() or a signal report ("599", "579").
// Plausible when at least 30 % of the words count and two different strong
// ones are among them (callsigns, "CQ", vocabulary words of three letters
// or more), or a callsign and "DE"; or when most words are groups of five
// characters (coded traffic).
//
// Without this, decoders fed signals of other modes (random-data FSK, one
// tone of an MFSK signal, BPSK carrying no Varicode) print letters that
// pass every per-character statistic: the program would label unknown
// military modems "CW" or "PSK63".
bool plausibleText(const std::string& text) {
  std::vector<std::string> words;
  std::string cur;
  for (char ch : text) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '/') {
      cur += static_cast<char>(std::toupper(u));
    } else {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
    }
  }
  // `cur` (the word being received) is left out.
  if (words.size() > 16) words.erase(words.begin(), words.end() - 16);
  if (words.size() < 6) return false;
  int groups5 = 0;
  for (const auto& w : words) groups5 += w.size() == 5 ? 1 : 0;
  const bool codedTraffic = words.size() >= 6 && groups5 * 10 >= static_cast<int>(words.size()) * 7;
  std::set<std::string> strong;
  bool callsign = false, de = false;
  int counted = 0, garbage = 0;
  std::map<std::string, int> seen;
  for (const auto& w : words) ++seen[w];
  for (const auto& w : words) {
    bool known = false;
    std::size_t suffix = 0;
    if (isCallsign(w, &suffix)) {
      known = callsign = true;
      // Strong with the usual two or three letters after the digit, or when
      // repeated: one or four letters ("ET1N", "H5HEYN") is what noise keyed
      // as Morse makes, while a contest station ("HA8A") sends its call twice.
      if (suffix == 2 || suffix == 3 || seen[w] >= 2) strong.insert(w);
    } else if (vocabulary().count(w)) {
      known = true;
      if (w.size() >= 3 || w == "CQ") strong.insert(w);
      if (w == "DE") de = true;
    } else if (w.size() == 3 && w[0] >= '1' && w[0] <= '5' && w[1] >= '1' && w[1] <= '9' &&
               (w[2] == '9' || w[2] == 'N')) {
      known = true;  // RST report
    }
    if (known) {
      ++counted;
    } else {
      // Letters and digits mixed in a word that is no callsign ("H5HEYN",
      // "NO0EO9"), or a run of letters no language has ("ENTTLFZ0ND8TOTNOH").
      bool letters = false, digits = false;
      for (char ch : w) (std::isdigit(static_cast<unsigned char>(ch)) ? digits : letters) = true;
      if ((letters && digits && w.size() >= 4) || w.size() > 12) ++garbage;
    }
  }
  if (codedTraffic) return true;  // groups of five may mix letters and digits
  if (garbage * 3 >= static_cast<int>(words.size())) return false;
  return (strong.size() >= 2 || (callsign && de)) && counted * 10 >= static_cast<int>(words.size()) * 3;
}

// Morse whose words need not be known ones, for an emission measured as a
// keyed tone (OOK): utility and net traffic is short codes, numbers and
// abbreviations ("RPT K", "R 853 K" on a user's recording), which
// plausibleText() refuses, while other modes' signals (FSK, MFSK, PSK) are
// not measured as OOK. Still at least 6 words, short ones as people's are,
// none what noise or random keying makes of Morse (long runs, letters and
// digits mixed, odd characters).
bool keyedText(const std::string& text) {
  std::vector<std::string> words;
  std::string cur;
  for (char ch : text) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (std::isalnum(u) || ch == '/') cur += static_cast<char>(std::toupper(u));
    else if (!cur.empty()) words.push_back(cur), cur.clear();
  }
  if (words.size() > 16) words.erase(words.begin(), words.end() - 16);
  if (words.size() < 6) return false;
  // Random keying (an OOK data signal) seldom pauses for 7 units, so what
  // a Morse decoder makes of it runs into long "words" ("TTWAEEEX",
  // "5TMCNL5EE63SEHEPEV5"); people's words are short.
  int garbage = 0, longer = 0, long7 = 0;
  std::size_t letters = 0;
  for (const auto& w : words) {
    bool alpha = false, digits = false;
    for (char ch : w) (std::isdigit(static_cast<unsigned char>(ch)) ? digits : alpha) = true;
    if ((alpha && digits && w.size() >= 5 && !isCallsign(w)) || w.size() > 10) ++garbage;
    if (w.size() >= 2) ++longer;
    if (w.size() >= 7) ++long7;
    letters += w.size();
  }
  // Characters other than letters, digits, '/' and spaces in the same
  // stretch of text (the decoder's '*' for what it could not read, and the
  // punctuation random timing makes).
  int odd = 0;
  {
    std::size_t seen = 0;
    for (auto it = text.rbegin(); it != text.rend() && seen < letters + words.size() + 8; ++it, ++seen) {
      const unsigned char u = static_cast<unsigned char>(*it);
      if (!std::isalnum(u) && *it != '/' && *it != ' ' && *it != '\n') ++odd;
    }
  }
  return garbage == 0 && long7 * 6 <= static_cast<int>(words.size()) &&
         letters <= 5 * words.size() && odd <= 1 && longer >= 2;
}

}  // namespace

bool looksLikeTraffic(const std::string& text) { return plausibleText(text); }
bool looksLikeKeyedText(const std::string& text) { return keyedText(text); }

// --- Candidates -------------------------------------------------------------

// One decoder trying an emission. Character decoders feed addChar(); the
// gate decides when the text is believed. Line decoders (ALE, whose frames
// are checked by Golay codes or a CRC) feed addLine(), which is believed at
// once.
struct Candidate {
  std::string name;
  int view = 0;  // which audio view of the session it listens to
  Gate gate;
  bool needPlausibleText = false;  // no error detection of its own (CW, PSK, RTTY)
  // The emission is measured as a keyed tone: Morse may pass on its
  // decoder's confidence without known words (check(), keyedText()).
  bool keyedEvidence = false;
  bool passed = false;
  double quality = 0;
  std::string text;  // held before the gate, then what has not been published
  // Pictures the lines holding "[picture]" are about, in their order.
  std::deque<std::shared_ptr<const FaxImage>> images;
  double pendingSince = -1;

  struct Mark {
    char c;
    bool bad;
    float conf;
  };
  std::deque<Mark> window;

  virtual ~Candidate() = default;
  virtual void process(const float* audio, std::size_t n) = 0;
  virtual void flush() = 0;
  // Whether the keying it hears has the timing of Morse (CW only).
  virtual bool keyingLooksLikeMorse() const { return false; }

  void addChar(const char* s, float conf, bool bad) {
    if (!s[0]) return;
    for (const char* p = s; *p; ++p)
      if (*p != '\r') text.push_back(printable(*p) ? *p : '*');
    if (!passed && text.size() > kMaxHeldText) text.erase(0, text.size() - kMaxHeldText);
    if (s[0] == ' ' || s[0] == '\n' || s[0] == '\r') return;
    window.push_back({s[0], bad || !printable(s[0]) || s[0] == '*', conf});
    if (window.size() > kWindow) window.pop_front();
    if (!passed) check();
  }
  void addLine(const std::string& line, double q) {
    text += line;
    text += '\n';
    quality = q;
    passed = true;
  }
  void check() {
    if (static_cast<int>(window.size()) < gate.minChars) return;
    int bad = 0;
    double conf = 0;
    int shortChars = 0;
    std::set<char> distinct;
    std::map<char, int> count;
    for (const Mark& m : window) {
      bad += m.bad ? 1 : 0;
      conf += m.conf;
      if (!m.bad) distinct.insert(m.c), ++count[m.c];
      if (std::strchr("EISHT5", m.c)) ++shortChars;
    }
    const double n = static_cast<double>(window.size());
    conf /= n;
    int most = 0;
    for (const auto& [c, k] : count) most = std::max(most, k);
    const bool usual = bad / n <= gate.maxBad && conf >= gate.minConf &&
                       static_cast<int>(distinct.size()) >= gate.minDistinct && shortChars / n <= gate.maxShort &&
                       most <= gate.maxSame * n && (!needPlausibleText || plausibleText(text));
    // An emission measured as a keyed tone: real Morse traffic is often
    // short codes and many single E's or T's (a user's recording: "E RPTK E E
    // E E E TEJ R 853K", short characters 0.64, E half of them), which the
    // usual limits take for noise keyed as Morse; with that measurement and
    // a confident decoder they are wider, and no known words are needed.
    const bool keyed = keyedEvidence && keyingLooksLikeMorse() && bad / n <= gate.maxBad && conf >= kKeyedMinConf &&
                       static_cast<int>(distinct.size()) >= gate.minDistinct && shortChars / n <= kKeyedMaxShort &&
                       most <= kKeyedMaxSame * n && keyedText(text);
    if (usual || keyed) {
      passed = true;
      quality = gate.minConf > 0 ? conf : 1.0 - bad / n;
    }
  }
};

namespace {

// The key-down times of a keyed tone, measured apart from the Morse decoder:
// the envelope at the tone in 5 ms steps, a threshold halfway between the
// noise and the keyed level (in dB, over the last 20 s, with hysteresis),
// runs of 25 ms and longer. Morse has two key-down lengths, dashes about three times the dots,
// and next to nothing between them; random on-off keying at a fixed rate
// (a data signal) has runs of 1, 2, 3, 4... units, a quarter of them where
// Morse has none. Its letters pass a Morse decoder's own checks.
class KeyingTiming {
 public:
  KeyingTiming(double rate = 12000.0, double toneHz = 1500.0)
      : per_(std::max<std::size_t>(1, static_cast<std::size_t>(rate * 0.005))),
        turn_(std::polar(1.0, -2.0 * kPi * toneHz / rate)) {}
  void process(const float* a, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      acc_ += static_cast<double>(a[i]) * rot_;
      rot_ *= turn_;
      if (++count_ < per_) continue;
      rot_ /= std::abs(rot_);
      block(20.0 * std::log10(std::abs(acc_) / static_cast<double>(per_) + 1e-12));
      acc_ = 0;
      count_ = 0;
    }
  }
  bool morse() const {
    if (runs_.size() < 15) return false;
    std::vector<double> d(runs_.begin(), runs_.end());
    std::sort(d.begin(), d.end());
    // The unit: dots are the shortest common runs. A first guess from the
    // lower third, then the median of the runs around it.
    const double u0 = d[d.size() * 3 / 10];
    std::vector<double> near;
    for (double x : d)
      if (x >= 0.6 * u0 && x <= 1.5 * u0) near.push_back(x);
    if (near.size() < 3) return false;
    const double u = near[near.size() / 2];
    // Shares near 1 unit (dots), 2 units (Morse has none), 3 units
    // (dashes), and 5 or more (Morse has none; random keying 1 in 16).
    int one = 0, two = 0, three = 0, many = 0;
    for (double x : d) {
      if (x < 0.6 * u) continue;  // left from the unit's guess: counted as nothing
      if (x <= 1.5 * u) ++one;
      else if (x < 2.4 * u) ++two;
      else if (x <= 4.2 * u) ++three;
      else ++many;
    }
    const double n = static_cast<double>(d.size());
    const bool ok = two <= 0.15 * n && many <= 0.03 * n && three >= 0.08 * n && one >= 0.2 * n;
    return ok;
  }

 private:
  void block(double db) {
    levels_.push_back(db);
    if (levels_.size() > 4000) levels_.pop_front();  // 20 s
    if (++sinceThreshold_ >= 200 || threshold_ < -500) {  // once a second
      sinceThreshold_ = 0;
      std::vector<double> v(levels_.begin(), levels_.end());
      if (v.size() >= 100) {
        const auto at = [&](double q) {
          std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(q * (v.size() - 1)), v.end());
          return v[static_cast<std::size_t>(q * (v.size() - 1))];
        };
        const double lo = at(0.2), hi = at(0.95);
        threshold_ = hi - lo > 8.0 ? 0.5 * (lo + hi) : -1000.0;  // no keying to speak of: none
      }
    }
    // Hysteresis of 1.5 dB either side: a level wavering at the threshold
    // (fading, noise) must not cut a dash into pieces or make blips.
    const bool on = threshold_ > -500 && (onBlocks_ > 0 ? db > threshold_ - 1.5 : db > threshold_ + 1.5);
    if (on) {
      ++onBlocks_;
    } else {
      if (onBlocks_ >= 5) {  // 25 ms and longer (dots up to about 45 WPM)
        runs_.push_back(5.0 * onBlocks_);
        if (runs_.size() > 80) runs_.pop_front();
      }
      onBlocks_ = 0;
    }
  }

  std::size_t per_, count_ = 0;
  std::complex<double> turn_, rot_{1.0, 0.0}, acc_{0.0, 0.0};
  std::deque<double> levels_, runs_;
  double threshold_ = -1000.0;
  int sinceThreshold_ = 0, onBlocks_ = 0;
};

class CwCandidate : public Candidate, morse::DecoderSink {
 public:
  CwCandidate(double rate, double audioCentre, double spanHz) : timing_(rate, audioCentre) {
    name = "CW";
    gate = {8, 0.25, 0.6, 5, 0.5};
    needPlausibleText = true;
    morse::DecoderConfig c;
    c.sampleRate = rate;
    c.minToneHz = audioCentre - spanHz;
    c.maxToneHz = audioCentre + spanHz;
    c.fixedToneHz = audioCentre;
    dec_.configure(c);
    dec_.setSink(this);
  }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    timing_.process(a, n);
  }
  void flush() override { dec_.flush(); }
  void onCharacter(const morse::DecodedChar& c) override {
    addChar(c.text, c.confidence, std::strcmp(c.text, "*") == 0);
  }
  bool keyingLooksLikeMorse() const override { return timing_.morse(); }

 private:
  morse::MorseDecoder dec_;
  KeyingTiming timing_;
};

class PskCandidate : public Candidate {
 public:
  PskCandidate(double rate, psk::Mode mode, double audioCentre, const std::string& label) {
    name = label;
    // The decoder's quality (consistency of the phase changes) must be high
    // as well: BPSK carrying no Varicode still opens its squelch. QPSK's
    // quality (the fourth power of the phase change) reads much lower on a
    // weak signal its convolutional code still decodes cleanly (0.2-0.4 at
    // 3 kHz SNRs where BPSK of the same rate reads 0.8), so its squelch and
    // gate are lower; the text must still read like radio traffic.
    const bool qpsk = psk::IsQpsk(mode);
    gate = {8, 0.1, qpsk ? 0.25 : 0.5, 4, 1.0};
    needPlausibleText = true;
    dec_.Configure(rate, mode);
    dec_.Tune(audioCentre);
    dec_.SetAfc(true);
    if (qpsk) dec_.SetSquelch(0.25f);
    dec_.SetCharCallback([this](char c) {
      const char s[2] = {c, 0};
      addChar(s, dec_.Quality(), false);
    });
  }
  void process(const float* a, std::size_t n) override { dec_.Process(a, n, 1); }
  // QPSK: the Viterbi decoder still holds the last 32 symbols' bits.
  void flush() override { dec_.Flush(); }

 private:
  psk::Decoder dec_;
};

// SITOR-B with the NAVTEX message layer: named NAVTEX once a message
// header ("ZCZC B1B2B3B4") has been received.
class SitorCandidate : public Candidate, navtex::DecoderSink {
 public:
  SitorCandidate(double rate, double audioCentre) {
    name = "SITOR-B";
    gate = {10, 0.15, 0.0, 4, 1.0};
    navtex::DecoderConfig c;
    c.sampleRate = rate;
    c.minCentreHz = audioCentre - 250;
    c.maxCentreHz = audioCentre + 250;
    c.fixedCentreHz = audioCentre;
    dec_.configure(c);
    dec_.setSink(this);
  }
  void process(const float* a, std::size_t n) override { dec_.process(a, n); }
  void flush() override { dec_.flush(); }
  void onCharacter(const navtex::DecodedChar& c) override {
    if (c.inMessage) name = "NAVTEX";
    addChar(c.text, c.confidence, std::strcmp(c.text, "*") == 0);
  }

 private:
  navtex::NavtexDecoder dec_;
};

class RttyCandidate : public Candidate, navtex::DecoderSink {
 public:
  RttyCandidate(double rate, double baud, double shift, double audioCentre, const char* label) {
    name = label;
    gate = {10, 0.2, 0.0, 4, 1.0};
    needPlausibleText = true;
    navtex::FskConfig c;
    c.mode = navtex::FskMode::Rtty;
    c.sampleRate = rate;
    c.baud = baud;
    c.shiftHz = shift;
    c.minCentreHz = audioCentre - shift / 2 - 150;
    c.maxCentreHz = audioCentre + shift / 2 + 150;
    c.fixedCentreHz = audioCentre;
    dec_.configure(c);
    dec_.setSink(this);
  }
  void process(const float* a, std::size_t n) override { dec_.process(a, n); }
  void flush() override { dec_.flush(); }
  void onCharacter(const navtex::DecodedChar& c) override {
    addChar(c.text, c.confidence, std::strcmp(c.text, "*") == 0);
  }

 private:
  navtex::FskDecoder dec_;
};

class AleCandidate : public Candidate, navtex::AleSink {
 public:
  AleCandidate(double rate, bool g2, int viewIndex) {
    name = g2 ? "ALE 2G" : "ALE 3G";
    view = viewIndex;
    navtex::AleConfig c;
    c.sampleRate = rate;
    c.enable2g = g2;
    c.enable3g = !g2;
    c.enable4g = !g2;
    // 3G/4G: a burst whose preamble (a known sequence) was found but whose
    // data failed its CRC still identifies the waveform. The decoder tells
    // an intact burst (every received coded bit on the code word decoded:
    // encrypted by linking protection, or not made to the standard; so far
    // every real 3G/4G sample) from one too weak or damaged.
    c.showFailed = !g2;
    dec_.configure(c);
    dec_.setAleSink(this);
  }
  void process(const float* a, std::size_t n) override { dec_.process(a, n); }
  void flush() override { dec_.flush(); }
  void onAle(const navtex::AleEvent& e) override {
    if (e.kind != navtex::AleEvent::Frame2g && e.kind != navtex::AleEvent::Pdu && e.kind != navtex::AleEvent::Failed)
      return;
    name = e.generation == navtex::AleGeneration::G2 ? "ALE 2G"
           : e.generation == navtex::AleGeneration::G3 ? "ALE 3G"
                                                        : "ALE 4G";
    if (e.kind == navtex::AleEvent::Failed) {
      std::string line = std::string(e.waveform) +
                         (e.intact ? " burst received intact, data encrypted (linking protection) or not to the standard"
                                   : " burst, data not decoded (too weak or damaged)");
      addLine(line, 0.5 * e.quality);
    } else {
      addLine(e.text, e.quality);
    }
  }

 private:
  navtex::AleDecoder dec_;
};

// GMDSS digital selective calling: calls are reported only with their
// error-check character right, so each is believed at once.
class DscCandidate : public Candidate {
 public:
  DscCandidate(double rate, double audioCentre) : dec_(rate, audioCentre) { name = "DSC"; }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    for (const auto& call : dec_.take()) addLine(call, 1.0);
  }
  void flush() override {}

 private:
  DscDecoder dec_;
};

// HFDL: every report is a PDU whose frame check is right. Listens to the
// view with the middle of the emission at 1800 Hz, where the subcarrier is.
class HfdlCandidate : public Candidate {
 public:
  explicit HfdlCandidate(double rate) : dec_(rate, 1800.0) {
    name = "HFDL";
    view = 1;
  }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    for (const auto& pdu : dec_.take()) addLine(pdu, 1.0);
  }
  void flush() override {}

 private:
  HfdlDecoder dec_;
};

// Radiofax: named once the line rate is found (FaxDecoder), then every
// picture as it completes, and what there is of one when the emission ends.
class FaxCandidate : public Candidate {
 public:
  // In a session of ALE's width the signal's middle is at 1800 Hz in the
  // second view.
  FaxCandidate(double rate, double audioCentre, bool wide) : dec_(rate, wide ? 1800.0 : audioCentre) {
    name = "FAX";
    view = wide ? 1 : 0;
  }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    if (dec_.recognised() && !named_) {
      named_ = true;
      char b[96];
      std::snprintf(b, sizeof b, "radiofax, %d lines a minute: receiving the picture", dec_.linesPerMinute());
      addLine(b, 1.0);
    }
    for (const auto& im : dec_.take()) picture(im);
  }
  void flush() override {
    if (auto im = dec_.finish()) picture(im);
  }

 private:
  void picture(const std::shared_ptr<FaxImage>& im) {
    char b[160];
    std::snprintf(b, sizeof b, "[picture] %d lines at %d a minute, IOC %d%s", im->height, im->linesPerMinute, im->ioc,
                  im->phased ? "" : " (no phasing lines received: the picture is cut where reception began)");
    images.push_back(im);
    addLine(b, 1.0);
  }
  FaxDecoder dec_;
  bool named_ = false;
};

// SSTV: named when a picture's header (its parity right) or its train of
// sync pulses is found; then the picture.
class SstvCandidate : public Candidate {
 public:
  // (As FaxCandidate: in a session of ALE's width the signal's middle is
  // at 1800 Hz in the second view. SSTV's middle, between 1200 and 2300 Hz,
  // is 1750 Hz.)
  SstvCandidate(double rate, double audioCentre, bool wide) : dec_(rate, (wide ? 1800.0 : audioCentre) - 1750.0) {
    name = "SSTV";
    view = wide ? 1 : 0;
  }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    const std::string mode = dec_.mode();
    if (!mode.empty() && mode != named_) {
      named_ = mode;
      addLine("SSTV " + mode + (dec_.fromHeader() ? " (from its header)" : " (from its sync pulses: reception began inside the picture)") +
                  ": receiving the picture",
              1.0);
    }
    for (const auto& im : dec_.take()) picture(im);
  }
  void flush() override {
    if (auto im = dec_.finish()) picture(im);
  }

 private:
  void picture(const std::shared_ptr<FaxImage>& im) {
    char b[160];
    std::snprintf(b, sizeof b, "[picture] %s, %d x %d", im->what.c_str(), im->width, im->height);
    images.push_back(im);
    addLine(b, 1.0);
    named_.clear();
  }
  SstvDecoder dec_;
  std::string named_;
};

// Time signal stations: named once their seconds are found; then every
// minute of a time code (DCF77, MSF) whose checks all pass.
class TimeCandidate : public Candidate {
 public:
  TimeCandidate(double rate, double audioCentre, bool wide) : dec_(rate, wide ? 1800.0 : audioCentre) {
    name = "TIME";
    view = wide ? 1 : 0;
  }
  void process(const float* a, std::size_t n) override {
    dec_.process(a, n);
    for (const auto& line : dec_.take()) addLine(line, 1.0);
  }
  void flush() override {}

 private:
  TimeSignalDecoder dec_;
};

// DRM: recognised by its OFDM guard interval (DrmDetector), not decoded.
class DrmCandidate : public Candidate {
 public:
  DrmCandidate(double rate, double audioCentre, bool aleViews, double widthHz)
      : det_(rate, aleViews ? 1800.0 : audioCentre), width_(widthHz) {
    // Narrower than broadcasting: the timing is DRM's, the system may not be.
    name = widthHz >= 3200 ? "DRM" : "DRM-type OFDM";
    view = aleViews ? 1 : 0;
  }
  void process(const float* a, std::size_t n) override {
    det_.process(a, n);
    if (det_.mode() && !named_) {
      named_ = true;
      const std::string mode(1, det_.mode());
      if (width_ >= 3200)
        addLine("DRM robustness mode " + mode + ": OFDM digital broadcasting; the programme is not decoded", 1.0);
      else
        addLine("OFDM with the timing of DRM's robustness mode " + mode + ", in about " +
                    std::to_string(static_cast<int>(width_ / 100) * 100) +
                    " Hz: the amateur WinDRM and EasyPal (digital pictures and files) use it, other systems may too; content not decoded",
                1.0);
    }
  }
  void flush() override {}

 private:
  DrmDetector det_;
  double width_;
  bool named_ = false;
};

// A 7-bit constant-weight code (CIS-36-50 / CIS-50-50, the SITOR family of
// ARQ systems): recognised by the weight of its words, at a measured rate
// and shift. It names the code, not the system.
class ConstantWeightCandidate : public Candidate {
 public:
  ConstantWeightCandidate(double rate, double audioCentre, double baud, double shift) : det_(rate, audioCentre, baud, shift) {
    name = "4:3 code";
  }
  void process(const float* a, std::size_t n) override {
    det_.process(a, n);
    const std::string found = det_.take();
    if (!found.empty()) addLine(found, 0.9);
  }
  void flush() override {}

 private:
  ConstantWeightDetector det_;
};

// Akula bursts: recognised by their published sync and end groups (the
// traffic is encrypted and not shown). Each report is believed at once.
class AkulaCandidate : public Candidate {
 public:
  AkulaCandidate(double rate, double audioCentre) : det_(rate, audioCentre) { name = "Akula"; }
  void process(const float* a, std::size_t n) override {
    det_.process(a, n);
    for (const auto& b : det_.take()) addLine(b, 1.0);
  }
  void flush() override {}

 private:
  AkulaDetector det_;
};

}  // namespace

// --- Sessions ---------------------------------------------------------------

struct DecoderHost::Session {
  Emission emission;
  int channel = -1;
  struct View {
    Nco nco;
    std::vector<float> audio;
  };
  std::vector<View> views;
  std::vector<std::unique_ptr<Candidate>> candidates;
  std::vector<std::string> plan;  // candidate kinds started (candidatesFor names)
  bool ale = false;
  double width = 0;
  Candidate* winner = nullptr;
  bool firstSent = false;
  double lastUtc = 0;
  std::unique_ptr<FeatureMeter> meter;  // on view 0
  bool measured = false;                // features reported
  bool measuredTried = false;           // decoders for the measured parameters started (RTTY, PSK)
  double startedUtc = -1;               // first audio
  double cpu = 0;                       // seconds of processing so far
  bool chosen = false;                  // its decoder is the one a window of the user names
};

// The family a decoder's name belongs to, for switching decoders off.
static std::string familyOf(const std::string& n) {
  if (n.find("PSK") != std::string::npos) return "PSK";
  if (n.rfind("RTTY", 0) == 0) return "RTTY";
  if (n.rfind("ALE", 0) == 0) return "ALE";
  if (n.rfind("DRM", 0) == 0) return "DRM";
  if (n.rfind("4:3 code", 0) == 0) return "4:3 code";
  if (n == "NAVTEX") return "SITOR-B";
  return n;
}

bool DecoderHost::disabled(const std::string& name) const { return cfg_.disabled.count(familyOf(name)) > 0; }

void DecoderHost::setPolicy(const Config& p) {
  const bool rulesChanged = p.rules != cfg_.rules;
  const bool bandsChanged = p.disabled != cfg_.disabled || rulesChanged;
  cfg_.rules = p.rules;
  if (rulesChanged) {
    // Sessions the old or new windows decide start again under the new ones
    // (from the emission's next update).
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      auto next = std::next(it);
      if (it->second->chosen || ruleFor(it->second->emission.centreHz)) endSession(it);
      it = next;
    }
  }
  cfg_.maxSessions = std::max(1, p.maxSessions);
  cfg_.measureUnknown = p.measureUnknown;
  cfg_.giveUpSeconds = p.giveUpSeconds;
  cfg_.minSnrDb = p.minSnrDb;
  cfg_.disabled = p.disabled;
  if (bandsChanged && centreSet_) {
    planFtx();
    planHfdl();
  }
  enforce();
}

void DecoderHost::ignore(uint64_t id) {
  left_[id] = 'i';
  auto it = byEmission_.find(id);
  if (it == byEmission_.end()) return;
  auto sit = sessions_.find(it->second);
  if (sit != sessions_.end()) endSession(sit);
}

void DecoderHost::prune(Session& s) {
  if (s.chosen) return;  // the user's choice, whatever is switched off
  for (auto c = s.candidates.begin(); c != s.candidates.end();) {
    if (disabled((*c)->name)) {
      if (s.winner == c->get()) s.winner = nullptr;
      c = s.candidates.erase(c);
    } else {
      ++c;
    }
  }
}

void DecoderHost::enforce() {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    Session& s = *it->second;
    // Candidates of families now switched off go (a winner among them too).
    prune(s);
    char why = 0;
    const double age = s.startedUtc >= 0 ? s.lastUtc - s.startedUtc : 0;
    if (s.candidates.empty() && s.measured) why = 'm';  // measured, nothing to decode it with
    else if (s.candidates.empty() && !cfg_.measureUnknown) why = 'm';
    else if (!s.winner && !s.chosen && !s.candidates.empty() && cfg_.giveUpSeconds > 0 && age > cfg_.giveUpSeconds && s.measured) why = 'g';
    if (why) {
      left_[s.emission.id] = why;
      auto next = std::next(it);
      endSession(it);
      it = next;
    } else {
      ++it;
    }
  }
  while (static_cast<int>(sessions_.size()) > cfg_.maxSessions && evictForDecodable()) {
  }
}

// On a busy band the sessions are all taken; without this the first
// emissions keep them (a carrier, a broadcaster's sideband tried as CW)
// and the later ones are never tried. The undecided session tried longest,
// if for kMinTrySeconds, makes room; it is given up.
bool DecoderHost::evictUndecided() {
  double minTry = kMinTrySeconds;
  if (cfg_.giveUpSeconds > 0) minTry = std::min(minTry, cfg_.giveUpSeconds);
  auto oldest = sessions_.end();
  double oldestAge = minTry;
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
    const Session& s = *it->second;
    if (s.winner || s.chosen || s.startedUtc < 0) continue;
    const double age = s.lastUtc - s.startedUtc;
    if (age >= oldestAge) oldest = it, oldestAge = age;
  }
  if (oldest == sessions_.end()) return false;
  left_[oldest->second->emission.id] = 'g';
  endSession(oldest);
  return true;
}

bool DecoderHost::evictAnyUnchosen() {
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    if (!it->second->chosen && !it->second->winner) {
      left_[it->second->emission.id] = 'g';
      endSession(it);
      return true;
    }
  return false;
}

const DecodeRule* DecoderHost::ruleFor(double hz) const {
  if (!cfg_.rules) return nullptr;
  const DecodeRule* best = nullptr;
  for (const auto& r : *cfg_.rules)
    if (hz >= r.loHz && hz <= r.hiHz && (!best || r.hiHz - r.loHz < best->hiHz - best->loHz)) best = &r;
  return best;
}

bool DecoderHost::excluded(double lo, double hi) const {
  const DecodeRule* r = ruleFor(0.5 * (lo + hi));
  return r && r->decoder == "none";
}

std::vector<std::string> DecoderHost::chosenDecoders() {
  return {"CW",     "PSK31",  "PSK63",   "PSK125",  "PSK250",  "PSK500", "QPSK31", "QPSK63",
          "QPSK125", "QPSK250", "QPSK500", "RTTY 45.45/170", "RTTY 50/450", "RTTY 50/85", "RTTY 75/850",
          "RTTY 100/850", "SITOR-B", "DSC", "ALE 2G", "ALE 3G/4G", "HFDL", "FAX", "SSTV", "TIME", "DRM", "Akula"};
}

bool DecoderHost::choosable(const std::string& n) {
  const auto list = chosenDecoders();
  if (std::find(list.begin(), list.end(), n) != list.end()) return true;
  double baud = 0, shift = 0;
  char rest = 0;
  return std::sscanf(n.c_str(), "RTTY %lf/%lf%c", &baud, &shift, &rest) == 2 && baud >= 30 && baud <= 300 &&
         shift >= 50 && shift <= 1100;
}

// The channel a decoder the user chose needs, beyond the emission's band.
static double chosenWidth(const std::string& n) {
  double baud = 0, shift = 0;
  if (std::sscanf(n.c_str(), "RTTY %lf/%lf", &baud, &shift) == 2) return shift + 400.0;
  if (n == "PSK500" || n == "QPSK500") return 1400;
  if (n == "PSK250" || n == "QPSK250") return 900;
  if (n == "FAX") return 1900;
  if (n == "SSTV") return 2200;
  if (n == "HFDL" || n == "DRM" || n == "Akula") return 2900;
  if (n == "SITOR-B" || n == "DSC") return 700;
  return 0;
}

bool DecoderHost::evictForDecodable() {
  for (auto it = sessions_.begin(); it != sessions_.end(); ++it)
    if (it->second->candidates.empty()) {
      left_[it->second->emission.id] = 'm';
      endSession(it);
      return true;
    }
  return false;
}


std::vector<std::string> DecoderHost::candidatesFor(double b) {
  std::vector<std::string> out;
  // Morse up to 700 Hz: the measured band of a strong, hard-keyed tone
  // (key clicks, the receiver's AGC pumping) is often several hundred hertz
  // wide; the decoder finds the tone itself, and its text gate keeps out
  // what other modes make of it.
  if (b <= 700) out.push_back("CW");
  // PSK (BPSK and QPSK, fldigi's modes). The measured band of a PSK signal
  // varies a lot with its level and the receiver; the symbol rate the
  // feature meter measures adds the decoders for that rate as well
  // (addMeasuredCandidates).
  if (b <= 250) {
    out.push_back("PSK31");
    out.push_back("PSK63");
    out.push_back("QPSK31");
    out.push_back("QPSK63");
    if (b >= 100) {
      out.push_back("PSK125");
      out.push_back("QPSK125");
    }
  }
  if (b >= 250 && b <= 800) {
    out.push_back("PSK250");
    out.push_back("QPSK250");
  }
  // (Below ALE's range: a session is either ALE, with its two audio views,
  // or not.)
  if (b >= 500 && b <= 1250) {
    out.push_back("PSK500");
    out.push_back("QPSK500");
  }
  // A keyed carrier: LF time stations, and the ticks of HF ones in a
  // narrow filter.
  if (b <= 700) out.push_back("TIME");  // (keying sidebands widen a strong one)
  if (b >= 200 && b <= 700) {
    out.push_back("SITOR-B");
    out.push_back("RTTY 45.45/170");
    out.push_back("DSC");
  }
  if (b >= 350 && b <= 900) out.push_back("RTTY 50/450");
  // Black to white is 800 Hz; a chart, mostly white, occupies less.
  if (b >= 500 && b < 1300) out.push_back("FAX");
  // 1200 Hz sync to 2300 Hz white.
  if (b >= 700 && b < 1300) out.push_back("SSTV");
  if (b >= 1300 && b <= 3600) {
    out.push_back("ALE 2G");
    out.push_back("ALE 3G/4G");
    if (b >= 1800) out.push_back("HFDL");  // (about 2.4 kHz wide)
    if (b <= 1900) out.push_back("FAX");   // (a strong one: its skirts count)
    if (b <= 2200) out.push_back("SSTV");
    out.push_back("TIME");                 // (an AM station's ticks)
    out.push_back("DRM");                  // (WinDRM, EasyPal: about 2.5 kHz)
  }
  // 500 Bd on a 1000 Hz shift occupies about 1.5 kHz (after ALE, which
  // decides the session's audio views).
  if (b >= 1100 && b <= 2200) out.push_back("Akula");
  // Wider than every decoder above: DRM broadcasting (4.5 to 20 kHz).
  if (b > 3600) out.push_back("DRM");
  return out;
}

// The PSK decoder's mode for a candidate name ("PSK31" is BPSK31, as the
// mode is commonly called), false for other names.
static bool pskMode(const std::string& n, psk::Mode& mode) {
  static const std::pair<const char*, psk::Mode> kModes[] = {
      {"PSK31", psk::Mode::Bpsk31},   {"PSK63", psk::Mode::Bpsk63},   {"PSK125", psk::Mode::Bpsk125},
      {"PSK250", psk::Mode::Bpsk250}, {"PSK500", psk::Mode::Bpsk500}, {"QPSK31", psk::Mode::Qpsk31},
      {"QPSK63", psk::Mode::Qpsk63},  {"QPSK125", psk::Mode::Qpsk125}, {"QPSK250", psk::Mode::Qpsk250},
      {"QPSK500", psk::Mode::Qpsk500}};
  for (const auto& [name, m] : kModes)
    if (n == name) {
      mode = m;
      return true;
    }
  return false;
}

static std::unique_ptr<Candidate> makeCandidate(const std::string& n, double rate, double ac, double b) {
  if (n == "CW") return std::make_unique<CwCandidate>(rate, ac, std::max(150.0, b));
  psk::Mode mode;
  if (pskMode(n, mode)) return std::make_unique<PskCandidate>(rate, mode, ac, n);
  if (n == "SITOR-B") return std::make_unique<SitorCandidate>(rate, ac);
  if (n == "DSC") return std::make_unique<DscCandidate>(rate, ac);
  if (n == "Akula") return std::make_unique<AkulaCandidate>(rate, ac);
  if (n == "HFDL") return std::make_unique<HfdlCandidate>(rate);
  if (n == "FAX") return std::make_unique<FaxCandidate>(rate, ac, b >= 1300);
  if (n == "SSTV") return std::make_unique<SstvCandidate>(rate, ac, b >= 1300);
  if (n == "TIME") return std::make_unique<TimeCandidate>(rate, ac, b >= 1300);
  if (n == "DRM") return std::make_unique<DrmCandidate>(rate, ac, b >= 1300 && b <= 3600, b);
  double baud = 0, shift = 0;
  if (std::sscanf(n.c_str(), "RTTY %lf/%lf", &baud, &shift) == 2 && baud >= 30 && baud <= 300 && shift >= 50 && shift <= 1100)
    return std::make_unique<RttyCandidate>(rate, baud, shift, ac, n.c_str());
  if (n == "ALE 2G") return std::make_unique<AleCandidate>(rate, true, 0);
  if (n == "ALE 3G/4G") return std::make_unique<AleCandidate>(rate, false, 1);
  return nullptr;
}

static bool isAle(const std::vector<std::string>& names) {
  return !names.empty() && names.front().rfind("ALE", 0) == 0;
}

void DecoderHost::startSession(const Emission& e) {
  // Left before (given up, ignored, measured with nothing to decode it).
  auto left = left_.find(e.id);
  if (left != left_.end() && left->second != 'm') return;
  if (e.meanSnrDb < cfg_.minSnrDb) return;
  if (inFtxBand(e.loHz, e.hiHz)) return;
  // A window of the user's decides first: no session at all, or the
  // decoder it names and no other.
  const DecodeRule* rule = ruleFor(e.centreHz);
  if (rule && rule->decoder == "none") return;
  const bool chosen = rule != nullptr;
  // Decoders are chosen by the band holding 99 % of the power: the detected
  // band of a strong signal includes window skirts and key clicks.
  const double b = e.occupiedHz() > 0 ? e.occupiedHz() : e.bandwidthHz();
  std::vector<std::string> names;
  double hintWidth = 0;
  if (chosen) {
    names = {rule->decoder};
    hintWidth = chosenWidth(rule->decoder);
  } else {
  names = candidatesFor(b);
  names.erase(std::remove_if(names.begin(), names.end(), [this](const std::string& n) { return disabled(n); }), names.end());
  // Measured before, and still nothing to decode it with: not again.
  if (left != left_.end() && names.empty()) return;
  // A listed HFDL channel is decoded all the time (HfdlBand): not twice.
  if (inHfdlBand(e.loHz, e.hiHz)) names.erase(std::remove(names.begin(), names.end(), "HFDL"), names.end());
  // Decoders a frequency list names for this frequency, whatever the
  // bandwidth says (not into an ALE session, which has its own audio views).
  if (cfg_.frequencies && !isAle(names)) {
    for (const auto& l : cfg_.frequencies->lookup(e.loHz, e.hiHz, 3, e.startUtc > 1e6 ? e.startUtc : -1)) {
      std::string hint;
      if (l.mode == "NAVTEX / SITOR-B") hint = "SITOR-B", hintWidth = std::max(hintWidth, 600.0);
      else if (l.mode == "DSC-HF") hint = "DSC", hintWidth = std::max(hintWidth, 600.0);
      else if (l.mode == "CW" || l.mode.rfind("PSK", 0) == 0 || l.mode.rfind("QPSK", 0) == 0) hint = l.mode;
      else if (l.mode == "Time signal") hint = "TIME";
      else if (l.mode.rfind("RTTY", 0) == 0 && l.baud >= 30 && l.baud <= 300 && l.shiftHz >= 50 && l.shiftHz <= 1100) {
        char name[48];
        std::snprintf(name, sizeof name, "RTTY %g/%g", l.baud, l.shiftHz);
        hint = name;
        hintWidth = std::max(hintWidth, l.shiftHz + 400.0);
      }
      if (!hint.empty() && !disabled(hint) && std::find(names.begin(), names.end(), hint) == names.end()) names.push_back(hint);
    }
  }
  }
  // No decoder for it: still measured (FeatureMeter), if it fits a channel
  // and leaves a quarter of the sessions for emissions that can be decoded.
  if (names.empty() && (!cfg_.measureUnknown || b > 2600 || 4 * sessions_.size() >= 3 * static_cast<std::size_t>(cfg_.maxSessions)))
    return;
  // Full: an emission that can be decoded takes the place of one that is
  // only being measured, or of one tried long enough without a result;
  // else it waits.
  // (One the user gave a decoder takes the place of any undecided one.)
  if (static_cast<int>(sessions_.size()) >= cfg_.maxSessions &&
      (names.empty() || (!evictForDecodable() && !evictUndecided() && !(chosen && evictAnyUnchosen()))))
    return;
  // Part of a wider emission being decoded (one tone of an 8FSK signal,
  // keying sidebands): that session has it.
  for (const auto& [id, o] : sessions_)
    if (!chosen && o->emission.bandwidthHz() > 3.0 * b && e.centreHz > o->emission.loHz - 150 &&
        e.centreHz < o->emission.hiHz + 150)
      return;
  // Conversely, a new wide emission takes over undecided narrow ones inside it.
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const Emission& o = it->second->emission;
    if (!it->second->winner && b > 3.0 * o.bandwidthHz() && o.centreHz > e.loHz - 150 && o.centreHz < e.hiHz + 150) {
      auto next = std::next(it);
      dropSession(it);
      it = next;
    } else {
      ++it;
    }
  }

  auto s = std::make_unique<Session>();
  s->emission = e;
  s->chosen = chosen;
  s->ale = isAle(names);
  // Audio views: the emission centred at `audioCentre` (ALE: 2G tones are
  // centred at 1625 Hz, the 3G/4G carrier is at 1800 Hz).
  std::vector<double> centres = s->ale ? std::vector<double>{1625.0, 1800.0} : std::vector<double>{cfg_.audioCentre};
  // The channel's flat passband stays inside (100 Hz, 2 * centre - 100 Hz)
  // of the audio. (Its 300 Hz raised-cosine skirts reach a little below
  // 0 Hz, where they fold back attenuated: harmless for these decoders.)
  // Narrow emissions get room to drift and for the wider candidates they
  // may grow into.
  const double maxWidth = 2.0 * (*std::min_element(centres.begin(), centres.end()) - 100.0);
  s->width = s->ale ? std::min(maxWidth, 3000.0) : std::min(maxWidth, std::max({b + 600.0, 1200.0, hintWidth}));
  // SSTV's sync pulses are 300 Hz below its black, where little of its
  // power is: the emission's centre is the picture's, and the channel must
  // reach down to them.
  if (!s->ale && std::find(names.begin(), names.end(), "SSTV") != names.end()) s->width = std::min(maxWidth, std::max(s->width, 2000.0));
  for (double ac : centres) {
    Session::View v;
    v.nco.setStep(2.0 * kPi * ac / rate_);
    s->views.push_back(std::move(v));
  }
  for (const auto& n : names)
    if (auto c = makeCandidate(n, rate_, centres.front(), b)) {
      // The user said what it is: its text need not read like traffic.
      if (chosen) c->needPlausibleText = false;
      s->candidates.push_back(std::move(c));
    }
  s->plan = names;
  s->meter = std::make_unique<FeatureMeter>(rate_, centres.front(), 8.0);
  const double width = s->width;
  // ALE is centred on the middle of its band (the 2G tones span 750-2500 Hz
  // around 1625 Hz; the power-weighted centre wanders with the symbols, and
  // the decoder tolerates only about +-120 Hz).
  const double centre = s->ale && e.occupiedHz() > 0 ? 0.5 * (e.occLoHz + e.occHiHz) : e.centreHz;
  const int channel = s->channel = ch_.addChannel(centre - centre_, width);
  byEmission_[e.id] = channel;
  sessions_.emplace(channel, std::move(s));
  // From before the emission was confirmed (the session must exist first:
  // the replay arrives through channelSamples).
  ch_.replay(channel, kPreRollSeconds);
}

// Publishes the winner's text: everything when `all`, else complete lines,
// chunks of kChunk characters, or text that has waited long enough.
static void takeText(Candidate& c, bool all, double now, std::vector<std::string>& out) {
  while (!c.text.empty()) {
    std::size_t cut = std::string::npos;
    const std::size_t nl = c.text.find('\n');
    if (nl != std::string::npos) cut = nl;
    else if (c.text.size() >= kChunk) {
      const std::size_t sp = c.text.rfind(' ', kChunk);
      cut = sp != std::string::npos && sp > kChunk / 2 ? sp : kChunk;
    } else if (all) {
      cut = c.text.size();
    } else if (c.pendingSince >= 0 && now - c.pendingSince >= kPendingSeconds) {
      // Waited long enough: up to the last space, keeping a word being
      // received for the next piece.
      const std::size_t sp = c.text.rfind(' ');
      cut = sp != std::string::npos && sp > 0 ? sp : c.text.size();
      c.pendingSince = now;
    }
    if (cut == std::string::npos) break;
    std::string piece = c.text.substr(0, cut);
    // The separator cut at (a space or a line end) goes; a character cut
    // at stays for the next piece.
    const bool separator = cut < c.text.size() && (c.text[cut] == ' ' || c.text[cut] == '\n');
    c.text.erase(0, std::min(c.text.size(), cut + (separator ? 1 : 0)));
    while (!piece.empty() && piece.back() == ' ') piece.pop_back();
    while (!piece.empty() && piece.front() == ' ') piece.erase(0, 1);
    if (!piece.empty()) out.push_back(piece);
  }
  c.pendingSince = c.text.empty() ? -1 : (c.pendingSince < 0 ? now : c.pendingSince);
}

// A piece of the winner's text as an event, with the picture it is about.
static DecodeEvent pieceEvent(Candidate& c, uint64_t id, double utc, const std::string& piece, double freq, bool first) {
  DecodeEvent e{id, utc, c.name, piece, freq, c.quality, first, nullptr};
  if (!c.images.empty() && piece.find("[picture]") != std::string::npos) {
    e.image = c.images.front();
    c.images.pop_front();
  }
  return e;
}

// --- FT8 / FT4 --------------------------------------------------------------

struct DecoderHost::FtxBand {
  double dialHz = 0;
  bool ft4 = false;
  bool wspr = false;         // a WSPR sub-band: 2-minute slots
  int channel = -1;
  Nco nco;
  double slotSeconds = 15;
  long long slot = -1;       // current slot number (UTC / slotSeconds)
  bool partial = true;       // the current slot was joined late
  std::vector<float> audio;  // the current slot from its start
  std::set<uint64_t> emissionsReported;
};

// A listed HFDL channel inside the band: decoded all the time, like the
// FT8 sub-bands, since its bursts are short and often too weak or too
// close together for the detector to hand each to a session in time.
struct DecoderHost::HfdlBand {
  double dialHz = 0;
  int channel = -1;
  Nco nco;
  std::unique_ptr<HfdlDecoder> dec;
  std::vector<float> audio;
  std::set<uint64_t> emissionsReported;
};

class DecoderHost::FtxWorker {
 public:
  struct Job {
    double dialHz;
    bool ft4;
    double slotUtc;
    std::vector<float> audio;
    bool wspr = false;
  };
  struct Result {
    double dialHz;
    bool ft4;
    double slotUtc;
    FtxMessage msg;
    bool wspr = false;
  };

  FtxWorker() : thread_([this] { run(); }) {}
  ~FtxWorker() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  void submit(Job j) {
    {
      std::lock_guard<std::mutex> l(mu_);
      jobs_.push_back(std::move(j));
    }
    cv_.notify_all();
  }
  std::vector<Result> take() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Result> r;
    r.swap(results_);
    return r;
  }
  void wait() {
    std::unique_lock<std::mutex> l(mu_);
    idle_.wait(l, [this] { return jobs_.empty() && !busy_; });
  }
  void addUsage(std::map<std::string, double>& seconds) {
    std::lock_guard<std::mutex> l(mu_);
    if (ftxSeconds_ > 0) seconds["FT8/FT4 sub-bands"] += ftxSeconds_;
    if (wsprSeconds_ > 0) seconds["WSPR sub-bands"] += wsprSeconds_;
  }

 private:
  void run() {
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
      cv_.wait(l, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      Job j = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
      l.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      auto took = [&t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
      if (j.wspr) {
        // (audioHz: the signal's middle; score: its SNR in 2500 Hz.)
        auto msgs = wsprDecode(j.audio, 12000);
        l.lock();
        wsprSeconds_ += took();
        for (auto& m : msgs) {
          FtxMessage f;
          f.text = m.text();
          f.audioHz = m.audioHz;
          f.dtSeconds = m.dtSeconds;
          f.score = static_cast<int>(std::lround(m.snrDb));
          results_.push_back({j.dialHz, false, j.slotUtc, std::move(f), true});
        }
        busy_ = false;
        if (jobs_.empty()) idle_.notify_all();
        continue;
      }
      // One decoder per sub-band: its callsign hash table follows the band.
      const long long key = std::llround(j.dialHz) * 2 + (j.ft4 ? 1 : 0);
      auto& dec = decoders_[key];
      if (!dec) dec = std::make_unique<FtxDecoder>(j.ft4);
      auto msgs = dec->decodeSlot(j.audio.data(), j.audio.size());
      l.lock();
      ftxSeconds_ += took();
      for (auto& m : msgs) results_.push_back({j.dialHz, j.ft4, j.slotUtc, std::move(m)});
      busy_ = false;
      if (jobs_.empty()) idle_.notify_all();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_, idle_;
  std::deque<Job> jobs_;
  std::vector<Result> results_;
  std::map<long long, std::unique_ptr<FtxDecoder>> decoders_;  // worker thread only
  bool busy_ = false, stop_ = false;
  double ftxSeconds_ = 0, wsprSeconds_ = 0;  // processing time spent, under mu_
  std::thread thread_;
};

// --- Host -------------------------------------------------------------------

namespace {

// What one job of runPending() produces, handed on when all are done.
struct JobOutput {
  std::map<std::string, double> busy;
  double inside = 0;
  std::vector<DecodeEvent> events;
  std::vector<std::pair<uint64_t, EmissionFeatures>> features;
};
thread_local JobOutput* tlJob = nullptr;

}  // namespace

std::map<std::string, double>& DecoderHost::busyMap() { return tlJob ? tlJob->busy : busy_; }
double& DecoderHost::insideTime() { return tlJob ? tlJob->inside : inside_; }

void DecoderHost::runPending() {
  if (pending_.empty()) return;
  std::vector<std::pair<int, std::vector<Chunk>>> jobs;
  jobs.reserve(pending_.size());
  for (auto& p : pending_) jobs.emplace_back(p.first, std::move(p.second));
  pending_.clear();
  std::vector<JobOutput> out(jobs.size());
  const auto t0 = std::chrono::steady_clock::now();
  // The maps are only read meanwhile; each job changes its own session.
  pool_->run(jobs.size(), [&](std::size_t i) {
    tlJob = &out[i];
    const int id = jobs[i].first;
    auto hit = hfdl_.find(id);
    auto sit = sessions_.find(id);
    for (const Chunk& c : jobs[i].second) {
      if (hit != hfdl_.end()) hfdlSamples(*hit->second, c.samples.data(), c.samples.size(), c.firstSample);
      else if (sit != sessions_.end()) sessionSamples(*sit->second, c.samples.data(), c.samples.size(), c.firstSample);
    }
    tlJob = nullptr;
  });
  inside_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  for (JobOutput& o : out) {
    for (const auto& [k, v] : o.busy) busy_[k] += v;
    for (const auto& [id, f] : o.features)
      if (featuresFn_) featuresFn_(id, f);
    for (const DecodeEvent& e : o.events)
      if (decodeFn_) decodeFn_(e);
  }
}

DecoderHost::DecoderHost(Channelizer& channelizer, const Config& config)
    : ch_(channelizer), cfg_(config), rate_(channelizer.channelRate()) {
  // Room for a 1200 Hz channel above 100 Hz, and below 6 kHz.
  cfg_.audioCentre = std::clamp(cfg_.audioCentre, 700.0, 2500.0);
  // ft8_lib wants exactly 12 kHz; a rate that is not a multiple of a divisor
  // of 12 kHz gives channels slightly off it (Channelizer).
  if (rate_ != 12000.0) cfg_.ftx = false;
  if (cfg_.ftx) worker_ = std::make_unique<FtxWorker>();
  ch_.setHistorySeconds(kPreRollSeconds);
  pool_ = std::make_unique<WorkerPool>(WorkerPool::threadsFor(cfg_.threads));
  ch_.setBlockDoneCallback([this] { runPending(); });
}

DecoderHost::~DecoderHost() {
  ch_.setBlockDoneCallback(nullptr);
  for (auto& [id, s] : sessions_) ch_.removeChannel(id);
  for (auto& [id, b] : ftx_) ch_.removeChannel(id);
  for (auto& [id, b] : hfdl_) ch_.removeChannel(id);
}

double DecoderHost::utcOf(uint64_t sample) const {
  return utcFn_ ? utcFn_(sample) : static_cast<double>(sample) / ch_.inputRate();
}

void DecoderHost::emit(const DecodeEvent& e) {
  if (tlJob) tlJob->events.push_back(e);
  else if (decodeFn_) decodeFn_(e);
}

void DecoderHost::setCentre(double hz) {
  if (centreSet_ && hz == centre_) return;
  centreSet_ = true;
  while (!sessions_.empty()) endSession(sessions_.begin());
  byEmission_.clear();
  active_.clear();
  centre_ = hz;
  planFtx();
  planHfdl();
}

std::vector<double> DecoderHost::hfdlChannels() const {
  std::vector<double> out;
  for (const auto& [id, b] : hfdl_) out.push_back(b->dialHz);
  return out;
}

bool DecoderHost::inHfdlBand(double lo, double hi) const {
  for (const auto& [id, b] : hfdl_)
    if (hi >= b->dialHz + 200 && lo <= b->dialHz + 2700) return true;
  return false;
}

// One channel per frequency the lists give for HFDL (the dial frequency of
// an upper-sideband receiver: the subcarrier is 1440 Hz above), at most 16.
void DecoderHost::planHfdl() {
  runPending();
  for (auto& [id, b] : hfdl_) ch_.removeChannel(id);
  hfdl_.clear();
  if (!centreSet_ || !cfg_.frequencies || disabled("HFDL")) return;
  const double reach = 0.45 * ch_.inputRate();
  double last = -1;
  for (const auto& l : cfg_.frequencies->entries()) {
    if (l.mode != "HFDL" || l.hz == last) continue;
    if (l.hz + 150.0 - centre_ < -reach || l.hz + 2900.0 - centre_ > reach) continue;
    if (excluded(l.hz, l.hz + 2900.0)) continue;
    if (hfdl_.size() >= 16) break;
    last = l.hz;
    auto b = std::make_unique<HfdlBand>();
    b->dialHz = l.hz;
    // The channel is centred on the subcarrier; shifted up by 1800 Hz it
    // is audio with the subcarrier at 1800 Hz.
    b->nco.setStep(2.0 * kPi * 1800.0 / rate_);
    b->dec = std::make_unique<HfdlDecoder>(rate_, 1800.0);
    b->channel = ch_.addChannel(l.hz + 1440.0 - centre_, 3000.0);
    hfdl_.emplace(b->channel, std::move(b));
  }
}

void DecoderHost::hfdlSamples(HfdlBand& b, const cfloat* samples, std::size_t n, uint64_t firstSample) {
  const auto t0 = std::chrono::steady_clock::now();
  struct Account {
    std::map<std::string, double>& busy;
    double& inside;
    std::chrono::steady_clock::time_point t0;
    ~Account() {
      const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      busy["HFDL channels"] += d;
      inside += d;
    }
  } account{busyMap(), insideTime(), t0};
  b.audio.resize(n);
  for (std::size_t i = 0; i < n; ++i) b.audio[i] = (samples[i] * b.nco.next()).real();
  b.dec->process(b.audio.data(), n);
  const auto pdus = b.dec->take();
  if (pdus.empty()) return;
  const double utc = utcOf(firstSample) + static_cast<double>(n) / rate_;
  const double freq = b.dialHz + 1440.0;
  for (const auto& text : pdus) {
    // The emission it belongs to: one over the subcarrier that was on the
    // air within the burst's length.
    uint64_t id = 0;
    for (const auto& [eid, e] : active_)
      if (freq >= e.loHz - 100 && freq <= e.hiHz + 100 && utc + 1 >= e.startUtc && utc <= e.endUtc + 6) {
        id = eid;
        break;
      }
    const bool first = id ? b.emissionsReported.insert(id).second : true;
    emit({id, utc, "HFDL", text, freq, 1.0, first, nullptr});
  }
}

std::vector<DecoderHost::Band> DecoderHost::ftxBands() const {
  std::vector<Band> out;
  for (const auto& [id, b] : ftx_) out.push_back({b->dialHz, b->ft4, b->wspr});
  return out;
}

bool DecoderHost::inFtxBand(double lo, double hi) const {
  for (const auto& [id, b] : ftx_)
    if (b->wspr ? (lo >= b->dialHz + 1350 && hi <= b->dialHz + 1650) : (lo >= b->dialHz - 50 && hi <= b->dialHz + 3050)) return true;
  return false;
}

void DecoderHost::planFtx() {
  for (auto& [id, b] : ftx_) ch_.removeChannel(id);
  ftx_.clear();
  if (!cfg_.ftx) return;
  const double reach = 0.45 * ch_.inputRate();
  auto add = [&](double dialKhz, bool ft4, bool wspr) {
    if (disabled(wspr ? "WSPR" : ft4 ? "FT4" : "FT8")) return;
    const double dial = dialKhz * 1000.0;
    if (dial - centre_ < -reach || dial + 3000.0 - centre_ > reach) return;
    if (excluded(dial, dial + 3000.0)) return;  // a window of the user's: no processing
    auto b = std::make_unique<FtxBand>();
    b->dialHz = dial;
    b->ft4 = ft4;
    b->wspr = wspr;
    b->slotSeconds = wspr ? 120.0 : ft4 ? 7.5 : 15.0;
    // Channel centred 1500 Hz above the dial; shifting it up by 1500 Hz
    // puts the dial at 0 Hz, as in a USB receiver.
    b->nco.setStep(2.0 * kPi * 1500.0 / rate_);
    b->channel = ch_.addChannel(dial + 1500.0 - centre_, 2900.0);
    ftx_.emplace(b->channel, std::move(b));
  };
  for (double f : ft8DialFrequenciesKhz()) add(f, false, false);
  for (double f : ft4DialFrequenciesKhz()) add(f, true, false);
  // WSPR: 2-minute slots, signals 1400 to 1600 Hz above the dial.
  for (double f : wsprDialFrequenciesKhz()) add(f, false, true);
}

void DecoderHost::emissionStarted(const Emission& e) {
  active_[e.id] = e;
  // What the frequency lists give here, at once and for every emission
  // (measured or not); the measured features, when they come, carry it too.
  if (cfg_.frequencies && featuresFn_) {
    EmissionFeatures f;
    for (const auto& l : cfg_.frequencies->lookup(e.loHz, e.hiHz, 3, e.startUtc > 1e6 ? e.startUtc : -1)) f.listed.push_back({l.hz, l.label, l.mode, l.source});
    if (!f.listed.empty()) featuresFn_(e.id, f);
  }
  if (cfg_.enable) startSession(e);
}

void DecoderHost::emissionUpdated(const Emission& e) {
  active_[e.id] = e;
  // Once a second: sessions measured with nothing to decode them, given
  // up, of decoders switched off, end.
  if (e.endUtc > lastEnforce_ + 1.0) {
    lastEnforce_ = e.endUtc;
    enforce();
  }
  auto it = byEmission_.find(e.id);
  if (it == byEmission_.end()) {
    // Too narrow or too wide when it started, or no room then.
    if (cfg_.enable) startSession(e);
    return;
  }
  auto sit = sessions_.find(it->second);
  if (sit == sessions_.end()) return;
  Session& s = *sit->second;
  s.emission = e;
  // (A session of the user's choice keeps its decoder.)
  // Undecided and its bandwidth now calls for other decoders: add them
  // (none is removed, so a bandwidth wavering at a boundary costs nothing);
  // between narrow modes and ALE, start over with the right channel.
  if (!s.winner && !s.chosen) {
    const double occ = e.occupiedHz() > 0 ? e.occupiedHz() : e.bandwidthHz();
    auto want = candidatesFor(occ);
    // (Decoders switched off are not wanted: counted as missing, they
    // would start the session over and over.)
    want.erase(std::remove_if(want.begin(), want.end(), [this](const std::string& n) { return disabled(n); }), want.end());
    // (Not for an emission now wider than every decoder but DRM: its
    // session has DRM already, and a bandwidth wavering around 3.6 kHz
    // would start it over and over.)
    const bool onlyDrm = want.size() == 1 && want[0] == "DRM";
    if (!want.empty() && !onlyDrm && isAle(want) != s.ale) {
      endSession(sit);
      if (cfg_.enable) startSession(e);
      return;
    }
    // Grown beyond what the session's channel passes (it began as a
    // narrow one: the header tone of an SSTV picture, a fax's white), with
    // decoders wanted that are not in it: start over on the whole of it.
    // (The new session hears the 0.75 s before now again.)
    if (!s.ale && occ + 200.0 > s.width) {
      bool missing = false;
      for (const auto& n : want) missing = missing || std::find(s.plan.begin(), s.plan.end(), n) == s.plan.end();
      if (missing) {
        endSession(sit);
        if (cfg_.enable) startSession(e);
        return;
      }
    }
    for (const auto& n : want) {
      if (std::find(s.plan.begin(), s.plan.end(), n) != s.plan.end() || disabled(n)) continue;
      if (occ + 200.0 > s.width) continue;
      auto c = makeCandidate(n, rate_, cfg_.audioCentre, occ);
      // (A candidate for a view this session lacks never joins it.)
      if (c && c->view < static_cast<int>(s.views.size())) {
        // From the start of the session, while the meter keeps its audio.
        if (c->view == 0 && s.meter && !s.meter->audio().empty())
          c->process(s.meter->audio().data(), s.meter->audio().size());
        s.candidates.push_back(std::move(c));
      }
      s.plan.push_back(n);
    }
  }
  if (s.winner) {
    std::vector<std::string> pieces;
    takeText(*s.winner, false, s.lastUtc, pieces);
    for (auto& p : pieces) emit(pieceEvent(*s.winner, e.id, s.lastUtc, p, e.centreHz, false));
  }
}

void DecoderHost::emissionEnded(const Emission& e) {
  auto it = byEmission_.find(e.id);
  if (it != byEmission_.end()) {
    auto sit = sessions_.find(it->second);
    if (sit != sessions_.end()) {
      sit->second->emission = e;
      endSession(sit);
    }
  }
  // Kept a while for FT8 decodes that arrive after the emission ended.
  active_[e.id] = e;
  for (auto a = active_.begin(); a != active_.end();) {
    if (a->second.ended && e.endUtc - a->second.endUtc > 60) {
      for (auto& [cid, b] : ftx_) b->emissionsReported.erase(a->first);
      for (auto& [cid, b] : hfdl_) b->emissionsReported.erase(a->first);
      a = active_.erase(a);
    } else {
      ++a;
    }
  }
}

void DecoderHost::dropSession(std::map<int, std::unique_ptr<Session>>::iterator it) {
  pending_.erase(it->first);
  ch_.removeChannel(it->first);
  byEmission_.erase(it->second->emission.id);
  sessions_.erase(it);
}

void DecoderHost::reportFeatures(Session& s) {
  if (!s.meter || s.measured) return;
  const Emission& e = s.emission;
  EmissionFeatures f = s.meter->measure(e.occupiedHz() > 0 ? e.occupiedHz() : e.bandwidthHz());
  s.measured = true;
  if (f.valid && f.modulation == "OOK") markKeyed(s);
  s.meter.reset();
  // The learned model's opinion of the family (a prediction, kept apart
  // from the measurements).
  if (f.valid && FamilyModel::builtin().loaded()) {
    const FamilyPrediction p = FamilyModel::builtin().predict(f.vector);
    f.predicted = p.family;
    f.predictedConfidence = p.confidence;
    f.predictedSure = p.sure;
    f.modelVersion = FamilyModel::builtin().version();
  }
  // What the frequency lists give here; a catalogued mode they name goes
  // first among the consistent ones (the list is evidence that the mode is
  // used here, the measurements that this emission could be it).
  if (cfg_.frequencies)
    for (const auto& l : cfg_.frequencies->lookup(e.loHz, e.hiHz, 3, e.startUtc > 1e6 ? e.startUtc : -1)) f.listed.push_back({l.hz, l.label, l.mode, l.source});
  for (const auto& m : Catalogue::builtin().match(f, 12)) {
    ConsistentMode c{m.mode, m.provenance, m.partial, m.frame, false};
    for (const auto& l : f.listed) c.listed = c.listed || (!l.mode.empty() && l.mode == m.mode);
    f.consistent.push_back(std::move(c));
  }
  std::stable_sort(f.consistent.begin(), f.consistent.end(),
                   [](const ConsistentMode& a, const ConsistentMode& b) { return a.listed && !b.listed; });
  if (f.consistent.size() > 5) f.consistent.resize(5);
  if (!f.valid) return;
  if (tlJob) tlJob->features.emplace_back(e.id, std::move(f));
  else if (featuresFn_) featuresFn_(e.id, f);
}

// Measured as a keyed tone: its CW candidate may pass without known words
// (keyedText), checked at once on what it has.
void DecoderHost::markKeyed(Session& s) {
  for (auto& c : s.candidates)
    if (c->name == "CW" && !c->keyedEvidence) {
      c->keyedEvidence = true;
      if (!c->passed) c->check();
    }
}

// RTTY is sent at many rates and shifts (45.45, 50, 75, 100 Bd; 85, 170,
// 450, 850 Hz...): on a measured 2-FSK signal, RTTY decoders at its shift
// and at its measured rate (or, if that is unclear, the common rates) join
// the session's other candidates.
void DecoderHost::addMeasuredCandidates(Session& s) {
  s.measuredTried = true;
  if (s.winner || s.ale || s.chosen || !s.meter) return;
  const Emission& e = s.emission;
  const EmissionFeatures f = s.meter->measure(e.occupiedHz() > 0 ? e.occupiedHz() : e.bandwidthHz());
  if (f.valid && f.modulation == "OOK") markKeyed(s);
  auto snap = [](double v, std::initializer_list<double> to, double tol) {
    double best = v, err = tol;  // the nearest within the tolerance
    for (double t : to)
      if (std::fabs(v - t) / t <= err) best = t, err = std::fabs(v - t) / t;
    return best;
  };
  // Single-carrier PSK at one of fldigi's rates: BPSK and QPSK at that rate.
  if (f.modulation == "PSK" && f.symbolRate > 0) {
    double baud = 0;
    for (double r : {31.25, 62.5, 125.0, 250.0, 500.0})
      if (std::fabs(f.symbolRate - r) <= 0.03 * r) baud = r;
    if (baud == 0 || 2.4 * baud + 200 > s.width) return;
    const std::string rate = baud == 31.25 ? "31" : baud == 62.5 ? "63" : std::to_string(static_cast<int>(baud));
    for (const std::string& name : {"PSK" + rate, "QPSK" + rate}) {
      if (std::find(s.plan.begin(), s.plan.end(), name) != s.plan.end()) continue;
      if (auto c = makeCandidate(name, rate_, cfg_.audioCentre, 0)) {
        // From the start of the session: the audio the meter kept.
        c->process(s.meter->audio().data(), s.meter->audio().size());
        s.candidates.push_back(std::move(c));
      }
      s.plan.push_back(name);
    }
    return;
  }
  if (f.modulation != "FSK" || f.toneSpacingHz < 60 || f.toneSpacingHz > 1100) return;
  const double shift = std::round(snap(f.toneSpacingHz, {85, 170, 200, 400, 425, 450, 500, 850, 1000}, 0.07));
  // The measured rate, and the common ones (measurements early in a
  // transmission can be off by a factor of two).
  std::vector<double> bauds = {45.45, 50, 75, 100};
  if (f.symbolRate >= 40 && f.symbolRate <= 160) {
    const double b = snap(f.symbolRate, {45.45, 50, 56.88, 75, 100, 110, 150}, 0.03);
    if (std::find(bauds.begin(), bauds.end(), b) == bauds.end()) bauds.insert(bauds.begin(), b);
  }
  if (shift + 300 > s.width) return;
  // The constant-weight code of T-600 at the measured rate (36, 50, 75, 100
  // or 150 Bd) and at 50 and 100 Bd (the rate read after 3 s is often off),
  // but not where SITOR is (100 Bd on 170 Hz, which has its own decoder).
  {
    std::vector<double> rates = {50, 100};
    const double m = f.symbolRate > 0 ? snap(f.symbolRate, {36, 50, 75, 100, 150}, 0.04) : 0;
    if (m == 36 || m == 75 || m == 150) rates.insert(rates.begin(), m);
    for (double b : rates) {
      if (b == 100 && std::fabs(f.toneSpacingHz - 170) <= 15) continue;
      char name[48];
      std::snprintf(name, sizeof name, "4:3 code %g/%.0f", b, shift);
      if (std::find(s.plan.begin(), s.plan.end(), name) != s.plan.end()) continue;
      auto c = std::make_unique<ConstantWeightCandidate>(rate_, cfg_.audioCentre, b, f.toneSpacingHz);
      c->process(s.meter->audio().data(), s.meter->audio().size());
      s.candidates.push_back(std::move(c));
      s.plan.push_back(name);
    }
  }
  for (double baud : bauds) {
    char name[48];
    std::snprintf(name, sizeof name, "RTTY %g/%g", std::round(baud * 100) / 100, shift);
    if (std::find(s.plan.begin(), s.plan.end(), name) != s.plan.end()) continue;
    auto c = std::make_unique<RttyCandidate>(rate_, baud, shift, cfg_.audioCentre, name);
    // From the start of the session: the audio the meter kept.
    c->process(s.meter->audio().data(), s.meter->audio().size());
    s.candidates.push_back(std::move(c));
    s.plan.push_back(name);
  }
}

void DecoderHost::endSession(std::map<int, std::unique_ptr<Session>>::iterator it) {
  // Its samples of this block first (sessions' iterators stay valid).
  runPending();
  Session& s = *it->second;
  if (s.meter && !s.measured && s.meter->seconds() >= 1.5) reportFeatures(s);
  for (auto& c : s.candidates) c->flush();
  Candidate* w = s.winner;
  if (!w)
    for (auto& c : s.candidates)
      if (c->passed) {
        w = c.get();
        break;
      }
  if (w) {
    std::vector<std::string> pieces;
    takeText(*w, true, s.lastUtc, pieces);
    for (auto& p : pieces) {
      emit(pieceEvent(*w, s.emission.id, s.lastUtc, p, s.emission.centreHz, !s.firstSent));
      s.firstSent = true;
    }
  }
  ch_.removeChannel(it->first);
  byEmission_.erase(s.emission.id);
  sessions_.erase(it);
}

void DecoderHost::channelSamples(int id, const cfloat* samples, std::size_t n, uint64_t firstSample) {
  auto fit = ftx_.find(id);
  if (fit != ftx_.end()) {
    ftxSamples(*fit->second, samples, n, firstSample);
    return;
  }
  // Sessions and HFDL channels: kept until the block ends (runPending).
  if (hfdl_.count(id) || sessions_.count(id)) pending_[id].push_back({std::vector<cfloat>(samples, samples + n), firstSample});
}

void DecoderHost::sessionSamples(Session& s, const cfloat* samples, std::size_t n, uint64_t firstSample) {
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto lap = [](Clock::time_point& t) {
    const auto now = Clock::now();
    const double d = std::chrono::duration<double>(now - t).count();
    t = now;
    return d;
  };
  auto at = t0;
  s.lastUtc = utcOf(firstSample) + static_cast<double>(n) / rate_;
  if (s.startedUtc < 0) s.startedUtc = s.lastUtc;
  for (auto& v : s.views) {
    v.audio.resize(n);
    for (std::size_t i = 0; i < n; ++i) v.audio[i] = (samples[i] * v.nco.next()).real();
  }
  auto& busy = busyMap();
  busy["channels of signals"] += lap(at);
  for (auto& c : s.candidates) {
    c->process(s.views[static_cast<std::size_t>(c->view)].audio.data(), n);
    busy["decoder " + familyOf(c->name)] += lap(at);
  }
  // The meter measures from the emission's start: what the pre-roll brings
  // from before it is the noise before the signal.
  const bool started = s.lastUtc >= s.emission.startUtc - 0.15;
  struct Account {
    Session& s;
    std::map<std::string, double>& busy;
    double& inside;
    Clock::time_point t0, at;
    ~Account() {
      const auto now = Clock::now();
      busy[s.candidates.empty() ? "measuring signals no decoder fits" : "measuring signals"] +=
          std::chrono::duration<double>(now - at).count();
      const double all = std::chrono::duration<double>(now - t0).count();
      s.cpu += all;
      inside += all;
    }
  } account{s, busy, insideTime(), t0, at};
  if (s.meter && !s.measured && started) {
    s.meter->add(s.views[0].audio.data(), n);
    if (!s.measuredTried && s.meter->seconds() >= 3.0) {
      addMeasuredCandidates(s);
      prune(s);
    }
    if (s.meter->full()) reportFeatures(s);
  }

  if (!s.winner) {
    for (auto& c : s.candidates)
      if (c->passed) {
        s.winner = c.get();
        break;
      }
    if (s.winner) {
      // Keep the winner, drop the others.
      for (auto& c : s.candidates)
        if (c.get() == s.winner) {
          auto keep = std::move(c);
          s.candidates.clear();
          s.candidates.push_back(std::move(keep));
          break;
        }
    }
  }
  if (s.winner) {
    std::vector<std::string> pieces;
    takeText(*s.winner, false, s.lastUtc, pieces);
    for (auto& p : pieces) {
      emit(pieceEvent(*s.winner, s.emission.id, s.lastUtc, p, s.emission.centreHz, !s.firstSent));
      s.firstSent = true;
    }
  }
}

void DecoderHost::ftxSamples(FtxBand& b, const cfloat* samples, std::size_t n, uint64_t firstSample) {
  const auto clock0 = std::chrono::steady_clock::now();
  struct Account {
    std::map<std::string, double>& busy;
    double& inside;
    bool wspr;
    std::chrono::steady_clock::time_point t0;
    ~Account() {
      const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      busy[wspr ? "WSPR sub-bands" : "FT8/FT4 sub-bands"] += d;
      inside += d;
    }
  } account{busy_, inside_, b.wspr, clock0};
  const double t0 = utcOf(firstSample);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = t0 + static_cast<double>(i) / rate_;
    const auto slot = static_cast<long long>(std::floor(t / b.slotSeconds));
    if (slot != b.slot) {
      if (b.slot >= 0) submitSlot(b);
      b.slot = slot;
      b.audio.clear();
      // Joined more than half a second into the slot: transmissions started
      // before we listened, the slot is not decoded.
      // (WSPR transmissions start 1 s into theirs.)
      b.partial = t - static_cast<double>(slot) * b.slotSeconds > (b.wspr ? 1.0 : 0.5);
    }
    const auto at = static_cast<std::size_t>(std::max(0.0, std::round((t - static_cast<double>(slot) * b.slotSeconds) * rate_)));
    const float v = (samples[i] * b.nco.next()).real();
    if (at >= b.audio.size()) b.audio.resize(at + 1, 0.0f);
    b.audio[at] = v;
  }
}

void DecoderHost::submitSlot(FtxBand& b) {
  // FT8: 79 symbols of 0.16 s from 0.5 s into the slot end at 13.1 s, but
  // the last 7 are a sync pattern (the data ends at 12.0 s); FT4: 105 of
  // 0.048 s end at 5.5 s, its last 4 sync symbols at 5.3 s. A slot cut short
  // (the end of a recording) is decoded if it holds all the data.
  // WSPR: 162 symbols of 0.683 s from 1 s end at 111.6 s (the last two
  // carry tail bits of the code, which the decoder can do without).
  const double need = b.wspr ? 110.3 : b.ft4 ? 5.35 : 12.1;
  if (!worker_ || b.partial || static_cast<double>(b.audio.size()) < need * rate_) return;
  const auto full = static_cast<std::size_t>(b.slotSeconds * rate_);
  std::vector<float> audio = b.audio;
  audio.resize(full, 0.0f);
  worker_->submit({b.dialHz, b.ft4, static_cast<double>(b.slot) * b.slotSeconds, std::move(audio), b.wspr});
}

void DecoderHost::poll() {
  if (!worker_) return;
  for (auto& r : worker_->take()) {
    // Tone 0 at audioHz; the signal's centre is 3.5 (FT8) or 1.5 (FT4)
    // tone spacings above.
    // (WSPR's audioHz is its middle.)
    const double spacing = r.ft4 ? 20.833 : 6.25;
    const double freq = r.dialHz + r.msg.audioHz + (r.wspr ? 0.0 : (r.ft4 ? 1.5 : 3.5) * spacing);
    const double utc = r.slotUtc + r.msg.dtSeconds;
    uint64_t id = 0;
    for (const auto& [eid, e] : active_)
      if (freq >= e.loHz - 10 && freq <= e.hiHz + 10 && utc + (r.wspr ? 112 : 13) >= e.startUtc && utc <= e.endUtc + 3) {
        id = eid;
        break;
      }
    FtxBand* band = nullptr;
    for (auto& [cid, b] : ftx_)
      if (b->dialHz == r.dialHz && b->ft4 == r.ft4 && b->wspr == r.wspr) band = b.get();
    bool first = true;
    if (band && id) first = band->emissionsReported.insert(id).second;
    std::string text = r.msg.text;
    if (r.wspr) text += " (SNR " + std::to_string(r.msg.score) + " dB)";
    emit({id, utc, r.wspr ? "WSPR" : r.ft4 ? "FT4" : "FT8", text, freq, 1.0, first, nullptr});
  }
}

void DecoderHost::flush() {
  runPending();
  while (!sessions_.empty()) endSession(sessions_.begin());
  for (auto& [id, b] : ftx_) {
    if (b->slot >= 0) submitSlot(*b);
    b->slot = -1;
    b->audio.clear();
  }
  if (worker_) worker_->wait();
  poll();
}

DecoderHost::Usage DecoderHost::usage() const {
  Usage u;
  u.seconds = busy_;
  u.inside = inside_;
  if (worker_) worker_->addUsage(u.seconds);
  for (const auto& [id, s] : sessions_) {
    SessionInfo i;
    i.emissionId = s->emission.id;
    i.centreHz = s->emission.centreHz;
    i.widthHz = s->emission.bandwidthHz();
    i.snrDb = s->emission.meanSnrDb;
    i.state = s->winner ? "decoding" : s->candidates.empty() ? "measuring" : "trying";
    if (s->winner) i.decoder = s->winner->name;
    else
      for (const auto& c : s->candidates) i.decoder += (i.decoder.empty() ? "" : ", ") + c->name;
    i.ageSeconds = s->startedUtc >= 0 ? s->lastUtc - s->startedUtc : 0;
    i.cpuSeconds = s->cpu;
    u.sessions.push_back(i);
  }
  for (const auto& [id, why] : left_) (why == 'g' ? u.givenUp : why == 'i' ? u.ignored : u.measuredOnly) += 1;
  for (const auto& [id, e] : active_) {
    auto l = left_.find(id);
    if (l != left_.end() && !byEmission_.count(id)) u.leftRecent.emplace_back(id, l->second);
  }
  return u;
}

}  // namespace hfc
