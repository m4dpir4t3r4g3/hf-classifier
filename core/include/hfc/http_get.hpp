// Fetching a file over plain HTTP (no TLS: the lists the program offers to
// download, KiwiSDR's community labels and EiBi's schedules, are served
// over http). Follows up to 3 redirects to http addresses; a redirect to
// https is reported as such, for the user to download by hand.
#pragma once

#include <cstddef>
#include <string>

namespace hfc {

struct HttpResult {
  int status = 0;            // the last response's status (0: no response)
  std::string body;
  std::string error;         // "" on success (status 200)
  std::string finalUrl;
};

// GET `url` ("http://host[:port]/path"), at most `maxBytes` of body, each
// read waiting at most `timeoutMs`. A "User-Agent" names the program.
HttpResult httpGet(const std::string& url, int timeoutMs = 15000, std::size_t maxBytes = 32u << 20);

}  // namespace hfc
