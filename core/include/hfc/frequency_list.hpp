// Known frequencies: what is listed as transmitting where. WebSDR and
// KiwiSDR sites mark such frequencies on their waterfalls, and hobbyist
// databases list many utility and military ones. Here a list is evidence,
// never an identification: an emission near a listed frequency is shown
// with the listing ("listed here: ..."), a catalogued mode named by the
// listing goes first among the modes the measurements are consistent with,
// and a decoder the listing names is tried whatever the measured bandwidth.
//
// A small list is built in: the GMDSS digital selective calling and NAVTEX
// frequencies (ITU). Everything else comes from files the user puts in the
// `frequencies` folder of the data folder, read at start. The parser takes
// what such lists commonly look like, without their licences becoming
// ours (no list is shipped):
//
//   text   one entry per line: the first number is the frequency, the rest
//          of the line the label. Fields separated by commas, semicolons,
//          tabs or spaces. Lines without a number, and lines starting with
//          '#' or "//", are skipped.
//   JSON   any nesting of arrays and objects. An array starting with a
//          number is an entry (the strings in it make the label); so is an
//          object with "frequency", "freq", "khz", "hz" or "f" (label from
//          "name", "ident", "label", "description", "mode", "modulation",
//          "notes", "comment"). A receiver's demodulator setting among them
//          ("USB", "CW", "AM" ...) is not part of the label: it says how
//          to listen, and is kept as that (`demod`).
//   EiBi   the semicolon-separated schedule of eibispace.de (sked-*.csv,
//          recognised by its header line): station, country, the hours it
//          is on the air (UTC), and for utility stations what they send
//          (Morse, HFDL, teletype, time signal). A frozen copy is built in
//          (bundledEibi; EiBi's terms allow passing the lists on).
//
// One unit per list, guessed from its largest number: 100000 or more is
// Hz, 30 or less MHz, anything between kHz (as KiwiSDR's labels, WebSDR
// station lists and most hobby lists are; OpenWebRX's bookmarks are Hz).
//
// The label is searched for a catalogued mode ("STANAG 4285", "NAVTEX",
// "RTTY", "HFDL" ...) and for a rate and shift ("50 Bd", "450 Hz", "50/450").
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hfc {

struct ListedFrequency {
  double hz = 0;
  std::string label;   // as the list gives it
  std::string mode;    // the catalogue's name for it, "" if none recognised
  std::string source;  // the list's name (file name, or "ITU")
  double baud = 0, shiftHz = 0;  // when the label gives them
  // How to listen: "USB", "LSB", "CW" or "AM", as the list says or as the
  // mode implies; "" when neither does.
  std::string demod;
  // The daily hours on the air, minutes after 00:00 UTC (EiBi); -1: not given.
  int startMin = -1, stopMin = -1;
  // KiwiSDR's label type ({"T4":1} in its lists: 4), whose name and colour
  // its dx_community_config.json gives; -1: none.
  int category = -1;
  // False only when the entry has hours and `utc` (Unix seconds) is outside.
  bool onAir(double utc) const;
};

class FrequencyList {
 public:
  // The built-in entries only.
  static std::shared_ptr<FrequencyList> builtin();

  // Adds the entries of a list in any of the formats above; returns how many.
  std::size_t addText(const std::string& text, const std::string& source);
  // Reads a file; returns how many entries it gave (0 if unreadable).
  std::size_t addFile(const std::string& path);
  // One entry, as given (the user's own markers): `demod` "" for what the
  // mode named in the label implies. Counted under `source`.
  void addEntry(double hz, const std::string& label, const std::string& source, const std::string& demod);
  std::size_t size() const { return entries_.size(); }
  const std::vector<ListedFrequency>& entries() const { return entries_; }  // ascending frequency
  // The lists read, in the order they were added, with their entry counts.
  std::vector<std::pair<std::string, std::size_t>> sources() const;
  // The EiBi schedule shipped with the program (text of its CSV) and its
  // name as a source ("EiBi B26").
  static const std::string& bundledEibi();
  static const char* bundledEibiName();

  // The entries an emission occupying loHz..hiHz may be: those inside its
  // band (150 Hz of slack), and those up to 2.5 kHz below its lower edge
  // (lists often give the dial frequency of an upper-sideband receiver, or
  // a suppressed carrier, for a signal that sits 1 to 2 kHz above it).
  // Nearest the band first. With `utc` (Unix seconds, >= 0), entries that
  // have hours and are off the air then are left out.
  std::vector<ListedFrequency> lookup(double loHz, double hiHz, std::size_t max = 3, double utc = -1) const;

  // The catalogued mode a label names ("" if none), and its rate and shift.
  static std::string modeOf(const std::string& label, double* baud = nullptr, double* shiftHz = nullptr);

 private:
  std::vector<ListedFrequency> entries_;  // kept sorted by frequency
  void add(double hz, const std::string& label, const std::string& source, const std::string& demod = "", int category = -1);
  std::size_t addEibi(const std::string& text, const std::string& source);
  std::vector<std::pair<std::string, std::size_t>> sources_;
};

}  // namespace hfc
