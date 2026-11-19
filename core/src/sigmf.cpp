#include "hfc/sigmf.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "hfc/file.hpp"
#include "hfc/json.hpp"

namespace hfc {

std::string isoUtc(double unixSeconds) {
  // Years 1970 to 9999; anything else (or not a number) would overflow time_t.
  if (!(unixSeconds >= 0 && unixSeconds < 253402300800.0)) unixSeconds = 0;
  double whole = std::floor(unixSeconds);
  long micros = std::lround((unixSeconds - whole) * 1e6);
  if (micros >= 1000000) {
    whole += 1;
    micros -= 1000000;
  }
  const std::time_t t = static_cast<std::time_t>(whole);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, micros);
  return buf;
}

SigmfWriter::~SigmfWriter() { close(nullptr); }

bool SigmfWriter::open(const std::string& basePath, const Global& g, std::string* error) {
  close(nullptr);
  base_ = basePath;
  global_ = g;
  written_ = 0;
  captures_.clear();
  annotations_.clear();
  data_ = openFile(basePath + ".sigmf-data", "wb");
  if (!data_) {
    if (error) *error = "cannot create " + basePath + ".sigmf-data";
    return false;
  }
  return true;
}

bool SigmfWriter::write(const int16_t* iq, std::size_t pairs, bool newCapture, double centerHz, double utc) {
  if (!data_) return false;
  if (newCapture || captures_.empty()) {
    // Two segments may not start at the same sample.
    if (!captures_.empty() && captures_.back().start == written_) captures_.pop_back();
    captures_.push_back({written_, centerHz, utc});
  }
  // ci16_le: write byte by byte so that the file is little endian on any host.
  std::vector<uint8_t> bytes(pairs * 4);
  for (std::size_t i = 0; i < pairs * 2; ++i) {
    const auto v = static_cast<uint16_t>(iq[i]);
    bytes[2 * i] = static_cast<uint8_t>(v);
    bytes[2 * i + 1] = static_cast<uint8_t>(v >> 8);
  }
  if (std::fwrite(bytes.data(), 1, bytes.size(), data_) != bytes.size()) return false;
  written_ += pairs;
  return true;
}

void SigmfWriter::addCapture(uint64_t start, double centerHz, double utc) {
  auto it = captures_.begin();
  while (it != captures_.end() && it->start < start) ++it;
  if (it != captures_.end() && it->start == start) *it = {start, centerHz, utc};
  else captures_.insert(it, {start, centerHz, utc});
}

void SigmfWriter::annotate(uint64_t start, uint64_t count, const std::string& label, const std::string& comment) {
  annotations_.push_back({start, count, label, comment});
}

bool SigmfWriter::close(std::string* error) {
  if (!data_) return true;
  const bool dataOk = std::fclose(data_) == 0;
  data_ = nullptr;
  JsonWriter j;
  j.beginObject();
  j.key("global").beginObject();
  j.key("core:datatype").value("ci16_le");
  j.key("core:sample_rate").value(global_.sampleRate);
  j.key("core:version").value("1.2.0");
  j.key("core:num_channels").value(1);
  if (!global_.hardware.empty()) j.key("core:hw").value(global_.hardware);
  if (!global_.recorder.empty()) j.key("core:recorder").value(global_.recorder);
  if (!global_.description.empty()) j.key("core:description").value(global_.description);
  j.endObject();
  j.key("captures").beginArray();
  for (const auto& c : captures_) {
    j.beginObject();
    j.key("core:sample_start").value(c.start);
    if (c.centerHz > 0) j.key("core:frequency").value(c.centerHz);
    if (c.utc > 0) j.key("core:datetime").value(isoUtc(c.utc));
    j.endObject();
  }
  j.endArray();
  j.key("annotations").beginArray();
  std::stable_sort(annotations_.begin(), annotations_.end(),
                   [](const Annotation& a, const Annotation& b) { return a.start < b.start; });
  for (const auto& a : annotations_) {
    j.beginObject();
    j.key("core:sample_start").value(a.start);
    if (a.count > 0) j.key("core:sample_count").value(a.count);
    if (!a.label.empty()) j.key("core:label").value(a.label);
    if (!a.comment.empty()) j.key("core:comment").value(a.comment);
    j.endObject();
  }
  j.endArray();
  j.endObject();
  std::FILE* meta = openFile(base_ + ".sigmf-meta", "wb");
  bool metaOk = meta != nullptr;
  if (meta) {
    const std::string text = j.str() + "\n";
    metaOk = std::fwrite(text.data(), 1, text.size(), meta) == text.size();
    metaOk = std::fclose(meta) == 0 && metaOk;
  }
  if (!(dataOk && metaOk) && error) *error = "cannot write " + base_ + (dataOk ? ".sigmf-meta" : ".sigmf-data");
  return dataOk && metaOk;
}

namespace {

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant).
long daysFromCivil(long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

}  // namespace

double parseIsoUtc(const std::string& t) {
  // YYYY-MM-DDTHH:MM:SS[.fraction](Z|+00:00|-00:00), nothing else.
  int y, mo, d, h, mi, n = 0;
  if (std::sscanf(t.c_str(), "%4d-%2d-%2dT%2d:%2d:%n", &y, &mo, &d, &h, &mi, &n) != 5 || n != 17) return -1;
  std::size_t i = 17;
  if (i + 2 > t.size() || !std::isdigit(static_cast<unsigned char>(t[i])) ||
      !std::isdigit(static_cast<unsigned char>(t[i + 1])))
    return -1;
  double sec = (t[i] - '0') * 10 + (t[i + 1] - '0');
  i += 2;
  if (i < t.size() && t[i] == '.') {
    double scale = 0.1;
    ++i;
    if (i >= t.size() || !std::isdigit(static_cast<unsigned char>(t[i]))) return -1;
    for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, scale /= 10) sec += (t[i] - '0') * scale;
  }
  const std::string zone = t.substr(i);
  if (zone != "Z" && zone != "+00:00" && zone != "-00:00") return -1;  // other offsets are not UTC
  static const int days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1970 || y > 9999 || mo < 1 || mo > 12 || d < 1 || d > days[mo - 1] || h > 23 || mi > 59 || sec >= 61)
    return -1;
  if (mo == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return -1;
  return static_cast<double>(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))) * 86400.0 +
         h * 3600.0 + mi * 60.0 + sec;
}

SigmfReader::~SigmfReader() { close(); }

void SigmfReader::close() {
  if (data_) std::fclose(data_);
  data_ = nullptr;
  captures_.clear();
  pos_ = total_ = 0;
}

bool SigmfReader::open(const std::string& path, std::string* error) {
  close();
  std::string base = path;
  for (const char* ext : {".sigmf-meta", ".sigmf-data"}) {
    const std::string e(ext);
    if (base.size() > e.size() && base.compare(base.size() - e.size(), e.size(), e) == 0) base.resize(base.size() - e.size());
  }
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    close();
    return false;
  };
  std::FILE* mf = openFile(base + ".sigmf-meta", "rb");
  if (!mf) return fail("cannot open " + base + ".sigmf-meta");
  std::string meta;
  char buf[65536];
  std::size_t got;
  while ((got = std::fread(buf, 1, sizeof(buf), mf)) > 0) {
    meta.append(buf, got);
    if (meta.size() > (std::size_t(64) << 20)) {
      std::fclose(mf);
      return fail(base + ".sigmf-meta is larger than 64 MB: not a SigMF metadata file?");
    }
  }
  std::fclose(mf);

  std::vector<JsonField> top, global, list, item;
  std::string err;
  if (!parseJsonObject(meta, top, &err)) return fail("bad SigMF metadata: " + err);
  if (!parseJsonObject(jsonField(top, "global"), global, &err)) return fail("SigMF metadata without a global object");
  type_ = jsonField(global, "core:datatype");
  rate_ = std::atof(jsonField(global, "core:sample_rate").c_str());
  description_ = jsonField(global, "core:description");
  if (type_ == "ci16_le") bytesPerSample_ = 4;
  else if (type_ == "cf32_le") bytesPerSample_ = 8;
  else if (type_ == "ci8" || type_ == "cu8") bytesPerSample_ = 2;
  else return fail("unsupported SigMF datatype '" + type_ + "' (ci16_le, cf32_le, ci8, cu8 are read)");
  if (!(rate_ > 0)) return fail("SigMF metadata without core:sample_rate");
  const std::string nch = jsonField(global, "core:num_channels");
  if (!nch.empty() && nch != "1") return fail("only single-channel recordings are read");
  if (parseJsonArray(jsonField(top, "captures"), list, nullptr)) {
    for (const auto& c : list) {
      if (!parseJsonObject(c.value, item, nullptr)) continue;
      Capture cap;
      cap.start = std::strtoull(jsonField(item, "core:sample_start").c_str(), nullptr, 10);
      cap.centerHz = std::atof(jsonField(item, "core:frequency").c_str());
      const double u = parseIsoUtc(jsonField(item, "core:datetime"));
      cap.utc = u > 0 ? u : 0;
      captures_.push_back(cap);
    }
  }
  std::sort(captures_.begin(), captures_.end(), [](const Capture& a, const Capture& b) { return a.start < b.start; });
  if (captures_.empty() || captures_.front().start != 0) captures_.insert(captures_.begin(), Capture());

  data_ = openFile(base + ".sigmf-data", "rb");
  if (!data_) return fail("cannot open " + base + ".sigmf-data");
  // 64-bit offsets: recordings pass 2 GB in minutes.
#ifdef _WIN32
  _fseeki64(data_, 0, SEEK_END);
  const long long bytes = _ftelli64(data_);
  _fseeki64(data_, 0, SEEK_SET);
#else
  fseeko(data_, 0, SEEK_END);
  const long long bytes = static_cast<long long>(ftello(data_));
  fseeko(data_, 0, SEEK_SET);
#endif
  total_ = bytes > 0 ? static_cast<uint64_t>(bytes) / static_cast<uint64_t>(bytesPerSample_) : 0;
  pos_ = 0;
  lastUtc_ = 0;
  lastUtcAt_ = 0;
  return true;
}

std::size_t SigmfReader::read(int16_t* iq, std::size_t pairs, Capture& cap, double& utc) {
  if (!data_ || pos_ >= total_) return 0;
  // The segment at pos_, and where the next one starts.
  std::size_t k = 0;
  while (k + 1 < captures_.size() && captures_[k + 1].start <= pos_) ++k;
  cap = captures_[k];
  const uint64_t end = k + 1 < captures_.size() ? captures_[k + 1].start : total_;
  const std::size_t n = static_cast<std::size_t>(std::min<uint64_t>(pairs, end - pos_));
  raw_.resize(n * static_cast<std::size_t>(bytesPerSample_));
  const std::size_t got = std::fread(raw_.data(), static_cast<std::size_t>(bytesPerSample_), n, data_);
  const uint8_t* r = raw_.data();
  const std::size_t values = 2 * got;
  if (type_ == "ci16_le") {
    for (std::size_t i = 0; i < values; ++i) iq[i] = static_cast<int16_t>(r[2 * i] | (r[2 * i + 1] << 8));
  } else if (type_ == "ci8") {
    for (std::size_t i = 0; i < values; ++i) iq[i] = static_cast<int16_t>(static_cast<int8_t>(r[i]) * 256);
  } else if (type_ == "cu8") {
    for (std::size_t i = 0; i < values; ++i) iq[i] = static_cast<int16_t>((static_cast<int>(r[i]) - 128) * 256);
  } else {
    for (std::size_t i = 0; i < values; ++i) {
      float f;
      std::memcpy(&f, r + 4 * i, 4);  // little-endian hosts
      // Full scale 1.0, as is usual for cf32 recordings; clipped beyond.
      // Not-a-number samples become 0.
      iq[i] = static_cast<int16_t>(
          std::isfinite(f) ? std::lround(std::max(-1.0f, std::min(1.0f, f)) * 32767.0f) : 0L);
    }
  }
  if (cap.utc > 0) {
    lastUtc_ = cap.utc + static_cast<double>(pos_ - cap.start) / rate_;
  } else {
    lastUtc_ += static_cast<double>(pos_ - lastUtcAt_) / rate_;
  }
  lastUtcAt_ = pos_;
  utc = lastUtc_;
  pos_ += got;
  return got;
}

}  // namespace hfc
