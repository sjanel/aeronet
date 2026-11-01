#include "aeronet/internal/http1-pipeline.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/compiler-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/http-codec.hpp"
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
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-callbacks.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/transfer-coding.hpp"
#include "http-error-build.hpp"
#include "http1-writer-transport.hpp"
#include "trace-echo.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/protocol-handler.hpp"
#include "aeronet/upgrade-handler.hpp"
#endif

namespace aeronet::internal {

namespace {

// The input consumed by the requests served by Http1Pipeline::serveRequests(), erased from the input buffer at once
// when it returns: erasing each request once served would move the requests pipelined behind it each time, in a time
// quadratic in their number.
struct ConsumedInputRAII {
  explicit ConsumedInputRAII(RawChars& inBuffer) noexcept : inBuffer(inBuffer) {}

  ConsumedInputRAII(const ConsumedInputRAII&) = delete;
  ConsumedInputRAII(ConsumedInputRAII&&) noexcept = delete;
  ConsumedInputRAII& operator=(const ConsumedInputRAII&) = delete;
  ConsumedInputRAII& operator=(ConsumedInputRAII&&) noexcept = delete;

  ~ConsumedInputRAII() { inBuffer.erase_front(nbBytes); }

  // The input not consumed yet, starting with the current request.
  [[nodiscard]] std::span<char> remaining() const noexcept {
    return {inBuffer.data() + nbBytes, inBuffer.size() - nbBytes};
  }

  RawChars& inBuffer;
  std::size_t nbBytes{0};
};

// Consumes the bytes of the request once it is served.
struct RequestFinalizationRAII {
  RequestFinalizationRAII(ConsumedInputRAII& consumedInput, std::size_t consumedBytes) noexcept
      : consumedInput(consumedInput), consumedBytes(consumedBytes) {}

  RequestFinalizationRAII(const RequestFinalizationRAII&) = delete;
  RequestFinalizationRAII(RequestFinalizationRAII&&) noexcept = delete;
  RequestFinalizationRAII& operator=(const RequestFinalizationRAII&) = delete;
  RequestFinalizationRAII& operator=(RequestFinalizationRAII&&) noexcept = delete;

  ~RequestFinalizationRAII() { consumedInput.nbBytes += consumedBytes; }

  ConsumedInputRAII& consumedInput;
  std::size_t consumedBytes;
};

constexpr std::size_t kWaitForRequestLine = static_cast<std::size_t>(-1);

// RFC 9112 §2.2: a server SHOULD ignore at least one empty line (CRLF) received before a request line - some clients
// send one after the body of a POST request. Returns the size of the empty lines starting the input, once followed by a
// byte that cannot start another one, or kWaitForRequestLine. Until then, they stay in the input buffer like the start
// of a request head, bounded by maxHeaderBytes: a client sending only empty lines cannot keep its connection forever.
// Out of line: rare, its loop would otherwise be inlined in the loop serving the requests.
AERONET_NOINLINE std::size_t LeadingEmptyLinesSize(std::string_view input) noexcept {
  std::string_view rest = input;
  while (rest.starts_with(http::CRLF)) {
    rest.remove_prefix(http::CRLF.size());
  }
  // A CR alone may start another empty line.
  if (rest.empty() || rest == "\r") {
    return kWaitForRequestLine;
  }
  return input.size() - rest.size();
}

// A response sent before the whole body was received cannot consume the request: the rest of its body would then be
// parsed as the next request (and the same request answered again and again). The connection closes after it, as the
// response announces (RFC 9110 §10.1.1).
constexpr bool CloseIfBodyPending(bool bodyReady) noexcept { return !bodyReady; }

// Whether the request announces a body: chunked, or with a Content-Length other than 0.
bool AnnouncesBody(const HttpRequestView& request) {
  return request.headerValue(http::TransferEncoding).has_value() ||
         request.headerValueOrEmpty(http::ContentLength).find_first_not_of('0') != std::string_view::npos;
}

// Whether the request upgrades its connection to the WebSocket endpoint of its route. The Upgrade header of an HTTP/1.0
// request is ignored (RFC 9110 §7.8): it is served like any GET.
AERONET_ALWAYS_INLINE bool IsWebSocketUpgrade([[maybe_unused]] const HttpRequestView& request,
                                              [[maybe_unused]] const Router::RoutingResult& routingResult) {
#ifdef AERONET_ENABLE_WEBSOCKET
  return routingResult.webSocketEndpoint() != nullptr && request.method() == http::Method::GET &&
         request.version() == http::HTTP_1_1 &&
         upgrade::DetectUpgradeTarget(request.headerValueOrEmpty(http::Upgrade)) == ProtocolType::WebSocket;
#else
  return false;
#endif
}

}  // namespace

bool Http1Pipeline::processRequests(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const NativeHandle fd = cnxIt->fd();
  const bool stopReading = serveRequests(cnxIt);
  ctx.io.batchOutputOf(nullptr);

  // Serving the requests may have inserted connections (CONNECT).
  cnxIt = ctx.connections.iterator(fd);
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  // Write the responses batched while serving the requests (the HTTP/1 output that does not wait for the socket).
  if (state.hasPendingOutput() && !state.waitingWritable) {
    ctx.io.flushOutbound(cnxIt);
  }
  return stopReading;
}

bool Http1Pipeline::serveRequests(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
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
  const uint64_t bytesWrittenAtStart = ctx.totalBytesWritten();
  // The current request starts at consumedInput.nbBytes in the input buffer: the requests served before it are erased
  // when this call returns (a request waiting for more of its bytes then starts the input buffer again).
  ConsumedInputRAII consumedInput(state.inBuffer);
  // The responses are batched until the last request of the input (see ConnectionIo::batchOutputOf()).
  ctx.io.batchOutputOf(&state);
  do {
    std::span<char> input = consumedInput.remaining();
    // Do not parse the next pipelined request while a file is being sent, or while output waits for the socket: the
    // previous response hasn't been fully written yet, and the responses to the requests pipelined behind it would only
    // accumulate in the connection (the output batched by this call, not written yet, does not wait for the socket).
    if (state.isSendingFile() || (state.waitingWritable && state.hasPendingOutput())) {
      state.inputBlockedByOutput = !input.empty();
      break;
    }
    if (!input.empty() && input.front() == '\r') [[unlikely]] {
      // A head received in part, or waiting for its body, starts with its method: these are empty lines before a
      // request line, consumed like a request served before it.
      assert(request._headParseState == HttpRequestView::HeadParseState::None);
      const std::size_t emptyLinesSize = LeadingEmptyLinesSize(std::string_view(input.data(), input.size()));
      if (emptyLinesSize == kWaitForRequestLine) {
        break;
      }
      consumedInput.nbBytes += emptyLinesSize;
      input = input.subspan(emptyLinesSize);
    }
    // If we don't have enough bytes for the minimum request line, wait for more data
    if (input.size() < http::kHttpReqLineMinLen) {
      break;
    }
    // The batched responses count as written: they will be.
    if (ctx.config.fairnessBudgetExhausted(static_cast<std::size_t>(ctx.totalBytesWritten() - bytesWrittenAtStart) +
                                           state.bufferedOutputBytes())) {
      ctx.io.deferInput(cnxFd, state);
      return true;
    }

    bool isChunked = false;
    const LoopAction headAction = readHead(cnxIt, state, input, isChunked);
    if (headAction == LoopAction::Break) {
      break;
    }
    if (headAction == LoopAction::Continue) {
      continue;
    }

    // Route matching
    const Router::RoutingResult routingResult = ctx.router.match(request.method(), request.path());
    const CorsPolicy* pCorsPolicy = routingResult.corsPolicy();
    if (!applyRouteLimits(cnxIt, state, routingResult)) {
      break;
    }

    // Handle Expect header tokens beyond the built-in 100-continue.
    // RFC: if any expectation token is not understood and not handled, respond 417.
    // A request whose body is received in several reads is served again for each of them: its expectations are only
    // answered (interim responses, 100 Continue) the first time.
    bool found100Continue = false;
    if (!state.expectationAnswered) {
      auto optExpect = request.headerValue(http::Expect);
      if (optExpect && handleExpectHeader(cnxIt, *optExpect, pCorsPolicy, found100Continue)) {
        // Answered without its body (an error or a final response closes the connection if it announces one): its head
        // is consumed, and the requests behind it are served.
        consumedInput.nbBytes += request.headSpanSize();
        continue;
      }
    }
    std::size_t consumedBytes = 0;
    const BodyDecodeStatus decodeStatus =
        decodeBodyIfReady(cnxIt, input, isChunked, routingResult.pathConfig().maxBodyBytes, consumedBytes);
    if (decodeStatus == BodyDecodeStatus::Error) {
      break;
    }
    const bool bodyReady = decodeStatus == BodyDecodeStatus::Ready;
    state.expectationAnswered = !bodyReady;

    ctx.maintenance.trackBodyReception(state, bodyReady);
    if (bodyReady) {
      const bool usePerConnectionBodyStorage = state.trailerLen != 0;
      if (!request._body.empty() && !maybeDecompressRequestBody(cnxIt, usePerConnectionBodyStorage)) {
        break;
      }
      state.installAggregatedBodyBridge();
    } else {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      const bool waitForBody = routingResult.asyncRequestHandler() == nullptr;
#else
      static constexpr bool waitForBody = true;
#endif
      if (waitForBody) {
        // Its client may wait for 100 Continue before sending its body: unless it is answered without it, 100 Continue
        // is sent once (a body already complete with its head does not need it, RFC 9110 §10.1.1).
        if (found100Continue) {
          const LoopAction action = answerBeforeBody(cnxIt, state, routingResult);
          if (action == LoopAction::Continue) {
            continue;
          }
          if (action == LoopAction::Break) {
            break;
          }
        }
        // Served again once more of its body is received, without parsing its head again.
        request.keepHeadForBody();
        break;
      }
    }

    RequestFinalizationRAII requestFinalizationRAII(consumedInput, consumedBytes);
    // The response to the last request of the input is written right away, with the batched ones.
    ctx.io.batchOutputOf(input.size() > consumedBytes ? &state : nullptr);

    // Handle OPTIONS and TRACE per RFC 7231 §4.3
    // processSpecialMethods may insert an upstream connection and updates cnxIt if insertion invalidates it.
    // However, no need to update state because the pointer stays valid.
    const auto action = processSpecialMethods(cnxIt, consumedInput.nbBytes + consumedBytes, pCorsPolicy, bodyReady);
    if (action == LoopAction::SwitchProtocol) {
      if (state.tunnelResolving) {
        // The CONNECT request is answered once its target is resolved: the input buffer, where the request views point
        // to, stays as is until then. The tunnel then consumes it up to the end of the request.
        requestFinalizationRAII.consumedBytes = 0;
        consumedInput.nbBytes = 0;
        return true;  // stop reading
      }
      return state.isAnyCloseRequested();
    }
    if (action == LoopAction::Continue) {
      continue;
    }
    if (action == LoopAction::Break) {
      break;
    }

    const LoopAction handlerAction = callHandlers(cnxIt, state, routingResult, bodyReady);
    if (handlerAction == LoopAction::SwitchProtocol) {
      // The connection is now a WebSocket and will be handled differently
      return false;
    }
    if (handlerAction == LoopAction::Break) {
      break;
    }
    if (handlerAction == LoopAction::Continue) {
      continue;
    }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    if (dispatchAsyncHandler(cnxIt, routingResult.sharedAsyncRequestHandler(), bodyReady, isChunked, found100Continue,
                             consumedInput.nbBytes + consumedBytes, pCorsPolicy, routingResult.postMiddlewareRange(),
                             routingResult.pathConfig().maxBodyBytes)) {
      // The request bytes are consumed by the async handler completion (already done if it completed right away).
      // With its body, along with the requests served before it: the input buffer, where its body is, must not move
      // until then. Without, it starts the input buffer once this call returns (its head was copied), its body being
      // received behind it.
      requestFinalizationRAII.consumedBytes = 0;
      if (bodyReady) {
        consumedInput.nbBytes = 0;
      }
      if (state.pAsyncState()->active) {
        return state.isAnyCloseRequested() || PauseReadingDuringAsyncHandler(state);
      }
      // Completed synchronously: serve the next pipelined request.
    }
#endif
  } while (!state.isAnyCloseRequested());

  return state.isAnyCloseRequested();
}

AERONET_ALWAYS_INLINE Http1Pipeline::LoopAction Http1Pipeline::readHead(ConnectionIt cnxIt, ConnectionState& state,
                                                                        std::span<char> input, bool& isChunked) {
  ServerContext& ctx = *_pCtx;
  HttpRequestView& request = state.request;

  // The head of a request whose body is received in several reads is parsed once, and the request counted once.
  const bool headWaitingForBody = request.isHeadWaitingForBody();
  http::StatusCode statusCode = http::StatusCodeOK;
  if (headWaitingForBody) {
    request.resumeHeadWaitingForBody(input);
  } else {
    // The body of the previous request was completely decoded (or the connection closes).
    assert(!state.isDecodingChunkedBody());
    if (ctx.config.traceMethodPolicy != HttpServerConfig::TraceMethodPolicy::Disabled) [[unlikely]] {
      // A TRACE request echoes its head as received: it is copied before being parsed in place, once complete. A head
      // parsed in part already (received while TRACE was disabled) has no echo.
      if (request._headParseState == HttpRequestView::HeadParseState::Partial) {
        ctx.sharedBuffers.trailersOrTraceEcho.clear();
      } else if (!CopyTraceEcho(std::string_view(input.data(), input.size()), ctx.config.maxHeaderBytes,
                                ctx.sharedBuffers.trailersOrTraceEcho)) {
        return LoopAction::Break;
      }
    }
    statusCode =
        request.initTrySetHead(input, ctx.sharedBuffers.buf, ctx.config.maxHeaderBytes,
                               ctx.config.mergeUnknownRequestHeaders, ctx.telemetry.createSpan("http.request"));
    if (statusCode == HttpRequestView::kStatusNeedMoreData) {
      return LoopAction::Break;
    }

    ++state.requestsServed;
    ++ctx.stats.totalRequestsServed;
  }

  if (statusCode != http::StatusCodeOK) {
    emitSimpleError(cnxIt, statusCode, {});

    // We break unconditionally; the connection will be torn down after any queued error bytes are flushed. No partial
    // recovery is attempted for a malformed / protocol-violating start line or headers.
    return LoopAction::Break;
  }

  if (!headWaitingForBody) {
    request._reqStart = state.lastActivity;
  }

  // A full request head (and body, if present) will now be processed; mark header parsing as done.
  // headerStartTp is kept alive as the reference for relative body/deadline timestamps.
  ctx.maintenance.stopHeaderReadTimer(state);

  // Automatic HTTP -> HTTPS redirect: answer every request on this plaintext listener with a 3xx redirect
  // to the equivalent https:// URL, bypassing routing, upgrades and body handling. The connection is closed
  // afterwards (the client reconnects over TLS).
  if (ctx.config.httpsRedirect.enabled()) {
    emitHttpsRedirect(cnxIt);
    return LoopAction::Break;
  }

  if (const auto optTransferEncoding = request.headerValue(http::TransferEncoding)) {
    if (!acceptChunkedTransferEncoding(cnxIt, *optTransferEncoding)) {
      return LoopAction::Break;
    }
    isChunked = true;
  }

  const auto [encoding, reject] =
      ctx.compressionState.selector.negotiateAcceptEncoding(request.headerValueOrEmpty(http::AcceptEncoding));

  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    emitSimpleError(cnxIt, http::StatusCodeNotAcceptable, "No acceptable content-coding available");
    return LoopAction::Continue;
  }

  request._responsePossibleEncoding = encoding;
  return LoopAction::Nothing;
}

AERONET_ALWAYS_INLINE bool Http1Pipeline::applyRouteLimits(ConnectionIt cnxIt, ConnectionState& state,
                                                           const Router::RoutingResult& routingResult) {
  // Arm per-route request deadline for sweep enforcement (async/streaming handlers).
  if (routingResult.pathConfig().requestTimeout != std::chrono::milliseconds::max()) {
    _pCtx->maintenance.trackRequestDeadline(
        state,
        static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  state.lastActivity - state.headerStartTp + routingResult.pathConfig().requestTimeout)
                                  .count()));
  }

  // `Upgrade: h2c` (HTTP/1.1 -> cleartext HTTP/2) is deliberately not honored: RFC 9113 §3.1 deprecated that
  // mechanism and RFC 9110 §7.8 lets a server ignore Upgrade, so such a request is answered over HTTP/1.1 like any
  // other. Cleartext HTTP/2 is only available with prior knowledge (Http2Config::enableH2c).

  // Per-route request head size limit (already clamped against the global one, see Router::clampConfigs())
  if (state.request.headSpanSize() > routingResult.pathConfig().maxHeaderBytes) {
    emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge, {});
    return false;
  }
  return true;
}

AERONET_ALWAYS_INLINE Http1Pipeline::LoopAction Http1Pipeline::callHandlers(ConnectionIt cnxIt, ConnectionState& state,
                                                                            const Router::RoutingResult& routingResult,
                                                                            bool bodyReady) {
  HttpRequestView& request = state.request;

  const bool isStreaming = routingResult.streamingHandler() != nullptr;
  // Each response is constructed in place from its call: assigning an optional HttpResponse would move and destroy one
  // (out of line) for each request.
  if (state.requestMiddlewareRan) [[unlikely]] {
    // It ran before 100 Continue was sent. Its path and query parameters, reset when the request was served again with
    // its body, are installed again.
    state.requestMiddlewareRan = false;
    request.finalizeBeforeHandlerCall(routingResult.pathParams());
  } else if (HttpResponse shortCircuit =
                 _pCtx->requestPipeline().runRequestMiddleware(request, routingResult, isStreaming);
             !shortCircuit.empty()) {
    sendResponse(cnxIt, std::move(shortCircuit), routingResult, isStreaming, CloseIfBodyPending(bodyReady));
    return LoopAction::Continue;
  }
#ifdef AERONET_ENABLE_WEBSOCKET
  // A WebSocket upgrade goes through the request middleware (above) and the origin checks, like a request.
  if (IsWebSocketUpgrade(request, routingResult)) {
    return processWebSocketUpgrade(cnxIt, routingResult);
  }
#endif
  if (HttpResponse response = RequestPipeline::CallRequestHandler(request, routingResult); !response.empty()) {
    sendResponse(cnxIt, std::move(response), routingResult, isStreaming, CloseIfBodyPending(bodyReady));
    return LoopAction::Continue;
  }
  if (isStreaming) {
    return serveStreamingRequest(cnxIt, routingResult);
  }
  // Its async handler is to be called.
  return LoopAction::Nothing;
}

Http1Pipeline::LoopAction Http1Pipeline::answerBeforeBody(ConnectionIt cnxIt, ConnectionState& state,
                                                          const Router::RoutingResult& routingResult) {
  HttpRequestView& request = state.request;
  // A CONNECT request is not routed: its tunnel is established once its body is received.
  if (request.method() != http::Method::CONNECT) {
    // Its client may send its body anyway, not waiting for 100 Continue any longer (RFC 9110 §10.1.1): a response sent
    // before it closes the connection (see CloseIfBodyPending()).
    static constexpr bool kBodyReady = false;
    const LoopAction action = processSpecialMethods(cnxIt, 0, routingResult.corsPolicy(), kBodyReady);
    if (action != LoopAction::Nothing) {
      return action;
    }
    // The steps of callHandlers() up to the call of the handler.
    const bool isStreaming = routingResult.streamingHandler() != nullptr;
    if (HttpResponse shortCircuit = _pCtx->requestPipeline().runRequestMiddleware(request, routingResult, isStreaming);
        !shortCircuit.empty()) {
      sendResponse(cnxIt, std::move(shortCircuit), routingResult, isStreaming, CloseIfBodyPending(kBodyReady));
      return LoopAction::Continue;
    }
    if (!IsWebSocketUpgrade(request, routingResult) &&
        RequestPipeline::IsAnsweredWithoutHandler(request, routingResult)) {
      sendResponse(cnxIt, RequestPipeline::CallRequestHandler(request, routingResult), routingResult, isStreaming,
                   CloseIfBodyPending(kBodyReady));
      return LoopAction::Continue;
    }
    state.requestMiddlewareRan = true;
  }
  _pCtx->io.queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_100_CONTINUE}));
  return LoopAction::Nothing;
}

bool Http1Pipeline::acceptChunkedTransferEncoding(ConnectionIt cnxIt, std::string_view transferEncoding) {
  const HttpRequestView& request = _pCtx->connections.connectionState(cnxIt).request;
  if (request.version() == http::HTTP_1_0) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Transfer-Encoding not allowed in HTTP/1.0");
    return false;
  }
  if (!CaseInsensitiveEqual(transferEncoding, http::chunked)) {
    // RFC 9112 §6.3: without chunked as final coding, the body length cannot be determined. Otherwise, chunked follows
    // codings the server cannot decode (RFC 9112 §6.1).
    if (http::LastTransferCodingIsChunked(transferEncoding)) {
      emitSimpleError(cnxIt, http::StatusCodeNotImplemented, "Unsupported Transfer-Encoding");
    } else {
      emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Transfer-Encoding must end with chunked");
    }
    return false;
  }
  if (request.headerValue(http::ContentLength)) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Content-Length and Transfer-Encoding cannot be used together");
    return false;
  }
  return true;
}

void Http1Pipeline::sendResponse(ConnectionIt cnxIt, HttpResponse&& resp, const Router::RoutingResult& routingResult,
                                 bool streaming, bool close) {
  _pCtx->requestPipeline().applyResponseMiddleware(_pCtx->connections.connectionState(cnxIt).request, resp,
                                                   routingResult.postMiddlewareRange(), streaming);
  finalizeAndSendResponse(cnxIt, std::move(resp), routingResult.corsPolicy(), close);
}

Http1Pipeline::LoopAction Http1Pipeline::serveStreamingRequest(ConnectionIt cnxIt,
                                                               const Router::RoutingResult& routingResult) {
  static constexpr bool kStreaming = true;
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  // Create the protocol-specific transport backend and the protocol-agnostic writer
  Http1WriterTransport transport(ctx, cnxIt->fd(), routingResult.corsPolicy(), routingResult.postMiddlewareRange());
  HttpMessage::Options opts;
  if (ctx.config.addTrailerHeader) {
    opts.addTrailerHeader();
  }
  if (request.method() == http::Method::HEAD) {
    opts.setHeadMethod();
  }
  opts.setPrepared();

  HttpResponseWriter writer(transport, request, request.responsePossibleEncoding(), ctx.config.compression,
                            ctx.compressionState, ctx.config.globalHeaders.fullStringWithLastSep(), opts);

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
      sendResponse(cnxIt, request.makeResponse(http::StatusCodeInternalServerError), routingResult, kStreaming);
      return LoopAction::Continue;
    }
    // Part of the response was sent: close the connection without completing the body (no final chunk, or a short
    // Content-Length), so that the client knows that it is incomplete.
    endRequest(request, http::StatusCodeInternalServerError);
    state.requestDrainAndClose();
    return LoopAction::Break;
  }
  if (!writer.finished()) {
    writer.end();
  }

  endRequest(request, writer.status());

  // A close-delimited body (HTTP/1.0 without Content-Length) ends with the connection, announced by its head.
  // A drain or a stop that began after the head was sent, announcing a persistent connection, closes it too. So does a
  // failed writer (a compression error): the client must not take the next response for the rest of this one.
  // A close requested meanwhile (a write error, the output limit...) made the transport fail the writer.
  assert(!state.isAnyCloseRequested() || writer.failed());
  if (transport.closeAnnounced() || writer.failed() || !ctx.lifecycle.isRunning()) {
    state.requestDrainAndClose();
    return LoopAction::Break;
  }
  return LoopAction::Continue;
}

bool Http1Pipeline::maybeDecompressRequestBody(ConnectionIt cnxIt, bool usePerConnectionBodyStorage) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  usePerConnectionBodyStorage = usePerConnectionBodyStorage || state.trailerLen != 0;

  RawChars& decompressedBuffer =
      usePerConnectionBodyStorage ? state.bodyAndTrailersBuffer : ctx.sharedBuffers.decompressedBody;

  const auto res = HttpCodec::MaybeDecompressRequestBody(ctx.decompressionState, ctx.config.decompression, request,
                                                         decompressedBuffer, ctx.sharedBuffers.buf);

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

bool Http1Pipeline::isKeepAlive(const HttpRequestView& request) const {
  const ServerContext& ctx = *_pCtx;
  return ctx.config.enableKeepAlive && request._pConnectionInfo->requestsServed < ctx.config.maxRequestsPerConnection &&
         ctx.lifecycle.isRunning() && request.clientKeepsAliveForHttp1();
}

void Http1Pipeline::endRequest(HttpRequestView& request, http::StatusCode status) {
  if (_pCtx->callbacks.metrics || _pCtx->accessLog) {
    _pCtx->emitRequestMetrics(request, status);
  }
  request.end(status);
}

bool Http1Pipeline::rejectIncompleteHead(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const ConnectionState& state = ctx.connections.connectionState(cnxIt);
  if (!state.protocolHandler && state.parsingHeaders && state.inBuffer.size() > ctx.config.maxHeaderBytes) {
    // Safety cap: prevent unbounded buffer growth while headers are still being received.
    // Checked after the input processing so that a single read delivering headers + body together
    // does not falsely trigger: initTrySetHead clears parsingHeaders once the request head is parsed.
    // If we reach here, the header delimiter was not found and the buffer exceeds the header limit.
    // The end of the request line itself was not found when none of the head was parsed, unless the input only holds
    // empty lines waiting for it: its request-target is too long (RFC 9112 §3).
    const bool isRequestLineIncomplete =
        state.request._headParseState == HttpRequestView::HeadParseState::None &&
        LeadingEmptyLinesSize(std::string_view(state.inBuffer.data(), state.inBuffer.size())) != kWaitForRequestLine;
    emitSimpleError(
        cnxIt, isRequestLineIncomplete ? http::StatusCodeURITooLong : http::StatusCodeRequestHeaderFieldsTooLarge, {});
    return true;
  }

  // Header read timeout enforcement: if headers of current pending request are not complete yet
  // (heuristic: no full request parsed and buffer not empty) and duration exceeded -> close.
  if (ctx.config.headerReadTimeout.count() > 0 && state.parsingHeaders &&
      ctx.connections.now - state.headerStartTp > ctx.config.headerReadTimeout) {
    emitSimpleError(cnxIt, http::StatusCodeRequestTimeout, {});
    return true;
  }
  return false;
}

void Http1Pipeline::emitSimpleError(ConnectionIt cnxIt, http::StatusCode statusCode, std::string_view body) {
  ServerContext& ctx = *_pCtx;
  ctx.io.queueData(
      cnxIt, HttpMessageData(BuildSimpleError(statusCode, ctx.config.globalHeaders, body, ctx.dateHeader.data())));

  if (ctx.callbacks.parserErr) {
    // Swallow exceptions from user callback to avoid destabilizing the server
    try {
      ctx.callbacks.parserErr(statusCode);
    } catch (const std::exception& ex) {
      log::error("Exception raised in user callback: {}", ex.what());
    } catch (...) {
      log::error("Unknown exception raised in user callback");
    }
  }

  ConnectionState& state = ctx.connections.connectionState(cnxIt);

  state.requestDrainAndClose();
  state.request.end(statusCode);
}

void Http1Pipeline::emitHttpsRedirect(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  // Build the absolute https:// target into a scratch buffer, re-encoding the (decoded) path and query so the
  // result is always a valid URL / header value. HttpResponse::location() copies it out, so the scratch buffer
  // can be reused freely afterwards.
  RawChars& urlBuf = ctx.sharedBuffers.buf;
  if (!http::BuildHttpsRedirectUrl(urlBuf, request, ctx.config.httpsRedirect.targetPort)) {
    // No (or empty) Host header: an absolute https URL cannot be constructed.
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, "Missing Host header for HTTPS redirect");
    return;
  }

  const http::StatusCode statusCode = ctx.config.httpsRedirect.statusCode;
  HttpResponse resp(statusCode);
  resp.location(urlBuf);

  ctx.maintenance.clearRequestDeadline(state);
  state.headerStartTp = {};

  // Force connection close: the redirected client reconnects over TLS, and closing avoids any
  // request-body framing concerns since the body is intentionally not consumed.
  HttpResponse::Options opts;
  opts.setClose();
  if (request.method() == http::Method::HEAD) {
    opts.setHeadMethod();
  }

  ctx.io.queueData(cnxIt, resp.finalizeForHttp1(ctx.dateHeader.data(), request.version(), opts,
                                                &ctx.config.globalHeaders, ctx.config.minCapturedBodySize));

  state.requestDrainAndClose();
  endRequest(request, statusCode);
}

bool Http1Pipeline::handleExpectHeader(ConnectionIt cnxIt, std::string_view expectHeader, const CorsPolicy* pCorsPolicy,
                                       bool& found100Continue) {
  ServerContext& ctx = *_pCtx;
  HttpRequestView& request = ctx.connections.connectionState(cnxIt).request;
  // Parse comma-separated tokens (trim OWS). Case-insensitive comparison for 100-continue.
  for (std::size_t pos = 0; pos < expectHeader.size();) {
    const auto commaPos = expectHeader.find(',', pos);
    const auto tokenEnd = (commaPos == std::string_view::npos) ? expectHeader.size() : commaPos;

    const auto token = TrimOws(expectHeader.substr(pos, tokenEnd - pos));
    pos = (commaPos == std::string_view::npos) ? expectHeader.size() : commaPos + 1;

    if (token.empty()) {
      continue;
    }
    if (CaseInsensitiveEqual(token, http::h100_continue)) {
      // The 100 Continue is sent by the body decoding. An HTTP/1.0 client cannot receive a 1xx response (RFC 9110
      // §15.2): its 100-continue expectation is ignored (§10.1.1).
      found100Continue = request.version() == http::HTTP_1_1;
      continue;
    }
    if (!ctx.callbacks.expectation) {
      // No handler and not 100-continue -> RFC says respond 417
      emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, {});
      return true;
    }
    try {
      auto expectationResult = ctx.callbacks.expectation(request, token);
      switch (expectationResult.kind) {
        case ExpectationResultKind::Reject:
          emitSimpleError(cnxIt, http::StatusCodeExpectationFailed, {});
          return true;
        case ExpectationResultKind::Interim: {
          // Emit an interim response immediately. Common case: 102 "Processing"
          const auto status = expectationResult.interimStatus;
          // Validate that the handler returned an informational 1xx status. 101 Switching Protocols is not an interim
          // response: the connection would no longer speak HTTP/1.1 after it (RFC 9110 §15.2.2).
          if (status < 100U || status >= 200U || status == http::StatusCodeSwitchingProtocols) {
            emitSimpleError(cnxIt, http::StatusCodeInternalServerError,
                            "Invalid interim status (must be 1xx other than 101)");
            return true;
          }
          // An HTTP/1.0 client cannot receive a 1xx response (RFC 9110 §15.2): the request goes on without it.
          if (request.version() == http::HTTP_1_0) {
            break;
          }

          switch (status) {
            case 100:
              ctx.io.queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_100_CONTINUE}));
              break;
            case 102:
              ctx.io.queueData(cnxIt, HttpMessageData(RawChars{http::HTTP11_102_PROCESSING}));
              break;
            default: {
              static constexpr std::string_view kHttpResponseLinePrefix = "HTTP/1.1 ";
              // The reason phrase is optional, not the space before it (RFC 9112 §4).
              static constexpr std::string_view kEmptyReasonDoubleCRLF = " \r\n\r\n";

              RawChars buf(kHttpResponseLinePrefix.size() + 3U + kEmptyReasonDoubleCRLF.size());
              buf.setSize(buf.capacity());

              char* insertPtr = AppendFixed<kHttpResponseLinePrefix>(buf.data());
              insertPtr = writeStatusCode(insertPtr, status);
              CopyFixed<kEmptyReasonDoubleCRLF>(insertPtr);

              ctx.io.queueData(cnxIt, HttpMessageData(std::move(buf)));
              break;
            }
          }

          break;
        }
        case ExpectationResultKind::FinalResponse: {
          // Send the provided final response immediately and skip body processing. The client may send the body that
          // the request announces anyway (it is not required to wait for the response), which could not be told apart
          // from the next request: the connection then closes after the response.
          finalizeAndSendResponse(cnxIt, std::move(expectationResult.finalResponse), pCorsPolicy,
                                  AnnouncesBody(request));
          return true;
        }
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

Http1Pipeline::LoopAction Http1Pipeline::processSpecialMethods(ConnectionIt& cnxIt, std::size_t requestEnd,
                                                               const CorsPolicy* pCorsPolicy, bool bodyReady) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  // Handle OPTIONS and TRACE via shared protocol-agnostic code
  if (request.method() == http::Method::OPTIONS || request.method() == http::Method::TRACE) {
    const SpecialMethodConfig config{
        .tracePolicy = ctx.config.traceMethodPolicy,
        .isTls = !request.tlsVersion().empty(),
    };

    // A TRACE request echoes the copy of its head made before its parsing (see readHead()).
    std::string_view traceEcho;
    if (request.method() == http::Method::TRACE && config.traceEnabled()) {
      // A client must not send content in a TRACE request (RFC 9110 §9.3.8). Without, the request is served right after
      // its head is parsed, and has no chunked body whose trailers would overwrite the copy of its head: no other
      // request, TRACE or chunked, was served meanwhile.
      if (AnnouncesBody(request)) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, "TRACE request with content");
        return LoopAction::Break;
      }
      traceEcho = ctx.sharedBuffers.trailersOrTraceEcho;
    }

    HttpResponse result = ProcessSpecialMethods(request, ctx.router, config, pCorsPolicy, traceEcho);
    if (!result.empty()) {
      finalizeAndSendResponse(cnxIt, std::move(result), pCorsPolicy, CloseIfBodyPending(bodyReady));
      return LoopAction::Continue;
    }
    // Not handled (e.g., not a preflight), fall through to normal processing
    return LoopAction::Nothing;
  }

  // CONNECT requires protocol-specific handling (TCP tunnel setup)
  if (request.method() == http::Method::CONNECT) {
    return TunnelManager::Of(ctx).startHttp1Tunnel(cnxIt, requestEnd);
  }

  return LoopAction::Nothing;
}

void Http1Pipeline::finalizeAndSendResponse(ConnectionIt cnxIt, HttpResponse&& resp, const CorsPolicy* pCorsPolicy,
                                            bool close) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, resp);
  }

  // Clear per-route request deadline and headerStartTp now that a response is being sent.
  // Resetting headerStartTp to epoch lets the first bytes of the next keep-alive request start its header read timer.
  ctx.maintenance.clearRequestDeadline(state);
  state.headerStartTp = {};

  request.prefinalizeHttpResponse(resp, ctx.telemetry);

  const bool keepAlive = !close && isKeepAlive(request);

  const auto respStatusCode = resp.status();

  HttpResponse::Options opts;

  if (!keepAlive) {
    opts.setClose();
  }
  if (ctx.config.addTrailerHeader) {
    opts.addTrailerHeader();
  }
  if (request.method() == http::Method::HEAD) {
    opts.setHeadMethod();
  }

  ctx.io.queueData(cnxIt, resp.finalizeForHttp1(ctx.dateHeader.data(), request.version(), opts,
                                                &ctx.config.globalHeaders, ctx.config.minCapturedBodySize));

  if (!keepAlive) {
    // Always request drain+close for non-keep-alive connections. The actual close only triggers once the output is
    // written (checked by canCloseConnectionForDrain), so it is safe to call this before a large response finishes
    // flushing.
    state.requestDrainAndClose();
  }
  // End the span after response is finalized
  endRequest(request, respStatusCode);
}

}  // namespace aeronet::internal
