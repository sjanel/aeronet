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
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "accept-encoding-negotiation.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/otel-config.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "connection-state.hpp"
#include "errno_throw.hpp"
#include "event-loop.hpp"
#include "event.hpp"
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
#endif

namespace aeronet {

namespace {
constexpr int kListenSocketType = SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC;

// Snapshot of immutable HttpServerConfig fields that require socket rebind or structural reinitialization.
// These fields are captured before allowing config updates and silently restored afterward to prevent
// runtime modification of settings that cannot be changed without recreating the server.
struct ImmutableConfigSnapshot {
  uint16_t port;
  bool reusePort;
  TLSConfig tls;
  OtelConfig otel;
};

ImmutableConfigSnapshot CaptureImmutable(const HttpServerConfig& cfg) {
  return {cfg.port, cfg.reusePort, cfg.tls, cfg.otel};
}

void RestoreImmutable(HttpServerConfig& cfg, ImmutableConfigSnapshot snapshot) {
  // Restore immutable fields regardless of whether they changed
  cfg.port = snapshot.port;
  cfg.reusePort = snapshot.reusePort;
  cfg.tls = std::move(snapshot.tls);
  cfg.otel = std::move(snapshot.otel);
}

}  // namespace

HttpServer::HttpServer(HttpServerConfig config, RouterConfig routerConfig)
    : _config(std::move(config)),
      _listenSocket(kListenSocketType),
      _eventLoop(_config.pollInterval),
      _router(std::move(routerConfig)),
      _encodingSelector(_config.compression),
      _telemetry(_config.otel) {
  init();
}

HttpServer::HttpServer(HttpServerConfig cfg, Router router)
    : _config(std::move(cfg)),
      _listenSocket(kListenSocketType),
      _eventLoop(_config.pollInterval),
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
    throw std::logic_error("Cannot move-construct a running HttpServer");
  }
  // transfer pending updates state; mutex remains with each instance (do not move mutex)
  _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_acq_rel);
  _pendingConfigUpdates = std::move(other._pendingConfigUpdates);
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
      throw std::logic_error("Cannot move-assign from a running HttpServer");
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

    // transfer pending updates state; keep mutex per-instance
    _hasPendingConfigUpdates.store(other._hasPendingConfigUpdates.exchange(false), std::memory_order_acq_rel);
    _pendingConfigUpdates = std::move(other._pendingConfigUpdates);

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

void HttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  // Capture snapshot of immutable fields before queuing the update
  auto configSnapshot = CaptureImmutable(_config);

  {
    std::scoped_lock lock(_configUpdateLock);
    // Wrap user's updater with immutability enforcement: apply user changes then restore immutable fields

    struct WrappedUpdater {
      void operator()(HttpServerConfig& cfg) {
        userUpdater(cfg);
        RestoreImmutable(cfg, std::move(snapshot));
      }

      std::function<void(HttpServerConfig&)> userUpdater;
      ImmutableConfigSnapshot snapshot;
    };

    _pendingConfigUpdates.emplace_back(WrappedUpdater{std::move(updater), std::move(configSnapshot)});
    _hasPendingConfigUpdates.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

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
  _lifecycle.enterStopping();
  closeListener();
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
  static constexpr EventBmp kEvents = EventIn | EventOut | EventEt;

  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), kEvents})) {
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
  static constexpr EventBmp kEvents = EventIn | EventEt;
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), kEvents})) {
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
        _request.initTrySetHead(state, _tmpBuffer, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders,
                                _telemetry.createSpan("http.request"));
    if (statusCode == 0) {
      // need more data
      break;
    }

    static constexpr uint64_t kShrinkRequestNnRequestPeriod = 1000;

    if (++_stats.totalRequestsServed % kShrinkRequestNnRequestPeriod == 0) {
      _request.shrink_to_fit();
    }

    if (statusCode != http::StatusCodeOK) {
      emitSimpleError(cnxIt, statusCode, true);

      // We break unconditionally; the connection
      // will be torn down after any queued error bytes are flushed. No partial recovery is
      // attempted for a malformed / protocol-violating start line or headers.
      break;
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

    const auto routingResult = _router.match(_request.method(), _request.path());
    const CorsPolicy* pCorsPolicy = routingResult.pCorsPolicy;

    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    const std::string_view expectHeader = _request.headerValueOrEmpty(http::Expect);
    bool found100Continue = false;
    if (!expectHeader.empty() && handleExpectHeader(cnxIt, state, pCorsPolicy, found100Continue)) {
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
    const auto action = processSpecialMethods(cnxIt, consumedBytes, pCorsPolicy);
    if (action == LoopAction::Continue) {
      continue;
    }
    if (action == LoopAction::Break) {
      break;
    }

    _request._pathParams.clear();
    for (const auto& capture : routingResult.pathParams) {
      _request._pathParams.emplace(capture.key, capture.value);
    }

    if (routingResult.streamingHandler != nullptr) {
      const bool streamingClose =
          callStreamingHandler(*routingResult.streamingHandler, cnxIt, consumedBytes, pCorsPolicy);
      if (streamingClose) {
        break;
      }
    } else if (routingResult.requestHandler != nullptr) {
      if (pCorsPolicy != nullptr) {
        HttpResponse corsProbe;
        if (pCorsPolicy->applyToResponse(_request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
          finalizeAndSendResponse(cnxIt, std::move(corsProbe), consumedBytes, pCorsPolicy);
          continue;
        }
      }

      // normal handler
      try {
        // Use RVO on the HttpResponse in the nominal case
        finalizeAndSendResponse(cnxIt, (*routingResult.requestHandler)(_request), consumedBytes, pCorsPolicy);
      } catch (const std::exception& ex) {
        log::error("Exception in path handler: {}", ex.what());
        HttpResponse resp(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
        resp.body(ex.what());
        finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      } catch (...) {
        log::error("Unknown exception in path handler");
        HttpResponse resp(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
        resp.body("Unknown error");
        finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      }
    } else {
      HttpResponse resp;
      if (routingResult.redirectPathIndicator != Router::RoutingResult::RedirectSlashMode::None) {
        // Emit 301 redirect to canonical form.
        resp.status(http::StatusCodeMovedPermanently, http::MovedPermanently).body("Redirecting");
        if (routingResult.redirectPathIndicator == Router::RoutingResult::RedirectSlashMode::AddSlash) {
          _tmpBuffer.assign(_request.path());
          _tmpBuffer.push_back('/');
          resp.location(_tmpBuffer);
        } else {
          resp.location(_request.path().substr(0, _request.path().size() - 1));
        }

        consumedBytes = 0;  // already advanced
      } else if (routingResult.methodNotAllowed) {
        resp.status(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed).body(resp.reason());
      } else {
        resp.status(http::StatusCodeNotFound, http::NotFound).body(http::NotFound);
      }
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
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
                                      std::size_t consumedBytes, const CorsPolicy* pCorsPolicy) {
  bool wantClose = _request.wantClose();
  bool isHead = _request.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = cnxIt->second;

  // Determine active CORS policy (route-specific if provided, otherwise global)
  if (pCorsPolicy != nullptr) {
    HttpResponse corsProbe;
    if (pCorsPolicy->applyToResponse(_request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
      finalizeAndSendResponse(cnxIt, std::move(corsProbe), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
  }

  if (!isHead) {
    auto encHeader = _request.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _encodingSelector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(http::StatusCodeNotAcceptable, http::ReasonNotAcceptable);
      resp.body("No acceptable content-coding available");
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }

  // Pass the resolved activeCors pointer to the streaming writer so it can apply headers lazily
  HttpResponseWriter writer(*this, cnxIt->first.fd(), isHead, wantClose, compressionFormat, pCorsPolicy);
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

  if (!_listenSocket) {
    _listenSocket = Socket(kListenSocketType);
    _eventLoop = EventLoop(_config.pollInterval);
  }

  const int listenFd = _listenSocket.fd();

  // Initialize TLS context if requested (OpenSSL build).
  if (_config.tls.enabled) {
#ifdef AERONET_ENABLE_OPENSSL
    // Allocate TlsContext on the heap so its address remains stable even if HttpServer is moved.
    // (See detailed rationale in header next to _tlsCtxHolder.)
    _tlsCtxHolder = std::make_unique<TlsContext>(_config.tls, &_tlsMetricsExternal);
#else
    throw std::invalid_argument("aeronet built without OpenSSL support but TLS configuration provided");
#endif
  }
  static constexpr int enable = 1;
  auto errc = ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (errc < 0) {
    throw_errno("setsockopt(SO_REUSEADDR) failed");
  }
  if (_config.reusePort) {
    errc = ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
    if (errc < 0) {
      throw_errno("setsockopt(SO_REUSEPORT) failed");
    }
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(_config.port);
  errc = ::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (errc < 0) {
    throw_errno("bind failed");
  }
  if (::listen(listenFd, SOMAXCONN) < 0) {
    throw_errno("listen failed");
  }
  if (_config.port == 0) {
    sockaddr_in actual{};
    socklen_t alen = sizeof(actual);
    errc = ::getsockname(listenFd, reinterpret_cast<sockaddr*>(&actual), &alen);
    if (errc == -1) {
      throw_errno("getsockname failed");
    }
    _config.port = ntohs(actual.sin_port);
  }
  _eventLoop.add_or_throw(EventLoop::EventFd{listenFd, EventIn});
  _eventLoop.add_or_throw(EventLoop::EventFd{_lifecycle.wakeupFd.fd(), EventIn});

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
    throw std::logic_error("Server is already running");
  }
  if (!_listenSocket) {
    init();
  }
  log::info("Server running on port :{}", port());
}

void HttpServer::eventLoop() {
  sweepIdleConnections();

  // Apply any pending config updates posted from other threads. Fast-path: check
  // atomic flag before taking the lock to avoid contention in the nominal case.
  if (_hasPendingConfigUpdates.load(std::memory_order_acquire)) {
    applyConfigUpdates();
  }

  int ready = _eventLoop.poll([this](EventLoop::EventFd eventFd) {
    if (eventFd.fd == _listenSocket.fd()) {
      if (_lifecycle.acceptingConnections()) {
        acceptNewConnections();
      } else {
        log::warn("Not accepting new incoming connection");
      }
    } else if (eventFd.fd == _lifecycle.wakeupFd.fd()) {
      _lifecycle.wakeupFd.read();
    } else {
      if (eventFd.eventBmp & EventOut) {
        handleWritableClient(eventFd.fd);
      }
      if (eventFd.eventBmp & EventIn) {
        handleReadableClient(eventFd.fd);
      }
    }
  });

  if (ready > 0) {
    _telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(ready));
  } else if (ready < 0) {
    _telemetry.counterAdd("aeronet.events.errors", 1);
    log::error("eventLoop.poll failed: {}", std::strerror(errno));
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
    _eventLoop.del(_listenSocket.fd());
    _listenSocket.close();
    // Trigger wakeup to break any blocking epoll_wait quickly.
    _lifecycle.wakeupFd.send();
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

  _request.end(code);
}

void HttpServer::registerBuiltInProbes() {
  // liveness: lightweight, should not depend on external systems
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.livenessPath()),
                  [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body("OK\n"); });

  // readiness: reflects lifecycle.ready
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.readinessPath()), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.ready.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Not Ready\n");
    }
    return resp;
  });

  // startup: reflects lifecycle.started
  _router.setPath(http::Method::GET, std::string(_config.builtinProbes.startupPath()), [this](const HttpRequest&) {
    HttpResponse resp(http::StatusCodeOK);
    if (_lifecycle.started.load(std::memory_order_relaxed)) {
      resp.body("OK\n");
    } else {
      resp.status(http::StatusCodeServiceUnavailable);
      resp.body("Starting\n");
    }
    return resp;
  });
}

bool HttpServer::handleExpectHeader(ConnectionMapIt cnxIt, ConnectionState& state, const CorsPolicy* pCorsPolicy,
                                    bool& found100Continue) {
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
          finalizeAndSendResponse(cnxIt, std::move(expectationResult.finalResponse), headerEnd, pCorsPolicy);
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

void HttpServer::applyConfigUpdates() {
  ConfigUpdateVector pendingUpdates;
  {
    std::scoped_lock lock(_configUpdateLock);
    pendingUpdates.swap(_pendingConfigUpdates);
    _hasPendingConfigUpdates.store(false, std::memory_order_release);
  }

  for (auto& updater : pendingUpdates) {
    try {
      updater(_config);
    } catch (const std::exception& ex) {
      log::error("Exception while applying posted config update: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception while applying posted config update");
    }
  }
  _encodingSelector = EncodingSelector(_config.compression);
}

}  // namespace aeronet
