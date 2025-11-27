#include "aeronet/http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/decoder.hpp"
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
#include "aeronet/nchars.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"
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
  cnxIt->second->requestDrainAndClose();
}
}  // namespace

bool HttpServer::enableWritableInterest(ConnectionMapIt cnxIt, const char* ctx) {
  static constexpr EventBmp kEvents = EventIn | EventOut | EventEt;
  ConnectionState* state = cnxIt->second.get();

  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), kEvents})) {
    if (!state->waitingWritable) {
      state->waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
    return true;
  }
  RecordModFailure(cnxIt, kEvents, ctx, _stats);
  return false;
}

bool HttpServer::disableWritableInterest(ConnectionMapIt cnxIt, const char* ctx) {
  static constexpr EventBmp kEvents = EventIn | EventEt;
  ConnectionState* state = cnxIt->second.get();
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), kEvents})) {
    state->waitingWritable = false;
    return true;
  }
  RecordModFailure(cnxIt, kEvents, ctx, _stats);
  return false;
}

bool HttpServer::processRequestsOnConnection(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  if (state.asyncState.active) {
    handleAsyncBodyProgress(cnxIt);
    return state.isAnyCloseRequested();
  }
  HttpRequest& request = state.request;
  request._ownerState = &state;
  do {
    // If we don't yet have a full request line (no '\n' observed) wait for more data
    if (state.inBuffer.size() < http::kHttpReqLineMinLen) {
      break;  // need more bytes for at least the request line
    }
    const auto statusCode =
        request.initTrySetHead(state, _tmpBuffer, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders,
                               _telemetry.createSpan("http.request"));
    if (statusCode == HttpRequest::kStatusNeedMoreData) {
      break;
    }

    if (statusCode != http::StatusCodeOK) {
      emitSimpleError(cnxIt, statusCode, true, {});

      // We break unconditionally; the connection
      // will be torn down after any queued error bytes are flushed. No partial recovery is
      // attempted for a malformed / protocol-violating start line or headers.
      break;
    }

    // A full request head (and body, if present) will now be processed; reset headerStart to signal
    // that the header timeout should track the next pending request only.
    state.headerStartTp = {};
    bool isChunked = false;
    bool hasTransferEncoding = false;
    const std::string_view transferEncoding = request.headerValueOrEmpty(http::TransferEncoding);
    if (!transferEncoding.empty()) {
      hasTransferEncoding = true;
      if (request.version() == http::HTTP_1_0) {
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

    const std::string_view contentLength = request.headerValueOrEmpty(http::ContentLength);
    const bool hasContentLength = !contentLength.empty();
    if (hasContentLength && hasTransferEncoding) {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, true,
                      "Content-Length and Transfer-Encoding cannot be used together");
      break;
    }

    // Route matching
    const Router::RoutingResult routingResult = _router.match(request.method(), request.path());
    const CorsPolicy* pCorsPolicy = routingResult.pCorsPolicy;

    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    const std::string_view expectHeader = request.headerValueOrEmpty(http::Expect);
    bool found100Continue = false;
    if (!expectHeader.empty() && handleExpectHeader(cnxIt, pCorsPolicy, found100Continue)) {
      break;  // stop processing this request (response queued)
    }
    const bool expectContinue = found100Continue || request.hasExpectContinue();
    std::size_t consumedBytes = 0;
    const BodyDecodeStatus decodeStatus = decodeBodyIfReady(cnxIt, isChunked, expectContinue, consumedBytes);
    if (decodeStatus == BodyDecodeStatus::Error) {
      break;
    }
    const bool bodyReady = decodeStatus == BodyDecodeStatus::Ready;
    if (!bodyReady) {
      if (_config.bodyReadTimeout.count() > 0) {
        state.waitingForBody = true;
        state.bodyLastActivity = std::chrono::steady_clock::now();
      }
    } else {
      if (_config.bodyReadTimeout.count() > 0) {
        state.waitingForBody = false;
        state.bodyLastActivity = {};
      }
      if (!request._body.empty() && !maybeDecompressRequestBody(cnxIt)) {
        break;
      }
      state.installAggregatedBodyBridge();
    }

    if (!bodyReady && routingResult.asyncRequestHandler() == nullptr) {
      break;
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
    request._pathParams.clear();
    for (const auto& capture : routingResult.pathParams) {
      request._pathParams.emplace(capture.key, capture.value);
    }

    auto requestMiddlewareRange = routingResult.requestMiddlewareRange;
    auto responseMiddlewareRange = routingResult.responseMiddlewareRange;

    auto sendResponse = [this, responseMiddlewareRange, cnxIt, consumedBytes, pCorsPolicy](HttpResponse&& resp) {
      applyResponseMiddleware(cnxIt->second->request, resp, responseMiddlewareRange, false);
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
    };

    auto corsRejected = [&]() {
      if (pCorsPolicy == nullptr) {
        return false;
      }
      HttpResponse corsProbe;
      if (pCorsPolicy->applyToResponse(request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
        sendResponse(std::move(corsProbe));
        return true;
      }
      return false;
    };

    const bool isStreaming = routingResult.streamingHandler() != nullptr;

    HttpResponse middlewareResponse;
    const auto globalPreChain = _router.globalRequestMiddleware();
    bool shortCircuited = runPreChain(request, isStreaming, globalPreChain, middlewareResponse, true);
    if (!shortCircuited) {
      shortCircuited = runPreChain(request, isStreaming, requestMiddlewareRange, middlewareResponse, false);
    }
    if (shortCircuited) {
      sendResponse(std::move(middlewareResponse));
      continue;
    }

    if (routingResult.streamingHandler() != nullptr) {
      const bool streamingClose = callStreamingHandler(*routingResult.streamingHandler(), cnxIt, consumedBytes,
                                                       pCorsPolicy, responseMiddlewareRange);
      if (streamingClose) {
        break;
      }
    } else if (routingResult.asyncRequestHandler() != nullptr) {
      if (corsRejected()) {
        continue;
      }

      const bool handlerActive =
          dispatchAsyncHandler(cnxIt, *routingResult.asyncRequestHandler(), bodyReady, isChunked, expectContinue,
                               consumedBytes, pCorsPolicy, responseMiddlewareRange);
      if (handlerActive) {
        return state.isAnyCloseRequested();
      }
    } else if (routingResult.requestHandler() != nullptr) {
      if (corsRejected()) {
        continue;
      }

      // normal handler
      try {
        // Use RVO on the HttpResponse in the nominal case
        sendResponse((*routingResult.requestHandler())(request));
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
          _tmpBuffer.assign(request.path());
          _tmpBuffer.push_back('/');
          resp.location(_tmpBuffer);
        } else {
          resp.location(request.path().substr(0, request.path().size() - 1));
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
  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  const auto& decompressionConfig = _config.decompression;
  auto& headersMap = request._headers;
  const auto encodingHeaderIt = headersMap.find(http::ContentEncoding);
  if (encodingHeaderIt == headersMap.end() || CaseInsensitiveEqual(encodingHeaderIt->second, http::identity)) {
    return true;  // nothing to do
  }
  if (!decompressionConfig.enable) {
    // Pass-through mode: leave compressed body & header intact; user code must decode manually
    // if it cares. We intentionally skip size / ratio guards in this mode to avoid surprising
    // rejections when opting out. Global body size limits have already been enforced.
    return true;
  }
  const std::size_t originalCompressedSize = request.body().size();
  if (decompressionConfig.maxCompressedBytes != 0 && originalCompressedSize > decompressionConfig.maxCompressedBytes) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true, {});
    return false;
  }

  const std::string_view encodingStr = encodingHeaderIt->second;

  // We'll alternate between bodyAndTrailersBuffer (source) and _tmpBuffer (target) each stage.
  // TODO: if there are trailers, they will be erased if there is a two step decompression (e.g. gzip + zstd).
  std::string_view src = request.body();
  RawChars* dst = &_tmpBuffer;

  const auto contentLenIt = headersMap.find(http::ContentLength);

#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
  const std::size_t maxPlainBytes = decompressionConfig.maxDecompressedBytes == 0
                                        ? std::numeric_limits<std::size_t>::max()
                                        : decompressionConfig.maxDecompressedBytes;

  bool useStreamingDecode = false;
  std::size_t declaredLen = 0;
  if (decompressionConfig.streamingDecompressionThresholdBytes > 0 && contentLenIt != headersMap.end()) {
    // If Content-Length is present it has already been validated previously, so it should be valid.
    // It is not present in chunked requests.
    // TODO: is it possible to have originalCompressedSize != declaredLen here?
    const std::string_view contentLenValue = contentLenIt->second;
    declaredLen = StringToIntegral<std::size_t>(contentLenValue);
    useStreamingDecode = declaredLen >= decompressionConfig.streamingDecompressionThresholdBytes;
  }

  const auto runDecoder = [&](Decoder& decoder) -> bool {
    if (!useStreamingDecode) {
      return decoder.decompressFull(src, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst);
    }
    auto ctx = decoder.makeContext();
    if (!ctx) {
      return false;
    }
    if (src.empty()) {
      return ctx->decompressChunk(std::string_view{}, true, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst);
    }
    std::size_t processed = 0;
    while (processed < src.size()) {
      const std::size_t remaining = src.size() - processed;
      const std::size_t chunkLen = std::min(decompressionConfig.decoderChunkSize, remaining);
      std::string_view chunk(src.data() + processed, chunkLen);
      processed += chunkLen;
      const bool lastChunk = processed == src.size();
      if (!ctx->decompressChunk(chunk, lastChunk, maxPlainBytes, decompressionConfig.decoderChunkSize, *dst)) {
        return false;
      }
    }
    return true;
  };
#endif

  // If we have trailers, we need to exclude them in the decompression process, and avoid them
  // being overriden during the decompression swaps.
  RawChars trailers;
  const std::size_t trailersSize =
      state.trailerStartPos > 0 ? state.bodyAndTrailersBuffer.size() - state.trailerStartPos : 0;
  if (trailersSize > 0) {
    // We need to save trailers in another buffer as its data will be
    // overridden during decompression swaps (they are stored at the end of bodyAndTrailersBuffer).
    trailers.assign(state.bodyAndTrailersBuffer.data() + state.trailerStartPos, trailersSize);
  }

  // Decode in reverse order.
  const char* first = encodingStr.data();
  const char* last = first + encodingStr.size();
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
    bool stageOk = false;
    if (CaseInsensitiveEqual(encoding, http::identity)) {
      last = comma;
      continue;  // no-op layer
#ifdef AERONET_ENABLE_ZLIB
      // NOLINTNEXTLINE(readability-else-after-return)
    } else if (CaseInsensitiveEqual(encoding, http::gzip)) {
      ZlibDecoder decoder(/*isGzip=*/true);
      stageOk = runDecoder(decoder);
    } else if (CaseInsensitiveEqual(encoding, http::deflate)) {
      ZlibDecoder decoder(/*isGzip=*/false);
      stageOk = runDecoder(decoder);
#endif
#ifdef AERONET_ENABLE_ZSTD
    } else if (CaseInsensitiveEqual(encoding, http::zstd)) {
      ZstdDecoder decoder;
      stageOk = runDecoder(decoder);
#endif
#ifdef AERONET_ENABLE_BROTLI
    } else if (CaseInsensitiveEqual(encoding, http::br)) {
      BrotliDecoder decoder;
      stageOk = runDecoder(decoder);
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
    if (decompressionConfig.maxExpansionRatio > 0.0 && originalCompressedSize > 0) {
      double ratio = static_cast<double>(dst->size()) / static_cast<double>(originalCompressedSize);
      if (ratio > decompressionConfig.maxExpansionRatio) {
        emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true, "Decompression expansion too large");
        return false;
      }
    }

    src = *dst;
    dst = dst == &state.bodyAndTrailersBuffer ? &_tmpBuffer : &state.bodyAndTrailersBuffer;

    last = comma;
  }

  if (src.data() == _tmpBuffer.data()) {
    // make sure we use bodyAndTrailersBuffer and not _tmpBuffer to store the body.
    _tmpBuffer.swap(state.bodyAndTrailersBuffer);
  }
  RawChars& buf = state.bodyAndTrailersBuffer;

  // Append to the buffer the new Content-Length value (decompressed size). It is not seen by the body.
  const std::size_t decompressedSizeNbChars = static_cast<std::size_t>(nchars(src.size()));
  // Unique memory reallocation so that std::string_views that will be pointing to it are not invalidated later.
  buf.ensureAvailableCapacity(trailers.size() + decompressedSizeNbChars);

  src = state.bodyAndTrailersBuffer;
  if (!trailers.empty()) {
    state.trailerStartPos = buf.size();
    // Append trailers data to the end of the buffer.
    buf.unchecked_append(trailers);
    // Re-parse trailers in the trailers map now that they are at the end of the buffer.
    parseHeadersUnchecked(request._trailers, buf.data(), buf.data() + state.trailerStartPos, buf.end());
  }

  std::string_view decompressedSizeStr(buf.end(), decompressedSizeNbChars);
  // This to_chars cannot fail as we have reserved enough space.
  std::to_chars(buf.end(), buf.end() + decompressedSizeNbChars, src.size());
  buf.addSize(decompressedSizeNbChars);

  // Final decompressed data now resides in src after last swap.
  request._body = src;

  // Strip Content-Encoding header so user handlers observe a canonical, already-decoded body.
  const std::string_view originalContentLenStr = contentLenIt != headersMap.end() ? contentLenIt->second : "";

  headersMap.erase(encodingHeaderIt);
  headersMap.insert_or_assign(http::ContentLength, decompressedSizeStr);
  headersMap.insert_or_assign(http::OriginalEncodingHeaderName, encodingStr);
  if (!originalContentLenStr.empty()) {
    headersMap.insert_or_assign(http::OriginalEncodedLengthHeaderName, originalContentLenStr);
  }

  return true;
}

bool HttpServer::callStreamingHandler(const StreamingHandler& streamingHandler, ConnectionMapIt cnxIt,
                                      std::size_t consumedBytes, const CorsPolicy* pCorsPolicy,
                                      std::span<const ResponseMiddleware> postMiddleware) {
  HttpRequest& request = cnxIt->second->request;
  bool wantClose = request.wantClose();
  bool isHead = request.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = *cnxIt->second;

  // Determine active CORS policy (route-specific if provided, otherwise global)
  if (pCorsPolicy != nullptr) {
    HttpResponse corsProbe;
    if (pCorsPolicy->applyToResponse(request, corsProbe) == CorsPolicy::ApplyStatus::OriginDenied) {
      applyResponseMiddleware(request, corsProbe, postMiddleware, false);
      finalizeAndSendResponse(cnxIt, std::move(corsProbe), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
  }

  if (!isHead) {
    auto encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _encodingSelector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(http::StatusCodeNotAcceptable, http::ReasonNotAcceptable);
      resp.body("No acceptable content-coding available");
      applyResponseMiddleware(request, resp, postMiddleware, false);
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }

  // Pass the resolved activeCors pointer to the streaming writer so it can apply headers lazily
  HttpResponseWriter writer(*this, cnxIt->first.fd(), request, isHead, wantClose, compressionFormat, pCorsPolicy,
                            postMiddleware);
  try {
    streamingHandler(request, writer);
  } catch (const std::exception& ex) {
    log::error("Exception in streaming handler: {}", ex.what());
  } catch (...) {
    log::error("Unknown exception in streaming handler");
  }
  if (!writer.finished()) {
    writer.end();
  }

  ++state.requestsServed;
  ++_stats.totalRequestsServed;
  state.inBuffer.erase_front(consumedBytes);

  const bool shouldClose = !_config.enableKeepAlive || request.version() != http::HTTP_1_1 || wantClose ||
                           state.requestsServed + 1 >= _config.maxRequestsPerConnection ||
                           state.isAnyCloseRequested() || _lifecycle.isDraining() || _lifecycle.isStopping();
  if (shouldClose) {
    state.requestDrainAndClose();
  }

  if (_metricsCb) {
    emitRequestMetrics(request, http::StatusCodeOK, request.body().size(), state.requestsServed > 1);
  }

  return shouldClose;
}

bool HttpServer::dispatchAsyncHandler(ConnectionMapIt cnxIt, const AsyncRequestHandler& handler, bool bodyReady,
                                      bool isChunked, bool expectContinue, std::size_t consumedBytes,
                                      const CorsPolicy* pCorsPolicy,
                                      std::span<const ResponseMiddleware> responseMiddleware) {
  ConnectionState& state = *cnxIt->second;
  auto failFast = [&](std::string_view message) {
    if (!bodyReady) {
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true, message);
      return;
    }
    HttpResponse resp(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
    resp.body(message);
    applyResponseMiddleware(state.request, resp, responseMiddleware, false);
    finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
  };

  RequestTask<HttpResponse> task;
  try {
    task = handler(state.request);
  } catch (const std::exception& ex) {
    log::error("Exception while creating async handler task: {}", ex.what());
    failFast(ex.what());
    return false;
  } catch (...) {
    log::error("Unknown exception while creating async handler task");
    failFast("Unknown error");
    return false;
  }

  if (!task.valid()) {
    log::error("Async path handler returned an invalid RequestTask for path {}", state.request.path());
    failFast("Async handler inactive");
    return false;
  }

  auto handle = task.release();
  if (!handle) {
    log::error("Async path handler returned a null coroutine for path {}", state.request.path());
    failFast("Async handler inactive");
    return false;
  }

  auto& asyncState = state.asyncState;

  asyncState.active = true;
  asyncState.handle = handle;
  asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::None;
  asyncState.needsBody = !bodyReady;
  asyncState.responsePending = false;
  asyncState.isChunked = isChunked;
  asyncState.expectContinue = expectContinue;
  asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
  asyncState.corsPolicy = pCorsPolicy;
  asyncState.responseMiddleware = responseMiddleware.data();
  asyncState.responseMiddlewareCount = responseMiddleware.size();
  asyncState.pendingResponse = HttpResponse{};

  if (asyncState.needsBody) {
    state.request.pinHeadStorage(state);
  }

  resumeAsyncHandler(cnxIt);
  return asyncState.active;
}

void HttpServer::resumeAsyncHandler(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;
  if (!async.active || !async.handle) {
    return;
  }

  while (async.handle && !async.handle.done()) {
    async.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::None;
    async.handle.resume();
    if (async.awaitReason != ConnectionState::AsyncHandlerState::AwaitReason::None) {
      return;
    }
  }

  if (async.handle && async.handle.done()) {
    onAsyncHandlerCompleted(cnxIt);
  }
}

void HttpServer::handleAsyncBodyProgress(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;
  if (!async.active) {
    return;
  }

  if (async.needsBody) {
    std::size_t consumedBytes = 0;
    const BodyDecodeStatus status = decodeBodyIfReady(cnxIt, async.isChunked, async.expectContinue, consumedBytes);
    if (status == BodyDecodeStatus::Error) {
      state.asyncState.clear();
      return;
    }
    if (status == BodyDecodeStatus::NeedMore) {
      return;
    }

    async.needsBody = false;
    async.consumedBytes = consumedBytes;
    if (!state.request._body.empty() && !maybeDecompressRequestBody(cnxIt)) {
      state.asyncState.clear();
      return;
    }
    state.installAggregatedBodyBridge();
    if (_config.bodyReadTimeout.count() > 0) {
      state.waitingForBody = false;
      state.bodyLastActivity = {};
    }

    if (async.awaitReason == ConnectionState::AsyncHandlerState::AwaitReason::WaitingForBody) {
      async.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::None;
      resumeAsyncHandler(cnxIt);
      return;
    }
  }

  if (async.responsePending) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void HttpServer::onAsyncHandlerCompleted(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;
  if (!async.handle) {
    return;
  }

  auto typedHandle =
      std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(async.handle.address());
  HttpResponse resp;
  bool fromException = false;
  try {
    resp = std::move(typedHandle.promise().consume_result());
  } catch (const std::exception& ex) {
    fromException = true;
    log::error("Exception in async path handler: {}", ex.what());
    resp = HttpResponse(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
    resp.body(ex.what());
  } catch (...) {
    fromException = true;
    log::error("Unknown exception in async path handler");
    resp = HttpResponse(http::StatusCodeInternalServerError, http::ReasonInternalServerError);
    resp.body("Unknown error");
  }
  typedHandle.destroy();
  async.handle = {};

  if (async.needsBody) {
    async.responsePending = true;
    async.pendingResponse = std::move(resp);
    if (fromException) {
      // Body will still be drained before response is flushed; nothing else to do here.
    }
    return;
  }

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  applyResponseMiddleware(state.request, resp, middlewareSpan, false);
  finalizeAndSendResponse(cnxIt, std::move(resp), async.consumedBytes, async.corsPolicy);
  state.asyncState.clear();
}

bool HttpServer::tryFlushPendingAsyncResponse(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;
  if (!async.responsePending || async.needsBody) {
    return false;
  }

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  applyResponseMiddleware(state.request, async.pendingResponse, middlewareSpan, false);
  finalizeAndSendResponse(cnxIt, std::move(async.pendingResponse), async.consumedBytes, async.corsPolicy);
  state.asyncState.clear();
  return true;
}

void HttpServer::emitRequestMetrics(const HttpRequest& request, http::StatusCode status, std::size_t bytesIn,
                                    bool reusedConnection) {
  if (!_metricsCb) {
    return;
  }
  RequestMetrics metrics;
  metrics.status = status;
  metrics.bytesIn = bytesIn;
  metrics.reusedConnection = reusedConnection;
  metrics.method = request.method();
  metrics.path = request.path();
  metrics.duration = std::chrono::steady_clock::now() - request.reqStart();
  _metricsCb(metrics);
}

void HttpServer::applyResponseMiddleware(const HttpRequest& request, HttpResponse& response,
                                         std::span<const ResponseMiddleware> routeChain, bool streaming) {
  auto runChain = [&](std::span<const ResponseMiddleware> postMiddleware, bool isGlobal) {
    for (uint32_t hookIdx = 0; hookIdx < postMiddleware.size(); ++hookIdx) {
      const auto& middleware = postMiddleware[hookIdx];
      auto spanScope = startMiddlewareSpan(request, MiddlewareMetrics::Phase::Post, isGlobal, hookIdx, streaming);
      const auto start = std::chrono::steady_clock::now();
      bool threwEx = false;
      try {
        middleware(request, response);
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
      emitMiddlewareMetrics(request, MiddlewareMetrics::Phase::Post, isGlobal, hookIdx,
                            static_cast<uint64_t>(duration.count()), false, threwEx, streaming);
    }
  };
  runChain(routeChain, false);
  runChain(_router.globalResponseMiddleware(), true);
}

void HttpServer::emitMiddlewareMetrics(const HttpRequest& request, MiddlewareMetrics::Phase phase, bool isGlobal,
                                       uint32_t index, uint64_t durationNs, bool shortCircuited, bool threw,
                                       bool streaming) {
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
  metrics.method = request.method();
  metrics.requestPath = request.path();

  _middlewareMetricsCb(metrics);
}

tracing::SpanRAII HttpServer::startMiddlewareSpan(const HttpRequest& request, MiddlewareMetrics::Phase phase,
                                                  bool isGlobal, uint32_t index, bool streaming) {
  tracing::SpanRAII spanScope(_telemetry.createSpan("http.middleware"));
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
  spanScope.span->setAttribute("http.method", http::MethodToStr(request.method()));
  spanScope.span->setAttribute("http.target", request.path());
  return spanScope;
}

void HttpServer::eventLoop() {
  sweepIdleConnections();

  // Apply any pending config updates posted from other threads. Fast-path: check
  // atomic flag before taking the lock to avoid contention in the nominal case.
  applyPendingUpdates();

  // Poll for events
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
    _lifecycle.exchangeStopping();
  } else {
    // ready == 0: timeout. Retry pending writes to handle edge-triggered epoll timing issues.
    // With EPOLLET, if a socket becomes writable after sendfile() returns EAGAIN but before
    // epoll_ctl(EPOLL_CTL_MOD), we miss the edge. Periodic retries ensure we eventually resume.
    for (auto it = _activeConnectionsMap.begin(); it != _activeConnectionsMap.end();) {
      if (it->second->fileSend.active && it->second->waitingWritable) {
        flushFilePayload(it);
        if (it->second->isImmediateCloseRequested()) {
          it = closeConnection(it);
          continue;
        }
      }
      ++it;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  const bool noConnections = _activeConnectionsMap.empty();

  if (_lifecycle.isStopping() || (_lifecycle.isDraining() && noConnections)) {
    closeAllConnections(true);
    _lifecycle.reset();
    if (!_isInMultiHttpServer) {
      log::info("Server stopped");
    }
  } else if (_lifecycle.isDraining()) {
    if (_lifecycle.hasDeadline() && now >= _lifecycle.deadline()) {
      log::warn("Drain deadline reached with {} active connection(s); forcing close", _activeConnectionsMap.size());
      closeAllConnections(true);
      _lifecycle.reset();
      log::info("Server drained after deadline");
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
  for (auto it = _activeConnectionsMap.begin(); it != _activeConnectionsMap.end();) {
    if (immediate) {
      it = closeConnection(it);
    } else {
      it->second->requestDrainAndClose();
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
                                 std::string_view body) {
  queueData(cnxIt, HttpResponseData(BuildSimpleError(statusCode, _config.globalHeaders, body)));

  if (_parserErrCb) {
    try {
      _parserErrCb(statusCode);
    } catch (const std::exception& ex) {
      // Swallow exceptions from user callback to avoid destabilizing the server
      log::error("Exception raised in user callback: {}", ex.what());
    }
  }

  if (immediate) {
    cnxIt->second->requestImmediateClose();
  } else {
    cnxIt->second->requestDrainAndClose();
  }

  cnxIt->second->request.end(statusCode);
}

bool HttpServer::handleExpectHeader(ConnectionMapIt cnxIt, const CorsPolicy* pCorsPolicy, bool& found100Continue) {
  HttpRequest& request = cnxIt->second->request;
  const std::string_view expectHeader = request.headerValueOrEmpty(http::Expect);
  const std::size_t headerEnd = request.headSpanSize();
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
      emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, true, {});
      return true;
    }
    try {
      auto expectationResult = _expectationHandler(request, token);
      switch (expectationResult.kind) {
        case ExpectationResultKind::Reject:
          emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, true, {});
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
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true, {});
      return true;
    } catch (...) {
      log::error("Unknown exception in ExpectationHandler");
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true, {});
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

void HttpServer::applyPendingUpdates() {
  if (_hasPendingConfigUpdates.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updateLock, _pendingConfigUpdates, _hasPendingConfigUpdates, _config, "config");

    _config.validate();

    // Reinitialize components dependent on config values.
    _encodingSelector = EncodingSelector(_config.compression);
    _eventLoop.updatePollTimeout(_config.pollInterval);
    registerBuiltInProbes();
    createEncoders();
  }
  if (_hasPendingRouterUpdates.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updateLock, _pendingRouterUpdates, _hasPendingRouterUpdates, _router, "router");
  }
}

bool HttpServer::runPreChain(HttpRequest& request, bool willStream, std::span<const RequestMiddleware> chain,
                             HttpResponse& out, bool isGlobal) {
  for (uint32_t idx = 0; idx < chain.size(); ++idx) {
    const auto& middleware = chain[idx];
    const auto start = std::chrono::steady_clock::now();
    auto spanScope = startMiddlewareSpan(request, MiddlewareMetrics::Phase::Pre, isGlobal, idx, willStream);
    MiddlewareResult decision;
    bool threwEx = true;
    try {
      decision = middleware(request);
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
    emitMiddlewareMetrics(request, MiddlewareMetrics::Phase::Pre, isGlobal, idx,
                          static_cast<uint64_t>(duration.count()), shortCircuited, false, willStream);
    if (shortCircuited) {
      out = std::move(decision).takeResponse();
      return true;
    }
  }
  return false;
}

std::unique_ptr<ConnectionState> HttpServer::getNewConnectionState() {
  if (!_cachedConnections.empty()) {
    // Reuse a cached ConnectionState object
    auto statePtr = std::move(_cachedConnections.back());
    if (statePtr->lastActivity + _config.cachedConnectionsTimeout > std::chrono::steady_clock::now()) {
      _cachedConnections.pop_back();
      statePtr->clear();
      _telemetry.counterAdd("aeronet.connections.reused_from_cache", 1UL);
      return statePtr;
    }
    // all connections are older than timeout, clear cache
    _cachedConnections.clear();
  }
  return std::make_unique<ConnectionState>();
}

}  // namespace aeronet
