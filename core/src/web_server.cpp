#include "hfc/web_server.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <deque>

#include "hfc/websocket.hpp"

namespace hfc {

namespace {

constexpr std::size_t kMaxQueued = 64;     // messages per client before droppable ones are dropped
constexpr std::size_t kMaxQueuedAll = 4096; // ... and before a client that does not read is disconnected
constexpr std::size_t kMaxConnections = 64; // threads: connections beyond this are dropped
constexpr int kHeaderTimeoutMs = 5000;      // for the whole request header, not for each read

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Position of "\r\nname:" in the header, ignoring case, from `from`; no copy of the header.
std::size_t findHeader(const std::string& header, const std::string& name, std::size_t from) {
  const std::size_t n = name.size() + 3;
  for (std::size_t p = from; p + n <= header.size(); ++p) {
    if (header[p] != '\r' || header[p + 1] != '\n') continue;
    std::size_t k = 0;
    while (k < name.size() && std::tolower(static_cast<unsigned char>(header[p + 2 + k])) ==
                                  std::tolower(static_cast<unsigned char>(name[k])))
      ++k;
    if (k == name.size() && header[p + 2 + k] == ':') return p;
  }
  return std::string::npos;
}

int headerCount(const std::string& header, const std::string& name) {
  int count = 0;
  for (std::size_t p = findHeader(header, name, 0); p != std::string::npos; p = findHeader(header, name, p + 2)) ++count;
  return count;
}

std::string headerField(const std::string& header, const std::string& name) {
  const std::size_t p = findHeader(header, name, 0);
  if (p == std::string::npos) return "";
  std::size_t b = p + name.size() + 3;
  const std::size_t e = header.find("\r\n", b);
  while (b < e && (header[b] == ' ' || header[b] == '\t')) ++b;
  std::string v = header.substr(b, e - b);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
  return v;
}

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

void sendResponse(TcpSocket& sock, const HttpResponse& r) {
  std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) +
                     "\r\nContent-Type: " + r.contentType + "\r\nContent-Length: " + std::to_string(r.body.size()) +
                     "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" +
                     r.extraHeaders + "\r\n";
  sock.sendAll(head.data(), head.size());
  if (!r.body.empty()) sock.sendAll(r.body.data(), r.body.size());
}

}  // namespace

std::string urlDecode(const std::string& s, bool plusIsSpace) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2])) && !(s[i + 1] == '0' && s[i + 2] == '0')) {
      o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));  // %00 stays as text: no NUL in paths
      i += 2;
    } else if (s[i] == '+' && plusIsSpace) {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

HttpResponse HttpResponse::notFound() {
  HttpResponse r;
  r.status = 404;
  r.body = "not found";
  return r;
}

struct WebServer::Client {
  std::shared_ptr<WebSocket> ws;
  std::mutex m;
  std::condition_variable cv;
  struct Item {
    std::shared_ptr<const std::string> data;  // shared by every client it goes to
    bool binary;
  };
  std::deque<Item> q;
  bool closed = false;
  std::thread sender;
};

WebServer::~WebServer() { stop(); }

void WebServer::websocket(const std::string& path, WsOpenFn onOpen, WsMessageFn onMessage) {
  wsPath_ = path;
  wsOpen_ = std::move(onOpen);
  wsMessage_ = std::move(onMessage);
}

bool WebServer::start(const std::string& address, uint16_t port, std::string* error) {
  if (!listener_.listen(address, port, error)) return false;
  running_ = true;
  acceptThread_ = std::thread([this] { acceptLoop(); });
  return true;
}

void WebServer::stop() {
  if (!running_.exchange(false)) return;
  if (acceptThread_.joinable()) acceptThread_.join();
  listener_.close();
  {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& [id, c] : clients_) c->ws->shutdown();
  }
  for (auto& t : connThreads_)
    if (t.joinable()) t.join();
  connThreads_.clear();
  connDone_.clear();
}

void WebServer::reap() {
  for (std::size_t i = 0; i < connThreads_.size();) {
    if (connDone_[i]->load()) {
      connThreads_[i].join();
      connThreads_.erase(connThreads_.begin() + static_cast<std::ptrdiff_t>(i));
      connDone_.erase(connDone_.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void WebServer::acceptLoop() {
  while (running_) {
    TcpSocket s = listener_.accept(200);
    reap();
    if (!s.valid()) continue;
    if (connThreads_.size() >= kMaxConnections) {
      HttpResponse r;
      r.status = 503;
      r.body = "too many connections";
      sendResponse(s, r);
      continue;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    connDone_.push_back(done);
    connThreads_.emplace_back([this, done, sock = std::move(s)]() mutable {
      serve(std::move(sock));
      *done = true;
    });
  }
}

void WebServer::serve(TcpSocket sock) {
  std::string header;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeaderTimeoutMs);
  char buf[2048];
  while (header.find("\r\n\r\n") == std::string::npos) {
    if (header.size() > 16384) return;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return;
    sock.setRecvTimeout(static_cast<int>(left.count()));
    const long r = sock.recvSome(buf, sizeof(buf));
    if (r <= 0) return;
    header.append(buf, static_cast<std::size_t>(r));
  }
  const std::size_t headerEnd = header.find("\r\n\r\n") + 4;
  const std::string early = header.substr(headerEnd);  // a WebSocket's first frames, if sent at once
  header.resize(headerEnd);
  sock.setRecvTimeout(0);

  // Only pages asked for by this machine's browser under this server's own
  // name: a Host other than localhost / 127.0.0.1 (DNS rebinding) or a
  // WebSocket from another site's page (Origin) is refused.
  if (headerCount(header, "Host") != 1) {
    HttpResponse r;
    r.status = 400;
    r.body = "exactly one Host header is required";
    sendResponse(sock, r);
    return;
  }
  const std::string host = lower(headerField(header, "Host"));
  // Browsers leave the port out of Host and Origin when it is the default.
  const std::string p = port() == 80 ? "" : ":" + std::to_string(port());
  if (host != "127.0.0.1" + p && host != "localhost" + p && host != "[::1]" + p) {
    HttpResponse r;
    r.status = 403;
    r.body = "forbidden host";
    sendResponse(sock, r);
    return;
  }

  HttpRequest req;
  const std::size_t sp1 = header.find(' '), sp2 = header.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) return;
  req.method = header.substr(0, sp1);
  const std::string target = header.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::size_t qm = target.find('?');
  req.path = urlDecode(target.substr(0, qm), false);
  if (qm != std::string::npos) {
    std::size_t i = qm + 1;
    while (i <= target.size()) {
      std::size_t amp = target.find('&', i);
      if (amp == std::string::npos) amp = target.size();
      const std::string kv = target.substr(i, amp - i);
      const std::size_t eq = kv.find('=');
      if (!kv.empty())
        req.query[urlDecode(kv.substr(0, eq), true)] = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1), true);
      i = amp + 1;
    }
  }
  req.header = header;

  if (!wsPath_.empty() && req.path == wsPath_ && !headerField(header, "Upgrade").empty()) {
    const std::string origin = lower(headerField(header, "Origin"));
    if (!origin.empty() && origin != "http://127.0.0.1" + p && origin != "http://localhost" + p &&
        origin != "http://[::1]" + p) {
      HttpResponse r;
      r.status = 403;
      r.body = "forbidden origin";
      sendResponse(sock, r);
      return;
    }
    runWebSocket(std::move(sock), header, early);
    return;
  }
  if (req.method != "GET") {
    HttpResponse r;
    r.status = 405;
    r.body = "GET only";
    sendResponse(sock, r);
    return;
  }
  const Handler* h = nullptr;
  auto it = routes_.find(req.path);
  if (it != routes_.end()) {
    h = &it->second;
  } else {
    std::size_t best = 0;
    for (const auto& [prefix, fn] : routes_)
      if (prefix.size() > 1 && prefix.back() == '/' && req.path.compare(0, prefix.size(), prefix) == 0 &&
          prefix.size() > best) {
        best = prefix.size();
        h = &fn;
      }
  }
  sendResponse(sock, h ? (*h)(req) : HttpResponse::notFound());
}

void WebServer::runWebSocket(TcpSocket sock, const std::string& header, const std::string& early) {
  auto c = std::make_shared<Client>();
  c->ws = std::make_shared<WebSocket>();
  if (!c->ws->acceptUpgrade(std::move(sock), header, nullptr, early)) return;
  int id;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (!running_) return;
    id = nextClient_++;
    clients_[id] = c;
  }
  c->sender = std::thread([c] {
    std::unique_lock<std::mutex> l(c->m);
    for (;;) {
      c->cv.wait(l, [&] { return c->closed || !c->q.empty(); });
      if (c->closed) return;
      Client::Item item = std::move(c->q.front());
      c->q.pop_front();
      l.unlock();
      const bool ok = item.binary ? c->ws->sendBinary(item.data->data(), item.data->size()) : c->ws->sendText(*item.data);
      l.lock();
      if (!ok) {
        c->closed = true;
        return;
      }
    }
  });
  if (wsOpen_) wsOpen_(id);
  WsMessage msg;
  while (c->ws->receive(msg, nullptr))
    if (!msg.binary && wsMessage_) wsMessage_(id, msg.text());
  {
    std::lock_guard<std::mutex> l(c->m);
    c->closed = true;
  }
  c->cv.notify_all();
  // Shut the socket first: a sender blocked writing to a peer that no longer
  // reads would otherwise never return to be joined.
  c->ws->shutdown();
  c->sender.join();
  std::lock_guard<std::mutex> l(mu_);
  clients_.erase(id);
}

void WebServer::enqueue(const std::shared_ptr<Client>& c, const std::shared_ptr<const std::string>& data, bool binary,
                        bool droppable) {
  bool tooSlow = false;
  {
    std::lock_guard<std::mutex> l(c->m);
    if (c->closed) return;
    if (droppable && c->q.size() >= kMaxQueued) return;
    if (c->q.size() >= kMaxQueuedAll) {
      // Not reading at all (a stopped tab, a stuck connection): let it go
      // rather than hold an ever-growing queue.
      c->closed = true;
      c->q.clear();
      tooSlow = true;
    } else {
      c->q.push_back({data, binary});
    }
  }
  if (tooSlow) c->ws->shutdown();
  c->cv.notify_all();
}

void WebServer::broadcastText(const std::string& text, bool droppable) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (clients_.empty()) return;
  }
  const auto data = std::make_shared<const std::string>(text);
  std::lock_guard<std::mutex> l(mu_);
  for (auto& [id, c] : clients_) enqueue(c, data, false, droppable);
}

void WebServer::broadcastBinary(const std::vector<uint8_t>& bytes, bool droppable) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (clients_.empty()) return;  // nobody to copy the bytes for
  }
  const auto data = std::make_shared<const std::string>(bytes.begin(), bytes.end());
  std::lock_guard<std::mutex> l(mu_);
  for (auto& [id, c] : clients_) enqueue(c, data, true, droppable);
}

void WebServer::sendText(int client, const std::string& text) {
  std::lock_guard<std::mutex> l(mu_);
  auto it = clients_.find(client);
  if (it != clients_.end()) enqueue(it->second, std::make_shared<const std::string>(text), false, false);
}

int WebServer::clients() const {
  std::lock_guard<std::mutex> l(mu_);
  return static_cast<int>(clients_.size());
}

}  // namespace hfc
