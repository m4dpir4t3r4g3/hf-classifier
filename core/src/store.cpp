#include "hfc/store.hpp"

#include <chrono>
#include <cstdio>
#include <map>

#include "hfc/json.hpp"
#include "hfc/sigmf.hpp"
#include "sqlite3.h"

namespace hfc {

namespace {

double steadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Schema version 2: emissions and decodes are keyed by receiver too (each
// receiver numbers its emissions on its own). Version 1 tables, if any, are
// kept as emissions_v1 / decodes_v1. Version 3: emissions have their
// measured features (JSON, EmissionFeatures::json()).
constexpr int kSchemaVersion = 3;
const char* kMigrateFrom2 = "ALTER TABLE emissions ADD COLUMN features TEXT NOT NULL DEFAULT '';";
const char* kMigrateFrom1 = R"SQL(
ALTER TABLE emissions RENAME TO emissions_v1;
ALTER TABLE decodes RENAME TO decodes_v1;
DROP INDEX IF EXISTS emissions_time;
DROP INDEX IF EXISTS emissions_freq;
DROP INDEX IF EXISTS decodes_emission;
DROP INDEX IF EXISTS decodes_time;
)SQL";
const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY,
  start_utc REAL NOT NULL,
  end_utc REAL,
  source TEXT,
  sample_rate REAL,
  centre_hz REAL
);
CREATE TABLE IF NOT EXISTS emissions (
  session_id INTEGER NOT NULL,
  emission_id INTEGER NOT NULL,
  rx INTEGER NOT NULL DEFAULT 0,
  start_utc REAL, end_utc REAL,
  lo_hz REAL, hi_hz REAL, centre_hz REAL,
  peak_snr_db REAL, mean_snr_db REAL, duty REAL, drift_hz_s REAL,
  ended INTEGER NOT NULL DEFAULT 0,
  label TEXT NOT NULL DEFAULT '',
  features TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (session_id, rx, emission_id)
);
CREATE INDEX IF NOT EXISTS emissions_time ON emissions(start_utc);
CREATE INDEX IF NOT EXISTS emissions_freq ON emissions(centre_hz);
CREATE TABLE IF NOT EXISTS decodes (
  id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL,
  emission_id INTEGER NOT NULL,
  rx INTEGER NOT NULL DEFAULT 0,
  utc REAL, decoder TEXT, freq_hz REAL, quality REAL, first INTEGER, text TEXT
);
CREATE INDEX IF NOT EXISTS decodes_emission ON decodes(session_id, rx, emission_id);
CREATE INDEX IF NOT EXISTS decodes_time ON decodes(utc);
)SQL";

std::string col(sqlite3_stmt* s, int i) {
  const unsigned char* t = sqlite3_column_text(s, i);
  return t ? reinterpret_cast<const char*>(t) : "";
}

std::string csvField(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string o = "\"";
  for (char c : s) {
    if (c == '"') o += '"';
    o += c;
  }
  return o + "\"";
}

// The "summary" of a features JSON object, for the CSV export.
std::string featuresSummary(const std::string& json) {
  std::vector<JsonField> f;
  if (json.empty() || !parseJsonObject(json, f, nullptr)) return "";
  return jsonField(f, "summary");
}

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

}  // namespace

Store::~Store() { close(); }

bool Store::exec(const char* sql, std::string* error) {
  char* msg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    if (error) *error = msg ? msg : "SQLite error";
    sqlite3_free(msg);
    return false;
  }
  return true;
}

sqlite3_stmt* Store::prepare(const char* sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK) return nullptr;
  return s;
}

bool Store::open(const std::string& path, std::string* error) {
  close();
  if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
    if (error) *error = db_ ? sqlite3_errmsg(db_) : "cannot open the database";
    close();
    return false;
  }
  sqlite3_busy_timeout(db_, 2000);
  int version = 0;
  bool haveEmissions = false;
  {
    sqlite3_stmt* q = prepare("PRAGMA user_version");
    if (q && sqlite3_step(q) == SQLITE_ROW) version = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    q = prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name='emissions'");
    haveEmissions = q && sqlite3_step(q) == SQLITE_ROW;
    sqlite3_finalize(q);
  }
  if (version > kSchemaVersion) {
    if (error) *error = "the database was made by a newer version of this program";
    close();
    return false;
  }
  if (version < 2 && haveEmissions && !exec(kMigrateFrom1, error)) {
    close();
    return false;
  }
  if (version == 2 && haveEmissions && !exec(kMigrateFrom2, error)) {
    close();
    return false;
  }
  if (!exec(kSchema, error) || !exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str(), error)) {
    close();
    return false;
  }
  insEmission_ = prepare(
      "INSERT INTO emissions (session_id, emission_id, rx, start_utc, end_utc, lo_hz, hi_hz, centre_hz, "
      "peak_snr_db, mean_snr_db, duty, drift_hz_s, ended, label) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(session_id, rx, emission_id) DO UPDATE SET end_utc=excluded.end_utc, lo_hz=excluded.lo_hz, "
      "hi_hz=excluded.hi_hz, centre_hz=excluded.centre_hz, peak_snr_db=excluded.peak_snr_db, "
      "mean_snr_db=excluded.mean_snr_db, duty=excluded.duty, drift_hz_s=excluded.drift_hz_s, "
      "ended=excluded.ended, label=CASE WHEN excluded.label<>'' THEN excluded.label ELSE label END");
  insDecode_ = prepare(
      "INSERT INTO decodes (session_id, emission_id, rx, utc, decoder, freq_hz, quality, first, text) "
      "VALUES (?,?,?,?,?,?,?,?,?)");
  if (!insEmission_ || !insDecode_) {
    if (error) *error = sqlite3_errmsg(db_);
    close();
    return false;
  }
  return true;
}

void Store::close() {
  if (!db_) return;
  commit();
  sqlite3_finalize(insEmission_);
  sqlite3_finalize(insDecode_);
  insEmission_ = insDecode_ = nullptr;
  sqlite3_close(db_);
  db_ = nullptr;
  session_ = 0;
}

void Store::begin() {
  if (inTransaction_) {
    if (steadySeconds() - txStarted_ >= 1.0) commit();
    else return;
  }
  exec("BEGIN");
  inTransaction_ = true;
  txStarted_ = steadySeconds();
}

void Store::commit() {
  if (!db_ || !inTransaction_) return;
  exec("COMMIT");
  inTransaction_ = false;
}

int64_t Store::beginSession(double utc, const std::string& source, double sampleRate, double centreHz) {
  if (!db_) return 0;
  begin();
  sqlite3_stmt* s = prepare("INSERT INTO sessions (start_utc, source, sample_rate, centre_hz) VALUES (?,?,?,?)");
  sqlite3_bind_double(s, 1, utc);
  sqlite3_bind_text(s, 2, source.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(s, 3, sampleRate);
  sqlite3_bind_double(s, 4, centreHz);
  sqlite3_step(s);
  sqlite3_finalize(s);
  session_ = sqlite3_last_insert_rowid(db_);
  commit();
  return session_;
}

void Store::setSessionTuning(double sampleRate, double centreHz) {
  if (!db_ || !session_) return;
  begin();
  sqlite3_stmt* s = prepare("UPDATE sessions SET sample_rate=?, centre_hz=? WHERE id=?");
  sqlite3_bind_double(s, 1, sampleRate);
  sqlite3_bind_double(s, 2, centreHz);
  sqlite3_bind_int64(s, 3, session_);
  sqlite3_step(s);
  sqlite3_finalize(s);
}

void Store::endSession(double utc) {
  if (!db_ || !session_) return;
  begin();
  sqlite3_stmt* s = prepare("UPDATE sessions SET end_utc=? WHERE id=?");
  sqlite3_bind_double(s, 1, utc);
  sqlite3_bind_int64(s, 2, session_);
  sqlite3_step(s);
  sqlite3_finalize(s);
  commit();
}

void Store::putEmission(int rx, const Emission& e, const std::string& label) {
  if (!db_ || !session_) return;
  begin();
  sqlite3_stmt* s = insEmission_;
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, session_);
  sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(e.id));
  sqlite3_bind_int(s, 3, rx);
  sqlite3_bind_double(s, 4, e.startUtc);
  sqlite3_bind_double(s, 5, e.endUtc);
  sqlite3_bind_double(s, 6, e.loHz);
  sqlite3_bind_double(s, 7, e.hiHz);
  sqlite3_bind_double(s, 8, e.centreHz);
  sqlite3_bind_double(s, 9, e.peakSnrDb);
  sqlite3_bind_double(s, 10, e.meanSnrDb);
  sqlite3_bind_double(s, 11, e.duty);
  sqlite3_bind_double(s, 12, e.driftHzPerSecond);
  sqlite3_bind_int(s, 13, e.ended ? 1 : 0);
  sqlite3_bind_text(s, 14, label.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(s);
}

void Store::putFeatures(int rx, uint64_t emissionId, const std::string& json) {
  if (!db_ || !session_) return;
  begin();
  sqlite3_stmt* u = prepare("UPDATE emissions SET features=? WHERE session_id=? AND rx=? AND emission_id=?");
  sqlite3_bind_text(u, 1, json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(u, 2, session_);
  sqlite3_bind_int(u, 3, rx);
  sqlite3_bind_int64(u, 4, static_cast<sqlite3_int64>(emissionId));
  sqlite3_step(u);
  sqlite3_finalize(u);
}

void Store::putDecode(int rx, const DecodeEvent& d) {
  if (!db_ || !session_) return;
  begin();
  sqlite3_stmt* s = insDecode_;
  sqlite3_reset(s);
  sqlite3_bind_int64(s, 1, session_);
  sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(d.emissionId));
  sqlite3_bind_int(s, 3, rx);
  sqlite3_bind_double(s, 4, d.utc);
  sqlite3_bind_text(s, 5, d.decoder.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(s, 6, d.freqHz);
  sqlite3_bind_double(s, 7, d.quality);
  sqlite3_bind_int(s, 8, d.first ? 1 : 0);
  sqlite3_bind_text(s, 9, d.text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(s);
  // A decode's label also goes to its emission (FT8 decodes can arrive
  // after the emission's last update).
  if (d.first && d.emissionId) {
    sqlite3_stmt* u =
        prepare("UPDATE emissions SET label=? WHERE session_id=? AND rx=? AND emission_id=? AND label=''");
    sqlite3_bind_text(u, 1, d.decoder.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(u, 2, session_);
    sqlite3_bind_int(u, 3, rx);
    sqlite3_bind_int64(u, 4, static_cast<sqlite3_int64>(d.emissionId));
    sqlite3_step(u);
    sqlite3_finalize(u);
  }
}

std::vector<StoredEmission> Store::emissions(const Query& q) {
  std::vector<StoredEmission> out;
  if (!db_) return out;
  commit();
  std::string sql =
      "SELECT session_id, emission_id, rx, start_utc, end_utc, lo_hz, hi_hz, centre_hz, peak_snr_db, mean_snr_db, "
      "duty, drift_hz_s, ended, label, features FROM emissions e WHERE end_utc >= ? AND start_utc <= ? AND centre_hz >= ? "
      "AND centre_hz <= ?";
  if (q.label == "-") sql += " AND label = ''";
  else if (!q.label.empty()) sql += " AND label = ?5";
  if (!q.text.empty())
    sql += " AND EXISTS (SELECT 1 FROM decodes d WHERE d.session_id = e.session_id AND d.rx = e.rx AND "
           "d.emission_id = e.emission_id AND instr(d.text, ?6) > 0)";
  if (q.session) sql += " AND session_id = ?8";
  sql += " ORDER BY start_utc DESC LIMIT ?7";
  sqlite3_stmt* s = prepare(sql.c_str());
  if (!s) return out;
  sqlite3_bind_double(s, 1, q.fromUtc);
  sqlite3_bind_double(s, 2, q.toUtc);
  sqlite3_bind_double(s, 3, q.minHz);
  sqlite3_bind_double(s, 4, q.maxHz);
  if (!q.label.empty() && q.label != "-") sqlite3_bind_text(s, 5, q.label.c_str(), -1, SQLITE_TRANSIENT);
  if (!q.text.empty()) sqlite3_bind_text(s, 6, q.text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s, 7, q.limit);
  if (q.session) sqlite3_bind_int64(s, 8, q.session);
  while (sqlite3_step(s) == SQLITE_ROW) {
    StoredEmission e;
    e.session = sqlite3_column_int64(s, 0);
    e.id = static_cast<uint64_t>(sqlite3_column_int64(s, 1));
    e.rx = sqlite3_column_int(s, 2);
    e.startUtc = sqlite3_column_double(s, 3);
    e.endUtc = sqlite3_column_double(s, 4);
    e.loHz = sqlite3_column_double(s, 5);
    e.hiHz = sqlite3_column_double(s, 6);
    e.centreHz = sqlite3_column_double(s, 7);
    e.peakSnrDb = sqlite3_column_double(s, 8);
    e.meanSnrDb = sqlite3_column_double(s, 9);
    e.duty = sqlite3_column_double(s, 10);
    e.driftHzPerSecond = sqlite3_column_double(s, 11);
    e.ended = sqlite3_column_int(s, 12) != 0;
    e.label = col(s, 13);
    e.features = col(s, 14);
    out.push_back(std::move(e));
  }
  sqlite3_finalize(s);
  return out;
}

static StoredDecode readDecode(sqlite3_stmt* s) {
  StoredDecode d;
  d.session = sqlite3_column_int64(s, 0);
  d.emissionId = static_cast<uint64_t>(sqlite3_column_int64(s, 1));
  d.rx = sqlite3_column_int(s, 2);
  d.utc = sqlite3_column_double(s, 3);
  d.decoder = col(s, 4);
  d.freqHz = sqlite3_column_double(s, 5);
  d.quality = sqlite3_column_double(s, 6);
  d.first = sqlite3_column_int(s, 7) != 0;
  d.text = col(s, 8);
  return d;
}

std::vector<StoredDecode> Store::decodes(const Query& q) {
  std::vector<StoredDecode> out;
  if (!db_) return out;
  commit();
  std::string sql =
      "SELECT session_id, emission_id, rx, utc, decoder, freq_hz, quality, first, text FROM decodes WHERE utc >= ? "
      "AND utc <= ? AND freq_hz >= ? AND freq_hz <= ?";
  if (!q.label.empty() && q.label != "-") sql += " AND decoder = ?5";
  if (!q.text.empty()) sql += " AND instr(text, ?6) > 0";
  if (q.session) sql += " AND session_id = ?8";
  sql += " ORDER BY utc DESC, id DESC LIMIT ?7";
  sqlite3_stmt* s = prepare(sql.c_str());
  if (!s) return out;
  sqlite3_bind_double(s, 1, q.fromUtc);
  sqlite3_bind_double(s, 2, q.toUtc);
  sqlite3_bind_double(s, 3, q.minHz);
  sqlite3_bind_double(s, 4, q.maxHz);
  if (!q.label.empty() && q.label != "-") sqlite3_bind_text(s, 5, q.label.c_str(), -1, SQLITE_TRANSIENT);
  if (!q.text.empty()) sqlite3_bind_text(s, 6, q.text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s, 7, q.limit);
  if (q.session) sqlite3_bind_int64(s, 8, q.session);
  while (sqlite3_step(s) == SQLITE_ROW) out.push_back(readDecode(s));
  sqlite3_finalize(s);
  return out;
}

std::vector<StoredDecode> Store::decodesOf(int64_t session, int rx, uint64_t emissionId) {
  std::vector<StoredDecode> out;
  if (!db_) return out;
  commit();
  sqlite3_stmt* s = prepare(
      "SELECT session_id, emission_id, rx, utc, decoder, freq_hz, quality, first, text FROM decodes WHERE "
      "session_id = ? AND rx = ? AND emission_id = ? ORDER BY utc, id");
  sqlite3_bind_int64(s, 1, session);
  sqlite3_bind_int(s, 2, rx);
  sqlite3_bind_int64(s, 3, static_cast<sqlite3_int64>(emissionId));
  while (sqlite3_step(s) == SQLITE_ROW) out.push_back(readDecode(s));
  sqlite3_finalize(s);
  return out;
}

std::string Store::exportCsv(const Query& q) {
  std::string out =
      "session,emission,rx,start_utc,end_utc,duration_s,centre_khz,bandwidth_hz,peak_snr_db,mean_snr_db,duty,"
      "drift_hz_s,label,features,text\n";
  for (const auto& e : emissions(q)) {
    std::string text;
    for (const auto& d : decodesOf(e.session, e.rx, e.id)) {
      if (!text.empty()) text += " | ";
      text += d.text;
    }
    out += std::to_string(e.session) + "," + std::to_string(e.id) + "," + std::to_string(e.rx) + "," +
           isoUtc(e.startUtc) + "," + isoUtc(e.endUtc) + "," + fmt("%.1f", e.endUtc - e.startUtc) + "," +
           fmt("%.3f", e.centreHz / 1000.0) + "," + fmt("%.0f", e.hiHz - e.loHz) + "," + fmt("%.1f", e.peakSnrDb) +
           "," + fmt("%.1f", e.meanSnrDb) + "," + fmt("%.2f", e.duty) + "," + fmt("%.3f", e.driftHzPerSecond) + "," +
           csvField(e.label) + "," + csvField(featuresSummary(e.features)) + "," + csvField(text) + "\n";
  }
  return out;
}

std::string Store::exportJson(const Query& q) {
  JsonWriter w;
  w.beginArray();
  for (const auto& e : emissions(q)) {
    w.beginObject();
    w.key("session").value(static_cast<int64_t>(e.session));
    w.key("emission").value(e.id);
    w.key("rx").value(e.rx);
    w.key("start_utc").value(isoUtc(e.startUtc));
    w.key("end_utc").value(isoUtc(e.endUtc));
    w.key("lo_hz").value(e.loHz);
    w.key("hi_hz").value(e.hiHz);
    w.key("centre_hz").value(e.centreHz);
    w.key("peak_snr_db").value(e.peakSnrDb);
    w.key("mean_snr_db").value(e.meanSnrDb);
    w.key("duty").value(e.duty);
    w.key("drift_hz_s").value(e.driftHzPerSecond);
    w.key("label").value(e.label);
    if (!e.features.empty()) w.key("features").raw(e.features);
    w.key("decodes").beginArray();
    for (const auto& d : decodesOf(e.session, e.rx, e.id)) {
      w.beginObject();
      w.key("utc").value(isoUtc(d.utc));
      w.key("decoder").value(d.decoder);
      w.key("freq_hz").value(d.freqHz);
      w.key("quality").value(d.quality);
      w.key("text").value(d.text);
      w.endObject();
    }
    w.endArray();
    w.endObject();
  }
  w.endArray();
  return w.str();
}

}  // namespace hfc
