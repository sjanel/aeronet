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
#include "timedef.hpp"

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
  enum class ParserError : std::uint8_t {
    BadRequestLine,
    VersionUnsupported,
    HeadersTooLarge,
    PayloadTooLarge,
    MalformedChunk,
    GenericBadRequest
  };
  using ParserErrorCallback = std::function<void(ParserError)>;

  HttpServer() noexcept = default;

  explicit HttpServer(const ServerConfig& cfg);

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&& other) noexcept;
  HttpServer& operator=(HttpServer&& other) noexcept;

  ~HttpServer();

  void setHandler(RequestHandler handler);
  void setParserErrorCallback(ParserErrorCallback cb) { _parserErrCb = std::move(cb); }

  void run();
  void runUntil(const std::function<bool()>& predicate,
                std::chrono::milliseconds checkPeriod = std::chrono::milliseconds{500});

  void stop();

  [[nodiscard]] const ServerConfig& config() const { return _config; }

  [[nodiscard]] uint16_t port() const { return _config.port; }
  [[nodiscard]] bool isRunning() const { return _running; }

 private:
  struct ConnStateInternal {
    string buffer;       // accumulated raw data
    string bodyStorage;  // decoded body lifetime
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    uint32_t requestsServed{0};
    bool shouldClose{false};
  };
  void setupListener();
  void eventLoop(int timeoutMs);
  void refreshCachedDate();
  void sweepIdleConnections();
  void acceptNewConnections();
  void handleReadableClient(int fd);
  bool processRequestsOnConnection(int fd, ConnStateInternal& state);
  // Split helpers
  bool parseNextRequestFromBuffer(int fd, ConnStateInternal& state, HttpRequest& outReq, std::size_t& headerEnd,
                                  bool& closeConn);
  bool decodeBodyIfReady(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                         bool isChunked, bool expectContinue, bool& closeConn, size_t& consumedBytes);
  bool decodeFixedLengthBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                             bool expectContinue, bool& closeConn, size_t& consumedBytes);
  bool decodeChunkedBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                         bool expectContinue, bool& closeConn, size_t& consumedBytes);
  void finalizeAndSendResponse(int fd, ConnStateInternal& state, HttpRequest& req, HttpResponse& resp,
                               size_t consumedBytes, bool& closeConn);
  void closeConnection(int fd);

  int _listenFd{-1};
  bool _running{false};
  RequestHandler _handler;
  std::unique_ptr<EventLoop> _loop;
  ServerConfig _config{};                             // holds port & reusePort & limits
  flat_hash_map<int, ConnStateInternal> _connStates;  // per-server connection states
  string _cachedDate;
  TimePoint _cachedDateEpoch;  // last second-aligned timestamp used for Date header
  ParserErrorCallback _parserErrCb;
};
}  // namespace aeronet
