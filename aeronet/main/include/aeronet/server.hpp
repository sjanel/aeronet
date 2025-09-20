#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "aeronet/server-config.hpp"
#include "flat-hash-map.hpp"
#include "http-request.hpp"
#include "http-response.hpp"
#include "string.hpp"

namespace aeronet {

class EventLoop;  // forward declaration

// HttpServer
//  - Single-threaded event loop by design: one instance == one epoll/reactor running in the
//    calling thread (typically the thread invoking run() / runUntil()).
//  - Not internally synchronized; do not access a given instance concurrently from multiple
//    threads (except destroying after stop()).
//  - To utilize multiple CPU cores, create several HttpServer instances (possibly with
//    ServerConfig::withReusePort(true) on the same port) and run each in its own thread.
//  - Writes currently assume exclusive ownership of the connection fd within this single
//    thread, enabling simple sequential ::write / ::writev without partial-write state tracking.
//    A production multi-threaded / async-write version would need buffered output handling.
class HttpServer {
 public:
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer() noexcept = default;

  explicit HttpServer(const ServerConfig& cfg);

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&& other) noexcept;
  HttpServer& operator=(HttpServer&& other) noexcept;

  ~HttpServer();

  void setHandler(RequestHandler handler);

  void run();
  void runUntil(const std::function<bool()>& predicate,
                std::chrono::milliseconds checkPeriod = std::chrono::milliseconds{500});

  void stop();

  [[nodiscard]] const ServerConfig& config() const { return _config; }

  [[nodiscard]] uint16_t port() const { return _config.port; }
  [[nodiscard]] bool isRunning() const { return _running; }

 private:
  void setupListener();
  void eventLoop(int timeoutMs);

  int _listenFd{-1};
  bool _running{false};
  RequestHandler _handler;
  std::unique_ptr<EventLoop> _loop;
  ServerConfig _config{};  // holds port & reusePort & limits
  struct ConnStateInternal {
    string buffer;       // accumulated raw data
    string bodyStorage;  // decoded body lifetime
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    uint32_t requestsServed{0};
    bool shouldClose{false};
  };
  flat_hash_map<int, ConnStateInternal> _connStates;  // per-server connection states
  string _cachedDate;
  std::time_t _cachedDateEpoch{0};
};
}  // namespace aeronet
