#include "hfc/http_get.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "hfc/net.hpp"

namespace hfc {

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool splitUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path, std::string& error) {
  const std::string l = lower(url);
  if (l.rfind("https://", 0) == 0) {
    error = "https_not_supported";
    return false;
  }
  if (l.rfind("http://", 0) != 0) {
    error = "bad_url";
    return false;
  }
  const std::string rest = url.substr(7);
  const std::size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  port = 80;
  const std::size_t colon = hostPort.find(':');
  if (colon != std::string::npos) {
    port = static_cast<uint16_t>(std::atoi(hostPort.c_str() + colon + 1));
    hostPort.resize(colon);
  }
  host = hostPort;
  if (host.empty() || port == 0) {
    error = "bad_url";
    return false;
  }
  return true;
}

// Decodes a chunked body in place; false if it is malformed.
bool unchunk(std::string& body) {
  std::string out;
  std::size_t at = 0;
  for (;;) {
    const std::size_t eol = body.find("\r\n", at);
    if (eol == std::string::npos) return false;
    const std::size_t n = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
    at = eol + 2;
    if (n == 0) break;
    if (at + n > body.size()) return false;
    out.append(body, at, n);
    at += n + 2;
  }
  body.swap(out);
  return true;
}

}  // namespace

HttpResult httpGet(const std::string& url, int timeoutMs, std::size_t maxBytes) {
  HttpResult r;
  std::string current = url;
  for (int hop = 0; hop < 4; ++hop) {
    r.finalUrl = current;
    std::string host, path;
    uint16_t port = 80;
    if (!splitUrl(current, host, port, path, r.error)) return r;
    TcpSocket s;
    std::string err;
    if (!s.connect(host, port, timeoutMs, &err)) {
      r.error = "cannot_connect: " + err;
      return r;
    }
    s.setRecvTimeout(timeoutMs);
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + (port != 80 ? ":" + std::to_string(port) : "") +
                            "\r\nUser-Agent: HF-Classifier\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    if (!s.sendAll(req.data(), req.size())) {
      r.error = "send_failed";
      return r;
    }
    std::string data;
    char buf[16384];
    for (;;) {
      const long n = s.recvSome(buf, sizeof buf);
      if (n <= 0) break;
      data.append(buf, static_cast<std::size_t>(n));
      if (data.size() > maxBytes + 65536) {
        r.error = "too_large";
        return r;
      }
    }
    const std::size_t headEnd = data.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
      r.error = data.empty() ? "no_response" : "bad_response";
      return r;
    }
    const std::string head = data.substr(0, headEnd);
    r.body = data.substr(headEnd + 4);
    const std::size_t sp = head.find(' ');
    r.status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
    const std::string lh = lower(head);
    auto header = [&](const std::string& name) -> std::string {
      const std::size_t at = lh.find("\r\n" + name + ":");
      if (at == std::string::npos) return "";
      std::size_t from = at + 2 + name.size() + 1;
      const std::size_t to = head.find("\r\n", from);
      while (from < head.size() && head[from] == ' ') ++from;
      return head.substr(from, (to == std::string::npos ? head.size() : to) - from);
    };
    if (r.status >= 300 && r.status < 400) {
      std::string loc = header("location");
      if (loc.empty()) {
        r.error = "bad_redirect";
        return r;
      }
      if (loc[0] == '/') loc = "http://" + host + (port != 80 ? ":" + std::to_string(port) : "") + loc;
      current = loc;
      continue;
    }
    if (lower(header("transfer-encoding")).find("chunked") != std::string::npos && !unchunk(r.body)) {
      r.error = "bad_response";
      return r;
    }
    if (r.body.size() > maxBytes) {
      r.error = "too_large";
      return r;
    }
    if (r.status != 200) r.error = "http_" + std::to_string(r.status);
    return r;
  }
  r.error = "too_many_redirects";
  return r;
}

}  // namespace hfc
