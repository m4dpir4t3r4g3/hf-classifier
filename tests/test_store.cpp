// Tests of the SQLite log.
#include <cstdio>
#include <string>

#include "hfc/store.hpp"
#include "sqlite3.h"
#include "test_framework.hpp"

using namespace hfc;

namespace {

Emission emission(uint64_t id, double start, double centre, double bw) {
  Emission e;
  e.id = id;
  e.startUtc = start;
  e.endUtc = start + 5;
  e.centreHz = centre;
  e.loHz = centre - bw / 2;
  e.hiHz = centre + bw / 2;
  e.peakSnrDb = 20;
  e.meanSnrDb = 15;
  e.duty = 1;
  e.confirmed = true;
  return e;
}

}  // namespace

TEST(store_keeps_emissions_and_decodes) {
  Store s;
  std::string err;
  CHECK(s.open(":memory:", &err));
  const int64_t session = s.beginSession(1000, "test", 250000, 7e6);
  CHECK(session > 0);
  Emission a = emission(1, 1000, 7010000, 300), b = emission(2, 1001, 7020000, 50);
  s.putEmission(0, a, "");
  s.putEmission(0, b, "");
  DecodeEvent d;
  d.emissionId = 1;
  d.utc = 1003;
  d.decoder = "NAVTEX";
  d.text = "ZCZC OA23";
  d.freqHz = 7010000;
  d.quality = 1;
  d.first = true;
  s.putDecode(0, d);
  d.first = false;
  d.text = "GALE WARNING, \"SEVERE\"";
  s.putDecode(0, d);
  // An update without a label keeps the label the decode gave.
  a.endUtc = 1010;
  a.ended = true;
  s.putEmission(0, a, "");

  Query q;
  auto all = s.emissions(q);
  CHECK(all.size() == 2);
  if (all.size() == 2) {
    CHECK(all[0].id == 2 && all[0].label.empty());  // newest first
    CHECK(all[1].id == 1 && all[1].label == "NAVTEX" && all[1].ended && all[1].endUtc == 1010);
  }
  q.label = "NAVTEX";
  CHECK(s.emissions(q).size() == 1);
  q.label = "-";
  CHECK(s.emissions(q).size() == 1 && s.emissions(q)[0].id == 2);
  q.label.clear();
  q.text = "GALE";
  CHECK(s.emissions(q).size() == 1);
  CHECK(s.decodes(q).size() == 1);
  q.text.clear();
  q.minHz = 7015000;
  CHECK(s.emissions(q).size() == 1);
  q.minHz = 0;
  CHECK(s.decodesOf(session, 0, 1).size() == 2);

  const std::string csv = s.exportCsv(q);
  CHECK(csv.find("NAVTEX,,\"ZCZC OA23 | GALE WARNING, \"\"SEVERE\"\"\"") != std::string::npos);
  CHECK(csv.find("7010.000,300,") != std::string::npos);
  const std::string json = s.exportJson(q);
  CHECK(json.find("\"label\": \"NAVTEX\"") != std::string::npos || json.find("\"label\":\"NAVTEX\"") != std::string::npos);
  CHECK(json.find("\\\"SEVERE\\\"") != std::string::npos);
  s.endSession(1020);
}

TEST(store_keeps_each_receivers_emissions_apart) {
  // Both receivers number their emissions from 1.
  Store s;
  CHECK(s.open(":memory:", nullptr));
  s.beginSession(1000, "dual", 2000000, 7e6);
  s.setSessionTuning(2000000, 7.1e6);
  Emission a = emission(1, 1000, 7010000, 300), b = emission(1, 1001, 8010000, 50);
  s.putEmission(0, a, "NAVTEX");
  s.putEmission(1, b, "CW");
  DecodeEvent d;
  d.emissionId = 1;
  d.decoder = "CW";
  d.text = "CQ DE DL1ABC";
  d.first = true;
  s.putDecode(1, d);
  const auto all = s.emissions(Query());
  CHECK(all.size() == 2);
  for (const auto& e : all) {
    if (e.rx == 0) CHECK(e.centreHz == 7010000 && e.label == "NAVTEX");
    if (e.rx == 1) CHECK(e.centreHz == 8010000 && e.label == "CW");
  }
  CHECK(s.decodesOf(s.session(), 0, 1).empty());
  CHECK(s.decodesOf(s.session(), 1, 1).size() == 1);
}

TEST(store_filters_by_session) {
  Store s;
  CHECK(s.open(":memory:", nullptr));
  const int64_t first = s.beginSession(1000, "run 1", 0, 0);
  s.putEmission(0, emission(1, 1000, 7010000, 300), "");
  const int64_t second = s.beginSession(2000, "run 2", 0, 0);
  s.putEmission(0, emission(1, 2000, 7020000, 300), "");
  Query q;
  CHECK(s.emissions(q).size() == 2);
  q.session = second;
  const auto mine = s.emissions(q);
  CHECK(mine.size() == 1 && mine[0].session == second && mine[0].centreHz == 7020000);
  q.session = first;
  CHECK(s.emissions(q).size() == 1);
}

TEST(store_upgrades_a_version_1_database) {
  // A log written before emissions were keyed by receiver: its tables are
  // kept under other names, new ones are made.
  const std::string path = "hfc_test_store_v1.db";
  std::remove(path.c_str());
  {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db,
                 "CREATE TABLE sessions (id INTEGER PRIMARY KEY, start_utc REAL NOT NULL, end_utc REAL, source TEXT,"
                 " sample_rate REAL, centre_hz REAL);"
                 "CREATE TABLE emissions (session_id INTEGER NOT NULL, emission_id INTEGER NOT NULL, rx INTEGER NOT NULL"
                 " DEFAULT 0, start_utc REAL, end_utc REAL, lo_hz REAL, hi_hz REAL, centre_hz REAL, peak_snr_db REAL,"
                 " mean_snr_db REAL, duty REAL, drift_hz_s REAL, ended INTEGER NOT NULL DEFAULT 0, label TEXT NOT NULL"
                 " DEFAULT '', PRIMARY KEY (session_id, emission_id));"
                 "CREATE TABLE decodes (id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL, emission_id INTEGER NOT"
                 " NULL, rx INTEGER NOT NULL DEFAULT 0, utc REAL, decoder TEXT, freq_hz REAL, quality REAL, first"
                 " INTEGER, text TEXT);"
                 "INSERT INTO sessions VALUES (1, 1000, NULL, 'old', 0, 0);"
                 "INSERT INTO emissions (session_id, emission_id, label) VALUES (1, 1, 'CW');",
                 nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }
  {
    Store s;
    std::string err;
    CHECK(s.open(path, &err));
    s.beginSession(2000, "new", 0, 0);
    s.putEmission(1, emission(1, 2000, 7010000, 300), "");
    s.putEmission(0, emission(1, 2000, 7020000, 300), "");
    CHECK(s.emissions(Query()).size() == 2);
  }
  {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db, "SELECT label FROM emissions_v1", -1, &q, nullptr);
    CHECK(q && sqlite3_step(q) == SQLITE_ROW &&
          std::string(reinterpret_cast<const char*>(sqlite3_column_text(q, 0))) == "CW");
    sqlite3_finalize(q);
    sqlite3_close(db);
  }
  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
}

TEST(store_keeps_features_and_upgrades_a_version_2_database) {
  // A version 2 log (no features column) gets the column; its rows stay.
  const std::string path = "hfc_test_store_v2.db";
  std::remove(path.c_str());
  {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db,
                 "CREATE TABLE sessions (id INTEGER PRIMARY KEY, start_utc REAL NOT NULL, end_utc REAL, source TEXT,"
                 " sample_rate REAL, centre_hz REAL);"
                 "CREATE TABLE emissions (session_id INTEGER NOT NULL, emission_id INTEGER NOT NULL, rx INTEGER NOT NULL"
                 " DEFAULT 0, start_utc REAL, end_utc REAL, lo_hz REAL, hi_hz REAL, centre_hz REAL, peak_snr_db REAL,"
                 " mean_snr_db REAL, duty REAL, drift_hz_s REAL, ended INTEGER NOT NULL DEFAULT 0, label TEXT NOT NULL"
                 " DEFAULT '', PRIMARY KEY (session_id, rx, emission_id));"
                 "CREATE TABLE decodes (id INTEGER PRIMARY KEY, session_id INTEGER NOT NULL, emission_id INTEGER NOT"
                 " NULL, rx INTEGER NOT NULL DEFAULT 0, utc REAL, decoder TEXT, freq_hz REAL, quality REAL, first"
                 " INTEGER, text TEXT);"
                 "INSERT INTO sessions VALUES (1, 1000, NULL, 'old', 0, 0);"
                 "INSERT INTO emissions (session_id, emission_id, start_utc, end_utc, centre_hz, label)"
                 " VALUES (1, 1, 1000, 1010, 7000000, 'CW');"
                 "PRAGMA user_version=2;",
                 nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }
  Store s;
  std::string err;
  CHECK(s.open(path, &err));
  CHECK(s.emissions(Query()).size() == 1 && s.emissions(Query())[0].label == "CW");
  s.beginSession(2000, "new", 0, 0);
  s.putEmission(0, emission(1, 2000, 7010000, 300), "");
  s.putFeatures(0, 1, "{\"modulation\":\"FSK\",\"summary\":\"2-FSK, shift 850 Hz, 75.0 Bd\"}");
  Query q;
  q.session = s.session();
  const auto es = s.emissions(q);
  CHECK(es.size() == 1 && es[0].features.find("\"FSK\"") != std::string::npos);
  CHECK(s.exportCsv(q).find(",2-FSK, shift 850 Hz, 75.0 Bd,") == std::string::npos);  // quoted: it has commas
  CHECK(s.exportCsv(q).find("\"2-FSK, shift 850 Hz, 75.0 Bd\"") != std::string::npos);
  CHECK(s.exportJson(q).find("\"modulation\":\"FSK\"") != std::string::npos);
  s.close();
  std::remove(path.c_str());
}
