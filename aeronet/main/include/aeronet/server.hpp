#pragma once

#include <sys/uio.h>  // iovec

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "aeronet/server-config.hpp"
#include "flat-hash-map.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "http-request.hpp"
#include "http-response.hpp"
#include "raw-chars.hpp"
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
  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  // Methods are supplied via http::MethodsSet (small fixed-capacity flat set, non-allocating).
  // Mutually exclusive with setHandler: using both is invalid and will throw.
  void addPathHandler(std::string_view path, const http::MethodSet& methods, const RequestHandler& handler);
  // Convenience overload for a single method.
  void addPathHandler(std::string_view path, http::Method method, const RequestHandler& handler);

  void setParserErrorCallback(ParserErrorCallback cb) { _parserErrCb = std::move(cb); }

  void run();
  void runUntil(const std::function<bool()>& predicate, Duration checkPeriod = std::chrono::milliseconds{500});

  void stop();

  [[nodiscard]] const ServerConfig& config() const { return _config; }

  [[nodiscard]] uint16_t port() const { return _config.port; }
  [[nodiscard]] bool isRunning() const { return _running; }

 private:
  struct ConnStateInternal {
    RawChars buffer;       // accumulated raw data
    RawChars bodyStorage;  // decoded body lifetime
    RawChars outBuffer;    // pending outbound bytes not yet written
    std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
    uint32_t requestsServed{0};
    bool shouldClose{false};      // request to close once outBuffer drains
    bool waitingWritable{false};  // EPOLLOUT registered
  };
  void setupListener();
  void eventLoop(Duration timeout);
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
  // Outbound write helpers
  bool queueData(int fd, ConnStateInternal& state, const char* data, size_t len);
  bool queueVec(int fd, ConnStateInternal& state, const struct iovec* iov, int iovcnt);
  void flushOutbound(int fd, ConnStateInternal& state);
  void handleWritableClient(int fd, uint32_t ev);
  void closeConnection(int fd);

  struct StatsInternal {
    uint64_t totalBytesQueued{0};
    uint64_t totalBytesWrittenImmediate{0};
    uint64_t totalBytesWrittenFlush{0};
    uint64_t deferredWriteEvents{0};
    uint64_t flushCycles{0};
    size_t maxConnectionOutboundBuffer{0};
  } _stats;

 public:
  struct StatsPublic {
    uint64_t totalBytesQueued;
    uint64_t totalBytesWrittenImmediate;
    uint64_t totalBytesWrittenFlush;
    uint64_t deferredWriteEvents;
    uint64_t flushCycles;
    std::size_t maxConnectionOutboundBuffer;
  };

  [[nodiscard]] StatsPublic stats() const {
    return {_stats.totalBytesQueued,
            _stats.totalBytesWrittenImmediate,
            _stats.totalBytesWrittenFlush,
            _stats.deferredWriteEvents,
            _stats.flushCycles,
            _stats.maxConnectionOutboundBuffer};
  }

  int _listenFd{-1};
  bool _running{false};
  RequestHandler _handler;
  struct PathHandlerEntry {
    uint32_t methodMask{};
    std::array<RequestHandler, http::kNbMethods> handlers;
  };

  flat_hash_map<string, PathHandlerEntry, std::hash<std::string_view>, std::equal_to<>> _pathHandlers;  // path -> entry

  std::unique_ptr<EventLoop> _loop;
  ServerConfig _config;
  flat_hash_map<int, ConnStateInternal> _connStates;  // per-server connection states
  string _cachedDate;
  TimePoint _cachedDateEpoch;  // last second-aligned timestamp used for Date header
  ParserErrorCallback _parserErrCb;
};
}  // namespace aeronet
