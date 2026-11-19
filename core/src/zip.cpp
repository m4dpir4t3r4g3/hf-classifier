#include "hfc/zip.hpp"

#include <ctime>

namespace hfc {

uint32_t crc32(const void* data, std::size_t n, uint32_t crc) {
  static uint32_t table[256];
  static const bool made = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)made;
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

namespace {

void le16(std::string& s, uint32_t v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void le32(std::string& s, uint32_t v) {
  le16(s, v & 0xFFFF);
  le16(s, v >> 16);
}
uint32_t get16(const std::string& s, std::size_t at) {
  return static_cast<uint8_t>(s[at]) | static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8;
}
uint32_t get32(const std::string& s, std::size_t at) { return get16(s, at) | get16(s, at + 2) << 16; }

}  // namespace

std::string makeZip(const std::vector<ZipEntry>& entries, double unixTime) {
  std::time_t t = static_cast<std::time_t>(unixTime);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  const int year = tm.tm_year + 1900 < 1980 ? 1980 : tm.tm_year + 1900;
  const uint32_t dosTime = static_cast<uint32_t>(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
  const uint32_t dosDate = static_cast<uint32_t>((year - 1980) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
  const uint32_t kUtf8 = 1u << 11;

  // No ZIP64: refuse what the 32-bit fields cannot hold rather than write a corrupt archive.
  constexpr uint64_t kLimit = 0xFFFFFFFFull;
  uint64_t total = 22;
  for (const ZipEntry& e : entries) {
    if (e.name.size() > 0xFFFF || e.data.size() > kLimit) return std::string();
    total += 76 + 2 * static_cast<uint64_t>(e.name.size()) + e.data.size();
  }
  if (entries.size() > 0xFFFF || total > kLimit) return std::string();

  std::string out, central;
  out.reserve(static_cast<std::size_t>(total));
  for (const ZipEntry& e : entries) {
    const uint32_t crc = crc32(e.data.data(), e.data.size());
    const uint32_t offset = static_cast<uint32_t>(out.size());
    const uint32_t size = static_cast<uint32_t>(e.data.size());
    // Local file header.
    le32(out, 0x04034b50);
    le16(out, 20);     // version needed: 2.0
    le16(out, kUtf8);  // flags
    le16(out, 0);      // stored
    le16(out, dosTime);
    le16(out, dosDate);
    le32(out, crc);
    le32(out, size);
    le32(out, size);
    le16(out, static_cast<uint32_t>(e.name.size()));
    le16(out, 0);
    out += e.name;
    out += e.data;
    // Its central directory entry.
    le32(central, 0x02014b50);
    le16(central, 20);  // made by: MS-DOS / FAT attributes, 2.0
    le16(central, 20);
    le16(central, kUtf8);
    le16(central, 0);
    le16(central, dosTime);
    le16(central, dosDate);
    le32(central, crc);
    le32(central, size);
    le32(central, size);
    le16(central, static_cast<uint32_t>(e.name.size()));
    le16(central, 0);  // extra
    le16(central, 0);  // comment
    le16(central, 0);  // disk
    le16(central, 0);  // internal attributes
    le32(central, 0);  // external attributes
    le32(central, offset);
    central += e.name;
  }
  const uint32_t centralAt = static_cast<uint32_t>(out.size());
  out += central;
  le32(out, 0x06054b50);
  le16(out, 0);
  le16(out, 0);
  le16(out, static_cast<uint32_t>(entries.size()));
  le16(out, static_cast<uint32_t>(entries.size()));
  le32(out, static_cast<uint32_t>(central.size()));
  le32(out, centralAt);
  le16(out, 0);
  return out;
}

bool readStoredZip(const std::string& zip, std::vector<ZipEntry>& out) {
  out.clear();
  if (zip.size() < 22) return false;
  // The end record is the last 22 bytes unless an archive comment follows it.
  std::size_t end = zip.size() - 22;
  const std::size_t lowest = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
  while (get32(zip, end) != 0x06054b50) {
    if (end == lowest) return false;
    --end;
  }
  if (end + 22 + get16(zip, end + 20) != zip.size()) return false;
  const std::size_t count = get16(zip, end + 10);
  std::size_t at = get32(zip, end + 16);
  for (std::size_t i = 0; i < count; ++i) {
    if (at > zip.size() || zip.size() - at < 46 || get32(zip, at) != 0x02014b50) return false;
    const uint32_t method = get16(zip, at + 10), crc = get32(zip, at + 16), size = get32(zip, at + 20);
    const std::size_t nameLen = get16(zip, at + 28), extra = get16(zip, at + 30), comment = get16(zip, at + 32);
    const std::size_t local = get32(zip, at + 42);
    if (method != 0 || zip.size() - at - 46 < nameLen) return false;
    ZipEntry e;
    e.name = zip.substr(at + 46, nameLen);
    // Names that would escape a folder when extracted.
    if (e.name.empty() || e.name[0] == '/' || e.name.find('\\') != std::string::npos ||
        e.name.find(std::string(1, '\0')) != std::string::npos || e.name == ".." ||
        e.name.compare(0, 3, "../") == 0 || e.name.find("/../") != std::string::npos ||
        (e.name.size() >= 3 && e.name.compare(e.name.size() - 3, 3, "/..") == 0))
      return false;
    if (local > zip.size() || zip.size() - local < 30 || get32(zip, local) != 0x04034b50) return false;
    const std::size_t data = local + 30 + get16(zip, local + 26) + get16(zip, local + 28);
    if (data > zip.size() || zip.size() - data < size) return false;
    e.data = zip.substr(data, size);
    if (crc32(e.data.data(), e.data.size()) != crc) return false;
    out.push_back(std::move(e));
    at += 46 + nameLen + extra + comment;
  }
  return true;
}

}  // namespace hfc
