#include "aeronet/single-http-server.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
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
#include <thread>
#include <type_traits>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/access-log-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/https-redirect.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"
#include "http-error-build.hpp"
#include "http1-writer-transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#include "aeronet/websocket-upgrade.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/request-task.hpp"
#endif

#if defined(AERONET_ENABLE_HTTP2) || defined(AERONET_ENABLE_WEBSOCKET)
#include "upgrade-handler.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/tunnel-bridge.hpp"
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
  uint16_t _nbThreads;
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
          [this] -> Router& { return _router; }};
}

void SingleHttpServer::setParserErrorCallback(ParserErrorCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.parserErr = cb; });
}

void SingleHttpServer::setMetricsCallback(MetricsCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.metrics = cb; });
}

#ifdef AERONET_ENABLE_OPENSSL
void SingleHttpServer::setTlsHandshakeCallback(TlsHandshakeCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.tlsHandshake = cb; });
}
#endif

void SingleHttpServer::setExpectationHandler(ExpectationHandler handler) {
  submitCallbacksUpdate([handler = std::move(handler)](Callbacks& callbacks) { callbacks.expectation = handler; });
}

void SingleHttpServer::setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.middlewareMetrics = cb; });
}

void SingleHttpServer::submitCallbacksUpdate(std::function<void(Callbacks&)> updater) {
  if (_lifecycle.isEventLoopThread()) {
    // From a handler or a callback: the event loop is the thread using the callbacks.
    updater(_callbacks);
    return;
  }

  // While running, the change is applied by the event loop, and this call waits for it: the new callback is then used
  // for everything happening after this call (the event loop may otherwise still process, in its current iteration, a
  // connection initiated after it).
  auto applied = std::make_shared<std::atomic<bool>>(false);
  {
    std::scoped_lock lock(_updates.lock);
    if (_lifecycle.isIdle()) {
      applyQueuedCallbacksUpdates();
      updater(_callbacks);
      return;
    }
    _callbacksUpdates.emplace_back([updater = std::move(updater), applied](Callbacks& callbacks) {
      // Released even if the updater throws, so that the waiting caller does not wait for the server to stop.
      const auto release = [&applied] noexcept { applied->store(true, std::memory_order_release); };
      try {
        updater(callbacks);
      } catch (...) {
        release();
        throw;
      }
      release();
    });
    _hasCallbacksUpdates.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();

  while (!applied->load(std::memory_order_acquire)) {
    {
      std::scoped_lock lock(_updates.lock);
      if (_lifecycle.isIdle()) {
        // The event loop exited before applying it: no thread uses the callbacks anymore.
        applyQueuedCallbacksUpdates();
        return;
      }
    }
    // Bounded wait, so that the exit of the event loop is noticed even if the update is never applied by it.
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
}

void SingleHttpServer::applyQueuedCallbacksUpdates() {
  // Changes still queued (posted just before the event loop exited) first, so that the latest one wins.
  for (auto& pendingUpdater : _callbacksUpdates) {
    pendingUpdater(_callbacks);
  }
  _callbacksUpdates.clear();
  _hasCallbacksUpdates.store(false, std::memory_order_relaxed);
}

void SingleHttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  {
    std::scoped_lock lock(_updates.lock);
    // Wrap user's updater with immutability enforcement: apply user changes then restore immutable fields.
    // The snapshot is taken when applying the update, by the event loop: the config cannot be read from the calling
    // thread, the event loop possibly applying a previous update at the same time.
    struct WrappedUpdater {
      void operator()(HttpServerConfig& cfg) const {
        ImmutableConfigSnapshot snapshot(cfg);
        userUpdater(cfg);
        snapshot.restore(cfg);
      }

      std::function<void(HttpServerConfig&)> userUpdater;
    };

    _updates.config.emplace_back(WrappedUpdater{std::move(updater)});
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

  {
    std::scoped_lock lock(_updates.lock);
    // beginStartup() takes this same lock before publishing Starting. A direct update
    // therefore completes before startup begins; every later update is queued for the
    // event-loop thread and clamped there.
    if (_lifecycle.isIdle()) {
      wrappedUpdater(_router);
      return;
    }
    _updates.router.emplace_back(std::move(wrappedUpdater));
    _updates.hasRouter.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

bool SingleHttpServer::enableWritableInterest(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  assert(!state.waitingWritable);
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->fd(), EventIn | EventOut | EventRdHup | EventEt})) [[likely]] {
    state.waitingWritable = true;
    ++_connectionSweepState.writableConnections;
    ++_stats.deferredWriteEvents;
    return true;
  }
  // Cannot register for EPOLLOUT so buffered data will never be flushed.
  // Clear the write buffers so canCloseConnectionForDrain() can proceed;
  // without this, tunnel connections get stuck forever (exempt from keep-alive reaping).
  ++_stats.epollModFailures;
  state.clearBuffers();
  return false;
}

bool SingleHttpServer::disableWritableInterest(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  assert(state.waitingWritable);
  forgetWritableInterest(state);
  if (_eventLoop.mod(EventLoop::EventFd{cnxIt->fd(), EventIn | EventRdHup | EventEt})) [[likely]] {
    return true;
  }
  ++_stats.epollModFailures;
  state.clearBuffers();
  return false;
}

bool SingleHttpServer::processConnectionInput(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);

  // If we have a protocol handler installed (e.g., WebSocket, HTTP/2), use it
  if (state.protocolHandler) {
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
        state.protocolHandler = http2::CreateHttp2ProtocolHandler(_config.http2, _router, _config, _compressionState,
                                                                  _decompressionState, _telemetry, _sharedBuffers.buf,
                                                                  false, _dateHeader.data(), state.clientAddress());
        // Flag the connection as HTTP/2 like the ALPN path does: async resumption, stream deadline sweeps and
        // the HTTP/2 connection count all key off it.
        ++_connectionSweepState.http2Connections;
        state.protocol = ProtocolType::Http2;
        installH2TunnelBridge(cnxIt->fd(), state);
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

bool SingleHttpServer::processSpecialProtocolHandler(ConnectionIt cnxIt) {
  // Save the client fd before entering the loop. processInput() may call back into setupTunnelConnection()
  // (HTTP/2 CONNECT), whose insertion may invalidate cnxIt by growing the POSIX fd vector or rehashing the Windows
  // map. Re-find cnxIt by fd after each processInput() call, matching the HTTP/1.1 CONNECT path.
  const NativeHandle clientFd = cnxIt->fd();

  ConnectionState* pState = _connections.pConnectionState(clientFd);
  assert(pState != nullptr);
  auto& handler = *pState->protocolHandler;

  // Process input in a loop until no more bytes can be consumed.
  // This is important for HTTP/2 where the client may send multiple frames
  // (e.g., connection preface + SETTINGS) in a single TCP packet.
  //
  // Batch input processing, then gather protocol-owned frame fragments directly.
  bool hasAccumulatedOutput = handler.hasPendingOutput();

  while (!pState->inBuffer.empty()) {
    // Convert input buffer to span of bytes
    std::span<const std::byte> inputData(reinterpret_cast<const std::byte*>(pState->inBuffer.data()),
                                         pState->inBuffer.size());

    // Process input through the protocol handler.
    const auto result = handler.processInput(inputData, *pState);

    // processInput may insert new elements in connections.
    cnxIt = _connections.iterator(clientFd);
    pState = _connections.pConnectionState(clientFd);

    // Consume processed bytes from input buffer
    pState->inBuffer.erase_front(result.bytesConsumed);

    // Remember that the handler owns output to gather once this input batch finishes.
    if (handler.hasPendingOutput()) {
      hasAccumulatedOutput = true;
    }

    // Handle result
    switch (result.action) {
      case ProtocolProcessResult::Action::Continue:
        [[fallthrough]];
      case ProtocolProcessResult::Action::ResponseReady:
        // ResponseReady was already handled above via getPendingOutput
        // If no bytes were consumed, flush before deciding whether the socket can accept more input.
        if (result.bytesConsumed == 0) {
          if (hasAccumulatedOutput) {
            flushOutbound(cnxIt);
          }
          return pState->isAnyCloseRequested() || pState->hasPendingOutput();
        }
        break;

      case ProtocolProcessResult::Action::Upgrade:
        // Should not happen for WebSocket/HTTP2 handler
        log::warn("Unexpected upgrade action from protocol handler");
        break;

      case ProtocolProcessResult::Action::Close:
        // Protocol wants to close gracefully - flush GOAWAY etc. before closing
        if (hasAccumulatedOutput) {
          flushOutbound(cnxIt);
        }
        pState->requestDrainAndClose();
        return true;

      case ProtocolProcessResult::Action::CloseImmediate:
        // Protocol error - flush any error frames then close immediately
        if (hasAccumulatedOutput) {
          flushOutbound(cnxIt);
        }
        log::debug("Protocol handler reported error");
        pState->requestDrainAndClose();
        return true;
    }
  }

  // Batch-flush all accumulated output frames in a single transport write.
  if (hasAccumulatedOutput) {
    flushOutbound(cnxIt);
  }

  // A blocked output transport is also a signal to stop this readable-event loop. This keeps additional
  // HTTP/2 frames in the socket while the peer is not draining responses.
  return pState->isAnyCloseRequested() || pState->hasPendingOutput();
}

bool SingleHttpServer::processHttp1Requests(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.isAnyCloseRequested()) {
    // The bytes still buffered (a request answered by an error, its unread body...) must not be served.
    return true;
  }
  const auto cnxFd = cnxIt->fd();
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (auto* asyncState = state.pAsyncState(); asyncState != nullptr && asyncState->active) {
    handleAsyncBodyProgress(cnxIt);
    if (asyncState->active || state.isAnyCloseRequested()) {
      return state.isAnyCloseRequested() || PauseReadingDuringAsyncHandler(state);
    }
    // The handler completed: serve the requests pipelined behind it.
  }
#endif
  HttpRequestView& request = state.request;
  do {
    // Do not parse the next pipelined request while a file send is still in progress.
    // attachFilePayload would silently overwrite the in-flight file payload, corrupting
    // the response stream (partial data from file1 mixed with file2 headers+data).
    // Also skip when outBuffer is non-empty — the previous response hasn't been fully
    // flushed yet, and starting a new response would interleave data.
    if (state.isSendingFile() || !state.outBuffer.empty()) {
      break;
    }
    // If we don't have enough bytes for the minimum request line, wait for more data
    if (state.inBuffer.size() < http::kHttpReqLineMinLen) {
      break;
    }
    const auto statusCode =
        request.initTrySetHead(state.inBuffer, _sharedBuffers.buf, _config.maxHeaderBytes,
                               _config.mergeUnknownRequestHeaders, _telemetry.createSpan("http.request"));
    if (statusCode == HttpRequestView::kStatusNeedMoreData) {
      break;
    }

    ++state.requestsServed;
    ++_stats.totalRequestsServed;

    if (statusCode != http::StatusCodeOK) {
      emitSimpleError(cnxIt, statusCode, {});

      // We break unconditionally; the connection will be torn down after any queued error bytes are flushed. No partial
      // recovery is attempted for a malformed / protocol-violating start line or headers.
      break;
    }

    request._reqStart = state.lastActivity;

    // A full request head (and body, if present) will now be processed; mark header parsing as done.
    // headerStartTp is kept alive as the reference for relative body/deadline timestamps.
    if (state.parsingHeaders && _config.headerReadTimeout.count() > 0) {
      assert(_connectionSweepState.pendingTimeoutConnections > 0U);
      --_connectionSweepState.pendingTimeoutConnections;
    }
    state.parsingHeaders = false;

    // Automatic HTTP -> HTTPS redirect: answer every request on this plaintext listener with a 3xx redirect
    // to the equivalent https:// URL, bypassing routing, upgrades and body handling. The connection is closed
    // afterwards (the client reconnects over TLS).
    if (_config.httpsRedirect.enabled()) {
      emitHttpsRedirect(cnxIt);
      break;
    }

    bool isChunked = false;
    const auto optTransferEncoding = request.headerValue(http::TransferEncoding);
    if (optTransferEncoding) {
      if (request.version() == http::HTTP_1_0) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Transfer-Encoding not allowed in HTTP/1.0");
        break;
      }
      if (CaseInsensitiveEqual(*optTransferEncoding, http::chunked)) {
        isChunked = true;
      } else {
        emitSimpleError(cnxIt, http::StatusCodeNotImplemented, "Unsupported Transfer-Encoding");
        break;
      }
      if (request.headerValue(http::ContentLength)) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest,
                        "Content-Length and Transfer-Encoding cannot be used together");
        break;
      }
    }

    const auto [encoding, reject] =
        _compressionState.selector.negotiateAcceptEncoding(request.headerValueOrEmpty(http::AcceptEncoding));

    // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
    // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
    if (reject) {
      emitSimpleError(cnxIt, http::StatusCodeNotAcceptable, "No acceptable content-coding available");
      continue;
    }

    request._responsePossibleEncoding = encoding;

    // Route matching
    const Router::RoutingResult routingResult = _router.match(request.method(), request.path());
    const CorsPolicy* pCorsPolicy = routingResult.corsPolicy();

    // Arm per-route request deadline for sweep enforcement (async/streaming handlers).
    if (routingResult.pathConfig().requestTimeout != std::chrono::milliseconds::max()) {
      trackRequestDeadline(state, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            state.lastActivity - state.headerStartTp +
                                                            routingResult.pathConfig().requestTimeout)
                                                            .count()));
    }

    // `Upgrade: h2c` (HTTP/1.1 -> cleartext HTTP/2) is deliberately not honored: RFC 9113 §3.1 deprecated that
    // mechanism and RFC 9110 §7.8 lets a server ignore Upgrade, so such a request is answered over HTTP/1.1 like any
    // other. Cleartext HTTP/2 is only available with prior knowledge (Http2Config::enableH2c).

#ifdef AERONET_ENABLE_WEBSOCKET
    // Check for WebSocket upgrade request
    if (routingResult.webSocketEndpoint() != nullptr && request.method() == http::Method::GET) {
      const WebSocketEndpoint& endpoint = *routingResult.webSocketEndpoint();

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
          if (!wsHandler->hasCompression() && upgradeValidation.deflateParams.has_value()) {
            // Compression was negotiated but the factory did not configure it: enable it on the handler, keeping
            // the callbacks the factory installed.
            wsHandler->enableCompression(*upgradeValidation.deflateParams);
          }
        } else {
          auto config = endpoint.config;
          config.isServerSide = true;
          wsHandler = std::make_unique<websocket::WebSocketHandler>(config, websocket::WebSocketCallbacks(),
                                                                    upgradeValidation.deflateParams);
        }

        // Install the protocol handler
        state.protocolHandler = std::move(wsHandler);
        state.protocol = ProtocolType::WebSocket;

        // Queue the upgrade response
        char* pData = state.outBuffer.resizeUp(upgrade::ComputeWebSocketUpgradeResponseSize(upgradeValidation));

        upgrade::BuildWebSocketUpgradeResponse(upgradeValidation, pData);
        flushOutbound(cnxIt);

        // Return - the connection is now a WebSocket and will be handled differently
        return false;
      }
      // If upgrade validation failed but route has WebSocket endpoint, return 400
      if (upgrade::DetectUpgradeTarget(request.headerValueOrEmpty(http::Upgrade)) == ProtocolType::WebSocket) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, upgradeValidation.errorMessage);
        break;
      }
      // Otherwise, fall through to normal request handling (if there's a regular handler)
    }
#endif

    // Per-route request head size limit (already clamped against global in prepareRun)
    if (request.headSpanSize() > routingResult.pathConfig().maxHeaderBytes) {
      emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge, {});
      break;
    }

    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    // A request whose body is received in several reads is parsed again for each of them: its expectations are only
    // answered (interim responses, 100 Continue) the first time.
    bool found100Continue = false;
    if (!state.expectationAnswered) {
      auto optExpect = request.headerValue(http::Expect);
      if (optExpect && handleExpectHeader(cnxIt, *optExpect, pCorsPolicy, found100Continue)) {
        break;  // stop processing this request (response queued)
      }
    }
    std::size_t consumedBytes = 0;
    const BodyDecodeStatus decodeStatus =
        decodeBodyIfReady(cnxIt, isChunked, found100Continue, routingResult.pathConfig().maxBodyBytes, consumedBytes);
    if (decodeStatus == BodyDecodeStatus::Error) {
      break;
    }
    const bool bodyReady = decodeStatus == BodyDecodeStatus::Ready;
    state.expectationAnswered = !bodyReady;

    // A response sent before the whole body was received cannot consume the request: the rest of its body would then be
    // parsed as the next request (and the same request answered again and again). Close the connection after it.
    const auto closeIfBodyPending = [&state, bodyReady] {
      if (!bodyReady) {
        state.requestDrainAndClose();
      }
    };
    if (bodyReady) {
      if (_config.bodyReadTimeout.count() > 0) {
        if (state.waitingForBody) {
          assert(_connectionSweepState.pendingTimeoutConnections > 0U);
          --_connectionSweepState.pendingTimeoutConnections;
        }
        state.waitingForBody = false;
        state.bodyLastActivityMs = ConnectionState::kInactiveRelativeMs;
      }
      const bool usePerConnectionBodyStorage = state.trailerLen != 0;
      if (!request._body.empty() && !maybeDecompressRequestBody(cnxIt, usePerConnectionBodyStorage)) {
        break;
      }
      state.installAggregatedBodyBridge();
    } else {
      if (_config.bodyReadTimeout.count() > 0) {
        if (!state.waitingForBody) {
          ++_connectionSweepState.pendingTimeoutConnections;
        }
        state.waitingForBody = true;
        state.bodyLastActivityMs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(state.lastActivity - state.headerStartTp).count());
      }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      if (routingResult.asyncRequestHandler() == nullptr) {
        break;
      }
#else
      break;
#endif
    }

    struct RequestFinalizationRAII {
      RequestFinalizationRAII(ConnectionState& state, std::size_t consumedBytes)
          : state(state), consumedBytes(consumedBytes) {}

      RequestFinalizationRAII(const RequestFinalizationRAII&) = delete;
      RequestFinalizationRAII(RequestFinalizationRAII&&) noexcept = delete;
      RequestFinalizationRAII& operator=(const RequestFinalizationRAII&) = delete;
      RequestFinalizationRAII& operator=(RequestFinalizationRAII&&) noexcept = delete;

      ~RequestFinalizationRAII() { state.inBuffer.erase_front(consumedBytes); }

      ConnectionState& state;
      std::size_t consumedBytes;
    };

    RequestFinalizationRAII requestFinalizationRAII(state, consumedBytes);

    // Handle OPTIONS and TRACE per RFC 7231 §4.3
    // processSpecialMethods may insert an upstream connection and updates cnxIt if insertion invalidates it.
    // However, no need to update state because the pointer stays valid.
    const auto action = processSpecialMethods(cnxIt, consumedBytes, pCorsPolicy);
    if (action == LoopAction::SwitchProtocol) {
      return state.isAnyCloseRequested();
    }

    if (action == LoopAction::Continue) {
      closeIfBodyPending();
      continue;
    }
    if (action == LoopAction::Break) {
      break;
    }

    request.finalizeBeforeHandlerCall(routingResult.pathParams());

    const bool isStreaming = routingResult.streamingHandler() != nullptr && request.version() == http::HTTP_1_1;

    auto responseMiddlewareRange = routingResult.postMiddlewareRange();

    auto sendResponse = [this, isStreaming, responseMiddlewareRange, cnxIt, &state, pCorsPolicy](HttpResponse&& resp) {
      ApplyResponseMiddleware(state.request, resp, responseMiddlewareRange, _router.globalResponseMiddleware(),
                              _telemetry, isStreaming, _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), pCorsPolicy);
    };

    auto shortCircuitedResponse =
        RunRequestMiddleware(request, _router.globalRequestMiddleware(), routingResult.preMiddlewareRange(), _telemetry,
                             isStreaming, _callbacks.middlewareMetrics);

    if (shortCircuitedResponse.has_value()) {
      sendResponse(std::move(*shortCircuitedResponse));
      closeIfBodyPending();
      continue;
    }

    auto corsRejected = [pCorsPolicy, &request, &sendResponse] {
      if (pCorsPolicy != nullptr && pCorsPolicy->wouldApply(request) == CorsPolicy::ApplyStatus::OriginDenied) {
        sendResponse(request.makeResponse(http::StatusCodeForbidden, "Forbidden by CORS policy"));
        return true;
      }
      return false;
    };

    if (isStreaming) {
      // Invoke a registered streaming handler. Returns true if the connection should be closed after handling the
      // request (either because the client requested it or keep-alive limits reached). The HttpRequestView is non-const
      // because we may reuse shared response finalization paths (e.g. emitting a 406 early) that expect to mutate
      // transient fields (target normalization already complete at this point).

      if (corsRejected()) {
        continue;
      }

      bool wantClose = request.wantClose();

      // Create the protocol-specific transport backend and the protocol-agnostic writer
      Http1WriterTransport transport(*this, cnxFd, wantClose, pCorsPolicy, responseMiddlewareRange);
      HttpMessage::Options opts;
      if (_config.addTrailerHeader) {
        opts.addTrailerHeader();
      }
      if (request.method() == http::Method::HEAD) {
        opts.setHeadMethod();
      }
      opts.setPrepared();

      HttpResponseWriter writer(transport, request, request.responsePossibleEncoding(), _config.compression,
                                _compressionState, _config.globalHeaders.fullStringWithLastSep(), opts);

      try {
        (*routingResult.streamingHandler())(request, writer);
      } catch (const std::exception& ex) {
        log::error("Exception in streaming handler: {}", ex.what());
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError, ex.what()));
        continue;
      } catch (...) {
        log::error("Unknown exception in streaming handler");
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError, "Unknown error"));
        continue;
      }
      if (!writer.finished()) {
        writer.end();
      }

      const auto httpWriterStatusCode = writer.status();

      if (_callbacks.metrics || _accessLog) {
        emitRequestMetrics(request, httpWriterStatusCode, request._body.size(), state.requestsServed > 1);
      }

      request.end(httpWriterStatusCode);

      assert(request.version() == http::HTTP_1_1);
      if (!_config.enableKeepAlive || wantClose || state.requestsServed + 1 >= _config.maxRequestsPerConnection ||
          state.isAnyCloseRequested() || _lifecycle.isDraining() || _lifecycle.isStopping()) {
        state.requestDrainAndClose();
        break;
      }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    } else if (routingResult.asyncRequestHandler() != nullptr) {
      if (corsRejected()) {
        closeIfBodyPending();
        continue;
      }

      if (dispatchAsyncHandler(cnxIt, routingResult.sharedAsyncRequestHandler(), bodyReady, isChunked, consumedBytes,
                               pCorsPolicy, responseMiddlewareRange, routingResult.pathConfig().maxBodyBytes)) {
        // The request bytes are consumed by the async handler completion (already done if it completed right away).
        requestFinalizationRAII.consumedBytes = 0;
        if (state.pAsyncState()->active) {
          return state.isAnyCloseRequested() || PauseReadingDuringAsyncHandler(state);
        }
        // Completed synchronously: serve the next pipelined request.
        continue;
      }
#endif
    } else if (routingResult.requestHandler() != nullptr) {
      if (corsRejected()) {
        continue;
      }

      // normal handler
      try {
        sendResponse((*routingResult.requestHandler())(request));
      } catch (const std::exception& ex) {
        log::error("Exception in path handler: {}", ex.what());
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError, ex.what()));
      } catch (...) {
        log::error("Unknown exception in path handler");
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError, "Unknown error"));
      }
    } else if (routingResult.redirectSlashMode() != Router::RoutingResult::RedirectSlashMode::None) {
      // Emit 301 redirect to canonical form.
      // The path is percent-decoded: re-encode it so that the Location is a valid URI reference. Decoded control
      // characters (CR, LF...) would otherwise make the header value invalid and throw outside of any handler.
      static constexpr std::string_view kRedirecting = "Redirecting";
      const std::string_view reqPath = request.path();
      RawChars& location = _sharedBuffers.buf;
      location.clear();
      if (routingResult.redirectSlashMode() == Router::RoutingResult::RedirectSlashMode::AddSlash) {
        http::AppendUrlEncodedPath(location, reqPath);
        location.push_back('/');
      } else {
        http::AppendUrlEncodedPath(location, reqPath.substr(0, reqPath.size() - 1));
      }
      const std::size_t additionalCapacity =
          HttpResponse::BodySize(kRedirecting.size()) + http::HeaderSize(http::Location.size(), location.size());
      auto resp = request.makeResponse(additionalCapacity, http::StatusCodeMovedPermanently);
      resp.headerAddLine(http::Location, std::string_view(location));

      resp.body(kRedirecting);

      sendResponse(std::move(resp));
    } else if (routingResult.methodNotAllowed()) {
      sendResponse(request.makeResponse(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed));
    } else {
      sendResponse(request.makeResponse(http::StatusCodeNotFound));
    }

  } while (!state.isAnyCloseRequested());

  return state.isAnyCloseRequested();
}

bool SingleHttpServer::maybeDecompressRequestBody(ConnectionIt cnxIt, bool usePerConnectionBodyStorage) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  usePerConnectionBodyStorage = usePerConnectionBodyStorage || state.trailerLen != 0;

  RawChars& decompressedBuffer =
      usePerConnectionBodyStorage ? state.bodyAndTrailersBuffer : _sharedBuffers.decompressedBody;

  const auto res = HttpCodec::MaybeDecompressRequestBody(_decompressionState, _config.decompression, request,
                                                         decompressedBuffer, _sharedBuffers.buf);

  if (res.message != nullptr) {
    emitSimpleError(cnxIt, res.status, res.message);
    return false;
  }

  // Parse trailers if present
  if (state.trailerLen != 0) {
    auto* buf = decompressedBuffer.data();
    auto* end = buf + decompressedBuffer.size();
    [[maybe_unused]] const bool isSuccess = parseHeadersUnchecked(request._trailers, buf, end - state.trailerLen, end);
    // trailers should have been validated in decodeChunkedBody
    assert(isSuccess);
  }

  return true;
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
bool SingleHttpServer::PauseReadingDuringAsyncHandler(ConnectionState& state) noexcept {
  const auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr || !asyncState->holdsInput()) {
    return false;
  }
  // The input buffer holds the body (and the bytes) of the request of the async handler: it must neither grow nor move
  // until it completes. The next bytes are left in the socket (which also bounds them) and read once it completed,
  // see resumeInputAfterAsyncHandler().
  state.readPaused = true;
  return true;
}

bool SingleHttpServer::dispatchAsyncHandler(ConnectionIt cnxIt, const SharedAsyncRequestHandler& handler,
                                            bool bodyReady, bool isChunked, std::size_t consumedBytes,
                                            const CorsPolicy* pCorsPolicy,
                                            std::span<const ResponseMiddleware> responseMiddleware,
                                            std::size_t perRouteMaxBodyBytes) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  RequestTask<HttpResponse> task = (*handler)(request);

  if (!task.valid()) {
    static constexpr std::string_view kMessage = "Async handler inactive";
    log::error("Async path handler returned an invalid RequestTask for path {}", request.path());
    if (bodyReady) {
      HttpResponse resp(http::StatusCodeInternalServerError, kMessage);
      ApplyResponseMiddleware(request, resp, responseMiddleware, _router.globalResponseMiddleware(), _telemetry, false,
                              _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), pCorsPolicy);
    } else {
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, kMessage);
    }

    return false;
  }

  auto handle = task.release();
  assert(handle);

  auto& asyncState = state.ensureAsyncState(_connections.asyncHandlerStatePool());
  const std::string_view bodyView = state.request._body;
  bool usesSharedDecompressedBody = false;
  if (bodyReady && !bodyView.empty()) {
    const char* sharedBeg = _sharedBuffers.decompressedBody.data();
    const char* sharedEnd = sharedBeg + _sharedBuffers.decompressedBody.size();
    const char* bodyBeg = bodyView.data();
    const char* bodyEnd = bodyBeg + bodyView.size();

    usesSharedDecompressedBody = sharedBeg <= bodyBeg && bodyEnd <= sharedEnd;
  }

  // The coroutine may use the captures of the handler: keep it alive even if the router replaces it meanwhile.
  asyncState.handlerKeepAlive = handler;
  asyncState.active = true;
  asyncState.handle = std::move(handle);
  asyncState.awaitReason = AsyncHandlerState::AwaitReason::None;
  asyncState.needsBody = !bodyReady;
  asyncState.usesSharedDecompressedBody = usesSharedDecompressedBody;
  asyncState.isChunked = isChunked;
  asyncState.routeMayHaveChanged = false;
  asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
  asyncState.corsPolicy = pCorsPolicy;
  asyncState.responseMiddleware = responseMiddleware.data();
  asyncState.responseMiddlewareCount = static_cast<uint32_t>(responseMiddleware.size());
  asyncState.maxBodyBytes = perRouteMaxBodyBytes;
  asyncState.pendingResponse = {};

  // The request is not parsed again from now on: the expectations of the next one must be answered.
  state.expectationAnswered = false;

  // Keep header storage stable while async work runs so header string_views stay valid
  state.request.pinHeadStorage(state, _connections.asyncHandlerStatePool());

  // Install the postCallback function for deferred work
  asyncState.postCallback = [this, fd = cnxIt->fd(), generation = state.generation](std::coroutine_handle<> handle,
                                                                                    std::function<void()> work) {
    postAsyncCallback(fd, generation, handle, std::move(work));
  };

  refreshKeepAliveDeadline(cnxIt);
  resumeAsyncHandler(cnxIt);
  if (asyncState.active) {
    // Completes later: the router may be updated meanwhile.
    asyncState.routeMayHaveChanged = true;
  }

  return true;
}

void SingleHttpServer::resumeAsyncHandler(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  if (!async.active || !async.handle) {
    return;
  }

  while (async.handle && !async.handle.done()) {
    async.awaitReason = AsyncHandlerState::AwaitReason::None;
    async.handle.resume();
    if (async.awaitReason != AsyncHandlerState::AwaitReason::None) {
      if (async.usesSharedDecompressedBody && !pinAsyncSharedBodyToConnectionStorage(state)) {
        state.requestDrainAndClose();
      }
      return;
    }
    // Suspended by an awaitable that does not need the event loop (std::suspend_always for instance): resume it.
  }

  if (async.handle && async.handle.done()) {
    onAsyncHandlerCompleted(cnxIt);
  }
}

void SingleHttpServer::AbandonAsyncHandler(AsyncHandlerState& async) noexcept {
  if (async.isAwaitingCallback()) {
    // The deferred work may still use the coroutine frame and the request: they are released when it completes.
    async.active = false;
    async.pendingResponse.reset();
  } else {
    async.clear();
  }
}

void SingleHttpServer::handleAsyncBodyProgress(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  assert(async.active);

  if (async.needsBody) {
    std::size_t consumedBytes = 0;
    // 100 Continue (if expected) was sent when the request was dispatched.
    const BodyDecodeStatus status = decodeBodyIfReady(cnxIt, async.isChunked, false, async.maxBodyBytes, consumedBytes);
    if (status == BodyDecodeStatus::Error) {
      AbandonAsyncHandler(async);
      return;
    }
    if (status == BodyDecodeStatus::NeedMore) {
      return;
    }

    async.needsBody = false;
    async.consumedBytes = consumedBytes;
    if (!state.request._body.empty() && !maybeDecompressRequestBody(cnxIt, true)) {
      AbandonAsyncHandler(async);
      return;
    }
    state.installAggregatedBodyBridge();

    if (_config.bodyReadTimeout.count() > 0) {
      if (state.waitingForBody) {
        assert(_connectionSweepState.pendingTimeoutConnections > 0U);
        --_connectionSweepState.pendingTimeoutConnections;
      }
      state.waitingForBody = false;
      state.bodyLastActivityMs = ConnectionState::kInactiveRelativeMs;
    }

    if (async.awaitReason == AsyncHandlerState::AwaitReason::WaitingForBody) {
      async.awaitReason = AsyncHandlerState::AwaitReason::None;
      resumeAsyncHandler(cnxIt);
      return;
    }
  }

  if (async.pendingResponse.has_value()) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

bool SingleHttpServer::pinAsyncSharedBodyToConnectionStorage(ConnectionState& state) const {
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return true;
  }
  auto& async = *asyncState;
  if (!async.usesSharedDecompressedBody) {
    return true;
  }

  const std::string_view body = state.request._body;

  // Async shared-body pinning expects request body to reference shared decompressed storage
  assert(body.empty() || (_sharedBuffers.decompressedBody.data() <= body.data() &&
                          body.data() + body.size() <=
                              _sharedBuffers.decompressedBody.data() + _sharedBuffers.decompressedBody.size()));

  state.bodyAndTrailersBuffer.assign(body.data(), body.size());
  state.request._body = std::string_view(state.bodyAndTrailersBuffer.data(), body.size());

  if (state.request._pBodyAccessBridge != nullptr) {
    state.bodyStreamContext.body = state.request._body;
  }

  async.usesSharedDecompressedBody = false;
  return true;
}

void SingleHttpServer::onAsyncHandlerCompleted(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return;
  }
  auto& async = *asyncState;
  if (!async.handle) {
    return;
  }

  auto rawHandle = async.handle.release();
  auto typedHandle = std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(rawHandle.address());
  bool fromException = false;
  HttpResponse resp(HttpMessage::Check::No);  // do not allocate memory yet
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
  async.handlerKeepAlive.reset();
  async.pendingResponse = std::move(resp);

  if (async.needsBody) {
    if (fromException) {
      // Body will still be drained before response is flushed; nothing else to do here.
    }
  } else {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void SingleHttpServer::tryFlushPendingAsyncResponse(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return;
  }
  auto& async = *asyncState;

  assert(!async.needsBody);
  assert(async.pendingResponse.has_value());

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  const CorsPolicy* pCorsPolicy = async.corsPolicy;
  if (async.routeMayHaveChanged) {
    // The router may have been updated since the handler was dispatched, possibly destroying the metadata of its route:
    // look it up again.
    const Router::RoutingResult routingResult = _router.match(state.request.method(), state.request.path());
    middlewareSpan = routingResult.postMiddlewareRange();
    pCorsPolicy = routingResult.corsPolicy();
  }
  ApplyResponseMiddleware(state.request, *async.pendingResponse, middlewareSpan, _router.globalResponseMiddleware(),
                          _telemetry, false, _callbacks.middlewareMetrics);
  finalizeAndSendResponseForHttp1(cnxIt, std::move(*async.pendingResponse), pCorsPolicy);
  state.inBuffer.erase_front(async.consumedBytes);
  async.clear();
  state.lastActivity = std::chrono::steady_clock::now();
  refreshKeepAliveDeadline(cnxIt);
}

void SingleHttpServer::resumeInputAfterAsyncHandler(NativeHandle fd) {
  auto cnxIt = _connections.iterator(fd);
  ConnectionState* pState = _connections.pConnectionState(cnxIt);
  // Serve the requests pipelined behind the completed one: they were already read, no event will report them.
  if (!pState->inBuffer.empty()) {
    (void)processHttp1Requests(cnxIt);
    cnxIt = _connections.iterator(fd);
    pState = _connections.pConnectionState(cnxIt);
  }
  // Then read the bytes left in the socket while reading was paused: edge-triggered polling does not report them again.
  if (pState->readPaused && !pState->isAnyCloseRequested()) {
    pState->readPaused = false;
    if (handleReadableClient(cnxIt, false) == CloseStatus::Close) {
      const auto finalIt = _connections.iterator(fd);
      if (IsValid(_connections, finalIt)) {
        closeConnection(finalIt);
      }
    }
  }
}
#endif

void SingleHttpServer::emitRequestMetrics(const HttpRequestView& request, http::StatusCode status, std::size_t bytesIn,
                                          bool reusedConnection) {
  std::string_view clientIp = "-";

  if (_config.accessLog.useForwardedFor) {
    const auto xff = request.headerValueOrEmpty("x-forwarded-for");
    if (!xff.empty()) {
      // Use the first (leftmost) IP from X-Forwarded-For
      const auto comma = xff.find(',');
      clientIp = TrimOws(xff.substr(0, comma));
    }
  }

  if (clientIp == "-") {
    const auto requestClientIp = request.clientAddress();
    if (!requestClientIp.empty()) {
      clientIp = requestClientIp;
    }
  }

  RequestMetrics metrics{
      status,
      request.method(),
      request.version(),
      reusedConnection,
      request.path(),
      clientIp,
      request.headerValueOrEmpty("user-agent"),
      bytesIn,
      0,
      _connections.now - request.reqStart(),
  };

  if (_accessLog) {
    _accessLog.log(metrics);
  }

  if (_callbacks.metrics) {
    _callbacks.metrics(metrics);
  }
}

void SingleHttpServer::eventLoop() {
  // Apply any pending config updates posted from other threads.
  applyPendingUpdates();

  // Poll for events
  const auto events = _eventLoop.poll();

  // Update cached now time
  const auto now = std::chrono::steady_clock::now();
  _connections.now = now;
  _dateHeader.refreshIfNeeded(now);

  // Publish the loop heartbeat (see internal::Lifecycle::loopHeartbeat): a single relaxed store of the 'now' we just
  // read. A dedicated probe listener (MultiHttpServer, see BuiltinProbesConfig::dedicatedPort) reads it to tell a loop
  // wedged inside a request handler from an idle or progressing one - without any bookkeeping on the handler hot path.
  _lifecycle.loopHeartbeat(now);

  bool maintenanceTick = false;

  if (events.data() == nullptr) [[unlikely]] {
    _telemetry.counterAdd("aeronet.events.errors", 1);
    _lifecycle.exchangeStopping();
  } else if (!events.empty()) {
    for (auto event : events) {
      const auto fd = event.fd;
      if (fd == _listenSocket.fd()) {
        // Always attempt to accept new connections when the listener is signaled.
        // The lifecycle controls higher-level acceptance semantics; accepting
        // here is safe and allows probes to connect during drain.
        acceptNewConnections();
      } else if (fd == _lifecycle.wakeupFd.fd()) {
        _lifecycle.wakeupFd.read();
        // When stop() is called from another thread it sets the state to Stopping
        // and sends a wakeup.  Close the listener immediately so no new connections
        // are accepted while we wait for the maintenance tick to clean up.
        if (_lifecycle.isStopping()) {
          closeListener();
          maintenanceTick = true;
        }
      } else if (fd == _maintenanceTimer.fd()) {
        _maintenanceTimer.drain();
        maintenanceTick = true;
      } else {
        const auto bmp = event.eventBmp;
        const auto cnxIt = _connections.iterator(fd);
        if (!IsValid(_connections, cnxIt)) [[unlikely]] {
          log::warn("fd # {} not found (stale epoll event or race)", fd);
          // Remove stale entry from the event-loop poller so it doesn't keep firing.
          // Safe to call here because we are inside the event-loop thread.
          _eventLoop.del(fd);
          continue;
        }

        ConnectionState& eventState = _connections.connectionState(cnxIt);
        eventState.lastActivity = now;
        refreshKeepAliveDeadline(cnxIt);

        CloseStatus closeStatus = CloseStatus::Keep;
        if ((bmp & EventOut) != 0) {
          closeStatus = handleWritableClient(cnxIt);
        }
        // EPOLLERR/EPOLLHUP/EPOLLRDHUP can be delivered without EPOLLIN.
        // Treat them as a read trigger so we promptly observe EOF/errors and close.
        if ((bmp & (EventIn | EventErr | EventHup | EventRdHup)) != 0) {
          closeStatus =
              std::max(handleReadableClient(cnxIt, (bmp & (EventErr | EventHup | EventRdHup)) == 0), closeStatus);
        }
        if (closeStatus == CloseStatus::Close) {
          // A handler (e.g. shutdownTunnelPeerWrite) may have already recycled
          // this connection via a nested closeConnection call. Guard against that.
          const auto finalIt = _connections.iterator(fd);
          if (IsValid(_connections, finalIt)) {
            closeConnection(finalIt);
          }
        }
      }
    }
    // Request handlers run from the read path and may have blocked for longer than keepAliveTimeout.
    restartKeepAliveIdleWindows(events, now);
    _telemetry.counterAdd("aeronet.events.processed", static_cast<uint64_t>(events.size()));
  } else {
    // timeout / error::kInterrupted (treated as timeout). Retry pending writes to handle edge-triggered epoll timing
    // issues. With EPOLLET, if a socket becomes writable after sendfile() returns EAGAIN but before
    // epoll_ctl(EPOLL_CTL_MOD), we miss the edge. Periodic retries ensure we eventually resume.
    maintenanceTick = true;
  }

  // Re-process connections deferred by the per-event fairness cap.
  // Edge-triggered polling (EPOLLET / EV_CLEAR) only fires on state transitions;
  // a connection that still had TCP data after hitting the cap won't generate a new
  // read event, so we must re-read it here before waiting for events again.
  //
  // Swap into a local so that handleReadableClient can safely push new deferrals
  // into the (now-empty) member vector without invalidating our iteration and
  // without losing them to a blanket clear().
  if (!_pendingReadFds.empty()) {
    decltype(_pendingReadFds) batch;
    batch.swap(_pendingReadFds);
    for (const NativeHandle pendingFd : batch) {
      const auto pendingIt = _connections.iterator(pendingFd);
      if (!IsValid(_connections, pendingIt)) {
        continue;
      }
      const CloseStatus cs = handleReadableClient(pendingIt, false);
      if (cs == CloseStatus::Close) {
        const auto finalIt = _connections.iterator(pendingFd);
        if (IsValid(_connections, finalIt)) {
          closeConnection(finalIt);
        }
      } else {
        restartKeepAliveIdleWindow(pendingFd);
      }
    }
  }

  // Under high load epoll_wait may return immediately and never hit the timeout path.
  // We still need periodic maintenance for timeouts and edge-triggered sendfile progress.
  // On Windows and macOS the TimerFd does not fire real events (armPeriodic is a no-op);
  // maintenance runs only when poll returns empty (timeout). Under sustained event activity
  // the poll may never timeout, starving maintenance. Fall back to a time-based check.
#ifndef AERONET_LINUX
  if (!maintenanceTick && now - _lastMaintenanceTp >= _config.pollInterval) {
    maintenanceTick = true;
  }
#endif
  if (maintenanceTick) {
#ifndef AERONET_LINUX
    _lastMaintenanceTp = now;
#endif
    const auto nbActiveConnections = _connections.size();

    _telemetry.gauge("aeronet.connections.active_count", static_cast<int64_t>(nbActiveConnections));
    _telemetry.gauge("aeronet.events.capacity_current_count", static_cast<int64_t>(_eventLoop.capacity()));

    sweepIdleConnections();

    if (_accessLog) {
      _accessLog.flush();
    }

    if (_lifecycle.isStopping() || (_lifecycle.isDraining() && nbActiveConnections == 0)) {
      closeListener();
      closeAllConnections();
      _lifecycle.reset();
      if (!isInMultiHttpServer()) {
        log::info("Server stopped");
      }
    } else if (_lifecycle.isDraining()) {
      if (_lifecycle.drainDeadlineReached(now)) {
        log::warn("Drain deadline reached with {} active connection(s); forcing close", nbActiveConnections);
        closeListener();
        closeAllConnections();
        _lifecycle.reset();
        log::info("Server drained after deadline");
      }
    } else if (SignalHandler::IsStopRequested()) {
      beginDrain(SignalHandler::GetMaxDrainPeriod());
    }

    // Also shrink per-thread scratch buffers used during decompression / header parsing.
    _sharedBuffers.shrink_to_fit();
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
#ifndef AERONET_WINDOWS
    // On POSIX, epoll_ctl(DEL) / kevent(EV_DELETE) are kernel-level thread-safe,
    // so removing the fd from the event loop while the poll thread is blocked is fine.
    _eventLoop.del(_listenSocket.fd());
#endif
    // On Windows, EventLoop::del() mutates the user-space WSAPoll array.
    // Calling it from a different thread while WSAPoll() is blocked is a data race.
    // Just close the socket; the next WSAPoll() iteration will report POLLNVAL for
    // the stale entry, which the event loop handles gracefully (fd won't match
    // listen/wakeup/timer and the connection lookup returns not-found — logged and skipped).
    _listenSocket.close();
    // Trigger wakeup to break any blocking poll quickly.
    _lifecycle.wakeupFd.send();
  }
}

void SingleHttpServer::closeAllConnections() {
#ifdef AERONET_WINDOWS
  while (!_connections.empty()) {
    closeConnection(_connections.begin());
  }
#else
  for (auto cnxIt = _connections.begin(); cnxIt != _connections.end(); ++cnxIt) {
    if (*cnxIt) {
      closeConnection(cnxIt);
    }
  }
#endif
}

ServerStats SingleHttpServer::stats() const {
  ServerStats statsOut;
  statsOut.totalBytesQueued = _stats.totalBytesQueued.load();
  statsOut.totalBytesWrittenImmediate = _stats.totalBytesWrittenImmediate.load();
  statsOut.totalBytesWrittenFlush = _stats.totalBytesWrittenFlush.load();
  statsOut.deferredWriteEvents = _stats.deferredWriteEvents.load();
  statsOut.flushCycles = _stats.flushCycles.load();
  statsOut.epollModFailures = _stats.epollModFailures.load();
  statsOut.maxConnectionOutboundBuffer = _stats.maxConnectionOutboundBuffer.load();
  statsOut.totalRequestsServed = _stats.totalRequestsServed.load();
#ifdef AERONET_ENABLE_OPENSSL
  statsOut.tlsHandshakesSucceeded = _tls.metrics.handshakesSucceeded.load();
  statsOut.tlsHandshakesFull = _tls.metrics.handshakesFull.load();
  statsOut.tlsHandshakesResumed = _tls.metrics.handshakesResumed.load();
  statsOut.tlsHandshakesFailed = _tls.metrics.handshakesFailed.load();
  statsOut.tlsHandshakesRejectedConcurrency = _tls.metrics.handshakesRejectedConcurrency.load();
  statsOut.tlsHandshakesRejectedRateLimit = _tls.metrics.handshakesRejectedRateLimit.load();
  statsOut.tlsClientCertPresent = _tls.metrics.clientCertPresent.load();
  if (_tls.ctxHolder) {
    statsOut.tlsAlpnStrictMismatches = _tls.ctxHolder->alpnStrictMismatches();
  }
  // The maps are updated by the event loop on TLS handshakes.
  std::scoped_lock lock(_tls.metrics.mapsMutex.mutex);
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
  statsOut.tlsHandshakeDurationCount = _tls.metrics.handshakeDurationCount.load();
  statsOut.tlsHandshakeDurationTotalNs = _tls.metrics.handshakeDurationTotalNs.load();
  statsOut.tlsHandshakeDurationMaxNs = _tls.metrics.handshakeDurationMaxNs.load();
  statsOut.ktlsSendEnabledConnections = _tls.metrics.ktlsSendEnabledConnections.load();
  statsOut.ktlsSendEnableFallbacks = _tls.metrics.ktlsSendEnableFallbacks.load();
  statsOut.ktlsSendForcedShutdowns = _tls.metrics.ktlsSendForcedShutdowns.load();
  statsOut.ktlsSendBytes = _tls.metrics.ktlsSendBytes.load();
#endif
  return statsOut;
}

void SingleHttpServer::emitSimpleError(ConnectionIt cnxIt, http::StatusCode statusCode, std::string_view body) {
  queueData(cnxIt, HttpMessageData(BuildSimpleError(statusCode, _config.globalHeaders, body, _dateHeader.data())));

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

  ConnectionState& state = _connections.connectionState(cnxIt);

  state.requestDrainAndClose();
  state.request.end(statusCode);
}

void SingleHttpServer::emitHttpsRedirect(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  // Build the absolute https:// target into a scratch buffer, re-encoding the (decoded) path and query so the
  // result is always a valid URL / header value. HttpResponse::location() copies it out, so the scratch buffer
  // can be reused freely afterwards.
  RawChars& urlBuf = _sharedBuffers.buf;
  if (!http::AppendHttpsAuthority(urlBuf, request.headerValueOrEmpty(http::Host), _config.httpsRedirect.targetPort)) {
    // No (or empty) Host header: an absolute https URL cannot be constructed.
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Missing Host header for HTTPS redirect");
    return;
  }
  http::AppendUrlEncodedPath(urlBuf, request.path());
  char querySep = '?';
  for (const auto& [key, value] : request.queryParamsRange()) {
    http::AppendUrlEncodedQueryParam(urlBuf, querySep, key, value);
    querySep = '&';
  }

  const http::StatusCode statusCode = _config.httpsRedirect.statusCode;
  HttpResponse resp(statusCode);
  resp.location(std::string_view(urlBuf));

  clearRequestDeadline(state);
  state.headerStartTp = {};

  // Force connection close: the redirected client reconnects over TLS, and closing avoids any
  // request-body framing concerns since the body is intentionally not consumed.
  HttpResponse::Options opts;
  opts.setClose();
  if (request.method() == http::Method::HEAD) {
    opts.setHeadMethod();
  }

  queueData(cnxIt, resp.finalizeForHttp1(_dateHeader.data(), request.version(), opts, &_config.globalHeaders,
                                         _config.minCapturedBodySize));

  state.requestDrainAndClose();

  if (_callbacks.metrics || _accessLog) {
    emitRequestMetrics(request, statusCode, request._body.size(), state.requestsServed > 0);
  }
  request.end(statusCode);
}

bool SingleHttpServer::handleExpectHeader(ConnectionIt cnxIt, std::string_view expectHeader,
                                          const CorsPolicy* pCorsPolicy, bool& found100Continue) {
  HttpRequestView& request = _connections.connectionState(cnxIt).request;
  const std::size_t headerEnd = request.headSpanSize();
  // Parse comma-separated tokens (trim OWS). Case-insensitive comparison for 100-continue.
  // headerEnd = offset from connection buffer start to end of headers
  for (std::size_t pos = 0; pos < expectHeader.size();) {
    const auto commaPos = expectHeader.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? expectHeader.size() : commaPos;

    const auto token = TrimOws(expectHeader.substr(pos, tokenEnd - pos));
    pos = (commaPos == std::string_view::npos) ? expectHeader.size() : commaPos + 1;

    if (token.empty()) {
      continue;
    }
    if (CaseInsensitiveEqual(token, http::h100_continue)) {
      // Note presence of 100-continue; we'll use this to trigger interim 100
      found100Continue = true;
      // built-in behaviour; leave actual 100 emission to body-decoding logic
      continue;
    }
    if (!_callbacks.expectation) {
      // No handler and not 100-continue -> RFC says respond 417
      emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, {});
      return true;
    }
    try {
      auto expectationResult = _callbacks.expectation(request, token);
      switch (expectationResult.kind) {
        case ExpectationResultKind::Reject:
          emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, {});
          return true;
        case ExpectationResultKind::Interim: {
          // Emit an interim response immediately. Common case: 102 "Processing"
          const auto status = expectationResult.interimStatus;
          // Validate that the handler returned an informational 1xx status.
          if (status < 100U || status >= 200U) {
            emitSimpleError(cnxIt, http::StatusCodeInternalServerError, "Invalid interim status (must be 1xx)");
            return true;
          }

          switch (status) {
            case 100:
              queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_100_CONTINUE}));
              break;
            case 102:
              queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_102_PROCESSING}));
              break;
            default: {
              static constexpr std::string_view kHttpResponseLinePrefix = "HTTP/1.1 ";

              RawChars buf(kHttpResponseLinePrefix.size() + 3U + http::DoubleCRLF.size());
              buf.setSize(buf.capacity());

              char* insertPtr = AppendFixed<kHttpResponseLinePrefix>(buf.data());
              insertPtr = writeStatusCode(insertPtr, status);
              CopyFixed<http::DoubleCRLF>(insertPtr);

              queueData(cnxIt, HttpMessageData(std::move(buf)));
              break;
            }
          }

          break;
        }
        case ExpectationResultKind::FinalResponse:
          // Send the provided final response immediately and skip body processing.
          finalizeAndSendResponseForHttp1(cnxIt, std::move(expectationResult.finalResponse), pCorsPolicy);
          _connections.connectionState(cnxIt).inBuffer.erase_front(headerEnd);
          return true;
        default:
          assert(expectationResult.kind == ExpectationResultKind::Continue);
          break;
      }
    } catch (const std::exception& ex) {
      log::error("Exception in ExpectationHandler: {}", ex.what());
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, {});
      return true;
    } catch (...) {
      log::error("Unknown exception in ExpectationHandler");
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, {});
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
  bool needsClamp = false;

  if (_updates.hasConfig.load(std::memory_order_acquire)) {
#ifdef AERONET_ENABLE_OPENSSL
    // Capture TLS config before updates to detect changes
    const TLSConfig tlsBefore = _config.tls;
#endif

    AccessLogConfig accessLogConfigBefore = _config.accessLog;

    ApplyPendingUpdates(_updates.lock, _updates.config, _updates.hasConfig, _config, "config");

    // Reinitialize components dependent on config values.
    _compressionState.selector = EncodingSelector(_config.compression);
    _eventLoop.updatePollTimeoutPolicy(MakePollTimeoutPolicy(_config));
    updateMaintenanceTimer();
    rebuildKeepAliveDeadlines();
    registerBuiltInProbes();
    needsClamp = true;

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
    if (_config.accessLog != accessLogConfigBefore) {
      _accessLog = AccessLogWriter(_config.accessLog);
    }
  }
  if (_updates.hasRouter.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updates.lock, _updates.router, _updates.hasRouter, _router, "router");
    needsClamp = true;
  }
  if (_hasCallbacksUpdates.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updates.lock, _callbacksUpdates, _hasCallbacksUpdates, _callbacks, "callbacks");
  }

  if (needsClamp) {
    _router.clampConfigs(_config.maxHeaderBytes, _config.maxBodyBytes);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Process async callbacks posted from background threads
  if (_updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
    processAsyncCallbacks();
  }
#endif
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void SingleHttpServer::processAsyncCallbacks() {
  vector<internal::PendingUpdates::AsyncCallback> callbacks;
  {
    std::scoped_lock lock(_updates.lock);
    callbacks.swap(_updates.asyncCallbacks);
    _updates.hasAsyncCallbacks.store(false, std::memory_order_release);
  }

  for (auto& cb : callbacks) {
    try {
      auto it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it == _connections.end()) {
        // The connection was closed while the work was running: its coroutine was kept for it, release it now.
        _connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _config.maxCachedConnections);
        continue;
      }

      // Execute any pre-resume work
      if (cb.work) {
        try {
          cb.work();
        } catch (const std::exception& ex) {
          log::error("Exception in async callback work: {}", ex.what());
        } catch (...) {
          log::error("Unknown exception in async callback work");
        }
        it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
        if (it == _connections.end()) {
          _connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _config.maxCachedConnections);
          continue;
        }
      }

      ConnectionState& state = _connections.connectionState(it);
#ifdef AERONET_ENABLE_HTTP2
      // For HTTP/2 connections, delegate to the protocol handler which tracks per-stream async state.
      if (state.protocol == ProtocolType::Http2 && state.protocolHandler != nullptr) {
        auto* pH2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
        if (pH2Handler->resumeAsyncTaskByHandle(cb.handle)) {
          // Flush any pending output generated by the completed async handler
          if (pH2Handler->hasPendingOutput()) {
            flushOutbound(it);
          }
          // Deferred work that outlived keepAliveTimeout leaves a stale idle deadline behind, exactly as
          // a slow synchronous handler does (HTTP/1 goes through tryFlushPendingAsyncResponse instead).
          restartKeepAliveIdleWindow(cb.connectionFd);
        }
        continue;
      }
#endif
      // The coroutine frame is kept until its deferred work completes: a coroutine created meanwhile cannot have the
      // same address, the handle identifies it.
      auto* asyncState = state.pAsyncState();
      if (asyncState != nullptr && asyncState->handle == cb.handle) {
        if (asyncState->active) {
          asyncState->awaitReason = AsyncHandlerState::AwaitReason::None;
          resumeAsyncHandler(it);
          if (!asyncState->active) {
            resumeInputAfterAsyncHandler(cb.connectionFd);
          }
        } else {
          // Abandoned (its connection is closing) while the work was running.
          asyncState->clear();
        }
      }
      // Close connection immediately if response was sent and drain-and-close is pending.
      // On platforms without a real timer fd (Windows, macOS), sweep maintenance may not
      // run often enough, causing Connection: close requests to linger.
      it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it != _connections.end() && _connections.connectionState(it).canCloseConnectionForDrain()) {
        closeConnection(it);
      }
    } catch (const std::exception& ex) {
      log::error("Exception processing async callback for fd # {}: {}", static_cast<uintptr_t>(cb.connectionFd),
                 ex.what());
    } catch (...) {
      log::error("Unknown exception processing async callback for fd # {}", static_cast<uintptr_t>(cb.connectionFd));
    }
  }
}

void SingleHttpServer::postAsyncCallback(NativeHandle connectionFd, uint32_t connectionGeneration,
                                         std::coroutine_handle<> handle, std::function<void()> work) {
  std::scoped_lock lock(_updates.lock);
  _updates.asyncCallbacks.emplace_back(connectionFd, connectionGeneration, handle, std::move(work));
  _updates.hasAsyncCallbacks.store(true, std::memory_order_release);
  // Still under the lock: once it is released, the server may be destroyed (see waitForOrphanedAsyncWork()).
  _lifecycle.wakeupFd.send();
}

void SingleHttpServer::waitForOrphanedAsyncWork() {
  if (!_lifecycle.isIdle()) {
    // Still run by another thread, which owns the connections.
    return;
  }
  // Deferred work still running uses the coroutine frame and the request kept for it, and completes by posting to this
  // server: wait for it.
  static constexpr auto kLogPeriod = std::chrono::seconds{5};
  auto nextLog = std::chrono::steady_clock::now() + kLogPeriod;
  while (_connections.hasOrphanedConnectionStates()) {
    if (_updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
      processAsyncCallbacks();
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      log::warn("Waiting for the deferred work still running for {} closed connection(s)",
                _connections.nbOrphanedConnectionStates());
      nextLog = now + kLogPeriod;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}
#endif

#ifdef AERONET_ENABLE_HTTP2

/// Concrete ITunnelBridge implementation that delegates tunnel operations
/// to SingleHttpServer via shared helpers. Captures the server reference
/// and the client fd that owns the HTTP/2 connection.
class H2TunnelBridge final : public ITunnelBridge {
 public:
  H2TunnelBridge(SingleHttpServer& server, NativeHandle clientFd) noexcept : _server(server), _clientFd(clientFd) {}

  NativeHandle setupTunnel(uint32_t streamId, std::string_view host, uint16_t port) override {
    return _server.setupH2Tunnel(_clientFd, streamId, host, port);
  }

  void writeTunnel(NativeHandle upstreamFd, std::span<const std::byte> data) override {
    auto upIt = _server._connections.iterator(upstreamFd);
    if (!IsValid(_server._connections, upIt)) {
      return;
    }
    if (!_server.forwardTunnelData(upIt, std::string_view(reinterpret_cast<const char*>(data.data()), data.size())))
        [[unlikely]] {
      _server.closeConnection(upIt);
    }
  }

  void shutdownTunnelWrite(NativeHandle upstreamFd) override {
    auto upIt = _server._connections.iterator(upstreamFd);
    if (IsValid(_server._connections, upIt)) {
      _server.shutdownTunnelPeerWrite(upIt);
    }
  }

  void closeTunnel(NativeHandle upstreamFd) override {
    auto upIt = _server._connections.iterator(upstreamFd);
    if (!IsValid(_server._connections, upIt)) {
      return;
    }
    ConnectionState& state = _server._connections.connectionState(upIt);
    // Clear peerFd so closeConnection won't try to tear down the client HTTP/2 connection.
    state.peerFd = kInvalidHandle;
    state.peerStreamId = 0;
    _server.closeConnection(upIt);
  }

  void onTunnelWindowUpdate(NativeHandle upstreamFd) override {
    auto upIt = _server._connections.iterator(upstreamFd);
    if (IsValid(_server._connections, upIt)) {
      // If we have buffered data from upstream, try to inject it now that the window opened.
      ConnectionState& state = _server._connections.connectionState(upIt);
      if (!state.inBuffer.empty() || state.eofReceived) {
        auto closeStatus = _server.handleInH2Tunneling(upIt);
        if (closeStatus == SingleHttpServer::CloseStatus::Close) {
          _server.closeConnection(_server._connections.iterator(upstreamFd));
        }
      }
    }
  }

 private:
  SingleHttpServer& _server;
  NativeHandle _clientFd;
};

void SingleHttpServer::installH2TunnelBridge(NativeHandle clientFd, ConnectionState& state) {
  auto* h2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
  state.tunnelBridge = std::make_unique<H2TunnelBridge>(*this, clientFd);
  h2Handler->setTunnelBridge(state.tunnelBridge.get());

  // Install per-request completion callback for metrics, counters and tracing.
  h2Handler->setRequestCompletionCallback(
      [this, fd = clientFd](const HttpRequestView& request, http::StatusCode status) {
        auto& state = *_connections.pConnectionState(fd);
        ++state.requestsServed;
        ++_stats.totalRequestsServed;
        if (_callbacks.metrics || _accessLog) {
          emitRequestMetrics(request, status, request._body.size(), state.requestsServed > 1);
        }
      });

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Install async callback so HTTP/2 coroutines can post deferred work to the event loop.
  h2Handler->setAsyncPostCallback(
      [this, fd = clientFd, generation = state.generation](std::coroutine_handle<> handle, std::function<void()> work) {
        postAsyncCallback(fd, generation, handle, std::move(work));
      });
#endif
}

void SingleHttpServer::setupHttp2Connection(NativeHandle clientFd, TcpNoDelayMode tcpNoDelayMode,
                                            ConnectionState& state) {
  // Create HTTP/2 protocol handler with unified dispatcher
  // Pass sendServerPrefaceForTls=true: server must send SETTINGS immediately for TLS ALPN "h2"
  state.protocolHandler = http2::CreateHttp2ProtocolHandler(_config.http2, _router, _config, _compressionState,
                                                            _decompressionState, _telemetry, _sharedBuffers.buf, true,
                                                            _dateHeader.data(), state.clientAddress());
  ++_connectionSweepState.http2Connections;
  state.protocol = ProtocolType::Http2;

  // Install CONNECT tunnel bridge so the HTTP/2 handler can request TCP tunnel setup.
  installH2TunnelBridge(clientFd, state);

  if (tcpNoDelayMode == TcpNoDelayMode::Auto) {
    assert(state.tlsInfo.selectedAlpn() == http2::kAlpnH2);
    // Disable Nagle's algorithm for HTTP/2 connections by default to reduce latency.
    // The protocol handler may choose to re-enable it later if it determines it's beneficial.
    if (SetTcpNoDelay(clientFd)) [[likely]] {
      state.corkable = true;
    } else {
      const auto err = LastSystemError();
      log::error("setsockopt(TCP_NODELAY) failed for fd # {} err={}", clientFd, err);
      _telemetry.counterAdd("aeronet.connections.errors.tcp_nodelay_failed", 1UL);
    }
  }

  // The queued server preface is flushed by the caller through the protocol gather path.
}

NativeHandle SingleHttpServer::setupH2Tunnel(NativeHandle clientFd, uint32_t streamId, std::string_view host,
                                             uint16_t port) {
  const auto upstreamFd = setupTunnelConnection(clientFd, host, port);
  if (upstreamFd == kInvalidHandle) {
    return kInvalidHandle;
  }

  // Additionally set the HTTP/2 stream id on the upstream state.
  auto upIt = _connections.iterator(upstreamFd);
  assert(upIt != _connections.end() && *upIt);
  ConnectionState& state = _connections.connectionState(upIt);
  state.peerStreamId = streamId;

  return upstreamFd;
}

SingleHttpServer::CloseStatus SingleHttpServer::handleInH2Tunneling(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);

  // Find the client HTTP/2 connection via peerFd.
  auto peerIt = _connections.iterator(state.peerFd);
  if (!IsValid(_connections, peerIt)) [[unlikely]] {
    return CloseStatus::Close;
  }

  ConnectionState& peerState = _connections.connectionState(peerIt);
  auto* pH2Handler = static_cast<http2::Http2ProtocolHandler*>(peerState.protocolHandler.get());
  if (pH2Handler == nullptr) [[unlikely]] {
    return CloseStatus::Close;
  }

  auto* stream = pH2Handler->connection().getStream(state.peerStreamId);
  if (stream == nullptr) {
    return CloseStatus::Close;
  }

  bool hitEagain = false;
  std::size_t bytesReadThisEvent = 0;

  while (true) {
    // Read from upstream in a loop (edge-triggered, must drain), but respect flow control.
    // If inBuffer is already large, don't read more until we can inject it.
    if (readTunnelData(cnxIt, bytesReadThisEvent, hitEagain) == CloseStatus::Close) {
      return CloseStatus::Close;
    }

    if (state.inBuffer.empty()) {
      return state.eofReceived ? CloseStatus::Close : CloseStatus::Keep;
    }

    // Determine how much we can inject based on HTTP/2 flow control windows.
    int32_t streamWin = stream->sendWindow();
    int32_t connWin = pH2Handler->connection().connectionSendWindow();
    int32_t win = std::min(streamWin, connWin);

    if (win <= 0) {
      // Wait for WINDOW_UPDATE. The windowUpdate callback will re-invoke this function.
      return CloseStatus::Keep;
    }

    const auto injectSize = std::min(state.inBuffer.size(), static_cast<RawChars::size_type>(win));

    // Inject data as HTTP/2 DATA frame(s) on the tunnel stream.
    const auto data = std::as_bytes(std::span<const char>(state.inBuffer.data(), injectSize));
    const auto err = pH2Handler->injectTunnelData(state.peerStreamId, data);

    state.inBuffer.erase_front(injectSize);

    if (err != http2::ErrorCode::NoError) [[unlikely]] {
      log::warn("HTTP/2 CONNECT stream {} inject failed: {}", state.peerStreamId, http2::ErrorCodeName(err));
      return CloseStatus::Close;
    }

    // Flush the HTTP/2 handler's output through the client connection.
    if (pH2Handler->hasPendingOutput()) {
      flushOutbound(peerIt);
    }

    // If we hit EAGAIN, we are done for now.
    if (hitEagain) {
      break;
    }
    // If we didn't hit EAGAIN, but we injected some data, we can loop and read more.
    // If we didn't inject anything (win <= 0), we would have returned above.
  }
  return CloseStatus::Keep;
}
#endif

}  // namespace aeronet
