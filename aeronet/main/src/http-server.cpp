#include "aeronet/http-server.hpp"

#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
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
#include <utility>

#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "connection-state.hpp"
#include "event-loop.hpp"
#include "exception.hpp"
#include "flat-hash-map.hpp"
#include "http-error-build.hpp"
#include "log.hpp"
#include "simple-charconv.hpp"
#include "socket.hpp"
#include "string-equal-ignore-case.hpp"
#include "timedef.hpp"

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#include "brotli-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#include "zlib-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#include "zstd-encoder.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "tls-context.hpp"
#else
#include "invalid_argument_exception.hpp"
#endif

namespace aeronet {

HttpServer::HttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _config(std::move(config)),
      _router(std::move(routerConfig)),
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
      _lifecycle(std::move(other._lifecycle)),
      _router(std::move(other._router)),
      _connStates(std::move(other._connStates)),
      _encoders(std::move(other._encoders)),
      _encodingSelector(std::move(other._encodingSelector)),
      _parserErrCb(std::move(other._parserErrCb)),
      _metricsCb(std::move(other._metricsCb)),
      _expectationHandler(std::move(other._expectationHandler)),
      _request(std::move(other._request)),
      _tmpBuffer(std::move(other._tmpBuffer)),
      _telemetry(std::move(other._telemetry))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      _tlsCtxHolder(std::move(other._tlsCtxHolder)),
      _tlsMetrics(std::move(other._tlsMetrics)),
      _tlsMetricsExternal(std::exchange(other._tlsMetricsExternal, {}))
#endif

{
  if (!_lifecycle.isIdle()) {
    throw std::runtime_error("Cannot move-construct a running HttpServer");
  }
  other._lifecycle.reset();

  // Because probe handlers may capture 'this', they need to be re-registered on the moved-to instance
  if (_config.builtinProbes.enabled) {
    registerBuiltInProbes();
  }
}

// NOLINTNEXTLINE(bugprone-exception-escape,performance-noexcept-move-constructor)
HttpServer& HttpServer::operator=(HttpServer&& other) {
  if (this != &other) {
    stop();
    if (!other._lifecycle.isIdle()) {
      other.stop();
      throw std::runtime_error("Cannot move-assign from a running HttpServer");
    }
    _stats = std::exchange(other._stats, {});
    _config = std::move(other._config);
    _listenSocket = std::move(other._listenSocket);
    _eventLoop = std::move(other._eventLoop);
    _lifecycle = std::move(other._lifecycle);
    _router = std::move(other._router);
    _connStates = std::move(other._connStates);
    _encoders = std::move(other._encoders);
    _encodingSelector = std::move(other._encodingSelector);
    _parserErrCb = std::move(other._parserErrCb);
    _metricsCb = std::move(other._metricsCb);
    _expectationHandler = std::move(other._expectationHandler);
    _request = std::move(other._request);
    _tmpBuffer = std::move(other._tmpBuffer);
    _telemetry = std::move(other._telemetry);
#ifdef AERONET_ENABLE_OPENSSL
    _tlsCtxHolder = std::move(other._tlsCtxHolder);
    _tlsMetrics = std::move(other._tlsMetrics);
    _tlsMetricsExternal = std::exchange(other._tlsMetricsExternal, {});
#endif

    // Because probe handlers may capture 'this', they need to be re-registered on the moved-to instance
    if (_config.builtinProbes.enabled) {
      registerBuiltInProbes();
    }
  }
  other._lifecycle.reset();
  return *this;
}

void HttpServer::setParserErrorCallback(ParserErrorCallback cb) { _parserErrCb = std::move(cb); }

void HttpServer::setMetricsCallback(MetricsCallback cb) { _metricsCb = std::move(cb); }

void HttpServer::setExpectationHandler(ExpectationHandler handler) { _expectationHandler = std::move(handler); }

void HttpServer::run() {
  prepareRun();
  _lifecycle.enterRunning();
  while (_lifecycle.isActive()) {
    eventLoop();
  }
  _lifecycle.reset();
}

void HttpServer::runUntil(const std::function<bool()>& predicate) {
  prepareRun();
  _lifecycle.enterRunning();
  while (_lifecycle.isActive() && !predicate()) {
    eventLoop();
  }
  if (_lifecycle.isActive()) {
    _lifecycle.reset();
  }
}

void HttpServer::stop() noexcept {
  if (!_lifecycle.isActive()) {
    return;
  }
  log::debug("Stopping server");
  closeListener();
  _lifecycle.enterStopping();
}

void HttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept {
  if (!_lifecycle.isActive() || _lifecycle.isStopping()) {
    return;
  }

  const bool hasDeadline = maxWait.count() > 0;
  const auto deadline =
      hasDeadline ? std::chrono::steady_clock::now() + maxWait : std::chrono::steady_clock::time_point{};

  if (_lifecycle.isDraining()) {
    if (hasDeadline) {
      _lifecycle.shrinkDeadline(deadline);
    }
    return;
  }

  log::info("Initiating graceful drain (connections={})", _connStates.size());
  _lifecycle.enterDraining(deadline, hasDeadline);
  closeListener();
}

namespace {
void RecordModFailure(auto cnxIt, uint32_t events, const char* ctx, auto& stats) {
  const auto errCode = errno;
  ++stats.epollModFailures;
  // EBADF or ENOENT can occur during races where a connection is concurrently closed; downgrade severity.
  if (errCode == EBADF || errCode == ENOENT) {
    log::warn("epoll_ctl MOD benign failure (ctx={}, fd # {}, events=0x{:x}, errno={}, msg={})", ctx, cnxIt->first.fd(),
              events, errCode, std::strerror(errCode));
  } else {
    log::error("epoll_ctl MOD failed (ctx={}, fd # {}, events=0x{:x}, errno={}, msg={})", ctx, cnxIt->first.fd(),
               events, errCode, std::strerror(errCode));
  }
  cnxIt->second.requestDrainAndClose();
}
}  // namespace

bool HttpServer::enableWritableInterest(ConnectionMapIt cnxIt, const char* ctx) {
  static constexpr uint32_t kEvents = EPOLLIN | EPOLLOUT | EPOLLET;

  if (_eventLoop.mod(cnxIt->first.fd(), kEvents)) {
    if (!cnxIt->second.waitingWritable) {
      cnxIt->second.waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
    return true;
  }
  RecordModFailure(cnxIt, kEvents, ctx, _stats);
  return false;
}

bool HttpServer::disableWritableInterest(ConnectionMapIt cnxIt, const char* ctx) {
  static constexpr uint32_t kEvents = EPOLLIN | EPOLLET;
  if (_eventLoop.mod(cnxIt->first.fd(), kEvents)) {
    cnxIt->second.waitingWritable = false;
    return true;
  }
  RecordModFailure(cnxIt, kEvents, ctx, _stats);
  return false;
}

bool HttpServer::processRequestsOnConnection(ConnectionMapIt cnxIt) {
  ConnectionState& state = cnxIt->second;
  do {
    // If we don't yet have a full request line (no '\n' observed) wait for more data
    if (state.inBuffer.size() < http::kHttpReqLineMinLen) {
      break;  // need more bytes for at least the request line
    }
    const auto statusCode =
        _request.initTrySetHead(state, _tmpBuffer, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders);
    if (statusCode == 0) {
      // need more data
      break;
    }

    static constexpr uint64_t kShrinkRequestNnRequestPeriod = 1000;

    if (++_stats.totalRequestsServed % kShrinkRequestNnRequestPeriod == 0) {
      _request.shrink_to_fit();
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
      span->setAttribute("http.method", http::toMethodStr(_request.method()));
      span->setAttribute("http.target", _request.path());
      span->setAttribute("http.scheme", "http");

      const auto header = _request.headerValueOrEmpty("Host");
      if (!header.empty()) {
        span->setAttribute("http.host", header);
      }
    }

    // A full request head (and body, if present) will now be processed; reset headerStart to signal
    // that the header timeout should track the next pending request only.
    state.headerStart = {};
    bool isChunked = false;
    bool hasTransferEncoding = false;
    const std::string_view transferEncoding = _request.headerValueOrEmpty(http::TransferEncoding);
    if (!transferEncoding.empty()) {
      hasTransferEncoding = true;
      if (_request.version() == http::HTTP_1_0) {
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

    const std::string_view contentLength = _request.headerValueOrEmpty(http::ContentLength);
    const bool hasContentLength = !contentLength.empty();
    if (hasContentLength && hasTransferEncoding) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, true,
                      "Content-Length and Transfer-Encoding cannot be used together");
      break;
    }
    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    const std::string_view expectHeader = _request.headerValueOrEmpty(http::Expect);
    bool found100Continue = false;
    if (!expectHeader.empty() && handleExpectHeader(cnxIt, state, found100Continue)) {
      break;  // stop processing this request (response queued)
    }
    const bool expectContinue = found100Continue || _request.hasExpectContinue();
    std::size_t consumedBytes = 0;
    if (!decodeBodyIfReady(cnxIt, isChunked, expectContinue, consumedBytes)) {
      break;  // need more bytes or error
    }
    // Inbound request decompression (Content-Encoding). Performed after body aggregation but before dispatch.
    if (!_request.body().empty() && !maybeDecompressRequestBody(cnxIt)) {
      break;  // error already emitted; close or wait handled inside
    }

    // Handle OPTIONS and TRACE per RFC 7231 §4.3
    // processSpecialMethods may emplace into _connStates (inserting upstream) and
    // will update cnxIt by reference if rehashing occurs.
    const auto action = processSpecialMethods(cnxIt, consumedBytes);
    if (action == LoopAction::Continue) {
      continue;
    }
    if (action == LoopAction::Break) {
      break;
    }

    const auto routingResult = _router.match(_request.method(), _request.path());
    if (routingResult.streamingHandler != nullptr) {
      const bool streamingClose = callStreamingHandler(*routingResult.streamingHandler, cnxIt, consumedBytes);
      if (streamingClose) {
        break;
      }
      continue;
    }

    HttpResponse resp;
    if (routingResult.requestHandler != nullptr) {
      // normal handler
      try {
        resp = (*routingResult.requestHandler)(_request);
      } catch (const std::exception& ex) {
        log::error("Exception in path handler: {}", ex.what());
        resp.statusCode(http::StatusCodeInternalServerError)
            .reason(http::ReasonInternalServerError)
            .contentType(http::ContentTypeTextPlain)
            .body(ex.what());
      } catch (...) {
        log::error("Unknown exception in path handler");
        resp.statusCode(http::StatusCodeInternalServerError)
            .reason(http::ReasonInternalServerError)
            .contentType(http::ContentTypeTextPlain)
            .body("Unknown error");
      }
    } else if (routingResult.redirectPathIndicator != Router::RoutingResult::RedirectSlashMode::None) {
      // Emit 301 redirect to canonical form.
      resp.statusCode(http::StatusCodeMovedPermanently)
          .reason(http::MovedPermanently)
          .contentType(http::ContentTypeTextPlain)
          .body("Redirecting");
      if (routingResult.redirectPathIndicator == Router::RoutingResult::RedirectSlashMode::AddSlash) {
        _tmpBuffer.assign(_request.path());
        _tmpBuffer.push_back('/');
        resp.location(_tmpBuffer);
      } else {
        resp.location(_request.path().substr(0, _request.path().size() - 1));
      }

      consumedBytes = 0;  // already advanced
    } else if (routingResult.methodNotAllowed) {
      resp.statusCode(http::StatusCodeMethodNotAllowed)
          .reason(http::ReasonMethodNotAllowed)
          .contentType(http::ContentTypeTextPlain)
          .body(resp.reason());
    } else {
      resp.statusCode(http::StatusCodeNotFound);
      resp.reason(http::NotFound).contentType(http::ContentTypeTextPlain).body(http::NotFound);
    }

    const auto respStatusCode = resp.statusCode();

    finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes);

    // End the span after response is finalized
    if (span) {
      const auto reqEnd = std::chrono::steady_clock::now();
      const auto durationUs = std::chrono::duration_cast<std::chrono::microseconds>(reqEnd - _request.reqStart());

      span->setAttribute("http.status_code", respStatusCode);
      span->setAttribute("http.duration_us", durationUs.count());

      span->end();
    }
  } while (!state.isAnyCloseRequested());
  return state.isAnyCloseRequested();
}

bool HttpServer::maybeDecompressRequestBody(ConnectionMapIt cnxIt) {
  const auto& cfg = _config.decompression;
  std::string_view encHeader = _request.headerValueOrEmpty(http::ContentEncoding);
  if (encHeader.empty() || CaseInsensitiveEqual(encHeader, http::identity)) {
    return true;  // nothing to do
  }
  if (!cfg.enable) {
    // Pass-through mode: leave compressed body & header intact; user code must decode manually
    // if it cares. We intentionally skip size / ratio guards in this mode to avoid surprising
    // rejections when opting out. Global body size limits have already been enforced.
    return true;
  }
  const std::size_t originalCompressedSize = _request.body().size();
  if (cfg.maxCompressedBytes != 0 && originalCompressedSize > cfg.maxCompressedBytes) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
    return false;
  }

  // We'll alternate between bodyAndTrailersBuffer (source) and _tmpBuffer (target) each stage.
  std::string_view src = _request.body();
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
    dst = dst == &state.bodyAndTrailersBuffer ? &_tmpBuffer : &state.bodyAndTrailersBuffer;

    last = comma;
  }

  if (src.data() == _tmpBuffer.data()) {
    // make sure we use bodyAndTrailersBuffer to "free" usage of _tmpBuffer for other things
    _tmpBuffer.swap(state.bodyAndTrailersBuffer);
    src = state.bodyAndTrailersBuffer;
  }

  // Final decompressed data now resides in *src after last swap.
  _request._body = src;
  // Strip Content-Encoding header so user handlers observe a canonical, already-decoded body.
  // Rationale: After automatic request decompression the original header no longer reflects
  // the semantics of req.body() (which now holds the decoded representation). Exposing the stale
  // header risks double-decoding attempts or confusion about body length. The original compressed
  // size can be reintroduced later via RequestMetrics enrichment.
  _request._headers.erase(http::ContentEncoding);
  return true;
}

bool HttpServer::callStreamingHandler(const StreamingHandler& streamingHandler, ConnectionMapIt cnxIt,
                                      std::size_t consumedBytes) {
  bool wantClose = _request.wantClose();
  bool isHead = _request.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = cnxIt->second;
  if (!isHead) {
    auto encHeader = _request.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _encodingSelector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(406, http::ReasonNotAcceptable);
      resp.contentType(http::ContentTypeTextPlain).body("No acceptable content-coding available");
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }

  HttpResponseWriter writer(*this, cnxIt->first.fd(), isHead, wantClose, compressionFormat);
  try {
    streamingHandler(_request, writer);
  } catch (const std::exception& ex) {
    log::error("Exception in streaming handler: {}", ex.what());
  } catch (...) {
    log::error("Unknown exception in streaming handler");
  }
  if (!writer.finished()) {
    writer.end();
  }

  ++state.requestsServed;
  state.inBuffer.erase_front(consumedBytes);

  const bool shouldClose = !_config.enableKeepAlive || _request.version() != http::HTTP_1_1 || wantClose ||
                           state.requestsServed + 1 >= _config.maxRequestsPerConnection ||
                           state.isAnyCloseRequested() || _lifecycle.isDraining() || _lifecycle.isStopping();
  if (shouldClose) {
    state.requestDrainAndClose();
  }

  if (_metricsCb) {
    emitRequestMetrics(http::StatusCodeOK, _request.body().size(), state.requestsServed > 1);
  }

  return shouldClose;
}

void HttpServer::emitRequestMetrics(http::StatusCode status, std::size_t bytesIn, bool reusedConnection) {
  if (!_metricsCb) {
    return;
  }
  RequestMetrics metrics;
  metrics.method = _request.method();
  metrics.path = _request.path();
  metrics.status = status;
  metrics.bytesIn = bytesIn;
  metrics.reusedConnection = reusedConnection;
  metrics.duration = std::chrono::steady_clock::now() - _request.reqStart();
  _metricsCb(metrics);
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
  if (_config.tls.enabled) {
#ifdef AERONET_ENABLE_OPENSSL
    // Allocate TlsContext on the heap so its address remains stable even if HttpServer is moved.
    // (See detailed rationale in header next to _tlsCtxHolder.)
    _tlsCtxHolder = std::make_unique<TlsContext>(_config.tls, &_tlsMetricsExternal);
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
  if (!_eventLoop.add(_lifecycle.wakeupFd.fd(), EPOLLIN)) {
    throw exception("EventLoop add wakeup fd failed");
  }

  // Register builtin probes handlers if enabled in config
  if (_config.builtinProbes.enabled) {
    registerBuiltInProbes();
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
  if (_lifecycle.isActive()) {
    throw exception("Server is already running");
  }
  if (!_listenSocket) {
    init();
  }
  log::info("Server running on port :{}", port());
}

void HttpServer::eventLoop() {
  sweepIdleConnections();

  int ready = _eventLoop.poll([this](int fd, uint32_t ev) {
    if (fd == _listenSocket.fd()) {
      if (_lifecycle.acceptingConnections()) {
        acceptNewConnections();
      } else {
        log::warn("Not accepting new incoming connection");
      }
    } else if (fd == _lifecycle.wakeupFd.fd()) {
      _lifecycle.wakeupFd.read();
    } else {
      if (ev & EPOLLOUT) {
        handleWritableClient(fd);
      }
      if (ev & EPOLLIN) {
        handleReadableClient(fd);
      }
    }
  });

  if (ready > 0) {
    _telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(ready));
  } else if (ready < 0) {
    _telemetry.counterAdd("aeronet.events.errors", 1);
    log::error("epoll_wait (eventLoop) failed: {}", std::strerror(errno));
    _lifecycle.enterStopping();
  } else {
    // ready == 0: timeout. Retry pending writes to handle edge-triggered epoll timing issues.
    // With EPOLLET, if a socket becomes writable after sendfile() returns EAGAIN but before
    // epoll_ctl(EPOLL_CTL_MOD), we miss the edge. Periodic retries ensure we eventually resume.
    for (auto it = _connStates.begin(); it != _connStates.end();) {
      if (it->second.fileSend.active && it->second.waitingWritable) {
        flushFilePayload(it);
        if (it->second.isImmediateCloseRequested()) {
          it = closeConnection(it);
          continue;
        }
      }
      ++it;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  const bool noConnections = _connStates.empty();

  if (_lifecycle.isStopping()) {
    closeAllConnections(true);
    _lifecycle.reset();
    log::info("Server stopped");
    return;
  }

  if (_lifecycle.isDraining()) {
    if (_lifecycle.hasDeadline() && now >= _lifecycle.deadline()) {
      log::warn("Drain deadline reached with {} active connection(s); forcing close", _connStates.size());
      closeAllConnections(true);
      _lifecycle.reset();
      log::info("Server drained after deadline");
      return;
    }
    if (noConnections) {
      _lifecycle.reset();
      log::info("Server drained gracefully");
      return;
    }
  }
}

void HttpServer::closeListener() noexcept {
  if (_listenSocket) {
    const int fd = _listenSocket.fd();
    _eventLoop.del(fd);
    _listenSocket.close();
  }
}

void HttpServer::closeAllConnections(bool immediate) {
  for (auto it = _connStates.begin(); it != _connStates.end();) {
    if (immediate) {
      it = closeConnection(it);
    } else {
      it->second.requestDrainAndClose();
      ++it;
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
  statsOut.totalRequestsServed = _stats.totalRequestsServed;
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

void HttpServer::registerBuiltInProbes() {
  // liveness: lightweight, should not depend on external systems
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.livenessPath()), [](const HttpRequest&) {
    return HttpResponse(http::StatusCodeOK).contentType(http::ContentTypeTextPlain).body("OK\n");
  });

  // readiness: reflects lifecycle.ready
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.readinessPath()), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    resp.contentType(http::ContentTypeTextPlain);
    if (_lifecycle.ready.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.statusCode(http::StatusCodeServiceUnavailable);
      resp.body("Not Ready\n");
    }
    return resp;
  });

  // startup: reflects lifecycle.started
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.startupPath()), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    resp.contentType(http::ContentTypeTextPlain);
    if (_lifecycle.started.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.statusCode(http::StatusCodeServiceUnavailable);
      resp.body("Starting\n");
    }
    return resp;
  });
}

bool HttpServer::handleExpectHeader(ConnectionMapIt cnxIt, ConnectionState& state, bool& found100Continue) {
  const std::string_view expectHeader = _request.headerValueOrEmpty(http::Expect);
  const std::size_t headerEnd =
      static_cast<std::size_t>(_request._flatHeaders.data() + _request._flatHeaders.size() - state.inBuffer.data());
  // Parse comma-separated tokens (trim spaces/tabs). Case-insensitive comparison for 100-continue.
  // headerEnd = offset from connection buffer start to end of headers
  for (const char *cur = expectHeader.data(), *end = cur + expectHeader.size(); cur < end; ++cur) {
    // skip leading whitespace
    while (cur < end && http::IsHeaderWhitespace(*cur)) {
      ++cur;
    }
    if (cur >= end) {
      break;
    }
    const char* tokStart = cur;
    // find comma or end
    while (cur < end && *cur != ',') {
      ++cur;
    }
    const char* tokEnd = cur;
    // trim trailing whitespace
    while (tokEnd > tokStart && http::IsHeaderWhitespace(*(tokEnd - 1))) {
      --tokEnd;
    }
    if (tokStart == tokEnd) {
      continue;
    }
    std::string_view token(tokStart, tokEnd);
    if (CaseInsensitiveEqual(token, http::h100_continue)) {
      // Note presence of 100-continue; we'll use this to trigger interim 100
      found100Continue = true;
      // built-in behaviour; leave actual 100 emission to body-decoding logic
      continue;
    }
    if (!_expectationHandler) {
      // No handler and not 100-continue -> RFC says respond 417
      emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, true);
      return true;
    }
    try {
      auto expectationResult = _expectationHandler(_request, token);
      switch (expectationResult.kind) {
        case ExpectationResultKind::Reject:
          emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, true);
          return true;
        case ExpectationResultKind::Interim: {
          // Emit an interim response immediately. Common case: 102 "Processing"
          const auto status = expectationResult.interimStatus;
          // Validate that the handler returned an informational 1xx status.
          if (status < 100U || status >= 200U) {
            emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true, "Invalid interim status (must be 1xx)");
            return true;
          }

          switch (status) {
            case 100:
              queueData(cnxIt, HttpResponseData(http::HTTP11_100_CONTINUE));
              break;
            case 102: {
              static constexpr std::string_view k102Processing = "HTTP/1.1 102 Processing\r\n\r\n";
              queueData(cnxIt, HttpResponseData(k102Processing));
              break;
            }
            default: {
              static constexpr std::string_view kHttpResponseLinePrefix = "HTTP/1.1 ";

              char buf[kHttpResponseLinePrefix.size() + 3U + http::DoubleCRLF.size()];

              std::memcpy(buf, kHttpResponseLinePrefix.data(), kHttpResponseLinePrefix.size());
              std::memcpy(write3(buf + kHttpResponseLinePrefix.size(), status), http::DoubleCRLF.data(),
                          http::DoubleCRLF.size());

              queueData(cnxIt, HttpResponseData(std::string_view(buf, sizeof(buf))));
              break;
            }
          }

          break;
        }
        case ExpectationResultKind::FinalResponse:
          // Send the provided final response immediately and skip body processing.
          finalizeAndSendResponse(cnxIt, std::move(expectationResult.finalResponse), headerEnd);
          return true;
        case ExpectationResultKind::Continue:
          break;
        default:
          std::unreachable();
      }
    } catch (const std::exception& ex) {
      log::error("Exception in ExpectationHandler: {}", ex.what());
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true);
      return true;
    } catch (...) {
      log::error("Unknown exception in ExpectationHandler");
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true);
      return true;
    }
  }
  return false;
}

}  // namespace aeronet
