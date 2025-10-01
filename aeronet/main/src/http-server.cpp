#include "aeronet/http-server.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/compression-config.hpp"  // IWYU pragma: keep (indirectly used via encoding selector)
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "duration-format.hpp"
#include "event-loop.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#ifdef AERONET_ENABLE_ZLIB
#include "zlib-encoder.hpp"
#endif
#include "http-constants.hpp"
#include "http-error-build.hpp"
#include "http-method-build.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "log.hpp"
#include "socket.hpp"
#include "string-equal-ignore-case.hpp"
#include "sys-utils.hpp"
#include "timedef.hpp"
#include "timestring.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "tls-context.hpp"
#endif

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
HttpServer::HttpServer(HttpServerConfig cfg)
    : _listenSocket(Socket::Type::STREAM), _config(std::move(cfg)), _encodingSelector(_config.compression) {
  int listenFdLocal = _listenSocket.fd();
  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls) {
#ifdef AERONET_ENABLE_OPENSSL
    // Reset external metrics container (fresh server instance)
    _tlsMetricsExternal.alpnStrictMismatches = 0;
    // Allocate TlsContext on the heap so its address remains stable even if HttpServer is moved.
    // (See detailed rationale in header next to _tlsCtxHolder.)
    _tlsCtxHolder = std::make_unique<TlsContext>(*_config.tls, &_tlsMetricsExternal);
#else
    throw std::runtime_error("aeronet built without OpenSSL support but TLS configuration provided");
#endif
  }
  static constexpr int enable = 1;
  if (::setsockopt(listenFdLocal, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
    throw std::runtime_error("setsockopt(SO_REUSEADDR) failed");
  }
  if (_config.reusePort) {
    if (::setsockopt(listenFdLocal, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) < 0) {
      log::error("setsockopt(SO_REUSEPORT) error: {}", std::strerror(errno));
    }
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
  if (bind(listenFdLocal, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error("bind failed");
  }
  if (listen(listenFdLocal, SOMAXCONN) < 0) {
    throw std::runtime_error("listen failed");
  }
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(listenFdLocal, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
      _config.port = ntohs(actual.sin_port);
    }
  }
  if (setNonBlocking(listenFdLocal) < 0) {
    throw std::runtime_error("failed to set non-blocking");
  }
  if (!_loop.add(listenFdLocal, EPOLLIN)) {
    throw std::runtime_error("EventLoop add listen socket failed");
  }
  // Register wakeup fd
  if (!_loop.add(_wakeupFd, EPOLLIN)) {
    throw std::runtime_error("EventLoop add wakeup fd failed");
  }
// Pre-allocate encoders (one per supported format if available at compile time) so per-response paths can reuse them.
#ifdef AERONET_ENABLE_ZLIB
  _encoders[static_cast<std::size_t>(Encoding::gzip)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::gzip, _config.compression);
  _encoders[static_cast<std::size_t>(Encoding::deflate)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::deflate, _config.compression);
#endif
  // Identity encoder always available for Format::none index 0 (optional, can remain null to signal identity).
  log::info("Server created on port :{}", _config.port);
}

HttpServer::~HttpServer() { stop(); }

// NOTE: Move ctor / assignment definitions provided elsewhere earlier (not duplicated). Encoder array moved with
// object.

HttpServer::HttpServer(HttpServer&& other) noexcept
    : _stats(std::exchange(other._stats, {})),
      _listenSocket(std::move(other._listenSocket)),
      _wakeupFd(std::move(other._wakeupFd)),
      _running(std::exchange(other._running, false)),
      _handler(std::move(other._handler)),
      _streamingHandler(std::move(other._streamingHandler)),
      _pathHandlers(std::move(other._pathHandlers)),
      _loop(std::move(other._loop)),
      _config(std::move(other._config)),
      _connStates(std::move(other._connStates)),
      _encoders(std::move(other._encoders)),
      _encodingSelector(std::move(other._encodingSelector)),
      _cachedDate(std::exchange(other._cachedDate, {})),
      _cachedDateEpoch(std::exchange(other._cachedDateEpoch, TimePoint{})),
      _parserErrCb(std::move(other._parserErrCb)),
      _metricsCb(std::move(other._metricsCb))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tlsCtxHolder(std::move(other._tlsCtxHolder)),
      _tlsMetrics(std::move(other._tlsMetrics)),
      _tlsMetricsExternal(std::exchange(other._tlsMetricsExternal, {}))
#endif

{
  if (_running) {  // note: _running holds original state via std::exchange above
    log::error("Attempt to move-construct a running HttpServer (port={}) — unsupported", _config.port);
    assert(false && "Moving a running HttpServer is unsupported");
    _running = false;  // force destination to a stopped state to reduce further surprises
  }
}

HttpServer& HttpServer::operator=(HttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    if (other._running) {  // captured pre-move state from other
      log::error("Attempt to move-assign from a running HttpServer (port={}) — unsupported", _config.port);
      assert(false && "Moving from a running HttpServer is unsupported");
      other._running = false;
    }
    _stats = std::exchange(other._stats, {});
    _wakeupFd = std::move(other._wakeupFd);
    _listenSocket = std::move(other._listenSocket);
    _running = std::exchange(other._running, false);
    _handler = std::move(other._handler);
    _streamingHandler = std::move(other._streamingHandler);
    _pathHandlers = std::move(other._pathHandlers);
    _loop = std::move(other._loop);
    _config = std::move(other._config);
    _connStates = std::move(other._connStates);
    _encoders = std::move(other._encoders);
    _encodingSelector = std::move(other._encodingSelector);
    _cachedDate = std::exchange(other._cachedDate, {});
    _cachedDateEpoch = std::exchange(other._cachedDateEpoch, TimePoint{});
    _parserErrCb = std::move(other._parserErrCb);
    _metricsCb = std::move(other._metricsCb);
#ifdef AERONET_ENABLE_OPENSSL
    _tlsCtxHolder = std::move(other._tlsCtxHolder);
    _tlsMetrics = std::move(other._tlsMetrics);
    _tlsMetricsExternal = std::exchange(other._tlsMetricsExternal, {});
#endif
  }
  return *this;
}

void HttpServer::setHandler(RequestHandler handler) { _handler = std::move(handler); }

void HttpServer::setStreamingHandler(StreamingHandler handler) { _streamingHandler = std::move(handler); }

void HttpServer::addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler) {
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pEntry;
  if (it == _pathHandlers.end()) {
    pEntry = &_pathHandlers[std::move(path)];
  } else {
    pEntry = &it->second;
  }
  for (http::Method method : methods) {
    auto idx = static_cast<std::underlying_type_t<http::Method>>(method);
    if (pEntry->streamingHandlers[idx]) {
      throw exception("Cannot register normal handler: streaming handler already present for path+method");
    }
    pEntry->normalHandlers[idx] = handler;
    pEntry->normalMethodMask |= http::singleMethodToMask(method);
  }
}

void HttpServer::addPathHandler(std::string path, http::Method method, const RequestHandler& handler) {
  addPathHandler(std::move(path), http::MethodSet{method}, handler);
}

void HttpServer::addPathStreamingHandler(std::string path, const http::MethodSet& methods,
                                         const StreamingHandler& handler) {
  auto it = _pathHandlers.find(path);
  PathHandlerEntry* pEntry;
  if (it == _pathHandlers.end()) {
    pEntry = &_pathHandlers[std::move(path)];
  } else {
    pEntry = &it->second;
  }
  for (http::Method method : methods) {
    auto idx = static_cast<std::underlying_type_t<http::Method>>(method);
    if (pEntry->normalHandlers[idx]) {
      throw exception("Cannot register streaming handler: normal handler already present for path+method");
    }
    pEntry->streamingHandlers[idx] = handler;
    pEntry->streamingMethodMask |= http::singleMethodToMask(method);
  }
}

void HttpServer::addPathStreamingHandler(std::string path, http::Method method, const StreamingHandler& handler) {
  addPathStreamingHandler(std::move(path), http::MethodSet{method}, handler);
}

void HttpServer::run() {
  if (_running) {
    throw exception("Server is already running");
  }
  auto interval = _config.pollInterval;
  log::info("Server running (poll interval={})", PrettyDuration{interval});
  for (_running = true; _running;) {
    eventLoop(interval);
  }
}

void HttpServer::stop() {
  if (!_running) {
    return;
  }
  log::debug("Stopping server");
  _running = false;
  // Trigger wakeup to break any blocking epoll_wait quickly.
  _wakeupFd.send();
  // Attempt close only if descriptor still open.
  if (_listenSocket.fd() != -1) {
    _listenSocket.close();
    log::info("Server stopped");
  }
}

void HttpServer::runUntil(const std::function<bool()>& predicate) {
  if (_running) {
    throw exception("Server is already running");
  }
  auto interval = _config.pollInterval;
  log::info("Server running until predicate (poll interval={})", PrettyDuration{interval});
  for (_running = true; _running && !predicate();) {
    eventLoop(interval);
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

ssize_t HttpServer::transportRead(int fd, ConnectionState& state, std::size_t chunkSize, bool& wantRead,
                                  bool& wantWrite) {
  std::size_t oldSize = state.buffer.size();
  ssize_t bytesRead = 0;
  state.buffer.resize_and_overwrite(oldSize + chunkSize, [&](char* base, std::size_t /*n*/) {
    char* writePtr = base + oldSize;  // base points to beginning of existing buffer
    if (state.transport) {
      bytesRead = state.transport->read(writePtr, chunkSize, wantRead, wantWrite);
    } else {
      wantRead = wantWrite = false;
      bytesRead = ::read(fd, writePtr, chunkSize);
    }
    if (bytesRead > 0) {
      return oldSize + static_cast<std::size_t>(bytesRead);  // grew by bytesRead
    }
    return oldSize;  // retain previous logical size on EOF / EAGAIN / error
  });
  return bytesRead;
}

bool HttpServer::ModWithCloseOnFailure(EventLoop& loop, int fd, uint32_t events, ConnectionState& st, const char* ctx,
                                       StatsInternal& stats) {
  if (loop.mod(fd, events)) {
    return true;
  }
  auto errCode = errno;
  ++stats.epollModFailures;
  // EBADF or ENOENT can occur during races where a connection is concurrently closed; downgrade severity.
  if (errCode == EBADF || errCode == ENOENT) {
    log::warn("epoll_ctl MOD benign failure (ctx={}, fd={}, events=0x{:x}, errno={}, msg={})", ctx, fd, events, errCode,
              std::strerror(errCode));
  } else {
    log::error("epoll_ctl MOD failed (ctx={}, fd={}, events=0x{:x}, errno={}, msg={})", ctx, fd, events, errCode,
               std::strerror(errCode));
  }
  st.shouldClose = true;
  return false;
}

ssize_t HttpServer::transportWrite(int fd, ConnectionState& state, std::string_view data, bool& wantRead,
                                   bool& wantWrite) {
  if (state.transport) {
    return state.transport->write(data, wantRead, wantWrite);
  }
  wantRead = false;
  wantWrite = false;
  return ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
}

bool HttpServer::processRequestsOnConnection(int fd, ConnectionState& state) {
  bool closeConnection = false;
  while (true) {
    auto reqStart = std::chrono::steady_clock::now();
    // If we don't yet have a full request line (no '\n' observed) wait for more data instead of
    // attempting to parse and wrongly emitting a 400 which would close an otherwise healthy keep-alive.
    if (state.buffer.empty() || std::find(state.buffer.begin(), state.buffer.end(), '\n') == state.buffer.end()) {
      break;  // need more bytes for at least the request line
    }
    HttpRequest req;
    auto statusCode = req.setHead(state, _config.maxHeaderBytes);
    if (statusCode != http::StatusCodeOK) {
      emitSimpleError(fd, state, statusCode, closeConnection);
      // Consume the currently parsed (or attempted) header bytes to avoid infinite loop on same data.
      // If we failed early, closeConnection will be true; break either way.
      break;
    }

    // A full request head (and body, if present) will now be processed; reset headerStart to signal
    // that the header timeout should track the next pending request only.
    state.headerStart = {};
    bool isChunked = false;
    bool hasTransferEncoding = false;
    std::string_view transferEncoding = req.header(http::TransferEncoding);
    if (!transferEncoding.empty()) {
      hasTransferEncoding = true;
      if (req.version() == http::HTTP_1_0) {
        emitSimpleError(fd, state, 400, closeConnection);
        break;
      }
      if (CaseInsensitiveEqual(transferEncoding, http::chunked)) {
        isChunked = true;
      } else {
        emitSimpleError(fd, state, 501, closeConnection);
        break;
      }
    }

    bool hasContentLength = false;
    std::string_view lenViewAll = req.header(http::ContentLength);
    if (!lenViewAll.empty()) {
      hasContentLength = true;
    }
    if (hasContentLength && hasTransferEncoding) {
      emitSimpleError(fd, state, 400, closeConnection);
      break;
    }
    bool expectContinue = false;
    if (req.version() == http::HTTP_1_1) {
      if (std::string_view expectVal = req.header(http::Expect); !expectVal.empty()) {
        if (CaseInsensitiveEqual(expectVal, http::h100_continue)) {
          expectContinue = true;
        }
      }
    }
    std::size_t consumedBytes = 0;
    if (!decodeBodyIfReady(fd, state, req, isChunked, expectContinue, closeConnection, consumedBytes)) {
      break;  // need more bytes or error
    }
    // Determine dispatch: path streaming > path normal > global streaming > global normal > 404/405
    auto methodEnum = req.method();
    // Provide implicit HEAD->GET fallback (RFC7231: HEAD is identical to GET without body) when
    // a HEAD handler is not explicitly registered but a GET handler exists for the same path.
    auto effectiveMethodEnum = methodEnum;
    bool isHead = req.method() == http::Method::HEAD;
    bool handledStreaming = false;
    bool pathFound = false;
    if (!_pathHandlers.empty()) {
      // 1. Always attempt exact match first, independent of policy.
      auto pit = _pathHandlers.find(req.path());
      std::string normalizedStorage;  // potential temporary storage for Normalize
      bool endsWithSlash = req.path().size() > 1 && req.path().back() == '/';
      if (pit == _pathHandlers.end()) {
        // 2. No exact match: apply policy decision (excluding root "/").
        if (endsWithSlash && req.path().size() > 1) {
          // Candidate canonical form (strip single trailing slash)
          std::string_view canonical(req.path().data(), req.path().size() - 1);
          auto canonicalIt = _pathHandlers.find(canonical);
          if (canonicalIt != _pathHandlers.end()) {
            switch (_config.trailingSlashPolicy) {
              case HttpServerConfig::TrailingSlashPolicy::Normalize:
                // Treat as if request was canonical (no redirect) by mutating path temporarily.
                normalizedStorage.assign(canonical.data(), canonical.size());
                req._path = normalizedStorage;
                pit = canonicalIt;
                break;
              case HttpServerConfig::TrailingSlashPolicy::Redirect: {
                // Emit 301 redirect to canonical form.
                HttpResponse resp(301, http::MovedPermanently);
                resp.location(canonical).contentType(http::ContentTypeTextPlain).body("Redirecting");
                finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
                if (closeConnection) {
                  return closeConnection;  // connection closing, stop loop
                }
                consumedBytes = 0;  // already advanced
                continue;           // process next request (keep-alive)
              }
              case HttpServerConfig::TrailingSlashPolicy::Strict:
                break;  // 404 later
              default:
                std::unreachable();
            }
          }
        } else if (!endsWithSlash && _config.trailingSlashPolicy == HttpServerConfig::TrailingSlashPolicy::Normalize &&
                   req.path().size() > 1) {
          // Request lacks slash; check if ONLY slashed variant exists.
          std::string augmented(req.path().size() + 1U, '/');
          std::memcpy(augmented.data(), req.path().data(), req.path().size());
          auto altIt = _pathHandlers.find(augmented);
          if (altIt != _pathHandlers.end()) {
            // Normalize treats them the same: dispatch to the registered variant.
            pit = altIt;
            normalizedStorage = augmented;  // keep explicit variant for metrics consistency
            req._path = normalizedStorage;
          }
        }
      }
      if (pit != _pathHandlers.end()) {
        pathFound = true;
        auto& entry = pit->second;
        // If HEAD and no explicit HEAD handler, but GET handler exists, reuse GET handler index.
        auto idxOriginal = static_cast<std::underlying_type_t<http::Method>>(methodEnum);
        auto idx = idxOriginal;
        if (isHead) {
          auto headIdx = static_cast<std::underlying_type_t<http::Method>>(http::Method::HEAD);
          auto getIdx = static_cast<std::underlying_type_t<http::Method>>(http::Method::GET);
          if (!entry.streamingHandlers[headIdx] && !entry.normalHandlers[headIdx]) {
            if (entry.streamingHandlers[getIdx] || entry.normalHandlers[getIdx]) {
              effectiveMethodEnum = http::Method::GET;
              idx = getIdx;
            }
          }
        }
        if (entry.streamingHandlers[idx]) {
          closeConnection = callStreamingHandler(entry.streamingHandlers[idx], req, fd, state, consumedBytes, reqStart);
          handledStreaming = true;
        } else if (entry.normalHandlers[idx]) {
          HttpResponse resp(405);
          if (http::methodAllowed(entry.normalMethodMask | entry.streamingMethodMask, effectiveMethodEnum)) {
            // TODO: factorize this try catch code
            try {
              resp = entry.normalHandlers[idx](req);
            } catch (const std::exception& ex) {
              log::error("Exception in path handler: {}", ex.what());
              resp.statusCode(500)
                  .reason(http::ReasonInternalServerError)
                  .body(ex.what())
                  .contentType(http::ContentTypeTextPlain);
            } catch (...) {
              log::error("Unknown exception in path handler.");
              resp.statusCode(500)
                  .reason(http::ReasonInternalServerError)
                  .body("Unknown error")
                  .contentType(http::ContentTypeTextPlain);
            }
          } else {
            resp.reason(http::ReasonMethodNotAllowed).body(resp.reason()).contentType(http::ContentTypeTextPlain);
          }
          finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
        } else {
          // path found but method not registered -> 405
          HttpResponse resp(405, http::ReasonMethodNotAllowed);
          resp.body(resp.reason()).contentType(http::ContentTypeTextPlain);
          finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
        }
      } else if (!endsWithSlash && _config.trailingSlashPolicy == HttpServerConfig::TrailingSlashPolicy::Normalize &&
                 req.path().size() > 1) {
        // If we didn't find the unslashed variant, try adding a slash if such a path exists; treat as same handler.
        std::string augmented(req.path());
        augmented.push_back('/');
        auto altIt = _pathHandlers.find(augmented);
        if (altIt != _pathHandlers.end()) {
          pathFound = true;
          // Reuse logic by setting path temporarily to registered variant.
          req._path = augmented;  // dispatch using canonical stored path with slash
          auto& entry = altIt->second;
          auto idxOriginal = static_cast<std::underlying_type_t<http::Method>>(methodEnum);
          auto idx = idxOriginal;
          if (isHead) {
            auto headIdx = static_cast<std::underlying_type_t<http::Method>>(http::Method::HEAD);
            auto getIdx = static_cast<std::underlying_type_t<http::Method>>(http::Method::GET);
            if (!entry.streamingHandlers[headIdx] && !entry.normalHandlers[headIdx]) {
              if (entry.streamingHandlers[getIdx] || entry.normalHandlers[getIdx]) {
                effectiveMethodEnum = http::Method::GET;
                idx = getIdx;
              }
            }
          }
          if (entry.streamingHandlers[idx]) {
            closeConnection =
                callStreamingHandler(entry.streamingHandlers[idx], req, fd, state, consumedBytes, reqStart);
            handledStreaming = true;
          } else if (entry.normalHandlers[idx]) {
            HttpResponse resp(405);
            if (http::methodAllowed(entry.normalMethodMask | entry.streamingMethodMask, effectiveMethodEnum)) {
              try {
                resp = entry.normalHandlers[idx](req);
              } catch (const std::exception& ex) {
                log::error("Exception in path handler: {}", ex.what());
                resp.statusCode(500)
                    .reason(http::ReasonInternalServerError)
                    .body(ex.what())
                    .contentType(http::ContentTypeTextPlain);
              } catch (...) {
                log::error("Unknown exception in path handler.");
                resp.statusCode(500)
                    .reason(http::ReasonInternalServerError)
                    .body("Unknown error")
                    .contentType(http::ContentTypeTextPlain);
              }
            } else {
              resp.reason(http::ReasonMethodNotAllowed).body(resp.reason()).contentType(http::ContentTypeTextPlain);
            }
            finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
          } else {
            HttpResponse resp(405, http::ReasonMethodNotAllowed);
            resp.body(resp.reason()).contentType(http::ContentTypeTextPlain);

            finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
          }
          // restore original target for metric consistency? We keep canonical (augmented) for now.
        }
      }
    }
    if (handledStreaming) {
      if (closeConnection) {
        break;
      }
      continue;  // proceed next request in buffer
    }
    if (!pathFound) {
      if (_streamingHandler) {
        closeConnection = callStreamingHandler(_streamingHandler, req, fd, state, consumedBytes, reqStart);
        if (closeConnection) {
          break;
        }
        continue;
      }
      HttpResponse resp(500);
      if (_handler) {
        try {
          resp = _handler(req);
        } catch (const std::exception& ex) {
          log::error("Exception in request handler: {}", ex.what());
          resp.reason(http::ReasonInternalServerError).body(ex.what()).contentType(http::ContentTypeTextPlain);
        } catch (...) {
          log::error("Unknown exception in request handler");
          resp.reason(http::ReasonInternalServerError).body("Unknown error").contentType(http::ContentTypeTextPlain);
        }
      } else {  // 404
        resp.statusCode(404);
        resp.reason(http::NotFound).body(resp.reason()).contentType(http::ContentTypeTextPlain);
      }
      finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
    }
    if (closeConnection) {
      break;
    }
  }
  return closeConnection;
}

bool HttpServer::callStreamingHandler(const StreamingHandler& streamingHandler, HttpRequest& req, int fd,
                                      ConnectionState& state, std::size_t consumedBytes,
                                      std::chrono::steady_clock::time_point reqStart) {
  bool wantClose = req.wantClose();
  bool isHead = req.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  if (!isHead) {
    auto encHeader = req.header(http::AcceptEncoding);
    auto negotiated = _encodingSelector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(406, http::ReasonNotAcceptable);
      resp.body("No acceptable content-coding available").contentType(http::ContentTypeTextPlain);
      bool closeConnection = false;  // will be set inside finalizeAndSendResponse if keep-alive not allowed
      finalizeAndSendResponse(fd, state, req, resp, consumedBytes, reqStart, closeConnection);
      return closeConnection;
    }
    compressionFormat = negotiated.encoding;
  }
  HttpResponseWriter writer(*this, fd, isHead, wantClose, compressionFormat);
  try {
    streamingHandler(req, writer);
  } catch (const std::exception& ex) {
    log::error("Exception in streaming handler: {}", ex.what());
  } catch (...) {
    log::error("Unknown exception in streaming handler");
  }
  if (!writer.finished()) {
    writer.end();
  }
  if (wantClose) {
    state.shouldClose = true;  // honor client directive for streaming path
  }
  bool allowKeepAlive = _config.enableKeepAlive && req.version() == http::HTTP_1_1 && !wantClose &&
                        state.requestsServed + 1 < _config.maxRequestsPerConnection && !state.shouldClose;
  ++state.requestsServed;
  state.buffer.erase_front(consumedBytes);

  if (_metricsCb) {
    RequestMetrics metrics;
    metrics.method = req.method();
    metrics.path = req.path();
    metrics.status = 200;  // best effort (streaming handler controls status directly)
    metrics.bytesIn = req.body().size();
    metrics.reusedConnection = state.requestsServed > 1;
    metrics.duration = std::chrono::steady_clock::now() - reqStart;
    _metricsCb(metrics);
  }
  if (!allowKeepAlive) {
    state.shouldClose = true;
    return true;
  }
  return false;
}

void HttpServer::eventLoop(Duration timeout) {
  refreshCachedDate();
  sweepIdleConnections();
  int ready = _loop.poll(timeout, [&](int fd, uint32_t ev) {
    if (fd == _listenSocket.fd()) {
      acceptNewConnections();
    } else if (fd == _wakeupFd) {
      // Drain wakeup counter.
      _wakeupFd.read();
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
  // If stop() requested, loop condition will exit promptly after this iteration; we already wrote to wakeup fd.
}

ServerStats HttpServer::stats() const {
  ServerStats statsOut;
  statsOut.totalBytesQueued = _stats.totalBytesQueued;
  statsOut.totalBytesWrittenImmediate = _stats.totalBytesWrittenImmediate;
  statsOut.totalBytesWrittenFlush = _stats.totalBytesWrittenFlush;
  statsOut.deferredWriteEvents = _stats.deferredWriteEvents;
  statsOut.flushCycles = _stats.flushCycles;
  statsOut.epollModFailures = _stats.epollModFailures;
  statsOut.maxConnectionOutboundBuffer = _stats.maxConnectionOutboundBuffer;
  statsOut.streamingChunkCoalesced = _stats.streamingChunkCoalesced;
  statsOut.streamingChunkLarge = _stats.streamingChunkLarge;
#ifdef AERONET_ENABLE_OPENSSL
  statsOut.tlsHandshakesSucceeded = _tlsMetrics.handshakesSucceeded;
  statsOut.tlsClientCertPresent = _tlsMetrics.clientCertPresent;
  statsOut.tlsAlpnStrictMismatches = _tlsMetricsExternal.alpnStrictMismatches;
  statsOut.tlsAlpnDistribution.reserve(_tlsMetrics.alpnDistribution.size());
  for (const auto& kv : _tlsMetrics.alpnDistribution) {
    statsOut.tlsAlpnDistribution.emplace_back(kv.first, kv.second);
  }
  statsOut.tlsVersionCounts.reserve(_tlsMetrics.versionCounts.size());
  for (const auto& kv : _tlsMetrics.versionCounts) {
    statsOut.tlsVersionCounts.emplace_back(kv.first, kv.second);
  }
  statsOut.tlsCipherCounts.reserve(_tlsMetrics.cipherCounts.size());
  for (const auto& kv : _tlsMetrics.cipherCounts) {
    statsOut.tlsCipherCounts.emplace_back(kv.first, kv.second);
  }
  statsOut.tlsHandshakeDurationCount = _tlsMetrics.handshakeDurationCount;
  statsOut.tlsHandshakeDurationTotalNs = _tlsMetrics.handshakeDurationTotalNs;
  statsOut.tlsHandshakeDurationMaxNs = _tlsMetrics.handshakeDurationMaxNs;
#endif
  return statsOut;
}

void HttpServer::emitSimpleError(int fd, ConnectionState& state, http::StatusCode code, bool& closeConnection) {
  const auto err = BuildSimpleError(code, std::string_view(_cachedDate), true);
  queueData(fd, state, err);
  try {
    _parserErrCb(code);
  } catch (const std::exception& ex) {
    // Swallow exceptions from user callback to avoid destabilizing the server
    log::error("Exception raised in user callback: {}", ex.what());
  }
  closeConnection = true;
}

}  // namespace aeronet
