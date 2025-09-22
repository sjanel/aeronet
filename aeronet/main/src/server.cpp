#include "aeronet/server.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "duration-format.hpp"
#include "event-loop.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#include "http-constants.hpp"
#include "http-error-build.hpp"
#include "http-method-build.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "log.hpp"
#include "string-equal-ignore-case.hpp"
#include "sys-utils.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

// HttpServer constructor
// Performs full listener initialization (RAII style) so that port() is valid immediately after construction.
// Steps (in order) and rationale / failure characteristics:
//   1. socket(AF_INET, SOCK_STREAM, 0)
//        - Expected to succeed under normal conditions. Failure indicates resource exhaustion
//          (EMFILE per-process fd limit, ENFILE system-wide, ENOBUFS/ENOMEM) or misconfiguration (rare EACCES).
//   2. setsockopt(SO_REUSEADDR)
//        - Practically infallible unless programming error (EINVAL) or extreme memory pressure (ENOMEM).
//          Mandatory to allow rapid restart after TIME_WAIT collisions.
//   3. setsockopt(SO_REUSEPORT) (optional best-effort)
//        - Enabled only if cfg.reusePort. May fail on older kernels (EOPNOTSUPP/EINVAL) -> logged as warning only,
//          not fatal. This provides horizontal scaling (multi-reactor) when supported.
//   4. bind()
//        - Most common legitimate failure point: EADDRINUSE when user supplies a fixed port already in use, or
//          EACCES for privileged ports (<1024) without CAP_NET_BIND_SERVICE. With cfg.port == 0 (ephemeral) the
//          collision probability is effectively eliminated; failures then usually imply resource exhaustion or
//          misconfiguration. Chosen early to surface environmental issues promptly.
//   5. listen()
//        - Rarely fails after successful bind; would signal extreme resource pressure or unexpected kernel state.
//   6. getsockname() (only if ephemeral port requested)
//        - Retrieves the kernel-assigned port so tests / orchestrators can read it deterministically. Extremely
//          reliable; failure would imply earlier descriptor issues (EBADF) which would already have thrown.
//   7. fcntl(F_GETFL/F_SETFL O_NONBLOCK)
//        - Should not fail unless EBADF or EINVAL (programming error). Makes accept + IO non-blocking for epoll ET.
//   8. epoll add (via EventLoop::add)
//        - Registers the listening fd for readiness notifications. Possible errors: ENOMEM/ENOSPC (resource limits),
//          EBADF (logic bug), EEXIST (should not happen). Treated as fatal.
//
// Exception Semantics:
//   - On any fatal failure the constructor throws std::runtime_error after closing the partially created _listenFd.
//   - This yields strong exception safety: either you have a fully registered, listening server instance or no
//     observable side effects. Users relying on non-throwing control flow can wrap construction in a factory that
//     maps exceptions to error codes / expected<>.
//
// Operational Expectations:
//   - In a nominal environment using an ephemeral port (cfg.port == 0), the probability of an exception is ~0 unless
//     the process hits fd limits or severe memory pressure. Fixed ports may legitimately throw due to EADDRINUSE.
//   - Using ephemeral ports in tests removes port collision flakiness across machines / CI runs.
HttpServer::HttpServer(const ServerConfig& cfg) : _listenFd(::socket(AF_INET, SOCK_STREAM, 0)), _config(cfg) {
  if (_listenFd < 0) {
    throw std::runtime_error("socket failed");
  }
  static constexpr int enable = 1;
  if (::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
    safeClose(_listenFd, "listenFd(SO_REUSEADDR) after socket");
    throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
  }
  if (_config.reusePort) {
    if (::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) < 0) {
      log::error("setsockopt(SO_REUSEPORT) error: {}", std::strerror(errno));
    }
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
  if (bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    safeClose(_listenFd, "listenFd(bind)");
    throw std::runtime_error("bind failed");
  }
  if (listen(_listenFd, SOMAXCONN) < 0) {
    safeClose(_listenFd, "listenFd(listen)");
    throw std::runtime_error("listen failed");
  }
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
      _config.port = ntohs(actual.sin_port);
    }
  }
  if (setNonBlocking(_listenFd) < 0) {
    safeClose(_listenFd, "listenFd(setNonBlocking)");
    throw std::runtime_error("failed to set non-blocking");
  }
  if (_loop == nullptr) {
    _loop = std::make_unique<EventLoop>();
  }
  if (!_loop->add(_listenFd, EPOLLIN)) {
    safeClose(_listenFd, "listenFd(epoll add)");
    throw std::runtime_error("EventLoop add listen socket failed");
  }
  log::info("Server created on port :{}", _config.port);
}

HttpServer::~HttpServer() { stop(); }

HttpServer::HttpServer(HttpServer&& other) noexcept
    : _listenFd(std::exchange(other._listenFd, -1)),
      _running(std::exchange(other._running, false)),
      _handler(std::move(other._handler)),
      _pathHandlers(std::move(other._pathHandlers)),
      _loop(std::move(other._loop)),
      _config(std::move(other._config)),
      _connStates(std::move(other._connStates)),
      _cachedDate(std::exchange(other._cachedDate, {})),
      _cachedDateEpoch(std::exchange(other._cachedDateEpoch, TimePoint{})),
      _parserErrCb(std::move(other._parserErrCb)) {}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    _listenFd = std::exchange(other._listenFd, -1);
    _running = std::exchange(other._running, false);
    _handler = std::move(other._handler);
    _pathHandlers = std::move(other._pathHandlers);
    _loop = std::move(other._loop);
    _config = std::move(other._config);
    _connStates = std::move(other._connStates);
    _cachedDate = std::exchange(other._cachedDate, {});
    _cachedDateEpoch = std::exchange(other._cachedDateEpoch, TimePoint{});
    _parserErrCb = std::move(other._parserErrCb);
  }
  return *this;
}

void HttpServer::setHandler(RequestHandler handler) { _handler = std::move(handler); }

void HttpServer::setStreamingHandler(StreamingHandler handler) {
  if (_handler) {
    throw exception("Cannot set streaming handler when global handler already set");
  }
  if (!_pathHandlers.empty()) {
    throw exception("Cannot set streaming handler when path handlers are registered");
  }
  _streamingHandler = std::move(handler);
}

void HttpServer::addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler) {
  if (_handler) {
    throw exception("Cannot use addPathHandler after setHandler has been set");
  }
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pPathHandlerEntry;
  if (it == _pathHandlers.end()) {
    pPathHandlerEntry = &_pathHandlers[std::move(path)];
  } else {
    pPathHandlerEntry = &it->second;
  }
  pPathHandlerEntry->methodMask = http::methodListToMask(methods);
  for (http::Method method : methods) {
    pPathHandlerEntry->handlers[static_cast<std::underlying_type_t<http::Method>>(method)] = handler;
  }
}

void HttpServer::addPathHandler(std::string path, http::Method method, const RequestHandler& handler) {
  if (_handler) {
    throw exception("Cannot use addPathHandler after setHandler has been set");
  }
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pPathHandlerEntry;
  if (it == _pathHandlers.end()) {
    pPathHandlerEntry = &_pathHandlers[std::move(path)];
  } else {
    pPathHandlerEntry = &it->second;
  }
  pPathHandlerEntry->methodMask = http::singleMethodToMask(method);
  pPathHandlerEntry->handlers[static_cast<std::underlying_type_t<http::Method>>(method)] = handler;
}

void HttpServer::run(Duration checkPeriod) {
  if (_running) {
    throw exception("Server is already running");
  }
  log::info("Server running until SIGINT or SIGTERM (check period of {})", checkPeriod);
  for (_running = true; _running;) {
    eventLoop(checkPeriod);
  }
}

void HttpServer::stop() {
  log::info("Stopping server");
  _running = false;
  if (_listenFd != -1) {
    safeClose(_listenFd, "listenFd(stop)");
    _listenFd = -1;
    log::info("Server stopped");
  }
}

void HttpServer::runUntil(const std::function<bool()>& predicate, Duration checkPeriod) {
  if (_running) {
    throw exception("Server is already running");
  }
  log::info("Server running until predicate, SIGINT or SIGTERM (check period of {})", PrettyDuration{checkPeriod});
  for (_running = true; _running && !predicate();) {
    eventLoop(checkPeriod);
  }
  stop();
}

void HttpServer::refreshCachedDate() {
  using namespace std::chrono;
  TimePoint nowTp = Clock::now();
  auto nowSec = time_point_cast<seconds>(nowTp);
  if (time_point_cast<seconds>(_cachedDateEpoch) != nowSec) {
    _cachedDateEpoch = nowSec;
    [[maybe_unused]] char* end = TimeToStringRFC7231(nowSec, _cachedDate.data());
    assert(end <= _cachedDate.data() + _cachedDate.size());
  }
}

bool HttpServer::processRequestsOnConnection(int fd, HttpServer::ConnStateInternal& state) {
  bool closeCnx = false;
  while (true) {
    std::size_t headerEnd = 0;
    HttpRequest req{};
    if (!parseNextRequestFromBuffer(fd, state, req, headerEnd, closeCnx)) {
      break;  // need more data or connection closed
    }
    bool isChunked = false;
    bool hasTE = false;
    if (std::string_view te = req.findHeader(http::TransferEncoding); !te.empty()) {
      hasTE = true;
      if (req.version == http::HTTP10) {
        auto err = buildSimpleError(400, http::ReasonBadRequest, std::string_view(_cachedDate), true);
        queueData(fd, state, err.data(), err.size());
        closeCnx = true;
        break;
      }
      if (CaseInsensitiveEqual(te, http::chunked)) {
        isChunked = true;
      } else {
        auto err = buildSimpleError(501, http::ReasonNotImplemented, std::string_view(_cachedDate), true);
        queueData(fd, state, err.data(), err.size());
        closeCnx = true;
        break;
      }
    }
    bool hasCL = false;
    std::string_view lenViewAll = req.findHeader(http::ContentLength);
    if (!lenViewAll.empty()) {
      hasCL = true;
    }
    if (hasCL && hasTE) {
      auto err = buildSimpleError(400, http::ReasonBadRequest, std::string_view(_cachedDate), true);
      queueData(fd, state, err.data(), err.size());
      closeCnx = true;
      break;
    }
    bool expectContinue = false;
    if (req.version == http::HTTP11) {
      if (std::string_view expectVal = req.findHeader(http::Expect); !expectVal.empty()) {
        if (CaseInsensitiveEqual(expectVal, http::h100_continue)) {
          expectContinue = true;
        }
      }
    }
    std::size_t consumedBytes = 0;
    if (!decodeBodyIfReady(fd, state, req, headerEnd, isChunked, expectContinue, closeCnx, consumedBytes)) {
      break;  // need more bytes or error
    }
    // Streaming handler path (exclusive of global/path handlers)
    if (_streamingHandler && _pathHandlers.empty() && !_handler) {
      bool isHead = (req.method == http::HEAD);
      HttpResponseWriter writer(*this, fd, isHead);
      try {
        _streamingHandler(req, writer);
      } catch (const std::exception& ex) {
        log::error("Exception in streaming handler: {}", ex.what());
      } catch (...) {
        log::error("Unknown exception in streaming handler.");
      }
      if (!writer.finished()) {
        writer.end();
      }
      // Decide keep-alive: only if server config allows, HTTP/1.1, not exceeding maxRequestsPerConnection, writer not
      // failed.
      bool allowKeepAlive = _config.enableKeepAlive && req.version == http::HTTP11 &&
                            state.requestsServed + 1 < _config.maxRequestsPerConnection && !state.shouldClose;
      ++state.requestsServed;  // count this streaming request
      if (consumedBytes > 0) {
        state.buffer.erase_front(consumedBytes);
      }
      if (!allowKeepAlive) {
        state.shouldClose = true;
        closeCnx = true;  // will close after outbound buffer drains
      }
      break;  // done with this request (streaming is synchronous for now)
    }

    HttpResponse resp;
    if (!_pathHandlers.empty()) {
      auto it = _pathHandlers.find(req.target);
      if (it == _pathHandlers.end()) {
        resp.statusCode = 404;
        resp.reason = "Not Found";
        resp.body = resp.reason;
        resp.contentType = "text/plain";
      } else {
        auto method = http::toMethodEnum(req.method);
        if (!http::methodAllowed(it->second.methodMask, method)) {
          resp.statusCode = 405;
          resp.reason.assign(http::ReasonMethodNotAllowed);
          resp.body = resp.reason;
          resp.contentType = "text/plain";
        } else {
          try {
            resp = it->second.handlers[static_cast<std::underlying_type_t<http::Method>>(method)](req);
          } catch (const std::exception& ex) {
            log::error("Exception in path handler: {}", ex.what());
            resp.statusCode = 500;
            resp.reason.assign(http::ReasonInternalServerError);
            resp.body = resp.reason;
            resp.contentType = "text/plain";
          } catch (...) {
            log::error("Unknown exception in path handler.");
            resp.statusCode = 500;
            resp.reason.assign(http::ReasonInternalServerError);
            resp.body = resp.reason;
            resp.contentType = "text/plain";
          }
        }
      }
    } else if (_handler) {
      try {
        resp = _handler(req);
      } catch (const std::exception& ex) {
        log::error("Exception in request handler: {}", ex.what());
        resp.statusCode = 500;
        resp.reason.assign(http::ReasonInternalServerError);
        resp.body = resp.reason;
        resp.contentType = "text/plain";
      } catch (...) {
        log::error("Unknown exception in request handler.");
        resp.statusCode = 500;
        resp.reason.assign(http::ReasonInternalServerError);
        resp.body = resp.reason;
        resp.contentType = "text/plain";
      }
    }
    finalizeAndSendResponse(fd, state, req, resp, consumedBytes, closeCnx);
    if (closeCnx) {
      break;
    }
  }
  return closeCnx;
}

void HttpServer::eventLoop(Duration timeout) {
  if (!_loop) {
    return;
  }
  refreshCachedDate();
  sweepIdleConnections();
  int ready = _loop->poll(timeout, [&](int fd, uint32_t ev) {
    if (fd == _listenFd) {
      acceptNewConnections();
    } else {
      if (ev & EPOLLOUT) {
        handleWritableClient(fd);
      }
      if (ev & EPOLLIN) {
        handleReadableClient(fd);
      }
    }
  });
  // If epoll_wait failed with a non-EINTR error (EINTR is mapped to 0 in EventLoop::poll), ready will be -1.
  // Not handling this would cause a tight loop spinning on the failing epoll fd (e.g., after EBADF or EINVAL),
  // burning CPU while doing no useful work. Treat it as fatal: log and stop the server.
  if (ready < 0) {
    log::error("epoll_wait (eventLoop) failed: {}", std::strerror(errno));
    // Mark server as no longer running so outer loops terminate gracefully.
    _running = false;
  }
}

}  // namespace aeronet
