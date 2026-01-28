#include "aeronet/single-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#endif

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-error-build.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"
#endif

#if defined(AERONET_ENABLE_HTTP2) || defined(AERONET_ENABLE_WEBSOCKET)
#include "aeronet/upgrade-handler.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#endif
#endif

namespace aeronet {

namespace {

// Snapshot of immutable HttpServerConfig fields that require socket rebind or structural reinitialization.
// These fields are captured before allowing config updates and silently restored afterward to prevent
// runtime modification of settings that cannot be changed without recreating the server.
class ImmutableConfigSnapshot {
 public:
  explicit ImmutableConfigSnapshot(const HttpServerConfig& cfg)
      : _nbThreads(cfg.nbThreads), _port(cfg.port), _reusePort(cfg.reusePort), _telemetry(cfg.telemetry) {}

  void restore(HttpServerConfig& cfg) {
    if (cfg.nbThreads != _nbThreads) [[unlikely]] {
      cfg.nbThreads = _nbThreads;
      log::warn("Attempted to modify immutable HttpServerConfig.nbThreads at runtime; change ignored");
    }
    if (cfg.port != _port) [[unlikely]] {
      cfg.port = _port;
      log::warn("Attempted to modify immutable HttpServerConfig.port at runtime; change ignored");
    }
    if (cfg.reusePort != _reusePort) [[unlikely]] {
      cfg.reusePort = _reusePort;
      log::warn("Attempted to modify immutable HttpServerConfig.reusePort at runtime; change ignored");
    }
    if (cfg.telemetry != _telemetry) [[unlikely]] {
      cfg.telemetry = std::move(_telemetry);
      log::warn("Attempted to modify immutable HttpServerConfig.telemetry at runtime; change ignored");
    }
  }

 private:
  uint32_t _nbThreads;
  uint16_t _port;
  bool _reusePort;
  TelemetryConfig _telemetry;
};

}  // namespace

RouterUpdateProxy SingleHttpServer::router() {
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

void SingleHttpServer::setParserErrorCallback(ParserErrorCallback cb) { _callbacks.parserErr = std::move(cb); }

void SingleHttpServer::setMetricsCallback(MetricsCallback cb) { _callbacks.metrics = std::move(cb); }

#ifdef AERONET_ENABLE_OPENSSL
void SingleHttpServer::setTlsHandshakeCallback(TlsHandshakeCallback cb) { _callbacks.tlsHandshake = std::move(cb); }
#endif

void SingleHttpServer::setExpectationHandler(ExpectationHandler handler) {
  _callbacks.expectation = std::move(handler);
}

void SingleHttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  // Capture snapshot of immutable fields before queuing the update
  ImmutableConfigSnapshot configSnapshot(_config);

  {
    std::scoped_lock lock(_updates.lock);
    // Wrap user's updater with immutability enforcement: apply user changes then restore immutable fields

    struct WrappedUpdater {
      void operator()(HttpServerConfig& cfg) {
        userUpdater(cfg);
        snapshot.restore(cfg);
      }

      std::function<void(HttpServerConfig&)> userUpdater;
      ImmutableConfigSnapshot snapshot;
    };

    _updates.config.emplace_back(WrappedUpdater{std::move(updater), std::move(configSnapshot)});
    _updates.hasConfig.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

void SingleHttpServer::postRouterUpdate(std::function<void(Router&)> updater) {
  submitRouterUpdate(std::move(updater), {});
}

void SingleHttpServer::submitRouterUpdate(std::function<void(Router&)> updater,
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
      assert(completionPtr == nullptr);
      log::error("Unknown exception while applying posted router update");
    }
  };

  if (!_lifecycle.isActive()) {
    wrappedUpdater(_router);
    return;
  }

  {
    std::scoped_lock lock(_updates.lock);
    _updates.router.emplace_back(std::move(wrappedUpdater));
    _updates.hasRouter.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

bool SingleHttpServer::enableWritableInterest(ConnectionMapIt cnxIt) {
  ConnectionState* state = cnxIt->second;
  assert(!state->waitingWritable);
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), EventIn | EventOut | EventRdHup | EventEt})) [[likely]] {
    state->waitingWritable = true;
    ++_stats.deferredWriteEvents;
    return true;
  }
  ++_stats.epollModFailures;
  cnxIt->second->requestDrainAndClose();
  return false;
}

bool SingleHttpServer::disableWritableInterest(ConnectionMapIt cnxIt) {
  ConnectionState* state = cnxIt->second;
  assert(state->waitingWritable);
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->first.fd(), EventIn | EventRdHup | EventEt})) [[likely]] {
    state->waitingWritable = false;
    return true;
  }
  ++_stats.epollModFailures;
  cnxIt->second->requestDrainAndClose();
  return false;
}

bool SingleHttpServer::processConnectionInput(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;

  // If we have a protocol handler installed (e.g., WebSocket, HTTP/2), use it
  if (state.protocolHandler != nullptr) {
    return processSpecialProtocolHandler(cnxIt);
  }

  // Check for h2c prior knowledge: client sending HTTP/2 connection preface directly
  // The preface is "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" (24 bytes)
  // h2c prior knowledge only applies to non-TLS (plaintext) connections
#ifdef AERONET_ENABLE_HTTP2
  if (_config.http2.enable && _config.http2.enableH2c && !_config.tls.enabled) {
    std::string_view bufView(state.inBuffer);
    // Check if buffer starts with "PRI " (first 4 chars of HTTP/2 preface)
    if (bufView.starts_with("PRI ")) {
      // Wait for full preface if we don't have enough data
      if (bufView.size() < http2::kConnectionPreface.size()) {
        return false;  // Need more data
      }
      // Verify full preface
      if (bufView.starts_with(http2::kConnectionPreface)) {
        // Switch to HTTP/2 protocol handler using unified dispatch
        state.protocolHandler =
            http2::CreateHttp2ProtocolHandler(_config.http2, _router, _config, _compression, _telemetry, _tmp.buf);
        return processSpecialProtocolHandler(cnxIt);
      }
      log::error("Invalid HTTP/2 preface, falling back to HTTP/1.1");
      // Invalid preface - continue with HTTP/1.1 (will likely fail with 400)
    }
  }
#endif

  // Default to HTTP/1.1 request processing
  return processHttp1Requests(cnxIt);
}

bool SingleHttpServer::processSpecialProtocolHandler(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;

  auto& handler = *state.protocolHandler;

  // Process input in a loop until no more bytes can be consumed
  // This is important for HTTP/2 where the client may send multiple frames
  // (e.g., connection preface + SETTINGS) in a single TCP packet
  while (!state.inBuffer.empty()) {
    // Convert input buffer to span of bytes
    std::span<const std::byte> inputData(reinterpret_cast<const std::byte*>(state.inBuffer.data()),
                                         state.inBuffer.size());

    // Process input through the protocol handler
    const auto result = handler.processInput(inputData, state);

    // Consume processed bytes from input buffer
    state.inBuffer.erase_front(result.bytesConsumed);

    // Queue any pending output from the handler
    if (handler.hasPendingOutput()) {
      auto pendingOutput = handler.getPendingOutput();
      assert(!pendingOutput.empty());

      state.outBuffer.append(
          std::string_view(reinterpret_cast<const char*>(pendingOutput.data()), pendingOutput.size()));
      handler.onOutputWritten(pendingOutput.size());

      flushOutbound(cnxIt);
    }

    // Handle result
    switch (result.action) {
      case ProtocolProcessResult::Action::Continue:
        [[fallthrough]];
      case ProtocolProcessResult::Action::ResponseReady:
        // ResponseReady was already handled above via getPendingOutput
        // If no bytes consumed, we need more data
        if (result.bytesConsumed == 0) {
          return state.isAnyCloseRequested();
        }
        break;

      case ProtocolProcessResult::Action::Upgrade:
        // Should not happen for WebSocket/HTTP2 handler
        log::warn("Unexpected upgrade action from protocol handler");
        break;

      case ProtocolProcessResult::Action::Close:
        // Protocol wants to close gracefully (e.g., close handshake complete)
        state.requestDrainAndClose();
        return true;

      case ProtocolProcessResult::Action::CloseImmediate:
        // Protocol error - close immediately
        log::warn("Protocol handler reported error");
        state.requestImmediateClose();
        return true;
    }
  }

  return state.isAnyCloseRequested();
}

bool SingleHttpServer::processHttp1Requests(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (state.asyncState.active) {
    handleAsyncBodyProgress(cnxIt);
    return state.isAnyCloseRequested();
  }
#endif
  HttpRequest& request = state.request;
  do {
    // If we don't yet have a full request line (no '\n' observed) wait for more data
    if (state.inBuffer.size() < http::kHttpReqLineMinLen) {
      break;  // need more bytes for at least the request line
    }
    const auto statusCode =
        request.initTrySetHead(state.inBuffer, _tmp.buf, _config.maxHeaderBytes, _config.mergeUnknownRequestHeaders,
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
    const auto optTransferEncoding = request.headerValue(http::TransferEncoding);
    if (optTransferEncoding) {
      if (request.version() == http::HTTP_1_0) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Transfer-Encoding not allowed in HTTP/1.0");
        break;
      }
      if (CaseInsensitiveEqual(*optTransferEncoding, http::chunked)) {
        isChunked = true;
      } else {
        emitSimpleError(cnxIt, http::StatusCodeNotImplemented, true, "Unsupported Transfer-Encoding");
        break;
      }
      if (request.headerValue(http::ContentLength)) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true,
                        "Content-Length and Transfer-Encoding cannot be used together");
        break;
      }
    }

    // Route matching
    const Router::RoutingResult routingResult = _router.match(request.method(), request.path());
    const CorsPolicy* pCorsPolicy = routingResult.pCorsPolicy;

    // Check for HTTP/2 cleartext upgrade (h2c) - only if HTTP/2 is enabled and not already TLS
#ifdef AERONET_ENABLE_HTTP2
    if (_config.http2.enable && !state.tlsEstablished &&
        upgrade::DetectUpgradeTarget(request.headerValueOrEmpty(http::Upgrade)) == ProtocolType::Http2) {
      const auto upgradeValidation = upgrade::ValidateHttp2Upgrade(request.headers());
      if (!upgradeValidation.valid) {
        // If h2c upgrade validation failed, respond with error
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, upgradeValidation.errorMessage);
        break;
      }
      // Generate and send 101 Switching Protocols response
      const std::size_t consumedBytesUpgrade = request.headSpanSize();
      state.inBuffer.erase_front(consumedBytesUpgrade);

      // Create HTTP/2 protocol handler using unified dispatch
      state.protocolHandler =
          http2::CreateHttp2ProtocolHandler(_config.http2, _router, _config, _compression, _telemetry, _tmp.buf);
      state.protocol = ProtocolType::Http2;

      // Queue the upgrade response
      state.outBuffer.append(upgrade::BuildHttp2UpgradeResponse(upgradeValidation));
      flushOutbound(cnxIt);

      log::debug("HTTP/2 connection established via h2c upgrade on fd {}", cnxIt->first.fd());

      ++state.requestsServed;
      ++_stats.totalRequestsServed;

      // Return - the connection is now HTTP/2 and will be handled differently
      return false;
    }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
    // Check for WebSocket upgrade request
    if (routingResult.pWebSocketEndpoint != nullptr && request.method() == http::Method::GET) {
      const WebSocketEndpoint& endpoint = *routingResult.pWebSocketEndpoint;

      // Build upgrade config from endpoint settings
      WebSocketUpgradeConfig upgradeConfig{endpoint.supportedProtocols, endpoint.config.deflateConfig};

      const auto upgradeValidation = upgrade::ValidateWebSocketUpgrade(request.headers(), upgradeConfig);
      if (upgradeValidation.valid) {
        // Generate and send 101 Switching Protocols response
        const std::size_t consumedBytes = request.headSpanSize();
        state.inBuffer.erase_front(consumedBytes);

        // Create WebSocket handler using the endpoint's factory or default
        std::unique_ptr<websocket::WebSocketHandler> wsHandler;
        if (endpoint.factory) {
          wsHandler = endpoint.factory(request);
          // If factory doesn't set compression, we need to potentially upgrade it
          if (!wsHandler->hasCompression() && upgradeValidation.deflateParams.has_value()) {
            // Factory didn't configure compression but it was negotiated - recreate handler
            auto config = wsHandler->config();
            wsHandler = std::make_unique<websocket::WebSocketHandler>(config, websocket::WebSocketCallbacks{},
                                                                      upgradeValidation.deflateParams);
          }
        } else {
          auto config = endpoint.config;
          config.isServerSide = true;
          wsHandler = std::make_unique<websocket::WebSocketHandler>(config, websocket::WebSocketCallbacks{},
                                                                    upgradeValidation.deflateParams);
        }

        // Install the protocol handler
        state.protocolHandler = std::move(wsHandler);
        state.protocol = ProtocolType::WebSocket;

        // Queue the upgrade response
        state.outBuffer.append(upgrade::BuildWebSocketUpgradeResponse(upgradeValidation));
        flushOutbound(cnxIt);

        ++state.requestsServed;
        ++_stats.totalRequestsServed;

        // Return - the connection is now a WebSocket and will be handled differently
        return false;
      }
      // If upgrade validation failed but route has WebSocket endpoint, return 400
      if (upgrade::DetectUpgradeTarget(request.headerValueOrEmpty(http::Upgrade)) == ProtocolType::WebSocket) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, upgradeValidation.errorMessage);
        break;
      }
      // Otherwise, fall through to normal request handling (if there's a regular handler)
    }
#endif

    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    bool found100Continue = false;
    auto optExpect = request.headerValue(http::Expect);
    if (optExpect && handleExpectHeader(cnxIt, *optExpect, pCorsPolicy, found100Continue)) {
      break;  // stop processing this request (response queued)
    }
    std::size_t consumedBytes = 0;
    const BodyDecodeStatus decodeStatus = decodeBodyIfReady(cnxIt, isChunked, found100Continue, consumedBytes);
    if (decodeStatus == BodyDecodeStatus::Error) {
      break;
    }
    const bool bodyReady = decodeStatus == BodyDecodeStatus::Ready;
    if (bodyReady) {
      if (_config.bodyReadTimeout.count() > 0) {
        state.waitingForBody = false;
        state.bodyLastActivity = {};
      }
      if (!request._body.empty() && !maybeDecompressRequestBody(cnxIt)) {
        break;
      }
      state.installAggregatedBodyBridge();
    } else {
      if (_config.bodyReadTimeout.count() > 0) {
        state.waitingForBody = true;
        state.bodyLastActivity = std::chrono::steady_clock::now();
      }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      if (routingResult.asyncRequestHandler() == nullptr) {
        break;
      }
#else
      break;
#endif
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

    request.finalizeBeforeHandlerCall(routingResult.pathParams);

    auto requestMiddlewareRange = routingResult.requestMiddlewareRange;
    auto responseMiddlewareRange = routingResult.responseMiddlewareRange;

    const bool isStreaming = routingResult.streamingHandler() != nullptr;

    auto sendResponse = [this, isStreaming, responseMiddlewareRange, cnxIt, consumedBytes,
                         pCorsPolicy](HttpResponse&& resp) {
      ApplyResponseMiddleware(cnxIt->second->request, resp, responseMiddlewareRange, _router.globalResponseMiddleware(),
                              _telemetry, isStreaming, _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
    };

    auto corsRejected = [&]() {
      if (pCorsPolicy == nullptr) {
        return false;
      }
      if (pCorsPolicy->wouldApply(request) == CorsPolicy::ApplyStatus::OriginDenied) {
        sendResponse(HttpResponse(http::StatusCodeForbidden).body("Forbidden by CORS policy"));
        return true;
      }
      return false;
    };

    auto shortCircuitedResponse =
        RunRequestMiddleware(request, _router.globalRequestMiddleware(), requestMiddlewareRange, _telemetry,
                             isStreaming, _callbacks.middlewareMetrics);

    if (shortCircuitedResponse.has_value()) {
      sendResponse(std::move(*shortCircuitedResponse));
      continue;
    }

    if (routingResult.streamingHandler() != nullptr) {
      const bool streamingClose = callStreamingHandler(*routingResult.streamingHandler(), cnxIt, consumedBytes,
                                                       pCorsPolicy, responseMiddlewareRange);
      if (streamingClose) {
        break;
      }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    } else if (routingResult.asyncRequestHandler() != nullptr) {
      if (corsRejected()) {
        continue;
      }

      const bool handlerActive =
          dispatchAsyncHandler(cnxIt, *routingResult.asyncRequestHandler(), bodyReady, isChunked, found100Continue,
                               consumedBytes, pCorsPolicy, responseMiddlewareRange);
      if (handlerActive) {
        return state.isAnyCloseRequested();
      }
#endif
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
        sendResponse(HttpResponse(http::StatusCodeInternalServerError, ex.what()));
      } catch (...) {
        log::error("Unknown exception in path handler");
        sendResponse(HttpResponse(http::StatusCodeInternalServerError, "Unknown error"));
      }
    } else {
      HttpResponse resp(http::StatusCodeNotFound);
      if (routingResult.redirectPathIndicator != Router::RoutingResult::RedirectSlashMode::None) {
        // Emit 301 redirect to canonical form.
        resp.status(http::StatusCodeMovedPermanently).body("Redirecting");
        if (routingResult.redirectPathIndicator == Router::RoutingResult::RedirectSlashMode::AddSlash) {
          _tmp.buf.assign(request.path());
          _tmp.buf.push_back('/');
          resp.location(_tmp.buf);
        } else {
          resp.location(request.path().substr(0, request.path().size() - 1));
        }

        consumedBytes = 0;  // already advanced
      } else if (routingResult.methodNotAllowed) {
        resp.status(http::StatusCodeMethodNotAllowed).body(http::ReasonMethodNotAllowed);
      }
      sendResponse(std::move(resp));
    }

  } while (!state.isAnyCloseRequested());
  return state.isAnyCloseRequested();
}

bool SingleHttpServer::maybeDecompressRequestBody(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  const auto res = internal::HttpCodec::MaybeDecompressRequestBody(_config.decompression, request,
                                                                   state.bodyAndTrailersBuffer, _tmp.buf);

  if (res.message != nullptr) {
    emitSimpleError(cnxIt, res.status, true, res.message);
    return false;
  }

  // Parse trailers if present
  if (state.trailerStartPos != 0) {
    auto* buf = state.bodyAndTrailersBuffer.data();
    [[maybe_unused]] const bool isSuccess = parseHeadersUnchecked(request._trailers, buf, buf + state.trailerStartPos,
                                                                  buf + state.bodyAndTrailersBuffer.size());
    // trailers should have been validated in decodeChunkedBody
    assert(isSuccess);
  }

  return true;
}

bool SingleHttpServer::callStreamingHandler(const StreamingHandler& streamingHandler, ConnectionMapIt cnxIt,
                                            std::size_t consumedBytes, const CorsPolicy* pCorsPolicy,
                                            std::span<const ResponseMiddleware> postMiddleware) {
  HttpRequest& request = cnxIt->second->request;
  bool wantClose = request.wantClose();
  bool isHead = request.method() == http::Method::HEAD;
  Encoding compressionFormat = Encoding::none;
  ConnectionState& state = *cnxIt->second;

  // Determine active CORS policy (route-specific if provided, otherwise global)
  if (pCorsPolicy != nullptr) {
    if (pCorsPolicy->wouldApply(request) == CorsPolicy::ApplyStatus::OriginDenied) {
      HttpResponse corsProbe(http::StatusCodeForbidden);
      corsProbe.body("Forbidden by CORS policy");
      ApplyResponseMiddleware(request, corsProbe, postMiddleware, _router.globalResponseMiddleware(), _telemetry, true,
                              _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(corsProbe), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
  }

  if (!isHead) {
    auto encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _compression.selector.negotiateAcceptEncoding(encHeader);
    if (negotiated.reject) {
      // Mirror buffered path semantics: emit a 406 and skip invoking user streaming handler.
      HttpResponse resp(http::StatusCodeNotAcceptable);
      resp.body("No acceptable content-coding available");
      ApplyResponseMiddleware(request, resp, postMiddleware, _router.globalResponseMiddleware(), _telemetry, true,
                              _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      return state.isAnyCloseRequested();
    }
    compressionFormat = negotiated.encoding;
  }

  // Pass the resolved activeCors pointer to the streaming writer so it can apply headers lazily
  HttpResponseWriter writer(*this, cnxIt->first.fd(), request, wantClose, compressionFormat, pCorsPolicy,
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

  if (_callbacks.metrics) {
    emitRequestMetrics(request, http::StatusCodeOK, request.body().size(), state.requestsServed > 1);
  }

  return shouldClose;
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
bool SingleHttpServer::dispatchAsyncHandler(ConnectionMapIt cnxIt, const AsyncRequestHandler& handler, bool bodyReady,
                                            bool isChunked, bool expectContinue, std::size_t consumedBytes,
                                            const CorsPolicy* pCorsPolicy,
                                            std::span<const ResponseMiddleware> responseMiddleware) {
  ConnectionState& state = *cnxIt->second;
  RequestTask<HttpResponse> task = handler(state.request);

  if (!task.valid()) {
    static constexpr std::string_view kMessage = "Async handler inactive";
    log::error("Async path handler returned an invalid RequestTask for path {}", state.request.path());
    if (bodyReady) {
      HttpResponse resp(http::StatusCodeInternalServerError);
      resp.body(kMessage);
      ApplyResponseMiddleware(state.request, resp, responseMiddleware, _router.globalResponseMiddleware(), _telemetry,
                              false, _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
    } else {
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, true, kMessage);
    }

    return false;
  }

  auto handle = task.release();
  assert(handle);

  auto& asyncState = state.asyncState;

  asyncState.active = true;
  asyncState.handle = handle;
  asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::None;
  asyncState.needsBody = !bodyReady;
  asyncState.isChunked = isChunked;
  asyncState.expectContinue = expectContinue;
  asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
  asyncState.corsPolicy = pCorsPolicy;
  asyncState.responseMiddleware = responseMiddleware.data();
  asyncState.responseMiddlewareCount = responseMiddleware.size();
  asyncState.pendingResponse = {};

  // Keep header storage stable while async work runs so header string_views stay valid
  state.request.pinHeadStorage(state);

  // Install the postCallback function for deferred work
  asyncState.postCallback = [this, fd = cnxIt->first.fd()](std::coroutine_handle<> handle, std::function<void()> work) {
    postAsyncCallback(fd, handle, std::move(work));
  };

  resumeAsyncHandler(cnxIt);
  return asyncState.active;
}

void SingleHttpServer::resumeAsyncHandler(ConnectionMapIt cnxIt) {
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

void SingleHttpServer::handleAsyncBodyProgress(ConnectionMapIt cnxIt) {
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

  if (async.pendingResponse.has_value()) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void SingleHttpServer::onAsyncHandlerCompleted(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;
  if (!async.handle) {
    return;
  }

  auto typedHandle =
      std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(async.handle.address());
  bool fromException = false;
  HttpResponse resp(HttpResponse::Check::No);  // do not allocate memory yet
  try {
    resp = std::move(typedHandle.promise().consume_result());
  } catch (const std::exception& ex) {
    fromException = true;
    log::error("Exception in async path handler: {}", ex.what());
    resp = HttpResponse(http::StatusCodeInternalServerError);
    resp.body(ex.what());
  } catch (...) {
    fromException = true;
    log::error("Unknown exception in async path handler");
    resp = HttpResponse(http::StatusCodeInternalServerError);
    resp.body("Unknown error");
  }
  typedHandle.destroy();
  async.handle = {};
  async.pendingResponse = std::move(resp);

  if (async.needsBody) {
    if (fromException) {
      // Body will still be drained before response is flushed; nothing else to do here.
    }
  } else {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void SingleHttpServer::tryFlushPendingAsyncResponse(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  auto& async = state.asyncState;

  assert(!async.needsBody);
  assert(async.pendingResponse.has_value());

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  ApplyResponseMiddleware(state.request, *async.pendingResponse, middlewareSpan, _router.globalResponseMiddleware(),
                          _telemetry, false, _callbacks.middlewareMetrics);
  finalizeAndSendResponseForHttp1(cnxIt, std::move(*async.pendingResponse), async.consumedBytes, async.corsPolicy);
  state.asyncState.clear();
}
#endif

void SingleHttpServer::emitRequestMetrics(const HttpRequest& request, http::StatusCode status, std::size_t bytesIn,
                                          bool reusedConnection) const {
  RequestMetrics metrics;
  metrics.status = status;
  metrics.bytesIn = bytesIn;
  metrics.reusedConnection = reusedConnection;
  metrics.method = request.method();
  metrics.path = request.path();
  metrics.duration = std::chrono::steady_clock::now() - request.reqStart();
  _callbacks.metrics(metrics);
}

void SingleHttpServer::eventLoop() {
  // Apply any pending config updates posted from other threads.
  applyPendingUpdates();

  // Poll for events
  const auto events = _eventLoop.poll();

  bool maintenanceTick = false;

  if (events.data() == nullptr) [[unlikely]] {
    _telemetry.counterAdd("aeronet.events.errors", 1);
    _lifecycle.exchangeStopping();
  } else if (!events.empty()) {
    _telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(events.size()));
    for (auto event : events) {
      const int fd = event.fd;
      if (fd == _listenSocket.fd()) {
        // Always attempt to accept new connections when the listener is signaled.
        // The lifecycle controls higher-level acceptance semantics; accepting
        // here is safe and allows probes to connect during drain.
        acceptNewConnections();
      } else if (fd == _lifecycle.wakeupFd.fd()) {
        _lifecycle.wakeupFd.read();
      } else if (fd == _maintenanceTimer.fd()) {
        _maintenanceTimer.drain();
        maintenanceTick = true;
      } else {
        const auto bmp = event.eventBmp;
        if ((bmp & EventOut) != 0) {
          handleWritableClient(fd);
        }
        // EPOLLERR/EPOLLHUP/EPOLLRDHUP can be delivered without EPOLLIN.
        // Treat them as a read trigger so we promptly observe EOF/errors and close.
        if ((bmp & (EventIn | EventErr | EventHup | EventRdHup)) != 0) {
          handleReadableClient(fd);
        }
      }
    }
  } else {
    // timeout / EINTR (treated as timeout). Retry pending writes to handle edge-triggered epoll timing issues.
    // With EPOLLET, if a socket becomes writable after sendfile() returns EAGAIN but before
    // epoll_ctl(EPOLL_CTL_MOD), we miss the edge. Periodic retries ensure we eventually resume.
    maintenanceTick = true;
  }

  // Under high load epoll_wait may return immediately and never hit the timeout path.
  // We still need periodic maintenance for timeouts and edge-triggered sendfile progress.
  if (maintenanceTick) {
    const auto nbActiveConnections = _connections.active.size();

    _telemetry.gauge("aeronet.connections.active_count", static_cast<int64_t>(nbActiveConnections));
    _telemetry.gauge("aeronet.events.capacity_current_count", static_cast<int64_t>(_eventLoop.capacity()));

    sweepIdleConnections();

    if (_lifecycle.isStopping() || (_lifecycle.isDraining() && nbActiveConnections == 0)) {
      closeAllConnections();
      _lifecycle.reset();
      if (!isInMultiHttpServer()) {
        log::info("Server stopped");
      }
    } else if (_lifecycle.isDraining()) {
      if (_lifecycle.hasDeadline() && std::chrono::steady_clock::now() >= _lifecycle.deadline()) {
        log::warn("Drain deadline reached with {} active connection(s); forcing close", nbActiveConnections);
        closeAllConnections();
        _lifecycle.reset();
        log::info("Server drained after deadline");
      }
    } else if (SignalHandler::IsStopRequested()) {
      beginDrain(SignalHandler::GetMaxDrainPeriod());
    }
  }
}

void SingleHttpServer::updateMaintenanceTimer() {
  // Periodic maintenance timer: drives idle sweeps / housekeeping without relying on epoll_wait timeouts.
  using namespace std::chrono;

  milliseconds minTimeout = milliseconds::max();
  const auto consider = [&](milliseconds dur) {
    if (dur.count() > 0) {
      minTimeout = std::min(minTimeout, dur);
    }
  };

  if (_config.enableKeepAlive) {
    consider(_config.keepAliveTimeout);
  }
  consider(_config.headerReadTimeout);
  consider(_config.bodyReadTimeout);
  consider(_config.pollInterval);

#ifdef AERONET_ENABLE_OPENSSL
  if (_config.tls.enabled) {
    consider(_config.tls.handshakeTimeout);
  }
#endif

  assert(minTimeout != milliseconds::max());

  _maintenanceTimer.armPeriodic(minTimeout);
}

void SingleHttpServer::closeListener() noexcept {
  if (_listenSocket) {
    _eventLoop.del(_listenSocket.fd());
    _listenSocket.close();
    // Trigger wakeup to break any blocking epoll_wait quickly.
    _lifecycle.wakeupFd.send();
  }
}

void SingleHttpServer::closeAllConnections() {
  for (auto it = _connections.active.begin(); it != _connections.active.end();) {
    it = closeConnection(it);
  }
}

ServerStats SingleHttpServer::stats() const {
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
  statsOut.tlsHandshakesSucceeded = _tls.metrics.handshakesSucceeded;
  statsOut.tlsHandshakesFull = _tls.metrics.handshakesFull;
  statsOut.tlsHandshakesResumed = _tls.metrics.handshakesResumed;
  statsOut.tlsHandshakesFailed = _tls.metrics.handshakesFailed;
  statsOut.tlsHandshakesRejectedConcurrency = _tls.metrics.handshakesRejectedConcurrency;
  statsOut.tlsHandshakesRejectedRateLimit = _tls.metrics.handshakesRejectedRateLimit;
  statsOut.tlsClientCertPresent = _tls.metrics.clientCertPresent;
  if (_tls.ctxHolder) {
    statsOut.tlsAlpnStrictMismatches = _tls.ctxHolder->alpnStrictMismatches();
  }
  statsOut.tlsAlpnDistribution.reserve(_tls.metrics.alpnDistribution.size());
  for (const auto& [key, value] : _tls.metrics.alpnDistribution) {
    statsOut.tlsAlpnDistribution.emplace_back(key, value);
  }
  statsOut.tlsHandshakeFailureReasons.reserve(_tls.metrics.handshakeFailureReasons.size());
  for (const auto& [key, value] : _tls.metrics.handshakeFailureReasons) {
    statsOut.tlsHandshakeFailureReasons.emplace_back(key, value);
  }
  statsOut.tlsVersionCounts.reserve(_tls.metrics.versionCounts.size());
  for (const auto& [key, value] : _tls.metrics.versionCounts) {
    statsOut.tlsVersionCounts.emplace_back(key, value);
  }
  statsOut.tlsCipherCounts.reserve(_tls.metrics.cipherCounts.size());
  for (const auto& [key, value] : _tls.metrics.cipherCounts) {
    statsOut.tlsCipherCounts.emplace_back(key, value);
  }
  statsOut.tlsHandshakeDurationCount = _tls.metrics.handshakeDurationCount;
  statsOut.tlsHandshakeDurationTotalNs = _tls.metrics.handshakeDurationTotalNs;
  statsOut.tlsHandshakeDurationMaxNs = _tls.metrics.handshakeDurationMaxNs;
  statsOut.ktlsSendEnabledConnections = _tls.metrics.ktlsSendEnabledConnections;
  statsOut.ktlsSendEnableFallbacks = _tls.metrics.ktlsSendEnableFallbacks;
  statsOut.ktlsSendForcedShutdowns = _tls.metrics.ktlsSendForcedShutdowns;
  statsOut.ktlsSendBytes = _tls.metrics.ktlsSendBytes;
#endif
  return statsOut;
}

void SingleHttpServer::emitSimpleError(ConnectionMapIt cnxIt, http::StatusCode statusCode, bool immediate,
                                       std::string_view body) {
  queueData(cnxIt, HttpResponseData(BuildSimpleError(statusCode, _config.globalHeaders, body)));

  if (_callbacks.parserErr) {
    // Swallow exceptions from user callback to avoid destabilizing the server
    try {
      _callbacks.parserErr(statusCode);
    } catch (const std::exception& ex) {
      log::error("Exception raised in user callback: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception raised in user callback");
    }
  }

  if (immediate) {
    cnxIt->second->requestImmediateClose();
  } else {
    cnxIt->second->requestDrainAndClose();
  }

  cnxIt->second->request.end(statusCode);
}

bool SingleHttpServer::handleExpectHeader(ConnectionMapIt cnxIt, std::string_view expectHeader,
                                          const CorsPolicy* pCorsPolicy, bool& found100Continue) {
  HttpRequest& request = cnxIt->second->request;
  const std::size_t headerEnd = request.headSpanSize();
  // Parse comma-separated tokens (trim spaces/tabs). Case-insensitive comparison for 100-continue.
  // headerEnd = offset from connection buffer start to end of headers
  // TODO: simplify this code using TrimOws
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
    if (!_callbacks.expectation) {
      // No handler and not 100-continue -> RFC says respond 417
      emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, true, {});
      return true;
    }
    try {
      auto expectationResult = _callbacks.expectation(request, token);
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
              queueData(cnxIt, HttpResponseData(http::HTTP11_102_PROCESSING));
              break;
            }
            default: {
              static constexpr std::string_view kHttpResponseLinePrefix = "HTTP/1.1 ";

              RawChars buf(kHttpResponseLinePrefix.size() + 3U + http::DoubleCRLF.size());

              std::memcpy(buf.data(), kHttpResponseLinePrefix.data(), kHttpResponseLinePrefix.size());
              std::memcpy(write3(buf.data() + kHttpResponseLinePrefix.size(), status), http::DoubleCRLF.data(),
                          http::DoubleCRLF.size());
              buf.setSize(buf.capacity());

              queueData(cnxIt, HttpResponseData(std::move(buf)));
              break;
            }
          }

          break;
        }
        case ExpectationResultKind::FinalResponse:
          // Send the provided final response immediately and skip body processing.
          finalizeAndSendResponseForHttp1(cnxIt, std::move(expectationResult.finalResponse), headerEnd, pCorsPolicy);
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
      if constexpr (std::is_same_v<std::remove_reference_t<decltype(objToUpdate)>, HttpServerConfig>) {
        objToUpdate.validate();
      }
    } catch (const std::exception& ex) {
      log::error("Exception while applying posted {} update: {}", name, ex.what());
    } catch (...) {
      log::error("Unknown exception while applying posted {} update", name);
    }
  }
}

}  // namespace

void SingleHttpServer::applyPendingUpdates() {
  if (_updates.hasConfig.load(std::memory_order_acquire)) {
#ifdef AERONET_ENABLE_OPENSSL
    // Capture TLS config before updates to detect changes
    const TLSConfig tlsBefore = _config.tls;
#endif

    ApplyPendingUpdates(_updates.lock, _updates.config, _updates.hasConfig, _config, "config");

    // Reinitialize components dependent on config values.
    _compression.selector = EncodingSelector(_config.compression);
    _eventLoop.updatePollTimeout(_config.pollInterval);
    updateMaintenanceTimer();
    registerBuiltInProbes();
    _compression.createEncoders(_config.compression);

#ifdef AERONET_ENABLE_OPENSSL
    // If TLS config changed, rebuild the OpenSSL context.
    // Note: keep old context alive for existing connections via ConnectionState::tlsContextKeepAlive.
    if (_config.tls != tlsBefore) {
      if (_config.tls.enabled) {
        _tls.ctxHolder = std::make_shared<TlsContext>(_config.tls, _tls.sharedTicketKeyStore);
      } else {
        _tls.ctxHolder.reset();
      }
    }
#endif
  }
  if (_updates.hasRouter.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updates.lock, _updates.router, _updates.hasRouter, _router, "router");
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Process async callbacks posted from background threads
  if (_updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
    vector<internal::PendingUpdates::AsyncCallback> callbacks;
    {
      std::scoped_lock lock(_updates.lock);
      callbacks = std::move(_updates.asyncCallbacks);
      _updates.asyncCallbacks.clear();
      _updates.hasAsyncCallbacks.store(false, std::memory_order_release);
    }

    for (auto& cb : callbacks) {
      // Execute any pre-resume work
      if (cb.work) {
        try {
          cb.work();
        } catch (const std::exception& ex) {
          log::error("Exception in async callback work: {}", ex.what());
        } catch (...) {
          log::error("Unknown exception in async callback work");
        }
      }

      // Use O(1) hash map lookup with the connection fd
      auto it = _connections.active.find(cb.connectionFd);
      if (it != _connections.active.end()) {
        auto& asyncState = it->second->asyncState;
        if (asyncState.active && asyncState.handle == cb.handle) {
          asyncState.awaitReason = ConnectionState::AsyncHandlerState::AwaitReason::None;
          resumeAsyncHandler(it);
        }
      }
    }
  }
#endif
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void SingleHttpServer::postAsyncCallback(int connectionFd, std::coroutine_handle<> handle, std::function<void()> work) {
  {
    std::scoped_lock lock(_updates.lock);
    _updates.asyncCallbacks.push_back({connectionFd, handle, std::move(work)});
    _updates.hasAsyncCallbacks.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}
#endif

#ifdef AERONET_ENABLE_HTTP2
void SingleHttpServer::setupHttp2Connection(ConnectionState& state) {
  // Create HTTP/2 protocol handler with unified dispatcher
  // Pass sendServerPrefaceForTls=true: server must send SETTINGS immediately for TLS ALPN "h2"
  state.protocolHandler =
      http2::CreateHttp2ProtocolHandler(_config.http2, _router, _config, _compression, _telemetry, _tmp.buf, true);
  state.protocol = ProtocolType::Http2;

  // Immediately flush the server preface (SETTINGS frame) that was queued during handler creation
  // For TLS ALPN "h2", the server must send SETTINGS before the client sends any data
  if (state.protocolHandler->hasPendingOutput()) {
    auto pendingOutput = state.protocolHandler->getPendingOutput();
    state.outBuffer.append(std::string_view(reinterpret_cast<const char*>(pendingOutput.data()), pendingOutput.size()));
    state.protocolHandler->onOutputWritten(pendingOutput.size());
  }
}
#endif

}  // namespace aeronet
