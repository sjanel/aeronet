#include "aeronet/http-server.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/router.hpp"
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
#include "aeronet/http-method.hpp"
#include "connection-state.hpp"
#include "http-error-build.hpp"
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

HttpServer::HttpServer(HttpServerConfig cfg)
    : _config(std::move(cfg)),
      _router(_config.router),
      _encodingSelector(_config.compression),
      _telemetry(_config.otel) {
  init();
}

HttpServer::HttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _router(std::move(router)),
      _encodingSelector(_config.compression),
      _telemetry(_config.otel) {
  init();
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
      _router(std::move(other._router)),
      _connStates(std::move(other._connStates)),
      _encoders(std::move(other._encoders)),
      _encodingSelector(std::move(other._encodingSelector)),
      _parserErrCb(std::move(other._parserErrCb)),
      _metricsCb(std::move(other._metricsCb)),
      _tmpBuffer(std::move(other._tmpBuffer)),
      _telemetry(std::move(other._telemetry))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tlsCtxHolder(std::move(other._tlsCtxHolder)),
      _tlsMetrics(std::move(other._tlsMetrics)),
      _tlsMetricsExternal(std::exchange(other._tlsMetricsExternal, {}))
#endif

{
  if (_running) {
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
    _router = std::move(other._router);
    _connStates = std::move(other._connStates);
    _encoders = std::move(other._encoders);
    _encodingSelector = std::move(other._encodingSelector);
    _parserErrCb = std::move(other._parserErrCb);
    _metricsCb = std::move(other._metricsCb);
    _tmpBuffer = std::move(other._tmpBuffer);
    _telemetry = std::move(other._telemetry);
#ifdef AERONET_ENABLE_OPENSSL
    _tlsCtxHolder = std::move(other._tlsCtxHolder);
    _tlsMetrics = std::move(other._tlsMetrics);
    _tlsMetricsExternal = std::exchange(other._tlsMetricsExternal, {});
#endif
  }
  return *this;
}

void HttpServer::run() {
  prepareRun();
  for (_running = true; _running;) {
    eventLoop();
  }
}

void HttpServer::runUntil(const std::function<bool()>& predicate) {
  prepareRun();
  for (_running = true; _running && !predicate();) {
    eventLoop();
  }
  _running = false;
}

void HttpServer::stop() noexcept {
  if (!_running) {
    return;
  }
  log::debug("Stopping server");
  _running = false;
  // Trigger wakeup to break any blocking epoll_wait quickly.
  _wakeupFd.send();

  // Close the socket
  _listenSocket.close();
  log::info("Server stopped");
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
  do {
    // If we don't yet have a full request line (no '\n' observed) wait for more data
    if (state.buffer.size() < http::kHttpReqLineMinLen) {
      break;  // need more bytes for at least the request line
    }
    const auto reqStart = std::chrono::steady_clock::now();

    HttpRequest req;
    const auto statusCode = req.setHead(state, _tmpBuffer, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders);
    if (statusCode == 0) {
      // need more data
      break;
    }

    if (statusCode != http::StatusCodeOK) {
      // If status code == 0, we need more data
      if (statusCode != 0) {
        emitSimpleError(cnxIt, statusCode, true);
      }

      // We break unconditionally; the connection
      // will be torn down after any queued error bytes are flushed. No partial recovery is
      // attempted for a malformed / protocol-violating start line or headers.
      break;
    }

    // Start a span for this request if tracing is enabled
    // We create it after parsing the request head so we have method and path available
    auto span = _telemetry.createSpan("http.request");
    if (span) {
      span->setAttribute("http.method", http::toMethodStr(req.method()));
      span->setAttribute("http.target", req.path());
      span->setAttribute("http.scheme", "http");

      const auto header = req.headerValueOrEmpty("Host");
      if (!header.empty()) {
        span->setAttribute("http.host", header);
      }
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
    // Provide implicit HEAD->GET fallback (RFC7231: HEAD is identical to GET without body) when
    // a HEAD handler is not explicitly registered but a GET handler exists for the same path.
    const auto res = _router.match(req.method(), req.path());
    if (res.streamingHandler != nullptr) {
      bool streamingClose = callStreamingHandler(*res.streamingHandler, req, cnxIt, consumedBytes, reqStart);
      if (streamingClose) {
        break;
      }
      continue;
    }

    HttpResponse resp;
    if (res.requestHandler != nullptr) {
      // normal handler
      try {
        resp = (*res.requestHandler)(req);
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
    } else if (res.redirectPathIndicator != Router::RoutingResult::RedirectSlashMode::None) {
      // Emit 301 redirect to canonical form.
      resp.statusCode(http::StatusCodeMovedPermanently)
          .reason(http::MovedPermanently)
          .contentType(http::ContentTypeTextPlain)
          .body("Redirecting");
      if (res.redirectPathIndicator == Router::RoutingResult::RedirectSlashMode::AddSlash) {
        _tmpBuffer.assign(req.path());
        _tmpBuffer.push_back('/');
        resp.location(_tmpBuffer);
      } else {
        resp.location(req.path().substr(0, req.path().size() - 1));
      }

      consumedBytes = 0;  // already advanced
    } else if (res.methodNotAllowed) {
      resp.statusCode(http::StatusCodeMethodNotAllowed)
          .reason(http::ReasonMethodNotAllowed)
          .body(resp.reason())
          .contentType(http::ContentTypeTextPlain);
    } else {
      resp.statusCode(http::StatusCodeNotFound);
      resp.reason(http::NotFound).body(resp.reason()).contentType(http::ContentTypeTextPlain);
    }
    finalizeAndSendResponse(cnxIt, req, resp, consumedBytes, reqStart);

    // End the span after response is finalized
    if (span) {
      span->setAttribute("http.status_code", resp.statusCode());
      const auto reqEnd = std::chrono::steady_clock::now();
      const auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(reqEnd - reqStart);
      span->setAttribute("http.duration_us", durationUs.count());
      span->end();
    }
  } while (!state.isAnyCloseRequested());
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
void HttpServer::init() {
  _config.validate();

  _eventLoop = EventLoop(_config.pollInterval);

  _listenSocket = Socket(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC);

  const int listenFdLocal = _listenSocket.fd();

  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls) {
#ifdef AERONET_ENABLE_OPENSSL
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

void HttpServer::prepareRun() {
  if (_running) {
    throw exception("Server is already running");
  }
  if (!_listenSocket.isOpened()) {
    init();
  }
  log::info("Server running on port :{}", port());
}

void HttpServer::eventLoop() {
  sweepIdleConnections();

  bool stopRequested = false;
  int ready = _eventLoop.poll([this, &stopRequested](int fd, uint32_t ev) {
    if (fd == _listenSocket.fd()) {
      acceptNewConnections();
    } else if (fd == _wakeupFd.fd()) {
      // Drain wakeup counter.
      _wakeupFd.read();
      stopRequested = true;
    } else {
      if (ev & EPOLLOUT) {
        handleWritableClient(fd);
      }
      if (ev & EPOLLIN) {
        handleReadableClient(fd);
      }
    }
  });

  // Record event loop activity metrics
  if (ready > 0) {
    _telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(ready));
  } else if (ready < 0) {
    _telemetry.counterAdd("aeronet.events.errors", 1);
  }

  // If epoll_wait failed with a non-EINTR error (EINTR is mapped to 0 in EventLoop::poll), ready will be -1.
  // Not handling this would cause a tight loop spinning on the failing epoll fd (e.g., after EBADF or EINVAL),
  // burning CPU while doing no useful work. Treat it as fatal: log and stop the server.
  if (ready < 0) {
    log::error("epoll_wait (eventLoop) failed: {}", std::strerror(errno));
    // Mark server as no longer running so outer loops terminate gracefully.
    _running = false;
  }

  // If stop() requested, loop condition will exit promptly after this iteration; we already wrote to wakeup fd.
  if (stopRequested) {
    // Close all remaining connections (if some) after all events processed.
    auto it = _connStates.begin();
    while (it != _connStates.end()) {
      it = closeConnection(it);
    }
  }
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
  queueData(cnxIt, BuildSimpleError(code, _config.globalHeaders, reason));
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

}  // namespace aeronet
