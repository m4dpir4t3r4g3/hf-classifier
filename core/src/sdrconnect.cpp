#include "hfc/sdrconnect.hpp"

#include <cctype>
#include <vector>

#include "hfc/json.hpp"

namespace hfc::sdrconnect {

const char* deviceName(Device d) { return d == Device::Secondary ? "secondary" : "primary"; }

bool parseDevice(const std::string& value, Device& d) {
  // Case-insensitive, as SDRplay's own C# client reads it.
  std::string s = value;
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s.empty() || s == "primary") {
    d = Device::Primary;
    return true;
  }
  if (s == "secondary") {
    d = Device::Secondary;
    return true;
  }
  return false;
}

std::string encodeMessage(const Message& m) {
  // Every field is a JSON string, as in the API document's examples.
  std::string out = "{\"event_type\":" + jsonQuote(m.eventType) + ",\"property\":" + jsonQuote(m.property) +
                    ",\"value\":" + jsonQuote(m.value);
  if (!m.device.empty()) out += ",\"device\":" + jsonQuote(m.device);
  return out + "}";
}

bool decodeMessage(const std::string& json, Message& m, std::string* error) {
  std::vector<JsonField> fields;
  if (!parseJsonObject(json, fields, error)) return false;
  m = Message();
  for (const auto& f : fields) {
    if (f.key == "event_type") m.eventType = f.value;
    else if (f.key == "property") m.property = f.value;
    else if (f.key == "value") m.value = f.value;
    else if (f.key == "device") m.device = f.value;
  }
  if (m.eventType.empty()) {
    if (error) *error = "message without event_type";
    return false;
  }
  return true;
}

}  // namespace hfc::sdrconnect
