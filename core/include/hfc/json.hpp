// Just enough JSON: a reader for the flat objects SDRconnect sends
// ({"event_type": "...", "property": "...", "value": "...", "device": "..."}),
// and a writer for those and for SigMF metadata files. Nested objects and
// arrays come out as their JSON text, to be parsed in turn (SigMF metadata).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hfc {

struct JsonField {
  std::string key;
  // Strings unescaped; numbers, true/false/null and nested objects or arrays
  // as their JSON text.
  std::string value;
  bool isString = false;
};

// Parses one JSON object into its top-level fields.
bool parseJsonObject(const std::string& text, std::vector<JsonField>& fields, std::string* error);
// Parses one JSON array into its elements (`key` left empty).
bool parseJsonArray(const std::string& text, std::vector<JsonField>& elements, std::string* error);
// The value of `key`, or "" if absent.
std::string jsonField(const std::vector<JsonField>& fields, const std::string& key);

// "text" with JSON escapes.
std::string jsonQuote(const std::string& text);

// Writes indented JSON. Calls must form a valid document: key() before every
// value inside an object.
class JsonWriter {
 public:
  JsonWriter& beginObject();
  JsonWriter& endObject();
  JsonWriter& beginArray();
  JsonWriter& endArray();
  JsonWriter& key(const std::string& k);
  JsonWriter& value(const std::string& s);
  JsonWriter& value(const char* s) { return value(std::string(s)); }
  JsonWriter& value(double v);
  JsonWriter& value(int64_t v);
  JsonWriter& value(uint64_t v);
  JsonWriter& value(int v) { return value(static_cast<int64_t>(v)); }
  JsonWriter& value(bool v);
  // A value that is already JSON, written as it is.
  JsonWriter& raw(const std::string& json);
  const std::string& str() const { return out_; }

 private:
  void beforeValue();
  void newline();
  std::string out_;
  std::vector<bool> first_;  // per open container: no element written yet
  bool afterKey_ = false;
};

}  // namespace hfc
