// SHA-1 and Base64, as needed by the WebSocket opening handshake (RFC 6455,
// section 4: Sec-WebSocket-Accept = base64(sha1(key + GUID))).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hfc {

std::array<uint8_t, 20> sha1(const void* data, std::size_t n);
std::string base64Encode(const void* data, std::size_t n);
// False on characters outside the Base64 alphabet or bad padding.
bool base64Decode(const std::string& text, std::vector<uint8_t>& out);
// The Sec-WebSocket-Accept value a server must answer to `key`.
std::string websocketAccept(const std::string& key);

}  // namespace hfc
