// Writing a ZIP archive (PKWARE APPNOTE 6.3), for the diagnostic report:
// files stored as they are (no compression; the report is small text),
// names in UTF-8, the time given in local DOS format. No ZIP64: each file
// and the whole archive must stay under 4 GiB (makeZip returns an empty
// string otherwise).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hfc {

// CRC-32 (ISO-HDLC, as in ZIP and PNG); pass the previous value to continue.
uint32_t crc32(const void* data, std::size_t n, uint32_t crc = 0);

struct ZipEntry {
  std::string name;  // with '/' between folders
  std::string data;
};

// The archive's bytes. `unixTime` is written as every entry's time, in the
// local time zone (as ZIP tools show it).
std::string makeZip(const std::vector<ZipEntry>& entries, double unixTime);

// Reads back an archive made of stored entries (tests; and to check a
// report before it is offered). False if it is not one, or if a name could
// escape the folder it is extracted to.
bool readStoredZip(const std::string& zip, std::vector<ZipEntry>& out);

}  // namespace hfc
