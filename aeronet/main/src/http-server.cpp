#include "aeronet/http-server.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-error-build.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/err.h>

#include "aeronet/tls-transport.hpp"
#endif

#ifdef AERONET_ENABLE_BROTLI
#include "aeronet/brotli-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include "aeronet/zstd-decoder.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#endif

namespace aeronet {

namespace {

// Snapshot of immutable HttpServerConfig fields that require socket rebind or structural reinitialization.
// These fields are captured before allowing config updates and silently restored afterward to prevent
// runtime modification of settings that cannot be changed without recreating the server.
struct ImmutableConfigSnapshot {
  uint16_t port;
  bool reusePort;
  TLSConfig tls;
  TelemetryConfig telemetry;
};

ImmutableConfigSnapshot CaptureImmutable(const HttpServerConfig& cfg) {
  return {cfg.port, cfg.reusePort, cfg.tls, cfg.telemetry};
}

void RestoreImmutable(HttpServerConfig& cfg, ImmutableConfigSnapshot snapshot) {
  if (cfg.port != snapshot.port) {
    cfg.port = snapshot.port;
    log::warn("Attempted to modify immutable HttpServerConfig.port at runtime; change ignored");
  }
  if (cfg.reusePort != snapshot.reusePort) {
    cfg.reusePort = snapshot.reusePort;
    log::warn("Attempted to modify immutable HttpServerConfig.reusePort at runtime; change ignored");
  }
  if (cfg.tls != snapshot.tls) {
    cfg.tls = std::move(snapshot.tls);
    log::warn("Attempted to modify immutable HttpServerConfig.tls at runtime; change ignored");
  }
  if (cfg.telemetry != snapshot.telemetry) {
    cfg.telemetry = std::move(snapshot.telemetry);
    log::warn("Attempted to modify immutable HttpServerConfig.telemetry at runtime; change ignored");
  }
}

}  // namespace

RouterUpdateProxy HttpServer::router() {
  return {[this](std::function<void(Router&)> updater) {
            auto completion = std::make_shared<std::promise<std::exception_ptr>>();
            auto future = completion->get_future();
            this->submitRouterUpdate(std::move(updater), std::move(completion));
            if (auto ex = future.get()) {
              std::rethrow_exception(ex);
            }
          },
          [this]() -> Router& { return _router; }};
}

void HttpServer::setParserErrorCallback(ParserErrorCallback cb) { _parserErrCb = std::move(cb); }

void HttpServer::setMetricsCallback(MetricsCallback cb) { _metricsCb = std::move(cb); }

void HttpServer::setExpectationHandler(ExpectationHandler handler) { _expectationHandler = std::move(handler); }

void HttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  // Capture snapshot of immutable fields before queuing the update
  auto configSnapshot = CaptureImmutable(_config);

  {
    std::scoped_lock lock(_updateLock);
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

void HttpServer::postRouterUpdate(std::function<void(Router&)> updater) { submitRouterUpdate(std::move(updater), {}); }

void HttpServer::submitRouterUpdate(std::function<void(Router&)> updater,
                                    std::shared_ptr<std::promise<std::exception_ptr>> completion) {
  auto wrappedUpdater = [fn = std::move(updater), completionPtr = std::move(completion)](Router& router) mutable {
    try {
      fn(router);
      if (completionPtr) {
        completionPtr->set_value(nullptr);
      }
    } catch (const std::exception& ex) {
      if (completionPtr) {
        completionPtr->set_value(std::current_exception());
      } else {
        log::error("Exception while applying posted router update: {}", ex.what());
      }
    } catch (...) {
      if (completionPtr) {
        completionPtr->set_value(std::current_exception());
      } else {
        log::error("Unknown exception while applying posted router update");
      }
    }
  };

  if (!_lifecycle.isActive()) {
    wrappedUpdater(_router);
    return;
  }

  {
    std::scoped_lock lock(_updateLock);
    _pendingRouterUpdates.emplace_back(std::move(wrappedUpdater));
    _hasPendingRouterUpdates.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
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

    static constexpr uint64_t kShrinkRequestNnRequestPeriod = 10000;

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

    const Router::RoutingResult routingResult = _router.match(_request.method(), _request.path());
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

    // Set path params map view
    _request._pathParams.clear();
    for (const auto& capture : routingResult.pathParams) {
      _request._pathParams.emplace(capture.key, capture.value);
    }

    auto requestMiddlewareRange = routingResult.requestMiddlewareRange;
    auto responseMiddlewareRange = routingResult.responseMiddlewareRange;

    auto sendResponse = [this, responseMiddlewareRange, cnxIt, consumedBytes, pCorsPolicy](HttpResponse&& resp) {
      applyResponseMiddleware(resp, responseMiddlewareRange, false);
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
    };

    const bool isStreaming = routingResult.pStreamingHandler != nullptr;

    HttpResponse middlewareResponse;
    const auto globalPreChain = _router.globalRequestMiddleware();
    bool shortCircuited = runPreChain(isStreaming, globalPreChain, middlewareResponse, true);
    if (!shortCircuited) {
      shortCircuited = runPreChain(isStreaming, requestMiddlewareRange, middlewareResponse, false);
    }
    if (shortCircuited) {
      sendResponse(std::move(middlewareResponse));
      continue;
    }

    if (routingResult.pStreamingHandler != nullptr) {
      const bool streamingClose = callStreamingHandler(*routingResult.pStreamingHandler, cnxIt, consumedBytes,
                                                       pCorsPolicy, responseMiddlewareRange);
      if (streamingClose) {
        break;
      }
    } else if (routingResult.pRequestHandler != nullptr) {
      if (pCorsPolicy != nullptr) {
        HttpResponse corsProbe;
        if (pCorsPolicy->applyToResponse(_request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
          sendResponse(std::move(corsProbe));
          continue;
        }
      }

      // normal handler
      try {
        // Use RVO on the HttpResponse in the nominal case
        sendResponse((*routingResult.pRequestHandler)(_request));
      } catch (const std::exception& ex) {
        log::error("Exception in path handler: {}", ex.what());
        HttpResponse resp(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
        resp.body(ex.what());
        sendResponse(std::move(resp));
      } catch (...) {
        log::error("Unknown exception in path handler");
        HttpResponse resp(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
        resp.body("Unknown error");
        sendResponse(std::move(resp));
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
      sendResponse(std::move(resp));
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
                                      std::size_t consumedBytes, const CorsPolicy* pCorsPolicy,
                                      std::span<const ResponseMiddleware> postMiddleware) {
  bool wantClose = _request.wantClose();
  bool isHead = _request.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = cnxIt->second;

  // Determine active CORS policy (route-specific if provided, otherwise global)
  if (pCorsPolicy != nullptr) {
    HttpResponse corsProbe;
    if (pCorsPolicy->applyToResponse(_request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
      applyResponseMiddleware(corsProbe, postMiddleware, false);
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
      applyResponseMiddleware(resp, postMiddleware, false);
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }

  // Pass the resolved activeCors pointer to the streaming writer so it can apply headers lazily
  HttpResponseWriter writer(*this, cnxIt->first.fd(), isHead, wantClose, compressionFormat, pCorsPolicy,
                            postMiddleware);
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

void HttpServer::applyResponseMiddleware(HttpResponse& response, std::span<const ResponseMiddleware> routeChain,
                                         bool streaming) {
  auto runChain = [&](std::span<const ResponseMiddleware> postMiddleware, bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < postMiddleware.size(); ++hookIdx) {
      const auto& middleware = postMiddleware[hookIdx];
      auto spanScope = startMiddlewareSpan(MiddlewareMetrics::Phase::Post, isGlobal, hookIdx, streaming);
      const auto start = std::chrono::steady_clock::now();
      bool threwEx = false;
      try {
        middleware(_request, response);
      } catch (const std::exception& ex) {
        threwEx = true;
        log::error("Exception in {} response middleware: {}", isGlobal ? "global" : "route", ex.what());
      } catch (...) {
        threwEx = true;
        log::error("Unknown exception in {} response middleware", isGlobal ? "global" : "route");
      }
      const auto duration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      if (spanScope.span) {
        spanScope.span->setAttribute("aeronet.middleware.exception", threwEx ? int64_t{1} : int64_t{0});
        spanScope.span->setAttribute("aeronet.middleware.short_circuit", int64_t{0});
        spanScope.span->setAttribute("aeronet.middleware.duration_ns", static_cast<int64_t>(duration.count()));
      }
      emitMiddlewareMetrics(MiddlewareMetrics::Phase::Post, isGlobal, hookIdx, static_cast<uint64_t>(duration.count()),
                            false, threwEx, streaming);
    }
  };
  runChain(routeChain, false);
  runChain(_router.globalResponseMiddleware(), true);
}

void HttpServer::emitMiddlewareMetrics(MiddlewareMetrics::Phase phase, bool isGlobal, uint32_t index,
                                       uint64_t durationNs, bool shortCircuited, bool threw, bool streaming) {
  if (!_middlewareMetricsCb) {
    return;
  }

  MiddlewareMetrics metrics;
  metrics.phase = phase;
  metrics.isGlobal = isGlobal;
  metrics.shortCircuited = shortCircuited;
  metrics.threw = threw;
  metrics.streaming = streaming;
  metrics.index = index;
  metrics.durationNs = durationNs;
  metrics.method = _request.method();
  metrics.requestPath = _request.path();
  _middlewareMetricsCb(metrics);
}

tracing::SpanRAII HttpServer::startMiddlewareSpan(MiddlewareMetrics::Phase phase, bool isGlobal, uint32_t index,
                                                  bool streaming) {
  auto spanPtr = _telemetry.createSpan("http.middleware");
  tracing::SpanRAII spanScope(std::move(spanPtr));
  if (!spanScope.span) {
    return spanScope;
  }

  spanScope.span->setAttribute("aeronet.middleware.phase", phase == MiddlewareMetrics::Phase::Pre
                                                               ? std::string_view("request")
                                                               : std::string_view("response"));
  spanScope.span->setAttribute("aeronet.middleware.scope",
                               isGlobal ? std::string_view("global") : std::string_view("route"));
  spanScope.span->setAttribute("aeronet.middleware.index", static_cast<int64_t>(index));
  spanScope.span->setAttribute("aeronet.middleware.streaming", streaming ? int64_t{1} : int64_t{0});
  spanScope.span->setAttribute("http.method", http::MethodIdxToStr(_request.method()));
  spanScope.span->setAttribute("http.target", _request.path());
  return spanScope;
}

void HttpServer::eventLoop() {
  sweepIdleConnections();

  // Apply any pending config updates posted from other threads. Fast-path: check
  // atomic flag before taking the lock to avoid contention in the nominal case.
  if (_hasPendingConfigUpdates.load(std::memory_order_acquire)) {
    applyConfigUpdates();
  }
  if (_hasPendingRouterUpdates.load(std::memory_order_acquire)) {
    applyRouterUpdates();
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
  } else if (SignalHandler::IsStopRequested()) {
    beginDrain(SignalHandler::GetMaxDrainPeriod());
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

#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
void HttpServer::maybeEnableKtlsSend(ConnectionState& state, TlsTransport& transport, int fd) {
  if (state.ktlsSendAttempted || _config.tls.ktlsMode == TLSConfig::KtlsMode::Disabled) {
    state.ktlsSendAttempted = true;
    return;
  }
  state.ktlsSendAttempted = true;

  const bool force = _config.tls.ktlsMode == TLSConfig::KtlsMode::Forced;
  // Treat Auto as an opportunistic mode but do NOT fail silently: emit a warning on fallback so
  // deployments using Auto are informed about why kernel offload wasn't available. This follows the
  // principle of least surprise while preserving Auto's opportunistic behavior.
  const bool warnOnFailure =
      _config.tls.ktlsMode == TLSConfig::KtlsMode::Enabled || _config.tls.ktlsMode == TLSConfig::KtlsMode::Auto;

  const auto result = transport.enableKtlsSend();
  switch (result.status) {
    case TlsTransport::KtlsEnableResult::Status::Enabled:
    case TlsTransport::KtlsEnableResult::Status::AlreadyEnabled:
      state.ktlsSendEnabled = true;
      ++_stats.ktlsSendEnabledConnections;
      log::debug("KTLS send enabled on fd # {}", fd);
      break;
    case TlsTransport::KtlsEnableResult::Status::Unsupported:
      ++_stats.ktlsSendEnableFallbacks;
      if (force) {
        ++_stats.ktlsSendForcedShutdowns;
        log::error("KTLS send unsupported on fd # {} while forced", fd);
        state.requestImmediateClose();
      } else if (warnOnFailure) {
        log::warn(
            "KTLS send unsupported on fd # {} (falling back to user-space TLS). Consider using "
            "TLSConfig::KtlsMode::Forced to treat this as fatal.",
            fd);
      } else {
        log::debug("KTLS send unsupported on fd # {} (fallback)", fd);
      }
      break;
    case TlsTransport::KtlsEnableResult::Status::Failed: {
      ++_stats.ktlsSendEnableFallbacks;
      RawChars reason;
      if (result.sysError != 0) {
        reason.append("errno=");
        reason.append(std::string_view(IntegralToCharVector(result.sysError)));
        reason.push_back(' ');
        reason.append(std::strerror(result.sysError));
      }
      if (result.sslError != 0) {
        char errBuf[256];
        ::ERR_error_string_n(result.sslError, errBuf, sizeof(errBuf));
        if (!reason.empty()) {
          reason.append("; ");
        }
        reason.append("ssl=");
        reason.append(errBuf);
      }
      if (force) {
        ++_stats.ktlsSendForcedShutdowns;
        log::error("KTLS send enable failed for fd # {} (forced mode) reason={}", fd,
                   reason.empty() ? std::string_view("unknown") : reason);
        state.requestImmediateClose();
      } else if (warnOnFailure) {
        log::warn("KTLS send enable failed for fd # {} (falling back) reason={}", fd,
                  reason.empty() ? std::string_view("unknown") : reason);
      } else {
        log::debug("KTLS send enable failed for fd # {} reason={}", fd,
                   reason.empty() ? std::string_view("unknown") : reason);
      }
      break;
    }
    default:
      std::unreachable();
  }
}
#endif

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
#if defined(AERONET_ENABLE_OPENSSL) && defined(AERONET_ENABLE_KTLS)
  statsOut.ktlsSendEnabledConnections = _stats.ktlsSendEnabledConnections;
  statsOut.ktlsSendEnableFallbacks = _stats.ktlsSendEnableFallbacks;
  statsOut.ktlsSendForcedShutdowns = _stats.ktlsSendForcedShutdowns;
  statsOut.ktlsSendBytes = _stats.ktlsSendBytes;
#endif
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

void HttpServer::emitSimpleError(ConnectionMapIt cnxIt, http::StatusCode statusCode, bool immediate,
                                 std::string_view reason) {
  queueData(cnxIt, HttpResponseData(BuildSimpleError(statusCode, _config.globalHeaders, reason)));

  try {
    _parserErrCb(statusCode);
  } catch (const std::exception& ex) {
    // Swallow exceptions from user callback to avoid destabilizing the server
    log::error("Exception raised in user callback: {}", ex.what());
  }
  if (immediate) {
    cnxIt->second.requestImmediateClose();
  } else {
    cnxIt->second.requestDrainAndClose();
  }

  _request.end(statusCode);
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

namespace {

void ApplyPendingUpdates(std::mutex& mutex, auto& vec, std::atomic<bool>& flag, auto& objToUpdate,
                         std::string_view name) {
  std::remove_reference_t<decltype(vec)> pendingUpdates;
  {
    std::scoped_lock lock(mutex);
    pendingUpdates.swap(vec);
    flag.store(false, std::memory_order_release);
  }

  for (auto& updater : pendingUpdates) {
    try {
      updater(objToUpdate);
    } catch (const std::exception& ex) {
      log::error("Exception while applying posted {} update: {}", name, ex.what());
    } catch (...) {
      log::error("Unknown exception while applying posted {} update", name);
    }
  }
}

}  // namespace

void HttpServer::applyConfigUpdates() {
  ApplyPendingUpdates(_updateLock, _pendingConfigUpdates, _hasPendingConfigUpdates, _config, "config");

  _config.validate();

  // Reinitialize components dependent on config values.
  _encodingSelector = EncodingSelector(_config.compression);
  _eventLoop.updatePollTimeout(_config.pollInterval);
  registerBuiltInProbes();
  createEncoders();
}

void HttpServer::applyRouterUpdates() {
  ApplyPendingUpdates(_updateLock, _pendingRouterUpdates, _hasPendingRouterUpdates, _router, "router");
}

bool HttpServer::runPreChain(bool willStream, std::span<const RequestMiddleware> chain, HttpResponse& out,
                             bool isGlobal) {
  for (uint32_t idx = 0; idx < chain.size(); ++idx) {
    const auto& middleware = chain[idx];
    const auto start = std::chrono::steady_clock::now();
    auto spanScope = startMiddlewareSpan(MiddlewareMetrics::Phase::Pre, isGlobal, idx, willStream);
    MiddlewareResult decision;
    bool threwEx = true;
    try {
      decision = middleware(_request);
      threwEx = false;
    } catch (const std::exception& ex) {
      log::error("Exception while applying pre middleware: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception while applying pre middleware");
    }

    const auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
    const bool shortCircuited = decision.shouldShortCircuit();
    if (spanScope.span) {
      spanScope.span->setAttribute("aeronet.middleware.exception", threwEx ? int64_t{1} : int64_t{0});
      spanScope.span->setAttribute("aeronet.middleware.short_circuit", shortCircuited ? int64_t{1} : int64_t{0});
      spanScope.span->setAttribute("aeronet.middleware.duration_ns", static_cast<int64_t>(duration.count()));
    }
    emitMiddlewareMetrics(MiddlewareMetrics::Phase::Pre, isGlobal, idx, static_cast<uint64_t>(duration.count()),
                          shortCircuited, false, willStream);
    if (shortCircuited) {
      out = std::move(decision).takeResponse();
      return true;
    }
  }
  return false;
}

}  // namespace aeronet
