#include "aeronet/http-server.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
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

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/server-stats.hpp"
#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#include "brotli-encoder.hpp"
#endif
#include "event-loop.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "zlib-encoder.hpp"
#endif
#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#include "zstd-encoder.hpp"
#endif
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method-set.hpp"
#include "aeronet/http-method.hpp"
#include "connection-state.hpp"
#include "http-error-build.hpp"
#include "http-method-build.hpp"
#include "log.hpp"
#include "socket.hpp"
#include "string-equal-ignore-case.hpp"
#include "timedef.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "tls-context.hpp"
#else
#include "invalid_argument_exception.hpp"
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
    : _config(std::move(cfg)),
      _listenSocket(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC),
      _eventLoop(_config.pollInterval),
      _encodingSelector(_config.compression) {
  _config.validate();
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
    throw invalid_argument("aeronet built without OpenSSL support but TLS configuration provided");
#endif
  }
  static constexpr int enable = 1;
  auto errc = ::setsockopt(listenFdLocal, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (errc < 0) {
    throw exception("setsockopt(SO_REUSEADDR) failed with error {}", std::strerror(errno));
  }
  if (_config.reusePort) {
    errc = ::setsockopt(listenFdLocal, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
    if (errc < 0) {
      throw exception("setsockopt(SO_REUSEPORT) error: {}", std::strerror(errno));
    }
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
  errc = ::bind(listenFdLocal, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (errc < 0) {
    throw exception("bind failed with error {}", std::strerror(errno));
  }
  if (::listen(listenFdLocal, SOMAXCONN) < 0) {
    throw exception("listen failed with error {}", std::strerror(errno));
  }
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    if (::getsockname(listenFdLocal, reinterpret_cast<sockaddr*>(&actual), &alen) == 0) {
      _config.port = ntohs(actual.sin_port);
    }
  }
  if (!_eventLoop.add(listenFdLocal, EPOLLIN)) {
    throw exception("EventLoop add listen socket failed");
  }
  // Register wakeup fd
  if (!_eventLoop.add(_wakeupFd.fd(), EPOLLIN)) {
    throw exception("EventLoop add wakeup fd failed");
  }
// Pre-allocate encoders (one per supported format if available at compile time) so per-response paths can reuse them.
#ifdef AERONET_ENABLE_ZLIB
  _encoders[static_cast<std::size_t>(Encoding::gzip)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::gzip, _config.compression);
  _encoders[static_cast<std::size_t>(Encoding::deflate)] =
      std::make_unique<ZlibEncoder>(details::ZStreamRAII::Variant::deflate, _config.compression);
#endif
#ifdef AERONET_ENABLE_ZSTD
  _encoders[static_cast<std::size_t>(Encoding::zstd)] = std::make_unique<ZstdEncoder>(_config.compression);
#endif
#ifdef AERONET_ENABLE_BROTLI
  _encoders[static_cast<std::size_t>(Encoding::br)] = std::make_unique<BrotliEncoder>(_config.compression);
#endif
}

HttpServer::~HttpServer() { stop(); }

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
HttpServer::HttpServer(HttpServer&& other)
    : _stats(std::exchange(other._stats, {})),
      _config(std::move(other._config)),
      _listenSocket(std::move(other._listenSocket)),
      _eventLoop(std::move(other._eventLoop)),
      _wakeupFd(std::move(other._wakeupFd)),
      _running(std::exchange(other._running, false)),
      _handler(std::move(other._handler)),
      _streamingHandler(std::move(other._streamingHandler)),
      _pathHandlers(std::move(other._pathHandlers)),
      _connStates(std::move(other._connStates)),
      _encoders(std::move(other._encoders)),
      _encodingSelector(std::move(other._encodingSelector)),
      _cachedDateEpoch(std::exchange(other._cachedDateEpoch, TimePoint{})),
      _parserErrCb(std::move(other._parserErrCb)),
      _metricsCb(std::move(other._metricsCb)),
      _tmpBuffer(std::move(other._tmpBuffer))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tlsCtxHolder(std::move(other._tlsCtxHolder)),
      _tlsMetrics(std::move(other._tlsMetrics)),
      _tlsMetricsExternal(std::exchange(other._tlsMetricsExternal, {}))
#endif

{
  if (_running) {  // original state captured before exchange
    // Restore source invariants not needed (members already moved) then throw.
    throw std::runtime_error("Cannot move-construct a running HttpServer");
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
HttpServer& HttpServer::operator=(HttpServer&& other) {
  if (this != &other) {
    stop();
    if (other._running) {
      other.stop();
      throw std::runtime_error("Cannot move-assign from a running HttpServer");
    }
    _stats = std::exchange(other._stats, {});
    _config = std::move(other._config);
    _listenSocket = std::move(other._listenSocket);
    _eventLoop = std::move(other._eventLoop);
    _wakeupFd = std::move(other._wakeupFd);
    _running = std::exchange(other._running, false);
    _handler = std::move(other._handler);
    _streamingHandler = std::move(other._streamingHandler);
    _pathHandlers = std::move(other._pathHandlers);
    _connStates = std::move(other._connStates);
    _encoders = std::move(other._encoders);
    _encodingSelector = std::move(other._encodingSelector);
    _cachedDateEpoch = std::exchange(other._cachedDateEpoch, TimePoint{});
    _parserErrCb = std::move(other._parserErrCb);
    _metricsCb = std::move(other._metricsCb);
    _tmpBuffer = std::move(other._tmpBuffer);
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

void HttpServer::addPathHandler(std::string path, const http::MethodSet& methods, RequestHandler handler) {
  auto it = _pathHandlers.emplace(std::move(path), PathHandlerEntry{}).first;
  PathHandlerEntry* pEntry = &it->second;
  RequestHandler* pHandler = nullptr;
  for (http::Method method : methods) {
    auto idx = static_cast<std::underlying_type_t<http::Method>>(method);
    if (pEntry->streamingHandlers[idx]) {
      throw exception("Cannot register normal handler: streaming handler already present for path+method");
    }
    if (pHandler == nullptr) {
      pEntry->normalHandlers[idx] = std::move(handler);  // NOLINT(bugprone-use-after-move)
      pHandler = &pEntry->normalHandlers[idx];
    } else {
      pEntry->normalHandlers[idx] = *pHandler;
    }
    pEntry->normalMethodMask |= http::singleMethodToMask(method);
  }
}

void HttpServer::addPathHandler(std::string path, http::Method method, RequestHandler handler) {
  addPathHandler(std::move(path), http::MethodSet{method}, std::move(handler));
}

void HttpServer::addPathStreamingHandler(std::string path, const http::MethodSet& methods, StreamingHandler handler) {
  auto it = _pathHandlers.emplace(std::move(path), PathHandlerEntry{}).first;
  PathHandlerEntry* pEntry = &it->second;
  StreamingHandler* pHandler = nullptr;
  for (http::Method method : methods) {
    auto idx = static_cast<std::underlying_type_t<http::Method>>(method);
    if (pEntry->normalHandlers[idx]) {
      throw exception("Cannot register streaming handler: normal handler already present for path+method");
    }
    if (pHandler == nullptr) {
      pEntry->streamingHandlers[idx] = std::move(handler);  // NOLINT(bugprone-use-after-move)
      pHandler = &pEntry->streamingHandlers[idx];
    } else {
      pEntry->streamingHandlers[idx] = *pHandler;
    }
    pEntry->streamingMethodMask |= http::singleMethodToMask(method);
  }
}

void HttpServer::addPathStreamingHandler(std::string path, http::Method method, StreamingHandler handler) {
  addPathStreamingHandler(std::move(path), http::MethodSet{method}, std::move(handler));
}

void HttpServer::run() {
  if (_running) {
    throw exception("Server is already running");
  }
  log::info("Server running on port :{}", port());
  for (_running = true; _running;) {
    eventLoop();
  }
}

void HttpServer::stop() noexcept {
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
  log::info("Server running on port :{}", port());
  for (_running = true; _running && !predicate();) {
    eventLoop();
  }
  stop();
}

bool HttpServer::ModWithCloseOnFailure(EventLoop& loop, ConnectionMapIt cnxIt, uint32_t events, const char* ctx,
                                       StatsInternal& stats) {
  if (loop.mod(cnxIt->first.fd(), events)) {
    return true;
  }
  const auto errCode = errno;
  ++stats.epollModFailures;
  // EBADF or ENOENT can occur during races where a connection is concurrently closed; downgrade severity.
  if (errCode == EBADF || errCode == ENOENT) {
    log::warn("epoll_ctl MOD benign failure (ctx={}, fd={}, events=0x{:x}, errno={}, msg={})", ctx, cnxIt->first.fd(),
              events, errCode, std::strerror(errCode));
  } else {
    log::error("epoll_ctl MOD failed (ctx={}, fd={}, events=0x{:x}, errno={}, msg={})", ctx, cnxIt->first.fd(), events,
               errCode, std::strerror(errCode));
  }
  cnxIt->second.requestDrainAndClose();
  return false;
}

bool HttpServer::processRequestsOnConnection(ConnectionMapIt cnxIt) {
  ConnectionState& state = cnxIt->second;
  while (true) {
    const auto reqStart = std::chrono::steady_clock::now();
    // If we don't yet have a full request line (no '\n' observed) wait for more data instead of
    // attempting to parse and wrongly emitting a 400 which would close an otherwise healthy keep-alive.
    if (state.buffer.empty() || std::ranges::find(state.buffer, '\n') == state.buffer.end()) {
      break;  // need more bytes for at least the request line
    }
    HttpRequest req;
    const auto statusCode = req.setHead(state, _tmpBuffer, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders);
    if (statusCode != http::StatusCodeOK) {
      emitSimpleError(cnxIt, statusCode, true);
      // EmitSimpleError was invoked with immediate=true, which requested an Immediate close
      // (ConnectionState::CloseMode::Immediate). We break unconditionally; the connection
      // will be torn down after any queued error bytes are flushed. No partial recovery is
      // attempted for a malformed / protocol-violating start line or headers.
      break;
    }

    // A full request head (and body, if present) will now be processed; reset headerStart to signal
    // that the header timeout should track the next pending request only.
    state.headerStart = {};
    bool isChunked = false;
    bool hasTransferEncoding = false;
    std::string_view transferEncoding = req.headerValueOrEmpty(http::TransferEncoding);
    if (!transferEncoding.empty()) {
      hasTransferEncoding = true;
      if (req.version() == http::HTTP_1_0) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Transfer-Encoding not allowed in HTTP/1.0");
        break;
      }
      if (CaseInsensitiveEqual(transferEncoding, http::chunked)) {
        isChunked = true;
      } else {
        emitSimpleError(cnxIt, http::StatusCodeNotImplemented, true, "Unsupported Transfer-Encoding");
        break;
      }
    }

    std::string_view contentLength = req.headerValueOrEmpty(http::ContentLength);
    bool hasContentLength = !contentLength.empty();
    if (hasContentLength && hasTransferEncoding) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, true,
                      "Content-Length and Transfer-Encoding cannot be used together");
      break;
    }
    bool expectContinue = req.hasExpectContinue();
    std::size_t consumedBytes = 0;
    if (!decodeBodyIfReady(cnxIt, req, isChunked, expectContinue, consumedBytes)) {
      break;  // need more bytes or error
    }
    // Inbound request decompression (Content-Encoding). Performed after body aggregation but before dispatch.
    if (!req.body().empty() && !maybeDecompressRequestBody(cnxIt, req)) {
      break;  // error already emitted; close or wait handled inside
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
      bool endsWithSlash = req.path().size() > 1 && req.path().back() == '/';
      if (pit == _pathHandlers.end()) {
        // 2. No exact match: apply policy decision (excluding root "/").
        if (endsWithSlash) {
          // Candidate canonical form (strip single trailing slash)
          std::string_view canonical(req.path().data(), req.path().size() - 1);
          auto canonicalIt = _pathHandlers.find(canonical);
          if (canonicalIt != _pathHandlers.end()) {
            switch (_config.trailingSlashPolicy) {
              case HttpServerConfig::TrailingSlashPolicy::Normalize:
                // Treat as if request was canonical (no redirect) by mutating path temporarily.
                req._path = canonical;
                pit = canonicalIt;
                break;
              case HttpServerConfig::TrailingSlashPolicy::Redirect: {
                // Emit 301 redirect to canonical form.
                HttpResponse resp(301, http::MovedPermanently);
                resp.location(canonical).contentType(http::ContentTypeTextPlain).body("Redirecting");
                finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
                if (state.isAnyCloseRequested()) {
                  return true;  // connection closing, stop loop
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
          // Request lacks slash; check if ONLY slashed variant exists using tmp buffer to add an extra slash.
          auto altIt = _pathHandlers.find(addSlash(req.path()));
          if (altIt != _pathHandlers.end()) {
            // Normalize treats them the same: dispatch to the registered variant.
            pit = altIt;
            // It's ok to point on an intem in _pathHandlers, as by design it's not modifiable after server starts.
            req._path = altIt->first;
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
          bool streamingClose = callStreamingHandler(entry.streamingHandlers[idx], req, cnxIt, consumedBytes, reqStart);
          handledStreaming = true;
          if (streamingClose) {
            // callStreamingHandler already set close mode if needed
          }
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
          finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
        } else {
          // path found but method not registered -> 405
          HttpResponse resp(405, http::ReasonMethodNotAllowed);
          resp.body(resp.reason()).contentType(http::ContentTypeTextPlain);
          finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
        }
      } else if (!endsWithSlash && _config.trailingSlashPolicy == HttpServerConfig::TrailingSlashPolicy::Normalize &&
                 req.path().size() > 1) {
        // If we didn't find the unslashed variant, try adding a slash if such a path exists; treat as same handler.
        auto altIt = _pathHandlers.find(addSlash(req.path()));
        if (altIt != _pathHandlers.end()) {
          pathFound = true;
          // Reuse logic by setting path temporarily to registered variant.
          req._path = altIt->first;
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
            bool streamingClose =
                callStreamingHandler(entry.streamingHandlers[idx], req, cnxIt, consumedBytes, reqStart);
            handledStreaming = true;
            if (streamingClose) {
              // close mode reflects decision
            }
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
            finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
          } else {
            HttpResponse resp(405, http::ReasonMethodNotAllowed);
            resp.body(resp.reason()).contentType(http::ContentTypeTextPlain);

            finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
          }
          // restore original target for metric consistency? We keep canonical (augmented) for now.
        }
      }
    }
    if (handledStreaming) {
      if (state.isAnyCloseRequested()) {
        break;
      }
      continue;  // proceed next request in buffer
    }
    if (!pathFound) {
      if (_streamingHandler) {
        bool streamingClose = callStreamingHandler(_streamingHandler, req, cnxIt, consumedBytes, reqStart);
        if (streamingClose) {
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
      finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
    }
    if (state.isAnyCloseRequested()) {
      break;
    }
  }
  return state.isAnyCloseRequested();
}

bool HttpServer::maybeDecompressRequestBody(ConnectionMapIt cnxIt, HttpRequest& req) {
  const auto& cfg = _config.requestDecompression;
  std::string_view encHeader = req.headerValueOrEmpty(http::ContentEncoding);
  if (encHeader.empty() || CaseInsensitiveEqual(encHeader, http::identity)) {
    return true;  // nothing to do
  }
  if (!cfg.enable) {
    // Pass-through mode: leave compressed body & header intact; user code must decode manually
    // if it cares. We intentionally skip size / ratio guards in this mode to avoid surprising
    // rejections when opting out. Global body size limits have already been enforced.
    return true;
  }
  const std::size_t originalCompressedSize = req.body().size();
  if (cfg.maxCompressedBytes != 0 && originalCompressedSize > cfg.maxCompressedBytes) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
    return false;
  }

  // We'll alternate between bodyBuffer (source) and _tmpBuffer (target) each stage.
  std::string_view src = req.body();
  RawChars* dst = &_tmpBuffer;
  ConnectionState& state = cnxIt->second;

  // Decode in reverse order.
  const char* first = encHeader.data();
  const char* last = first + encHeader.size();
  while (first < last) {
    const char* encodingLast = last;
    while (encodingLast != first && (*encodingLast == ' ' || *encodingLast == '\t')) {
      --encodingLast;
    }
    if (encodingLast == first) {
      break;
    }
    const char* comma = encodingLast - 1;
    while (comma != first && *comma != ',') {
      --comma;
    }
    if (comma == first) {
      --comma;
    }
    const char* encodingFirst = comma + 1;
    while (encodingFirst != encodingLast && (*encodingFirst == ' ' || *encodingFirst == '\t')) {
      ++encodingFirst;
    }
    if (encodingFirst == encodingLast) {  // empty token => malformed list
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Malformed Content-Encoding");
      return false;
    }

    std::string_view encoding(encodingFirst, encodingLast);
    dst->clear();
    bool stageOk;
    if (CaseInsensitiveEqual(encoding, http::identity)) {
      last = comma;
      continue;  // no-op layer
#ifdef AERONET_ENABLE_ZLIB
      // NOLINTNEXTLINE(readability-else-after-return)
    } else if (CaseInsensitiveEqual(encoding, http::gzip)) {
      stageOk = ZlibDecoder::Decompress(src, true, cfg.maxDecompressedBytes, cfg.decoderChunkSize, *dst);
    } else if (CaseInsensitiveEqual(encoding, http::deflate)) {
      stageOk = ZlibDecoder::Decompress(src, false, cfg.maxDecompressedBytes, cfg.decoderChunkSize, *dst);
#endif
#ifdef AERONET_ENABLE_ZSTD
    } else if (CaseInsensitiveEqual(encoding, http::zstd)) {
      stageOk = ZstdDecoder::Decompress(src, cfg.maxDecompressedBytes, cfg.decoderChunkSize, *dst);
#endif
#ifdef AERONET_ENABLE_BROTLI
    } else if (CaseInsensitiveEqual(encoding, http::br)) {
      stageOk = BrotliDecoder::Decompress(src, cfg.maxDecompressedBytes, cfg.decoderChunkSize, *dst);
#endif
    } else {
      emitSimpleError(cnxIt, http::StatusCodeUnsupportedMediaType, true, "Unsupported Content-Encoding");
      return false;
    }
    if (!stageOk) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Decompression failed");
      return false;
    }
    // Expansion guard after each stage (defensive against nested bombs).
    if (cfg.maxExpansionRatio > 0.0 && originalCompressedSize > 0) {
      double ratio = static_cast<double>(dst->size()) / static_cast<double>(originalCompressedSize);
      if (ratio > cfg.maxExpansionRatio) {
        emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true, "Decompression expansion too large");
        return false;
      }
    }

    src = *dst;
    dst = dst == &state.bodyBuffer ? &_tmpBuffer : &state.bodyBuffer;

    last = comma;
  }

  if (src.data() == _tmpBuffer.data()) {
    // make sure we use bodyBuffer to "free" usage of _tmpBuffer for other things
    _tmpBuffer.swap(state.bodyBuffer);
    src = state.bodyBuffer;
  }

  // Final decompressed data now resides in *src after last swap.
  req._body = src;
  // Strip Content-Encoding header so user handlers observe a canonical, already-decoded body.
  // Rationale: After automatic request decompression the original header no longer reflects
  // the semantics of req.body() (which now holds the decoded representation). Exposing the stale
  // header risks double-decoding attempts or confusion about body length. The original compressed
  // size can be reintroduced later via RequestMetrics enrichment.
  req._headers.erase(http::ContentEncoding);
  return true;
}

bool HttpServer::callStreamingHandler(const StreamingHandler& streamingHandler, HttpRequest& req, ConnectionMapIt cnxIt,
                                      std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart) {
  bool wantClose = req.wantClose();
  bool isHead = req.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = cnxIt->second;
  if (!isHead) {
    auto encHeader = req.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _encodingSelector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(406, http::ReasonNotAcceptable);
      resp.body("No acceptable content-coding available").contentType(http::ContentTypeTextPlain);
      finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }
  HttpResponseWriter writer(*this, cnxIt->first.fd(), isHead, wantClose, compressionFormat);
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
    state.requestDrainAndClose();  // honor client directive for streaming path
  }
  bool allowKeepAlive = _config.enableKeepAlive && req.version() == http::HTTP_1_1 && !wantClose &&
                        state.requestsServed + 1 < _config.maxRequestsPerConnection && !state.isAnyCloseRequested();
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
    state.requestDrainAndClose();
    return true;
  }
  return false;
}

void HttpServer::eventLoop() {
  _cachedDateEpoch = Clock::now();
  sweepIdleConnections();
  int ready = _eventLoop.poll([&](int fd, uint32_t ev) {
    if (fd == _listenSocket.fd()) {
      acceptNewConnections();
    } else if (fd == _wakeupFd.fd()) {
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

void HttpServer::emitSimpleError(ConnectionMapIt cnxIt, http::StatusCode code, bool immediate,
                                 std::string_view reason) {
  if (reason.empty()) {
    reason = http::reasonPhraseFor(code);
  }
  BuildSimpleError(code, _cachedDateEpoch, _config.globalHeaders, reason, _tmpBuffer);
  queueData(cnxIt, _tmpBuffer);
  try {
    _parserErrCb(code);
  } catch (const std::exception& ex) {
    // Swallow exceptions from user callback to avoid destabilizing the server
    log::error("Exception raised in user callback: {}", ex.what());
  }
  if (immediate) {
    cnxIt->second.requestImmediateClose();
  } else {
    cnxIt->second.requestDrainAndClose();
  }
}

std::string_view HttpServer::addSlash(std::string_view path) {
  RawChars& normalizedStorage = _tmpBuffer;
  normalizedStorage.clear();

  normalizedStorage.ensureAvailableCapacity(path.size() + 1U);
  std::memcpy(normalizedStorage.data(), path.data(), path.size());
  normalizedStorage[path.size()] = '/';
  normalizedStorage.setSize(path.size() + 1U);
  return normalizedStorage;
}

}  // namespace aeronet
