// The log: every emission and every decode, in an SQLite database
// (third_party/sqlite), so that what was heard can be searched and exported
// later. Times are UTC seconds since 1970 (REAL), frequencies RF Hz.
//
//   sessions   one run of the program on one receiver: source, rate, centre
//   emissions  one row per emission (updated while it lasts), keyed by
//              (session, receiver, emission id)
//   decodes    one row per piece of decoded text, tied to its emission
//
// Writes are batched in transactions committed about once a second (and by
// commit()), so a power cut loses at most that much. One thread at a time;
// the application serialises access.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hfc/decoder_host.hpp"
#include "hfc/detector.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace hfc {

struct StoredEmission {
  int64_t session = 0;
  uint64_t id = 0;
  int rx = 0;  // receiver: 0 primary, 1 secondary
  double startUtc = 0, endUtc = 0;
  double loHz = 0, hiHz = 0, centreHz = 0;
  double peakSnrDb = 0, meanSnrDb = 0, duty = 0, driftHzPerSecond = 0;
  bool ended = false;
  std::string label;  // the decoder that identified it, or ""
  std::string features;  // measured features, JSON (EmissionFeatures::json()), or ""
};

struct StoredDecode {
  int64_t session = 0;
  uint64_t emissionId = 0;
  int rx = 0;
  double utc = 0;
  std::string decoder;
  double freqHz = 0;
  double quality = 0;
  bool first = false;
  std::string text;
};

struct Query {
  double fromUtc = 0, toUtc = 1e300;
  double minHz = 0, maxHz = 1e300;
  std::string label;   // exact decoder name; "-" = undecoded only; "" = any
  std::string text;    // decodes containing this text
  int64_t session = 0; // one session only (0: all)
  int limit = 1000;
};

class Store {
 public:
  Store() = default;
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Opens (creating if needed) the database at `path`; ":memory:" for a
  // temporary one.
  bool open(const std::string& path, std::string* error);
  void close();
  bool isOpen() const { return db_ != nullptr; }

  // A new session; returns its id.
  int64_t beginSession(double utc, const std::string& source, double sampleRate, double centreHz);
  // The first receiver's sample rate and centre (the latest set).
  void setSessionTuning(double sampleRate, double centreHz);
  void endSession(double utc);
  int64_t session() const { return session_; }

  void putEmission(int rx, const Emission& e, const std::string& label);
  void putDecode(int rx, const DecodeEvent& d);
  void putFeatures(int rx, uint64_t emissionId, const std::string& json);
  // Commits now (else once a second, on the next put).
  void commit();

  std::vector<StoredEmission> emissions(const Query& q);
  std::vector<StoredDecode> decodes(const Query& q);
  std::vector<StoredDecode> decodesOf(int64_t session, int rx, uint64_t emissionId);

  // Emissions with their decoded text as CSV (one row per emission) and
  // JSON (emissions with their decodes).
  std::string exportCsv(const Query& q);
  std::string exportJson(const Query& q);

 private:
  bool exec(const char* sql, std::string* error = nullptr);
  sqlite3_stmt* prepare(const char* sql);
  void begin();

  sqlite3* db_ = nullptr;
  sqlite3_stmt* insEmission_ = nullptr;
  sqlite3_stmt* insDecode_ = nullptr;
  int64_t session_ = 0;
  bool inTransaction_ = false;
  double txStarted_ = 0;  // steady clock seconds
};

}  // namespace hfc
