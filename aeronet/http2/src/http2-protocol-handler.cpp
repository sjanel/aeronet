#include "aeronet/http2-protocol-handler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "http2-writer-transport.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet::http2 {

// ============================
// Http2ProtocolHandler
// ============================

Http2ProtocolHandler::Http2ProtocolHandler(const Http2Config& config, Router& router, HttpServerConfig& serverConfig,
                                           internal::ResponseCompressionState& compressionState,
                                           internal::RequestDecompressionState& decompressionState,
                                           tracing::TelemetryContext& telemetryContext, RawChars& tmpBuffer)
    : _connection(config, true),
      _pRouter(&router),
      _fileSendBuffer(64UL * 1024UL),
      _pServerConfig(&serverConfig),
      _pCompressionState(&compressionState),
      _pDecompressionState(&decompressionState),
      _pTmpBuffer(&tmpBuffer),
      _pTelemetryContext(&telemetryContext) {
  setupCallbacks();
}

Http2ProtocolHandler::~Http2ProtocolHandler() = default;

Http2ProtocolHandler::Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept = default;
Http2ProtocolHandler& Http2ProtocolHandler::operator=(Http2ProtocolHandler&&) noexcept = default;

void Http2ProtocolHandler::setupCallbacks() {
  _connection.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
    onHeadersDecodedReceived(streamId, headers, endStream);
  });

  _connection.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
    onDataReceived(streamId, data, endStream);
  });

  _connection.setOnStreamClosed([this](uint32_t streamId) { onStreamClosed(streamId); });

  _connection.setOnStreamReset([this](uint32_t streamId, ErrorCode errorCode) { onStreamReset(streamId, errorCode); });

  _connection.setOnWindowUpdate([this](uint32_t streamId, uint32_t /*increment*/) {
    if (streamId == 0) {
      // Connection-level window update: notify all tunnel streams
      for (const auto& [id, upstreamFd] : _tunnelStreams) {
        _tunnelBridge->onTunnelWindowUpdate(upstreamFd);
      }
    } else {
      // Stream-level window update
      auto it = _tunnelStreams.find(streamId);
      if (it != _tunnelStreams.end()) {
        _tunnelBridge->onTunnelWindowUpdate(it->second);
      }
    }
  });
}

ProtocolProcessResult Http2ProtocolHandler::processInput(std::span<const std::byte> data,
                                                         [[maybe_unused]] ::aeronet::ConnectionState& state) {
  auto result = _connection.processInput(data);

  // If the client granted more flow control (WINDOW_UPDATE), try to continue any pending sends.
  // Only do this when we don't already have pending output to avoid unbounded buffering.
  if (!_connection.hasPendingOutput()) {
    flushPendingFileSends();
    flushPendingStreamingSends();
    if (result.action == Http2Connection::ProcessResult::Action::Continue && _connection.hasPendingOutput()) {
      result.action = Http2Connection::ProcessResult::Action::OutputReady;
    }
  }

  ProtocolProcessResult output;
  output.bytesConsumed = result.bytesConsumed;

  switch (result.action) {
    case Http2Connection::ProcessResult::Action::Continue:
      output.action = ProtocolProcessResult::Action::Continue;
      break;
    case Http2Connection::ProcessResult::Action::OutputReady:
      output.action = ProtocolProcessResult::Action::ResponseReady;
      break;
    case Http2Connection::ProcessResult::Action::Error:
      output.action = ProtocolProcessResult::Action::CloseImmediate;
      log::error("HTTP/2 protocol error: {} ({})", result.errorMessage, ErrorCodeName(result.errorCode));
      break;
    case Http2Connection::ProcessResult::Action::GoAway:
      output.action = ProtocolProcessResult::Action::Close;
      break;
    default:
      assert(result.action == Http2Connection::ProcessResult::Action::Closed);
      output.action = ProtocolProcessResult::Action::Close;
      break;
  }

  return output;
}

namespace {

http::Method ParseHttpMethod(std::string_view method) noexcept {
  if (method == "GET") {
    return http::Method::GET;
  }
  if (method == "POST") {
    return http::Method::POST;
  }
  if (method == "PUT") {
    return http::Method::PUT;
  }
  if (method == "DELETE") {
    return http::Method::DELETE;
  }
  if (method == "HEAD") {
    return http::Method::HEAD;
  }
  if (method == "OPTIONS") {
    return http::Method::OPTIONS;
  }
  if (method == "PATCH") {
    return http::Method::PATCH;
  }
  if (method == "CONNECT") {
    return http::Method::CONNECT;
  }
  if (method == "TRACE") {
    return http::Method::TRACE;
  }
  // Fallback to GET for unknown methods
  return http::Method::GET;
}

}  // namespace

void Http2ProtocolHandler::onHeadersDecodedReceived(uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
  auto [it, inserted] = _streamRequests.try_emplace(streamId);
  assert(inserted);  // logic below should be adapted if we can call this multiple times per stream
  StreamRequest& streamReq = it->second;

  HttpRequest& req = streamReq.request;
  req.init(*_pServerConfig, *_pCompressionState);
  req._addTrailerHeader = false;  // no trailer header in HTTP/2

  // Pass 1 : compute total headers storage
  // +1 for :authority \r char for setupTunnelConnection
  std::size_t headersTotalLen = 1U;
  for (const auto& [name, value] : headers) {
    headersTotalLen += name.size() + value.size();
  }

  streamReq.headerStorage = std::make_unique_for_overwrite<char[]>(headersTotalLen);

  char* buf = streamReq.headerStorage.get();
  for (const auto& [name, value] : headers) {
    std::string_view storedName(buf, name.size());

    buf = Append(name, buf);

    std::string_view storedValue(buf, value.size());

    buf = Append(value, buf);

    assert(!name.empty());
    if (name[0] == ':') {
      if (storedName == ":method") {
        req._method = ParseHttpMethod(storedValue);
      } else if (storedName == ":scheme") {
        req._pScheme = storedValue.data();
        req._schemeLength = SafeCast<decltype(req._schemeLength)>(storedValue.size());
      } else if (storedName == ":authority") {
        req._pAuthority = storedValue.data();
        req._authorityLength = SafeCast<decltype(req._authorityLength)>(storedValue.size());
        *buf++ = '\r';  // sentinel char for setupTunnelConnection
      } else if (storedName == ":path") {
        // Split :path at '?' to separate path from query string, mirroring HTTP/1.1 parsing.
        // The stored value lives in headerStorage which we own, so in-place decoding is safe.
        char* pathStart = const_cast<char*>(storedValue.data());
        char* pathEnd = pathStart + storedValue.size();

        if (!req.decodePath(pathStart, pathEnd)) {
          (void)sendResponse(
              streamId,
              HttpResponse(
                  http::StatusCodeBadRequest,
                  "Invalid :path header - unable to decode percent-encoded characters or path is not valid UTF-8"),
              /*isHeadMethod=*/false);
          _streamRequests.erase(streamId);
          return;
        }
      }
    } else {
      req._headers[storedName] = storedValue;
    }
  }

  const auto [encoding, reject] =
      _pCompressionState->selector.negotiateAcceptEncoding(req.headerValueOrEmpty(http::AcceptEncoding));
  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeNotAcceptable, "No acceptable content-coding available"),
                       /*isHeadMethod=*/false);
    _streamRequests.erase(streamId);
    return;
  }

  req._streamId = streamId;
  req._version = http::HTTP_2_0;
  req._reqStart = std::chrono::steady_clock::now();
  req._responsePossibleEncoding = encoding;

  // Create trace span for HTTP/2 request (mirrors HTTP/1 span from initTrySetHead)
  req._traceSpan = _pTelemetryContext->createSpan("http.request");
  if (req._traceSpan) {
    req._traceSpan->setAttribute("http.method", http::MethodToStr(req._method));
    req._traceSpan->setAttribute("http.target", req.path());
    req._traceSpan->setAttribute("http.scheme", req.scheme());
    if (!req.authority().empty()) {
      req._traceSpan->setAttribute("http.host", req.authority());
    }
  }

  // CONNECT requests must be dispatched immediately after headers regardless of endStream,
  // because CONNECT has no request body — subsequent DATA frames carry tunnel payload.
  if (endStream || req.method() == http::Method::CONNECT) {
    dispatchRequest(it);
  }
}

void Http2ProtocolHandler::onDataReceived(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  // Check if this is a CONNECT tunnel stream — forward data to upstream.
  auto tunnelIt = _tunnelStreams.find(streamId);
  if (tunnelIt != _tunnelStreams.end()) {
    if (!data.empty()) {
      _tunnelBridge->writeTunnel(tunnelIt->second, data);
    }
    if (endStream) {
      // Client closed their end of the tunnel — half-close the upstream side.
      _tunnelBridge->shutdownTunnelWrite(tunnelIt->second);
    }
    return;
  }

  auto it = _streamRequests.find(streamId);
  if (it == _streamRequests.end()) [[unlikely]] {
    log::warn("HTTP/2 DATA frame for unknown stream {}", streamId);
    return;
  }

  StreamRequest& streamReq = it->second;

  // Accumulate body data
  streamReq.bodyBuffer.append(reinterpret_cast<const char*>(data.data()), data.size());

  if (endStream) {
    // Set body on HttpRequest
    streamReq.request._body = streamReq.bodyBuffer;

    if (!streamReq.request._body.empty()) {
      const auto res = internal::HttpCodec::MaybeDecompressRequestBody(
          *_pDecompressionState, _pServerConfig->decompression, streamReq.request, streamReq.bodyBuffer, *_pTmpBuffer);
      if (res.message != nullptr) {
        (void)sendResponse(streamId, HttpResponse(res.status, res.message), /*isHeadMethod=*/false);
        _streamRequests.erase(streamId);
        return;
      }
    }

    dispatchRequest(it);
  }
}

void Http2ProtocolHandler::onStreamClosed(uint32_t streamId) {
  _streamRequests.erase(streamId);
  _pendingFileSends.erase(streamId);
  _pendingStreamingSends.erase(streamId);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  _pendingAsyncTasks.erase(streamId);
#endif
  cleanupTunnel(streamId);
}

void Http2ProtocolHandler::onStreamReset(uint32_t streamId, ErrorCode errorCode) {
  log::debug("HTTP/2 stream {} reset with error: {}", streamId, ErrorCodeName(errorCode));
  _streamRequests.erase(streamId);
  _pendingFileSends.erase(streamId);
  _pendingStreamingSends.erase(streamId);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  _pendingAsyncTasks.erase(streamId);
#endif
  cleanupTunnel(streamId);
}

ErrorCode Http2ProtocolHandler::sendPendingFileBody(uint32_t streamId, PendingFileSend& pending,
                                                    bool endStreamAfterBody) {
  const uint32_t peerMaxFrame = _connection.peerSettings().maxFrameSize;

  while (pending.remaining != 0) {
    Http2Stream* stream = _connection.getStream(streamId);
    // Stream cannot vanish during synchronous output buffer writes — only processInput
    // (which is not re-entered) can close/reset streams.
    assert(stream != nullptr && "stream disappeared during synchronous file send loop");

    const int32_t streamWin = stream->sendWindow();
    const int32_t connWin = _connection.connectionSendWindow();
    if (streamWin <= 0 || connWin <= 0) {
      return ErrorCode::NoError;  // wait for WINDOW_UPDATE
    }

    const auto windowLimit = static_cast<std::size_t>(std::min(streamWin, connWin));
    const std::size_t chunkSize = std::min({pending.remaining, windowLimit, static_cast<std::size_t>(peerMaxFrame),
                                            static_cast<std::size_t>(_fileSendBuffer.capacity())});
    // All min() inputs are > 0: pending.remaining (loop condition), windowLimit (checked > 0),
    // peerMaxFrame (>= 16384 per HTTP/2), fileSendBuffer capacity (always > 0).
    assert(chunkSize != 0 && "chunkSize cannot be 0 when all inputs are positive");

    _fileSendBuffer.clear();
    _fileSendBuffer.ensureAvailableCapacityExponential(chunkSize);

    const std::size_t readCount = pending.file.readAt(
        std::span<std::byte>(reinterpret_cast<std::byte*>(_fileSendBuffer.data()), chunkSize), pending.offset);
    if (readCount == 0 || readCount == File::kError) [[unlikely]] {
      log::error("HTTP/2 file payload short read at offset {}", pending.offset);
      return ErrorCode::InternalError;
    }

    _fileSendBuffer.setSize(readCount);
    const bool lastBodyChunk = (readCount >= pending.remaining);
    const bool endStream = lastBodyChunk && endStreamAfterBody;

    const auto bytes =
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(_fileSendBuffer.data()), _fileSendBuffer.size());
    // sendData cannot fail here: stream is valid (asserted above), flow control windows
    // are sufficient (chunkSize bounded by both), and stream state allows sending.
    [[maybe_unused]] const ErrorCode err = _connection.sendData(streamId, bytes, endStream);
    assert(err == ErrorCode::NoError && "sendData failed despite valid stream and sufficient flow control");

    pending.offset += readCount;
    pending.remaining -= readCount;
  }

  return ErrorCode::NoError;
}

void Http2ProtocolHandler::flushPendingFileSends() {
  for (auto it = _pendingFileSends.begin(); it != _pendingFileSends.end();) {
    const uint32_t streamId = it->first;
    PendingFileSend& pending = it->second;

    // onStreamReset / onStreamClosed erase from _pendingFileSends before flush runs,
    // so all entries here have a live stream.
    assert(_connection.getStream(streamId) != nullptr && "pending file send references a dead stream");

    const bool endStreamAfterBody = pending.trailersData.empty();
    const ErrorCode err = sendPendingFileBody(streamId, pending, endStreamAfterBody);
    if (err != ErrorCode::NoError) [[unlikely]] {
      log::error("HTTP/2 failed to continue file payload on stream {}: {}", streamId, ErrorCodeName(err));
      _connection.sendRstStream(streamId, err);
      it = _pendingFileSends.erase(it);
      continue;
    }

    if (pending.remaining != 0) {
      ++it;
      continue;
    }

    // File + trailers is unreachable via the public HttpResponse API (trailerAddLine
    // requires an in-memory body, which file() clears), so endStreamAfterBody is always true.
    assert(endStreamAfterBody && "file + trailers is not supported by the current HttpResponse API");

    it = _pendingFileSends.erase(it);
  }
}

void Http2ProtocolHandler::flushPendingStreamingSends() {
  for (auto it = _pendingStreamingSends.begin(); it != _pendingStreamingSends.end();) {
    const uint32_t streamId = it->first;
    PendingStreamingSend& pending = it->second;

    // onStreamReset / onStreamClosed erase from _pendingStreamingSends before flush runs,
    // so all entries here have a live stream.
    assert(_connection.getStream(streamId) != nullptr && "pending streaming send references a dead stream");

    // Try to send buffered body data.
    while (pending.offset < pending.buffer.size()) {
      // Stream cannot vanish during synchronous output buffer writes.
      auto* stream = _connection.getStream(streamId);
      assert(stream != nullptr && "stream disappeared during synchronous streaming send loop");

      const int32_t streamWin = stream->sendWindow();
      const int32_t connWin = _connection.connectionSendWindow();
      if (streamWin <= 0 || connWin <= 0) {
        // Flow control still blocked — wait for WINDOW_UPDATE.
        break;
      }

      const std::size_t remaining = pending.buffer.size() - pending.offset;
      const auto maxFrame = static_cast<std::size_t>(_connection.peerSettings().maxFrameSize);
      const auto chunkSize =
          std::min({remaining, static_cast<std::size_t>(streamWin), static_cast<std::size_t>(connWin), maxFrame});

      // All min() inputs are > 0: remaining (loop condition), streamWin/connWin (checked > 0),
      // maxFrame (>= 16384 per HTTP/2).
      assert(chunkSize != 0 && "chunkSize cannot be 0 when all inputs are positive");

      const bool lastBodyChunk = (pending.offset + chunkSize >= pending.buffer.size());
      const bool endStream = lastBodyChunk && pending.trailersData.empty();

      const auto bytes = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(pending.buffer.data() + pending.offset), chunkSize);
      // sendData cannot fail: stream is valid (asserted), flow control windows are sufficient
      // (chunkSize bounded by both), and stream state allows sending.
      [[maybe_unused]] const ErrorCode err = _connection.sendData(streamId, bytes, endStream);
      assert(err == ErrorCode::NoError && "sendData failed despite valid stream and sufficient flow control");

      pending.offset += chunkSize;
    }

    // Check if all body data has been sent.
    if (pending.offset < pending.buffer.size()) {
      ++it;
      continue;
    }

    // Body fully sent. Send trailers if present.
    if (!pending.trailersData.empty()) {
      HeadersView tv(std::string_view{pending.trailersData.data(), pending.trailersData.size()});
      // sendHeaders cannot fail: stream is valid (asserted) and in correct state
      // (we just successfully sent DATA on it).
      [[maybe_unused]] const auto sendErr =
          _connection.sendHeaders(streamId, http::StatusCode{}, tv, /*endStream=*/true);
      assert(sendErr == ErrorCode::NoError && "sendHeaders failed for trailers on a valid stream");
    }

    it = _pendingStreamingSends.erase(it);
  }
}

void Http2ProtocolHandler::handleStreamingRequest(StreamRequestsMap::iterator it, const StreamingHandler& handler,
                                                  const CorsPolicy* pCorsPolicy,
                                                  std::span<const ResponseMiddleware> responseMiddleware) {
  const uint32_t streamId = it->first;
  HttpRequest& request = it->second.request;
  const bool isHead = (request.method() == http::Method::HEAD);

  // CORS preflight rejection
  if (pCorsPolicy != nullptr) {
    if (pCorsPolicy->wouldApply(request) == CorsPolicy::ApplyStatus::OriginDenied) {
      HttpResponse corsResp(http::StatusCodeForbidden);
      corsResp.body("Forbidden by CORS policy");
      ApplyResponseMiddleware(request, corsResp, responseMiddleware, _pRouter->globalResponseMiddleware(),
                              *_pTelemetryContext, false, {});
      if (pCorsPolicy != nullptr) {
        (void)pCorsPolicy->applyToResponse(request, corsResp);
      }
      request.prefinalizeHttpResponse(corsResp, *_pTelemetryContext);
      corsResp.finalizeForHttp2();
      [[maybe_unused]] const ErrorCode err = sendResponse(streamId, std::move(corsResp), isHead);
      assert(err == ErrorCode::NoError && "sendResponse cannot fail for small CORS rejection body");
      onRequestCompleted(request, http::StatusCodeForbidden);
      _streamRequests.erase(streamId);
      return;
    }
  }

  // Negotiate compression.
  // Note: encoding rejection (identity;q=0 with no alternatives) is already handled
  // in onHeadersDecodedReceived before the request reaches this point.
  Encoding compressionFormat = Encoding::none;
  if (!isHead) {
    auto encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
    auto negotiated = _pCompressionState->selector.negotiateAcceptEncoding(encHeader);
    assert(!negotiated.reject && "Encoding rejection should have been handled in onHeadersDecodedReceived");
    compressionFormat = negotiated.encoding;
  }

  const ConcatenatedHeaders* pGlobalHeaders =
      _pServerConfig->globalHeaders.empty() ? nullptr : &_pServerConfig->globalHeaders;

  // Create H2 transport and writer
  Http2WriterTransport transport(_connection, streamId, pGlobalHeaders);
  HttpResponseWriter writer(transport, request, compressionFormat, _pServerConfig->compression, *_pCompressionState,
                            _pServerConfig->globalHeaders.fullStringWithLastSep(), _pServerConfig->addTrailerHeader);

  // Apply response middleware to the writer's internal response (before handler call, for pre-set headers).
  // Note: CORS application is handled lazily by the writer through the transport.
  // Response middleware runs here too if needed? Actually, for streaming, response middleware runs in emitHeaders
  // via the transport... But Http2WriterTransport doesn't run middleware — that's an HTTP/1.1 concern.
  // For HTTP/2, we skip response middleware on streaming for now (TODO if needed).

  try {
    handler(request, writer);
  } catch (const std::exception& ex) {
    log::error("HTTP/2 streaming handler exception on stream {}: {}", streamId, ex.what());
  } catch (...) {
    log::error("HTTP/2 streaming handler unknown exception on stream {}", streamId);
  }
  if (!writer.finished()) {
    writer.end();
  }

  // Emit metrics for the streaming request (matching HTTP/1 behavior with StatusCodeOK)
  onRequestCompleted(request, http::StatusCodeOK);

  // Transfer any pending data from the transport to the protocol handler's deferred-send maps.
  if (transport.hasPendingFile()) {
    auto fileInfo = transport.extractPendingFile();
    PendingFileSend pendingFile;
    pendingFile.file = std::move(fileInfo.file);
    pendingFile.offset = fileInfo.offset;
    pendingFile.remaining = fileInfo.remaining;
    pendingFile.trailersData = transport.extractPendingTrailers();
    pendingFile.trailersView =
        HeadersView(std::string_view{pendingFile.trailersData.data(), pendingFile.trailersData.size()});
    _pendingFileSends[streamId] = std::move(pendingFile);
  } else if (transport.hasPendingData()) {
    PendingStreamingSend pending;
    pending.buffer = transport.extractPendingBuffer();
    pending.trailersData = transport.extractPendingTrailers();
    _pendingStreamingSends[streamId] = std::move(pending);
  }

  // Clean up stream request
  _streamRequests.erase(streamId);
}

bool Http2ProtocolHandler::applyRequestMiddleware(HttpRequest& request, uint32_t streamId, bool isHead, bool streaming,
                                                  const Router::RoutingResult& routingResult) {
  auto globalResult = RunRequestMiddleware(request, _pRouter->globalRequestMiddleware(),
                                           routingResult.requestMiddlewareRange, *_pTelemetryContext, streaming, {});
  if (globalResult.has_value()) {
    const CorsPolicy* pCorsPolicy = routingResult.pCorsPolicy;
    if (pCorsPolicy != nullptr) {
      (void)pCorsPolicy->applyToResponse(request, *globalResult);
    }
    request.prefinalizeHttpResponse(*globalResult, *_pTelemetryContext);
    globalResult->finalizeForHttp2();
    const auto middlewareStatus = globalResult->status();
    ErrorCode err = sendResponse(streamId, std::move(*globalResult), isHead);
    onRequestCompleted(request, middlewareStatus);
    if (err != ErrorCode::NoError) [[unlikely]] {
      log::error("HTTP/2 failed to send response on stream {}: {}", streamId, ErrorCodeName(err));
    }
    _streamRequests.erase(streamId);
    return true;
  }

  request.finalizeBeforeHandlerCall(routingResult.pathParams);

  return false;
}

void Http2ProtocolHandler::dispatchRequest(StreamRequestsMap::iterator it) {
  const uint32_t streamId = it->first;
  HttpRequest& request = it->second.request;

  // CONNECT requests establish a per-stream TCP tunnel (RFC 7540 §8.3).
  // Handle separately from normal request/response dispatch.
  if (request.method() == http::Method::CONNECT) {
    handleConnectRequest(streamId, request);
    _streamRequests.erase(streamId);
    return;
  }

  // Validate required pseudo-headers
  if (request.path().empty()) {
    log::error("HTTP/2 stream {} missing :path", streamId);
    _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
    _streamRequests.erase(streamId);
    return;
  }

  const Router::RoutingResult routingResult = _pRouter->match(request.method(), request.path());

  // Check path-specific HTTP/2 config
  if (routingResult.pathConfig.http2Enable == PathEntryConfig::Http2Enable::Disable) {
    [[maybe_unused]] ErrorCode err =
        sendResponse(streamId, HttpResponse(http::StatusCodeNotFound), /*isHeadMethod=*/false);
    assert(err == ErrorCode::NoError && "sendResponse cannot fail for empty 404 response");
    onRequestCompleted(request, http::StatusCodeNotFound);
    _streamRequests.erase(streamId);
    return;
  }

  const bool isHead = (request.method() == http::Method::HEAD);

  ErrorCode err = ErrorCode::NoError;
  http::StatusCode respStatusCode;

  // Dispatch to the callback provided by SingleHttpServer
  try {
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    // Check routing for async handlers before calling reply().
    // If an async handler is found and suspends, we defer the response.
    if (const auto* asyncHandler = routingResult.asyncRequestHandler(); asyncHandler != nullptr) {
      // Run request middleware before the async handler
      if (applyRequestMiddleware(request, streamId, isHead, false, routingResult)) {
        return;
      }

      if (startAsyncHandler(it, *asyncHandler, routingResult.pCorsPolicy, routingResult.responseMiddlewareRange)) {
        // Async handler is running; response will be sent later when it completes.
        return;
      }
      // startAsyncHandler returned false: handler completed synchronously, response already sent.
      return;
    }
#endif

    // Check routing for streaming handlers before calling reply().
    // Streaming handlers run synchronously via HttpResponseWriter + Http2WriterTransport.
    if (const auto* streamingHandler = routingResult.streamingHandler(); streamingHandler != nullptr) {
      // Run request middleware
      if (applyRequestMiddleware(request, streamId, isHead, true, routingResult)) {
        return;
      }

      handleStreamingRequest(it, *streamingHandler, routingResult.pCorsPolicy, routingResult.responseMiddlewareRange);
      return;
    }

    HttpResponse resp = reply(request, routingResult);

    request.prefinalizeHttpResponse(resp, *_pTelemetryContext);

    resp.finalizeForHttp2();

    respStatusCode = resp.status();
    err = sendResponse(streamId, std::move(resp), isHead);
  } catch (const std::exception& ex) {
    log::error("HTTP/2 dispatcher exception on stream {}: {}", streamId, ex.what());
    respStatusCode = http::StatusCodeInternalServerError;
    err = sendResponse(streamId, HttpResponse(respStatusCode, ex.what()),
                       /*isHeadMethod=*/false);
  } catch (...) {
    log::error("HTTP/2 unknown exception on stream {}", streamId);
    respStatusCode = http::StatusCodeInternalServerError;
    err = sendResponse(streamId, HttpResponse(respStatusCode, "Unknown error"),
                       /*isHeadMethod=*/false);
  }
  if (err != ErrorCode::NoError) [[unlikely]] {
    log::error("HTTP/2 failed to send response on stream {}: {}", streamId, ErrorCodeName(err));
  }

  onRequestCompleted(request, respStatusCode);

  // Clean up stream request
  _streamRequests.erase(streamId);
}

HttpResponse Http2ProtocolHandler::reply(HttpRequest& request, const Router::RoutingResult& routingResult) {
  // Determine active CORS policy for this route (if any)
  const CorsPolicy* pCorsPolicy = routingResult.pCorsPolicy;

  // Handle OPTIONS and TRACE via shared protocol-agnostic code
  // CONNECT is handled in dispatchRequest() before reply() is called
  if (request.method() == http::Method::OPTIONS || request.method() == http::Method::TRACE) {
    const SpecialMethodConfig config{
        .tracePolicy = _pServerConfig->traceMethodPolicy,
        .isTls = !request.scheme().empty() && request.scheme() == "https",
    };
    // Note: For HTTP/2, TRACE cannot echo raw request data (no wire format available)
    // So we pass empty requestData, which will result in 405 Method Not Allowed
    auto result = ProcessSpecialMethods(request, *_pRouter, config, pCorsPolicy, {});
    if (result) {
      if (pCorsPolicy != nullptr) {
        (void)pCorsPolicy->applyToResponse(request, *result);
      }
      return std::move(*result);
    }
    // Not handled (e.g., not a preflight), fall through to normal processing
  }

  // Execute request middleware chain
  auto requestMiddlewareRange = routingResult.requestMiddlewareRange;
  auto responseMiddlewareRange = routingResult.responseMiddlewareRange;

  // Streaming handlers are always intercepted in dispatchRequest() before reply() is called.
  assert(routingResult.streamingHandler() == nullptr);

  // Run global request middleware first
  auto globalResult = RunRequestMiddleware(request, _pRouter->globalRequestMiddleware(), requestMiddlewareRange,
                                           *_pTelemetryContext, false, {});
  if (globalResult.has_value()) {
    if (pCorsPolicy != nullptr) {
      (void)pCorsPolicy->applyToResponse(request, *globalResult);
    }
    return std::move(*globalResult);
  }

  // Helper to apply response middleware and CORS
  auto finalizeResponse = [&, responseMiddlewareRange](HttpResponse& resp) {
    ApplyResponseMiddleware(request, resp, responseMiddlewareRange, _pRouter->globalResponseMiddleware(),
                            *_pTelemetryContext, false, {});
    if (pCorsPolicy != nullptr) {
      (void)pCorsPolicy->applyToResponse(request, resp);
    }
  };

  request.finalizeBeforeHandlerCall(routingResult.pathParams);

  // Handle the request based on handler type
  if (const auto* reqHandler = routingResult.requestHandler(); reqHandler != nullptr) {
    HttpResponse resp = (*reqHandler)(request);
    finalizeResponse(resp);
    return resp;
  }
  HttpResponse resp(routingResult.methodNotAllowed ? http::StatusCodeMethodNotAllowed : http::StatusCodeNotFound);
  finalizeResponse(resp);
  return resp;
}

// ============================
// CONNECT tunnel methods
// ============================

void Http2ProtocolHandler::handleConnectRequest(uint32_t streamId, HttpRequest& request) {
  if (_tunnelBridge == nullptr) {
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeMethodNotAllowed, "CONNECT not supported"),
                       /*isHeadMethod=*/false);
    return;
  }

  // Per RFC 7540 §8.3, CONNECT uses :authority as the target (host:port).
  const std::string_view target = request.authority();
  const auto colonPos = target.rfind(':');
  if (colonPos == std::string_view::npos || colonPos == 0 || colonPos == target.size() - 1) {
    log::warn("HTTP/2 CONNECT stream {} malformed target: {}", streamId, target);
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeBadRequest, "Malformed CONNECT target"),
                       /*isHeadMethod=*/false);
    return;
  }

  const std::string_view host = target.substr(0, colonPos);
  const std::string_view portStr = target.substr(colonPos + 1);

  // this extra char is needed for setupTunnelConnection (should be added in onHeadersDecodedReceived when parsing
  // :authority)
  assert(*(portStr.data() + portStr.size()) == '\r');

  // Enforce CONNECT allowlist if configured.
  const auto& allowList = _pServerConfig->connectAllowlist();
  if (!allowList.empty() && !allowList.containsCI(host)) {
    log::info("HTTP/2 CONNECT stream {} target {} not in allowlist", streamId, target);
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeForbidden, "CONNECT target not allowed"),
                       /*isHeadMethod=*/false);
    return;
  }

  // Delegate TCP connection setup to the server (which owns the event loop).
  const auto upstreamFd = _tunnelBridge->setupTunnel(streamId, host, portStr);
  if (upstreamFd == kInvalidHandle) {
    log::warn("HTTP/2 CONNECT stream {} failed to connect to {}", streamId, target);
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeBadGateway, "Unable to connect to CONNECT target"),
                       /*isHeadMethod=*/false);
    return;
  }

  // Send 200 headers WITHOUT END_STREAM — the stream stays open for bidirectional DATA.
  [[maybe_unused]] ErrorCode err =
      _connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, /*endStream=*/false);
  assert(err == ErrorCode::NoError && "sendHeaders cannot fail for a just-opened CONNECT stream");

  // Track the tunnel mapping.
  _tunnelStreams[streamId] = upstreamFd;
  _tunnelUpstreams[upstreamFd] = streamId;

  log::debug("HTTP/2 CONNECT tunnel established on stream {} → {}", streamId, target);
}

void Http2ProtocolHandler::cleanupTunnel(uint32_t streamId) {
  auto it = _tunnelStreams.find(streamId);
  if (it == _tunnelStreams.end()) {
    return;
  }
  const auto upstreamFd = it->second;
  _tunnelUpstreams.erase(upstreamFd);
  _tunnelStreams.erase(it);

  _tunnelBridge->closeTunnel(upstreamFd);
}

ErrorCode Http2ProtocolHandler::injectTunnelData(uint32_t streamId, std::span<const std::byte> data) {
  return _connection.sendData(streamId, data, /*endStream=*/false);
}

void Http2ProtocolHandler::closeTunnelByUpstreamFd(NativeHandle upstreamFd) {
  auto it = _tunnelUpstreams.find(upstreamFd);
  if (it == _tunnelUpstreams.end()) {
    return;
  }
  const uint32_t streamId = it->second;
  _tunnelStreams.erase(streamId);
  _tunnelUpstreams.erase(it);

  // Send empty DATA with END_STREAM to gracefully close the tunnel stream.
  (void)_connection.sendData(streamId, {}, /*endStream=*/true);
}

void Http2ProtocolHandler::tunnelConnectFailed(uint32_t streamId) {
  auto it = _tunnelStreams.find(streamId);
  if (it != _tunnelStreams.end()) {
    _tunnelUpstreams.erase(it->second);
    _tunnelStreams.erase(it);
  }
  // Signal the tunnel failure per RFC 7540 §8.3 using CONNECT_ERROR.
  _connection.sendRstStream(streamId, ErrorCode::ConnectError);
}

bool Http2ProtocolHandler::isTunnelStream(uint32_t streamId) const noexcept {
  return _tunnelStreams.contains(streamId);
}

Http2ProtocolHandler::TunnelUpstreamsMap Http2ProtocolHandler::drainTunnelUpstreamFds() {
  auto ret = std::move(_tunnelUpstreams);
  _tunnelStreams.clear();
  _tunnelUpstreams.clear();
  return ret;
}

ErrorCode Http2ProtocolHandler::sendResponse(uint32_t streamId, HttpResponse response, bool isHeadMethod) {
  auto* pFilePayload = response.filePayloadPtr();

  // finalize Date header
  WriteCRLFDateHeader(SysClock::now(), response._data.data() + response.headersStartPos());

  const ConcatenatedHeaders* pGlobalHeaders = response._opts.isPrepared() ? nullptr : &_pServerConfig->globalHeaders;

  const auto trailersView = response.trailers();
  const bool hasTrailers = !isHeadMethod && (trailersView.begin() != trailersView.end());
  const bool hasFile = !isHeadMethod && pFilePayload != nullptr;
  const std::string_view bodyView = response.bodyInMemory();
  const bool hasBody = !isHeadMethod && (hasFile ? (pFilePayload->length != 0) : !bodyView.empty());

  // Determine when to set END_STREAM:
  // - On HEADERS if no body and no trailers
  // - On DATA if body present but no trailers
  // - On trailer HEADERS if trailers present
  const bool endStreamOnHeaders = !hasBody && !hasTrailers;
  const bool endStreamOnData = hasBody && !hasTrailers;

  // Send HEADERS frame (response headers)
  ErrorCode err = _connection.sendHeaders(streamId, response.status(), HeadersView(response.headersFlatViewWithDate()),
                                          endStreamOnHeaders, pGlobalHeaders);
  assert(err == ErrorCode::NoError && "sendHeaders cannot fail for a valid open stream");

  // Send DATA frame(s) with body if present.
  // For file payloads, stream the file content in chunks and respect flow control.
  if (hasBody) {
    if (hasFile) {
      PendingFileSend pending;
      pending.file = std::move(pFilePayload->file);
      pending.offset = pFilePayload->offset;
      pending.remaining = pFilePayload->length;

      // Pass 1 - try to send as much as possible now
      err = sendPendingFileBody(streamId, pending, endStreamOnData);
      if (err != ErrorCode::NoError) {
        return err;
      }

      if (pending.remaining != 0) {
        assert(!hasTrailers && "file + trailers is not supported by the current HttpResponse API");
        _pendingFileSends[streamId] = std::move(pending);
      } else {
        // File payload fully sent inline. Send trailers if present.
        // Note: hasFile && hasTrailers is currently unreachable via the public HttpResponse API
        // (trailerAddLine requires an in-memory body, which file() clears), but kept for
        // forward-compatibility if the API is relaxed in the future.
        assert(!hasTrailers && "file + trailers is not supported by the current HttpResponse API");
      }
    } else {
      // Note: sendData() already handles splitting into multiple frames based on peer's maxFrameSize
      const auto bytes = std::as_bytes(std::span<const char>(bodyView.data(), bodyView.size()));
      err = _connection.sendData(streamId, bytes, endStreamOnData);
      if (err != ErrorCode::NoError) {
        return err;
      }
    }
  }

  // Send trailers as a HEADERS frame with END_STREAM (RFC 9113 §8.1)
  if (hasTrailers && !hasFile) {
    // END_STREAM must be set on trailers
    err = _connection.sendHeaders(streamId, http::StatusCode{}, trailersView, true);
  }

  return err;
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS

bool Http2ProtocolHandler::startAsyncHandler(StreamRequestsMap::iterator it, const AsyncRequestHandler& handler,
                                             const CorsPolicy* pCorsPolicy,
                                             std::span<const ResponseMiddleware> responseMiddleware) {
  const uint32_t streamId = it->first;
  const bool isHead = (it->second.request.method() == http::Method::HEAD);

  // Prepare the pending task entry. We move the StreamRequest out of _streamRequests immediately
  // so the HttpRequest gets a stable memory address BEFORE we pass it by reference to the coroutine.
  auto& pendingRef = _pendingAsyncTasks[streamId];
  pendingRef.streamRequest = std::move(it->second);
  pendingRef.pCorsPolicy = pCorsPolicy;
  pendingRef.pResponseMiddleware = responseMiddleware.data();
  pendingRef.responseMiddlewareCount = responseMiddleware.size();
  pendingRef.isHead = isHead;
  pendingRef.suspended = false;

  _streamRequests.erase(it);

  // Install HTTP/2 async callback mechanism on the request NOW
  pendingRef.streamRequest.request._h2SuspendedFlag = &pendingRef.suspended;
  pendingRef.streamRequest.request._h2PostCallback = _asyncPostCallback;

  auto task = handler(pendingRef.streamRequest.request);
  if (!task.valid()) {
    log::error("HTTP/2 async handler returned invalid task on stream {} for path {}", streamId,
               pendingRef.streamRequest.request.path());
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeInternalServerError, "Async handler inactive"),
                       /*isHeadMethod=*/false);
    onRequestCompleted(pendingRef.streamRequest.request, http::StatusCodeInternalServerError);
    _pendingAsyncTasks.erase(streamId);
    return false;
  }

  pendingRef.task = std::move(task);

  // Resume the coroutine once (it starts suspended due to initial_suspend = suspend_always)
  pendingRef.task.resume();

  if (pendingRef.task.done()) {
    // Coroutine completed immediately (synchronous fast path)
    onAsyncTaskCompleted(streamId);
    return false;
  }

  if (pendingRef.suspended) {
    // Coroutine suspended on co_await (e.g., deferWork) — truly async.
    // It will be resumed later via resumeAsyncTaskByHandle when the callback fires.
    log::debug("HTTP/2 async handler suspended on stream {}", streamId);
    return true;
  }

  // All current RequestTask coroutines either complete immediately or suspend via deferWork.
  // A non-deferWork suspension point would indicate an unsupported awaitable.
  assert(pendingRef.task.done() && "coroutine suspended without deferWork — unsupported by current RequestTask design");

  onAsyncTaskCompleted(streamId);
  return false;
}

void Http2ProtocolHandler::resumeAsyncTask(uint32_t streamId) {
  auto it = _pendingAsyncTasks.find(streamId);
  // Only called from resumeAsyncTaskByHandle which just found the entry — cannot be absent.
  assert(it != _pendingAsyncTasks.end());

  PendingAsyncTask& pending = it->second;
  pending.suspended = false;

  while (!pending.task.done()) {
    pending.task.resume();
    if (pending.suspended && !pending.task.done()) {
      // Suspended again — wait for next callback
      return;
    }
  }

  onAsyncTaskCompleted(streamId);
}

void Http2ProtocolHandler::onAsyncTaskCompleted(uint32_t streamId) {
  auto it = _pendingAsyncTasks.find(streamId);
  // Called from startAsyncHandler (just inserted) or resumeAsyncTask (just found) — cannot be absent.
  assert(it != _pendingAsyncTasks.end());

  PendingAsyncTask& pending = it->second;
  HttpRequest& request = pending.streamRequest.request;
  const bool isHead = pending.isHead;
  ErrorCode err = ErrorCode::NoError;
  http::StatusCode respStatusCode{};

  try {
    HttpResponse resp = pending.task.runSynchronously();

    auto middlewareSpan =
        std::span<const ResponseMiddleware>(pending.pResponseMiddleware, pending.responseMiddlewareCount);
    ApplyResponseMiddleware(request, resp, middlewareSpan, _pRouter->globalResponseMiddleware(), *_pTelemetryContext,
                            false, {});
    if (pending.pCorsPolicy != nullptr) {
      (void)pending.pCorsPolicy->applyToResponse(request, resp);
    }

    request.prefinalizeHttpResponse(resp, *_pTelemetryContext);
    resp.finalizeForHttp2();

    respStatusCode = resp.status();
    err = sendResponse(streamId, std::move(resp), isHead);
  } catch (const std::exception& ex) {
    log::error("HTTP/2 async handler exception on stream {}: {}", streamId, ex.what());
    respStatusCode = http::StatusCodeInternalServerError;
    err = sendResponse(streamId, HttpResponse(respStatusCode, ex.what()),
                       /*isHeadMethod=*/false);
  } catch (...) {
    log::error("HTTP/2 async handler unknown exception on stream {}", streamId);
    respStatusCode = http::StatusCodeInternalServerError;
    err = sendResponse(streamId, HttpResponse(respStatusCode, "Unknown error"),
                       /*isHeadMethod=*/false);
  }

  if (err != ErrorCode::NoError) [[unlikely]] {
    log::error("HTTP/2 failed to send async response on stream {}: {}", streamId, ErrorCodeName(err));
  }

  onRequestCompleted(request, respStatusCode);

  _pendingAsyncTasks.erase(it);
}

bool Http2ProtocolHandler::resumeAsyncTaskByHandle(std::coroutine_handle<> handle) {
  const void* targetAddr = handle.address();
  for (auto& [streamId, pending] : _pendingAsyncTasks) {
    if (pending.task.coroutineAddress() == targetAddr) {
      pending.suspended = false;
      resumeAsyncTask(streamId);
      return true;
    }
  }
  return false;
}

#endif  // AERONET_ENABLE_ASYNC_HANDLERS

void Http2ProtocolHandler::onRequestCompleted(HttpRequest& request, http::StatusCode status) {
  request.end(status);
  if (_requestCompletionCallback) {
    _requestCompletionCallback(request, status);
  }
}

std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(const Http2Config& config, Router& router,
                                                             HttpServerConfig& serverConfig,
                                                             internal::ResponseCompressionState& compressionState,
                                                             internal::RequestDecompressionState& decompressionState,
                                                             tracing::TelemetryContext& telemetryContext,
                                                             RawChars& tmpBuffer, bool sendServerPrefaceForTls) {
  auto protocolHandler = std::make_unique<Http2ProtocolHandler>(config, router, serverConfig, compressionState,
                                                                decompressionState, telemetryContext, tmpBuffer);
  if (sendServerPrefaceForTls) {
    // For TLS ALPN "h2", the server must send SETTINGS immediately after TLS handshake
    protocolHandler->connection().sendServerPreface();
  }
  return protocolHandler;
}

}  // namespace aeronet::http2
