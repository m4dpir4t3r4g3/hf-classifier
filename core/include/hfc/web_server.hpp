// A small HTTP/1.1 and WebSocket server for the local user interface: pages
// and JSON/CSV answers on plain GETs, and live data to every connected
// browser over WebSockets, all on one port of 127.0.0.1.
//
// One thread accepts; each connection gets a thread. HTTP answers close the
// connection (no keep-alive: the UI makes few requests). Each WebSocket
// client gets an outgoing queue and its own sender thread, so a slow
// browser never blocks the caller of broadcast(): when its queue is full,
// droppable messages (waterfall rows) are dropped for that client, and a
// client that stops reading altogether is disconnected.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hfc/net.hpp"

namespace hfc {

struct HttpRequest {
  std::string method;  // "GET", ...
  std::string path;    // without the query, percent-decoded
  std::map<std::string, std::string> query;
  std::string header;  // the whole request header
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "text/plain; charset=utf-8";
  std::string body;
  std::string extraHeaders;  // each ending with \r\n
  static HttpResponse notFound();
};

class WebServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;
  using WsOpenFn = std::function<void(int client)>;
  using WsMessageFn = std::function<void(int client, const std::string& text)>;

  WebServer() = default;
  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  // Handlers are matched by exact path, then by the longest prefix ending in
  // '/' ("/" itself only matches exactly). Set them before start().
  void handle(const std::string& path, Handler h) { routes_[path] = std::move(h); }
  // WebSocket upgrades on this path.
  void websocket(const std::string& path, WsOpenFn onOpen, WsMessageFn onMessage);

  bool start(const std::string& address, uint16_t port, std::string* error);
  void stop();
  uint16_t port() const { return listener_.port(); }

  // To every WebSocket client (or one). Droppable messages are skipped for a
  // client whose queue is long.
  void broadcastText(const std::string& text, bool droppable = false);
  void broadcastBinary(const std::vector<uint8_t>& data, bool droppable = false);
  void sendText(int client, const std::string& text);
  int clients() const;

 private:
  struct Client;
  void acceptLoop();
  void serve(TcpSocket sock);
  void runWebSocket(TcpSocket sock, const std::string& header, const std::string& early);
  void enqueue(const std::shared_ptr<Client>& c, const std::shared_ptr<const std::string>& data, bool binary,
               bool droppable);
  void reap();

  TcpListener listener_;
  std::thread acceptThread_;
  std::atomic<bool> running_{false};
  std::map<std::string, Handler> routes_;
  std::string wsPath_;
  WsOpenFn wsOpen_;
  WsMessageFn wsMessage_;
  mutable std::mutex mu_;
  std::map<int, std::shared_ptr<Client>> clients_;
  int nextClient_ = 1;
  std::vector<std::thread> connThreads_;
  std::vector<std::shared_ptr<std::atomic<bool>>> connDone_;
};

// Percent-decoding of URL components ('+' is a space in queries).
std::string urlDecode(const std::string& s, bool plusIsSpace);

}  // namespace hfc
