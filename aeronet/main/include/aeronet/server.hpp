#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "flat-hash-map.hpp"

namespace aeronet {
class EventLoop;  // forward

using HttpHeaders = flat_hash_map<std::string_view, std::string_view>;

struct HttpRequest {
  std::string_view method;
  std::string_view target;
  std::string_view version;
  HttpHeaders headers;    // flattened header storage
  std::string_view body;  // not owned, subset of same backing buffer
};

struct HttpResponse {
  int statusCode{200};
  std::string reason{"OK"};
  std::string body{"Hello from aeronet"};
  std::string contentType{"text/plain"};
};

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  explicit HttpServer(uint16_t port);
  ~HttpServer();
  void setHandler(RequestHandler handler);
  void run();
  void runUntil(const std::function<bool()>& predicate,
                std::chrono::milliseconds checkPeriod = std::chrono::milliseconds{500});
  void stop();

  [[nodiscard]] uint16_t port() const { return _port; }
  [[nodiscard]] bool isRunning() const { return _running; }

 private:
  void setupListener();
  void eventLoop(int timeoutMs);

  int _listenFd{-1};
  uint16_t _port{};
  bool _running{false};
  RequestHandler _handler;
  EventLoop* _loop{nullptr};
};
}  // namespace aeronet
