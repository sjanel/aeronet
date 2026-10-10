#include "aeronet/single-http-server.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
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
#include "aeronet/native-handle.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/signal-handler.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"
#include "http-error-build.hpp"
#include "http1-writer-transport.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#endif

#if defined(AERONET_ENABLE_HTTP2) || defined(AERONET_ENABLE_WEBSOCKET)
#include "upgrade-handler.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#endif
#endif

namespace aeronet {

namespace {

// Consumes the bytes of the request from the input buffer once it is served.
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

}  // namespace

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
#ifdef AERONET_ENABLE_WEBSOCKET
    if (state.protocol == ProtocolType::WebSocket) {
      const NativeHandle fd = cnxIt->fd();
      const bool closeRequested = processSpecialProtocolHandler(cnxIt);
      // A callback may have started the close handshake, whose timeout can come before the armed deadline.
      refreshWebSocketDeadline(fd, *_connections.pConnectionState(fd));
      return closeRequested;
    }
#endif
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
  // Save the client fd before entering the loop. processInput() may set up a tunnel through the tunnel bridge
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
          return StopReadingIfOutputBlocked(*pState);
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

  return StopReadingIfOutputBlocked(*pState);
}

bool SingleHttpServer::StopReadingIfOutputBlocked(ConnectionState& state) {
  // A blocked output transport is also a signal to stop this readable-event loop. This keeps additional
  // HTTP/2 frames in the socket while the peer is not draining responses. The kernel reports them again once the output
  // drained, but not the frames already in the input buffer or in the records read ahead by the TLS transport.
  const bool outputBlocked = state.hasPendingOutput();
  state.inputBlockedByOutput = outputBlocked && (!state.inBuffer.empty() || state.transport.hasPendingReadData());
  return state.isAnyCloseRequested() || outputBlocked;
}

bool SingleHttpServer::processHttp1Requests(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.isAnyCloseRequested() || state.tunnelResolving) {
    // The bytes still buffered (a request answered by an error, its unread body, the CONNECT request waiting for its
    // target resolution and the tunnel bytes behind it...) must not be served.
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
  // A small request can have a large response: once the responses written by this call reach the fairness budget, the
  // next pipelined requests are served after the other ready connections. The read budget alone would let a client
  // pipelining such requests keep the event loop on its connection until it sent a whole budget of requests.
  const uint64_t bytesWrittenAtStart = totalBytesWritten();
  do {
    // Do not parse the next pipelined request while a file send is still in progress.
    // attachFilePayload would silently overwrite the in-flight file payload, corrupting
    // the response stream (partial data from file1 mixed with file2 headers+data).
    // Also skip when outBuffer is non-empty — the previous response hasn't been fully
    // flushed yet, and starting a new response would interleave data.
    if (state.isSendingFile() || !state.outBuffer.empty()) {
      state.inputBlockedByOutput = !state.inBuffer.empty();
      break;
    }
    // If we don't have enough bytes for the minimum request line, wait for more data
    if (state.inBuffer.size() < http::kHttpReqLineMinLen) {
      break;
    }
    if (_config.fairnessBudgetExhausted(static_cast<std::size_t>(totalBytesWritten() - bytesWrittenAtStart))) {
      deferInput(cnxFd, state);
      return true;
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
    if (const auto optTransferEncoding = request.headerValue(http::TransferEncoding)) {
      if (!acceptChunkedTransferEncoding(cnxIt, *optTransferEncoding)) {
        break;
      }
      isChunked = true;
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
    trackBodyReception(state, bodyReady);
    if (bodyReady) {
      const bool usePerConnectionBodyStorage = state.trailerLen != 0;
      if (!request._body.empty() && !maybeDecompressRequestBody(cnxIt, usePerConnectionBodyStorage)) {
        break;
      }
      state.installAggregatedBodyBridge();
    } else {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      if (routingResult.asyncRequestHandler() == nullptr) {
        break;
      }
#else
      break;
#endif
    }

    RequestFinalizationRAII requestFinalizationRAII(state, consumedBytes);

    // Handle OPTIONS and TRACE per RFC 7231 §4.3
    // processSpecialMethods may insert an upstream connection and updates cnxIt if insertion invalidates it.
    // However, no need to update state because the pointer stays valid.
    const auto action = processSpecialMethods(cnxIt, consumedBytes, pCorsPolicy);
    if (action == LoopAction::SwitchProtocol) {
      if (state.tunnelResolving) {
        // The CONNECT request is answered once its target is resolved: its bytes stay in the input buffer until then.
        requestFinalizationRAII.consumedBytes = 0;
        return true;  // stop reading
      }
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

    auto sendResponse = [this, cnxIt, &routingResult, isStreaming](HttpResponse&& resp) {
      sendHttp1Response(cnxIt, std::move(resp), routingResult, isStreaming);
    };

    auto shortCircuitedResponse =
        RunRequestMiddleware(request, _router.globalRequestMiddleware(), routingResult.preMiddlewareRange(), _telemetry,
                             isStreaming, _callbacks.middlewareMetrics);

    if (shortCircuitedResponse.has_value()) {
      sendResponse(std::move(*shortCircuitedResponse));
      closeIfBodyPending();
      continue;
    }

#ifdef AERONET_ENABLE_WEBSOCKET
    // A WebSocket upgrade goes through the request middleware (above) and the origin checks, like a request.
    if (routingResult.webSocketEndpoint() != nullptr && request.method() == http::Method::GET &&
        upgrade::DetectUpgradeTarget(request.headerValueOrEmpty(http::Upgrade)) == ProtocolType::WebSocket) {
      const LoopAction upgradeAction = processWebSocketUpgrade(cnxIt, routingResult);
      if (upgradeAction == LoopAction::SwitchProtocol) {
        // The connection is now a WebSocket and will be handled differently
        return false;
      }
      if (upgradeAction == LoopAction::Break) {
        break;
      }
      continue;
    }
#endif

    if (isStreaming) {
      if (rejectDeniedCorsOrigin(cnxIt, routingResult, isStreaming)) {
        continue;
      }
      if (serveStreamingRequest(cnxIt, routingResult) == LoopAction::Break) {
        break;
      }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    } else if (routingResult.asyncRequestHandler() != nullptr) {
      if (rejectDeniedCorsOrigin(cnxIt, routingResult, isStreaming)) {
        closeIfBodyPending();
        continue;
      }

      if (dispatchAsyncHandler(cnxIt, routingResult.sharedAsyncRequestHandler(), bodyReady, isChunked, consumedBytes,
                               pCorsPolicy, routingResult.postMiddlewareRange(),
                               routingResult.pathConfig().maxBodyBytes)) {
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
      if (rejectDeniedCorsOrigin(cnxIt, routingResult, isStreaming)) {
        continue;
      }

      // normal handler
      try {
        sendResponse((*routingResult.requestHandler())(request));
      } catch (const std::exception& ex) {
        log::error("Exception in path handler: {}", ex.what());
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError));
      } catch (...) {
        log::error("Unknown exception in path handler");
        sendResponse(request.makeResponse(http::StatusCodeInternalServerError));
      }
    } else if (routingResult.redirectSlashMode() != Router::RoutingResult::RedirectSlashMode::None) {
      sendResponse(makeTrailingSlashRedirect(request, routingResult.redirectSlashMode()));
    } else if (routingResult.methodNotAllowed()) {
      sendResponse(request.makeResponse(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed));
    } else {
      sendResponse(request.makeResponse(http::StatusCodeNotFound));
    }

  } while (!state.isAnyCloseRequested());

  return state.isAnyCloseRequested();
}

bool SingleHttpServer::acceptChunkedTransferEncoding(ConnectionIt cnxIt, std::string_view transferEncoding) {
  const HttpRequestView& request = _connections.connectionState(cnxIt).request;
  if (request.version() == http::HTTP_1_0) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Transfer-Encoding not allowed in HTTP/1.0");
    return false;
  }
  if (!CaseInsensitiveEqual(transferEncoding, http::chunked)) {
    emitSimpleError(cnxIt, http::StatusCodeNotImplemented, "Unsupported Transfer-Encoding");
    return false;
  }
  if (request.headerValue(http::ContentLength)) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Content-Length and Transfer-Encoding cannot be used together");
    return false;
  }
  return true;
}

void SingleHttpServer::trackBodyReception(ConnectionState& state, bool bodyReady) noexcept {
  if (_config.bodyReadTimeout.count() == 0) {
    return;
  }
  if (bodyReady) {
    if (state.waitingForBody) {
      assert(_connectionSweepState.pendingTimeoutConnections > 0U);
      --_connectionSweepState.pendingTimeoutConnections;
    }
    state.waitingForBody = false;
    state.bodyLastActivityMs = ConnectionState::kInactiveRelativeMs;
  } else {
    if (!state.waitingForBody) {
      ++_connectionSweepState.pendingTimeoutConnections;
    }
    state.waitingForBody = true;
    state.bodyLastActivityMs = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(state.lastActivity - state.headerStartTp).count());
  }
}

void SingleHttpServer::sendHttp1Response(ConnectionIt cnxIt, HttpResponse&& resp,
                                         const Router::RoutingResult& routingResult, bool streaming) {
  ApplyResponseMiddleware(_connections.connectionState(cnxIt).request, resp, routingResult.postMiddlewareRange(),
                          _router.globalResponseMiddleware(), _telemetry, streaming, _callbacks.middlewareMetrics);
  finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), routingResult.corsPolicy());
}

bool SingleHttpServer::rejectDeniedCorsOrigin(ConnectionIt cnxIt, const Router::RoutingResult& routingResult,
                                              bool streaming) {
  const CorsPolicy* pCorsPolicy = routingResult.corsPolicy();
  const HttpRequestView& request = _connections.connectionState(cnxIt).request;
  if (pCorsPolicy == nullptr || pCorsPolicy->wouldApply(request) != CorsPolicy::ApplyStatus::OriginDenied) {
    return false;
  }
  sendHttp1Response(cnxIt, request.makeResponse(http::StatusCodeForbidden, "Forbidden by CORS policy"), routingResult,
                    streaming);
  return true;
}

HttpResponse SingleHttpServer::makeTrailingSlashRedirect(const HttpRequestView& request,
                                                         Router::RoutingResult::RedirectSlashMode redirectSlashMode) {
  // The path is percent-decoded: re-encode it so that the Location is a valid URI reference. Decoded control
  // characters (CR, LF...) would otherwise make the header value invalid and throw outside of any handler.
  static constexpr std::string_view kRedirecting = "Redirecting";
  const std::string_view reqPath = request.path();
  RawChars& location = _sharedBuffers.buf;
  location.clear();
  if (redirectSlashMode == Router::RoutingResult::RedirectSlashMode::AddSlash) {
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
  return resp;
}

SingleHttpServer::LoopAction SingleHttpServer::serveStreamingRequest(ConnectionIt cnxIt,
                                                                     const Router::RoutingResult& routingResult) {
  static constexpr bool kStreaming = true;
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  const bool wantClose = request.wantClose();

  // Create the protocol-specific transport backend and the protocol-agnostic writer
  Http1WriterTransport transport(*this, cnxIt->fd(), wantClose, routingResult.corsPolicy(),
                                 routingResult.postMiddlewareRange());
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

  bool handlerThrew = false;
  try {
    (*routingResult.streamingHandler())(request, writer);
  } catch (const std::exception& ex) {
    handlerThrew = true;
    log::error("Exception in streaming handler: {}", ex.what());
  } catch (...) {
    handlerThrew = true;
    log::error("Unknown exception in streaming handler");
  }
  if (handlerThrew) {
    if (writer.headersPending()) {
      // Nothing was sent: answer like for any handler throwing.
      sendHttp1Response(cnxIt, request.makeResponse(http::StatusCodeInternalServerError), routingResult, kStreaming);
      return LoopAction::Continue;
    }
    // Part of the response was sent: close the connection without completing the body (no final chunk, or a short
    // Content-Length), so that the client knows that it is incomplete.
    endRequest(request, http::StatusCodeInternalServerError, state.requestsServed > 1);
    state.requestDrainAndClose();
    return LoopAction::Break;
  }
  if (!writer.finished()) {
    writer.end();
  }

  endRequest(request, writer.status(), state.requestsServed > 1);

  assert(request.version() == http::HTTP_1_1);
  if (!_config.enableKeepAlive || wantClose || state.requestsServed + 1 >= _config.maxRequestsPerConnection ||
      state.isAnyCloseRequested() || _lifecycle.isDraining() || _lifecycle.isStopping()) {
    state.requestDrainAndClose();
    return LoopAction::Break;
  }
  return LoopAction::Continue;
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

void SingleHttpServer::emitRequestMetrics(const HttpRequestView& request, http::StatusCode status,
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
      request._body.size(),
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

  if (_tunnels != nullptr && _tunnels->hasResolvedTargets()) {
    _tunnels->completeResolvedTunnels();
  }

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
          // A handler (e.g. the half-close of a tunnel peer) may have already recycled
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

  if (!_pendingReadFds.empty()) {
    resumeDeferredInputs();
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
    runMaintenance(now);
  }
}

void SingleHttpServer::resumeDeferredInputs() {
  // Re-process connections deferred by the per-event fairness cap, or whose input was left by the output backpressure.
  // Edge-triggered polling (EPOLLET / EV_CLEAR) only fires on state transitions;
  // a connection that still had TCP data after hitting the cap won't generate a new
  // read event, so we must re-read it here before waiting for events again.
  //
  // Swap into a local so that resumeDeferredInput can safely push new deferrals
  // into the (now-empty) member vector without invalidating our iteration and
  // without losing them to a blanket clear().
  decltype(_pendingReadFds) batch;
  batch.swap(_pendingReadFds);
  for (const NativeHandle pendingFd : batch) {
    const auto pendingIt = _connections.iterator(pendingFd);
    if (!IsValid(_connections, pendingIt)) {
      continue;
    }
    const CloseStatus cs = resumeDeferredInput(pendingIt);
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

void SingleHttpServer::runMaintenance(std::chrono::steady_clock::time_point now) {
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
  // Without connections, no tunnel is left: the resolver threads exit once their current resolution completes.
  _tunnels.reset();
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
  endRequest(request, statusCode, state.requestsServed > 0);
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

}  // namespace aeronet
