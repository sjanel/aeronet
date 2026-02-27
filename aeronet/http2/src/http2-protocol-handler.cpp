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
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-prefinalize.hpp"
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
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/tracing/tracer.hpp"

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

  // If the client granted more flow control (WINDOW_UPDATE), try to continue any pending file sends.
  // Only do this when we don't already have pending output to avoid unbounded buffering.
  if (!_connection.hasPendingOutput()) {
    flushPendingFileSends();
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
        req._pPath = storedValue.data();
        req._pathLength = SafeCast<decltype(req._pathLength)>(storedValue.size());
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
  cleanupTunnel(streamId);
}

void Http2ProtocolHandler::onStreamReset(uint32_t streamId, ErrorCode errorCode) {
  log::debug("HTTP/2 stream {} reset with error: {}", streamId, ErrorCodeName(errorCode));
  _streamRequests.erase(streamId);
  _pendingFileSends.erase(streamId);
  cleanupTunnel(streamId);
}

ErrorCode Http2ProtocolHandler::sendPendingFileBody(uint32_t streamId, PendingFileSend& pending,
                                                    bool endStreamAfterBody) {
  const uint32_t peerMaxFrame = _connection.peerSettings().maxFrameSize;

  while (pending.remaining != 0) {
    Http2Stream* stream = _connection.getStream(streamId);
    if (stream == nullptr) [[unlikely]] {
      return ErrorCode::StreamClosed;
    }

    const int32_t streamWin = stream->sendWindow();
    const int32_t connWin = _connection.connectionSendWindow();
    if (streamWin <= 0 || connWin <= 0) {
      return ErrorCode::NoError;  // wait for WINDOW_UPDATE
    }

    const auto windowLimit = static_cast<std::size_t>(std::min(streamWin, connWin));
    const std::size_t chunkSize = std::min({pending.remaining, windowLimit, static_cast<std::size_t>(peerMaxFrame),
                                            static_cast<std::size_t>(_fileSendBuffer.capacity())});
    if (chunkSize == 0) {
      return ErrorCode::NoError;
    }

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
    const ErrorCode err = _connection.sendData(streamId, bytes, endStream);
    if (err != ErrorCode::NoError) {
      return err;
    }

    pending.offset += readCount;
    pending.remaining -= readCount;
  }

  return ErrorCode::NoError;
}

void Http2ProtocolHandler::flushPendingFileSends() {
  for (auto it = _pendingFileSends.begin(); it != _pendingFileSends.end();) {
    const uint32_t streamId = it->first;
    PendingFileSend& pending = it->second;

    // Stream might have been closed/reset without callback ordering guarantees.
    if (_connection.getStream(streamId) == nullptr) {
      it = _pendingFileSends.erase(it);
      continue;
    }

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

    if (!endStreamAfterBody) {
      const auto sendErr = _connection.sendHeaders(streamId, http::StatusCode{}, pending.trailersView, true);
      if (sendErr != ErrorCode::NoError) {
        log::error("HTTP/2 failed to send trailers on stream {}: {}", streamId, ErrorCodeName(sendErr));
        _connection.sendRstStream(streamId, sendErr);
      }
    }

    it = _pendingFileSends.erase(it);
  }
}

void Http2ProtocolHandler::dispatchRequest(StreamRequestsMap::iterator it) {
  const uint32_t streamId = it->first;
  HttpRequest& request = it->second.request;

  // Validate required pseudo-headers
  if (request.path().empty() && request.method() != http::Method::CONNECT) {
    log::error("HTTP/2 stream {} missing :path", streamId);
    _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
    _streamRequests.erase(streamId);
    return;
  }

  // CONNECT requests establish a per-stream TCP tunnel (RFC 7540 §8.3).
  // Handle separately from normal request/response dispatch.
  if (request.method() == http::Method::CONNECT) {
    handleConnectRequest(streamId, request);
    _streamRequests.erase(streamId);
    return;
  }

  ErrorCode err = ErrorCode::NoError;

  // Dispatch to the callback provided by SingleHttpServer
  try {
    const bool isHead = (request.method() == http::Method::HEAD);
    HttpResponse resp = reply(request);

    internal::PrefinalizeHttpResponse(request, resp, isHead, *_pCompressionState, *_pTelemetryContext);

    resp.finalizeForHttp2();

    err = sendResponse(streamId, std::move(resp), isHead);
  } catch (const std::exception& ex) {
    log::error("HTTP/2 dispatcher exception on stream {}: {}", streamId, ex.what());
    err = sendResponse(streamId, HttpResponse(http::StatusCodeInternalServerError, ex.what()),
                       /*isHeadMethod=*/false);
  } catch (...) {
    log::error("HTTP/2 unknown exception on stream {}", streamId);
    err = sendResponse(streamId, HttpResponse(http::StatusCodeInternalServerError, "Unknown error"),
                       /*isHeadMethod=*/false);
  }
  if (err != ErrorCode::NoError) [[unlikely]] {
    log::error("HTTP/2 failed to send response on stream {}: {}", streamId, ErrorCodeName(err));
  }

  // Clean up stream request
  _streamRequests.erase(streamId);
}

HttpResponse Http2ProtocolHandler::reply(HttpRequest& request) {
  auto routingResult = _pRouter->match(request.method(), request.path());

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

  // Check path-specific HTTP/2 configuration
  const auto pathHttp2Mode = routingResult.pathConfig.http2Enable;
  if (pathHttp2Mode == PathEntryConfig::Http2Enable::Disable) {
    // HTTP/2 is explicitly disabled for this path
    return HttpResponse(http::StatusCodeNotFound);
  }

  // Execute request middleware chain
  auto requestMiddlewareRange = routingResult.requestMiddlewareRange;
  auto responseMiddlewareRange = routingResult.responseMiddlewareRange;

  // Run global request middleware first
  auto globalResult = RunRequestMiddleware(request, _pRouter->globalRequestMiddleware(), requestMiddlewareRange,
                                           *_pTelemetryContext, routingResult.streamingHandler() != nullptr, {});
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

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (const auto* asyncHandler = routingResult.asyncRequestHandler(); asyncHandler != nullptr) {
    // Async handlers: run the coroutine to completion synchronously
    // HTTP/2 streams are independent and don't block each other
    // TODO: make them really asynchronous
    auto task = (*asyncHandler)(request);
    if (task.valid()) {
      auto handle = task.release();
      while (!handle.done()) {
        handle.resume();
      }
      auto typedHandle = std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(handle.address());
      HttpResponse resp = std::move(typedHandle.promise().consume_result());
      typedHandle.destroy();
      finalizeResponse(resp);
      return resp;
    }
    log::error("HTTP/2 async handler returned invalid task for path {}", request.path());
    return {http::StatusCodeInternalServerError, "Async handler inactive"};
  }
#endif

  if (routingResult.streamingHandler() != nullptr) {
    // TODO: Streaming handlers not yet supported for HTTP/2
    // Full implementation requires:
    // 1. Create Http2ResponseWriter that emits HEADERS frame when headers are first sent
    // 2. Emit DATA frames for each writeBody() call (respecting peer's maxFrameSize)
    // 3. Handle HTTP/2 flow control (may need to pause/resume when window exhausted)
    // 4. Emit trailers as final HEADERS frame with END_STREAM
    // 5. Integrate with the event loop for backpressure handling
    log::warn("Streaming handlers not yet fully supported for HTTP/2, path: {}", request.path());
    HttpResponse resp(http::StatusCodeNotImplemented, "Streaming handlers not yet supported for HTTP/2");
    finalizeResponse(resp);
    return resp;
  }

  if (routingResult.methodNotAllowed) {
    HttpResponse resp(http::StatusCodeMethodNotAllowed);
    finalizeResponse(resp);
    return resp;
  }

  HttpResponse resp(http::StatusCodeNotFound);
  finalizeResponse(resp);
  return resp;
}

// ============================
// CONNECT tunnel methods
// ============================

void Http2ProtocolHandler::handleConnectRequest(uint32_t streamId, HttpRequest& request) {
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
  const int upstreamFd = _tunnelBridge->setupTunnel(streamId, host, portStr);
  if (upstreamFd < 0) {
    log::warn("HTTP/2 CONNECT stream {} failed to connect to {}", streamId, target);
    (void)sendResponse(streamId, HttpResponse(http::StatusCodeBadGateway, "Unable to connect to CONNECT target"),
                       /*isHeadMethod=*/false);
    return;
  }

  // Send 200 headers WITHOUT END_STREAM — the stream stays open for bidirectional DATA.
  ErrorCode err = _connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, /*endStream=*/false);
  if (err != ErrorCode::NoError) [[unlikely]] {
    log::error("HTTP/2 CONNECT stream {} failed to send 200 headers: {}", streamId, ErrorCodeName(err));
    _tunnelBridge->closeTunnel(upstreamFd);
    return;
  }

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
  const int upstreamFd = it->second;
  _tunnelUpstreams.erase(upstreamFd);
  _tunnelStreams.erase(it);

  _tunnelBridge->closeTunnel(upstreamFd);
}

ErrorCode Http2ProtocolHandler::injectTunnelData(uint32_t streamId, std::span<const std::byte> data) {
  return _connection.sendData(streamId, data, /*endStream=*/false);
}

void Http2ProtocolHandler::closeTunnelByUpstreamFd(int upstreamFd) {
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

  if (err != ErrorCode::NoError) {
    return err;
  }

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
        pending.trailersView = HeadersView(response.trailersFlatView());
        pending.trailersData = std::move(response._data);
        _pendingFileSends[streamId] = std::move(pending);
      } else if (hasTrailers) {
        err = _connection.sendHeaders(streamId, http::StatusCode{}, trailersView, true);
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
