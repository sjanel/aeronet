#include "aeronet/http2-protocol-handler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <ratio>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/connect-target.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
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
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-process-result-error-msg-strings.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/https-redirect.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/small-vector.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/tchars.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/uri-host.hpp"
#include "http2-writer-transport.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <variant>

#include "aeronet/request-task.hpp"
#endif

namespace aeronet::http2 {

// ============================
// Http2ProtocolHandler
// ============================

Http2ProtocolHandler::Http2ProtocolHandler(::aeronet::internal::ServerContext& ctx,
                                           ::aeronet::ConnectionState& connectionState, NativeHandle fd,
                                           std::unique_ptr<ITunnelBridge> tunnelBridge)
    : _connection(ctx.config.http2, true, &ctx.telemetry),
      _pCtx(&ctx),
      _pConnectionState(&connectionState),
      _tunnelBridge(std::move(tunnelBridge)),
      _fd(fd) {
  setupCallbacks();
}

Http2ProtocolHandler::~Http2ProtocolHandler() = default;

namespace {

// Largest buffer, and map, that a recycled request keeps: the ones that grew larger (for a large body or header block)
// are released, so that the requests kept by an idle connection stay small.
constexpr std::size_t kMaxRecycledBufferBytes = 16UL << 10U;  // 16 KiB
constexpr std::size_t kMaxRecycledMapBuckets = 64;

}  // namespace

Http2ProtocolHandler::StreamRequest::~StreamRequest() = default;

void Http2ProtocolHandler::StreamRequest::clear() noexcept {
  // Start over from a new request, giving it back the buckets of the maps of this one.
  HttpRequestView newRequest;
  for (const auto pMap : {
           &HttpRequestView::_headers,
           &HttpRequestView::_trailers,
           &HttpRequestView::_pathParams,
           &HttpRequestView::_queryParams,
       }) {
    SvToSvMap& map = request.*pMap;
    if (map.bucket_count() <= kMaxRecycledMapBuckets) {
      map.clear();
      (newRequest.*pMap).swap(map);
    }
  }
  request = std::move(newRequest);

  if (bodyBuffer.capacity() <= kMaxRecycledBufferBytes) {
    bodyBuffer.clear();
  } else {
    bodyBuffer = {};
  }
  declaredBodyLength = kUnknownBodyLength;
  if (storage.capacity() <= kMaxRecycledBufferBytes) {
    storage.clear();
  } else {
    storage.reset();
  }
  bodyContext = {};
}

Http2ProtocolHandler::Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept = default;
Http2ProtocolHandler& Http2ProtocolHandler::operator=(Http2ProtocolHandler&&) noexcept = default;

ProtocolProcessResult Http2ProtocolHandler::processInput(std::span<const std::byte> data) {
  auto result = _connection.processInput(data, _pCtx->config.maxOutboundBufferBytes);

  // If the client granted more flow control (WINDOW_UPDATE), try to continue any pending sends.
  // Only do this when we don't already have pending output to avoid unbounded buffering.
  if (_nbPendingSends != 0 && !_connection.hasPendingOutput()) {
    flushPendingSends();
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
      log::error("HTTP/2 protocol error: {} ({})", ErrorCodeName(result.errorCode),
                 ConvertProcessResultErrorMsgToSv(result.errorMsg));
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

http::Method ParseHttpMethod(std::string_view method) {
  assert(!method.empty());
  switch (method[0]) {
    case 'C':
      if (method == MethodToStr(http::Method::CONNECT)) {
        return http::Method::CONNECT;
      }
      return http::kMethodInvalid;
    case 'D':
      if (method == MethodToStr(http::Method::DELETE)) {
        return http::Method::DELETE;
      }
      return http::kMethodInvalid;
    case 'G':
      if (method == MethodToStr(http::Method::GET)) {
        return http::Method::GET;
      }
      return http::kMethodInvalid;
    case 'H':
      if (method == MethodToStr(http::Method::HEAD)) {
        return http::Method::HEAD;
      }
      return http::kMethodInvalid;
    case 'O':
      if (method == MethodToStr(http::Method::OPTIONS)) {
        return http::Method::OPTIONS;
      }
      return http::kMethodInvalid;
    case 'P':
      if (method == MethodToStr(http::Method::POST)) {
        return http::Method::POST;
      }
      if (method == MethodToStr(http::Method::PUT)) {
        return http::Method::PUT;
      }
      if (method == MethodToStr(http::Method::PATCH)) {
        return http::Method::PATCH;
      }
      return http::kMethodInvalid;
    case 'T':
      if (method == MethodToStr(http::Method::TRACE)) {
        return http::Method::TRACE;
      }
      return http::kMethodInvalid;
    default:
      return http::kMethodInvalid;
  }
}

}  // namespace

void Http2ProtocolHandler::onHeadersDecoded(uint32_t streamId, DecodedHeaders& decodedHeaders, bool endStream) {
  auto [it, inserted] = _streams.try_emplace(streamId);
  if (!inserted) {
    // A second HEADERS block on a stream that already has one carries request trailers (RFC 9113 §8.1).
    onTrailersReceived(it, decodedHeaders, endStream);
    return;
  }
  if (retainedOutboundBytes() >= _pCtx->config.maxOutboundBufferBytes) {
    log::debug("Rejecting HTTP/2 stream {} at connection outbound high-water mark: retained={} limit={}", streamId,
               retainedOutboundBytes(), _pCtx->config.maxOutboundBufferBytes);
    _streams.erase(it);
    _connection.sendRstStream(streamId, ErrorCode::RefusedStream);
    _pCtx->telemetry.counterAdd("aeronet.http2.responses.rejected_outbound_limit");
    return;
  }
  StreamState& state = it->second;
  state.request = acquireRequest();
  StreamRequest& streamReq = *state.request;

  HttpRequestView& req = streamReq.request;

  req.init(_pCtx->config, _pCtx->compressionState);
  req._pConnectionInfo = _pConnectionState;
  req._addTrailerHeader = false;  // no trailer header in HTTP/2

  // RFC 9113 §8.3: request pseudo-headers are context-specific, unique (enforced while decoding), and mandatory
  // according to the request form. :authority is optional for ordinary requests because a target URI can legitimately
  // have no authority component; CONNECT is the exception and requires it instead of :scheme/:path.
  for (std::size_t pseudoHeaderIdx = 0; pseudoHeaderIdx < std::size(kPseudoHeaderNames); ++pseudoHeaderIdx) {
    const auto pseudoHeader = static_cast<PseudoHeader>(pseudoHeaderIdx);
    if (decodedHeaders.hasPseudoHeader(pseudoHeader) && decodedHeaders.pseudoHeader(pseudoHeader).empty()) {
      // There is no possible valid HTTP/2 request with an empty pseudo-header field value, so this is a protocol error.
      rejectMalformedRequest(streamId, "empty pseudo-header field value");
      return;
    }
  }
  if (decodedHeaders.hasPseudoHeader(PseudoHeader::Status) || decodedHeaders.hasPseudoHeader(PseudoHeader::Protocol)) {
    rejectMalformedRequest(streamId, "unexpected or unsupported pseudo-header field");
    return;
  }

  const std::string_view methodValue = decodedHeaders.pseudoHeader(PseudoHeader::Method);
  if (!std::ranges::all_of(methodValue, [](char ch) { return is_tchar(ch); })) {
    rejectMalformedRequest(streamId, "invalid :method pseudo-header field");
    return;
  }
  if (methodValue.empty()) {
    rejectMalformedRequest(streamId, "missing :method pseudo-header field");
    return;
  }

  const http::Method parsedMethod = ParseHttpMethod(methodValue);
  const bool isHeadMethod = parsedMethod == http::Method::HEAD;
  if (parsedMethod == http::kMethodInvalid) {
    // Unknown extension methods are syntactically valid but unsupported by aeronet.
    log::debug("Unknown HTTP method received: {}", methodValue);
    (void)sendResponse(streamId, req.makeResponse(http::StatusCodeNotImplemented, "Unsupported HTTP method"),
                       isHeadMethod);
    releaseStreamAfterResponse(it);
    return;
  }

  const std::string_view schemeValue = decodedHeaders.pseudoHeader(PseudoHeader::Scheme);
  const std::string_view authorityValue = decodedHeaders.pseudoHeader(PseudoHeader::Authority);
  const std::string_view pathValue = decodedHeaders.pseudoHeader(PseudoHeader::Path);
  const bool isConnect = parsedMethod == http::Method::CONNECT;
  if (isConnect) {
    if (authorityValue.empty() || !schemeValue.empty() || !pathValue.empty()) {
      rejectMalformedRequest(streamId, "CONNECT requires :authority and forbids :scheme and :path");
      return;
    }
  } else if (schemeValue.empty() || pathValue.empty()) {
    rejectMalformedRequest(streamId, "missing or invalid :scheme or :path pseudo-header field");
    return;
  }

  // The request takes the decoded headers and their bytes over, without copying them: the decoder is given the
  // (empty) map and storage of the request instead, which keep the capacity of the previous requests.
  req._headers.swap(decodedHeaders.headers);
  streamReq.storage.swap(decodedHeaders.storage);

  req._method = parsedMethod;
  req._headSpanSize = decodedHeaders.namesAndValuesSize;
  req._pAuthority = authorityValue.data();
  req._authorityLength = SafeCast<decltype(req._authorityLength)>(authorityValue.size());
  req._pScheme = schemeValue.data();
  req._schemeLength = SafeCast<decltype(req._schemeLength)>(schemeValue.size());

  if (!isConnect) {
    // Split :path at '?' to separate path from query string, mirroring HTTP/1.1 parsing. The decoder stores its value
    // in the storage now owned by the request (never in the static table), so in-place decoding is safe.
    char* pPath = const_cast<char*>(pathValue.data());
    if (!req.decodePath(pPath, pPath + pathValue.size())) {
      (void)sendResponse(
          streamId,
          req.makeResponse(
              http::StatusCodeBadRequest,
              "Invalid :path header - unable to decode percent-encoded characters or path is not valid UTF-8"),
          isHeadMethod);
      releaseStreamAfterResponse(it);
      return;
    }
  }

  // As over HTTP/1 (RFC 9112 §3.2), a request is refused when its host, from which the https redirect and the handlers
  // build URLs, is not uri-host [ ":" port ] (RFC 9110 §7.2): in its :authority (absent when empty, see above) or its
  // Host header field, which a request may send instead (RFC 9113 §8.3.1).
  if (!http::IsValidHostFieldValue(authorityValue) ||
      !http::IsValidHostFieldValue(req.headerValueOrEmpty(http::Host))) {
    (void)sendResponse(streamId, req.makeResponse(http::StatusCodeBadRequest, "Invalid :authority or Host header"),
                       isHeadMethod);
    releaseStreamAfterResponse(it);
    return;
  }

  // RFC 9113 §8.1.1: a request is malformed when its content-length is not a number, or differs from the sum of the
  // lengths of its DATA frames, checked as they arrive (none follow END_STREAM). CONNECT has no content: its DATA
  // frames carry the tunneled bytes.
  std::size_t declaredBodyLength = StreamRequest::kUnknownBodyLength;
  bool declaredBodyTooLarge = false;
  const auto contentLengthValue = req.headerValue(http::ContentLength);
  if (contentLengthValue && !isConnect) {
    const char* const pContentLengthBeg = contentLengthValue->data();
    const char* const pContentLengthEnd = pContentLengthBeg + contentLengthValue->size();
    const auto [ptr, errc] = std::from_chars(pContentLengthBeg, pContentLengthEnd, declaredBodyLength);
    if (errc == std::errc::result_out_of_range) {
      declaredBodyTooLarge = true;
    } else if (errc != std::errc{} || ptr != pContentLengthEnd) {
      rejectMalformedRequest(streamId, "invalid content-length");
      return;
    } else {
      declaredBodyTooLarge = declaredBodyLength > _pCtx->config.maxBodyBytes;
    }
    if (endStream && (declaredBodyTooLarge || declaredBodyLength != 0)) {
      rejectMalformedRequest(streamId, "non-zero content-length on a request without content");
      return;
    }
  }

  req._streamId = streamId;
  req._version = http::HTTP_2_0;
  // Event loop time of the read that delivered these headers (as for HTTP/1): no clock read per stream.
  req._reqStart = _pConnectionState->lastActivity;

  // Create trace span for HTTP/2 request (mirrors HTTP/1 span from initTrySetHead)
  req._traceSpan = _pCtx->telemetry.createSpan("http.request");
  if (req._traceSpan) {
    req._traceSpan->setAttribute("http.method", http::MethodToStr(req._method));
    req._traceSpan->setAttribute("http.target", req.path());
    req._traceSpan->setAttribute("http.scheme", req.scheme());
    req._traceSpan->setAttribute("http.host", req.authority());
  }

  // Automatic HTTP -> HTTPS redirect of the requests of a plaintext listener (h2c), as for HTTP/1.1.
  if (_pCtx->config.httpsRedirect.enabled()) {
    answerHttpsRedirect(it, endStream);
    return;
  }

  const auto [encoding, reject] =
      _pCtx->compressionState.selector.negotiateAcceptEncoding(req.headerValueOrEmpty(http::AcceptEncoding));
  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    (void)sendResponse(streamId,
                       req.makeResponse(http::StatusCodeNotAcceptable, "No acceptable content-coding available"),
                       isHeadMethod);
    req.end(http::StatusCodeNotAcceptable);
    releaseStreamAfterResponse(it);
    return;
  }
  req._responsePossibleEncoding = encoding;

  // CONNECT requests must be dispatched immediately after headers regardless of endStream,
  // because CONNECT has no request body - subsequent DATA frames carry tunnel payload.
  if (endStream || req.method() == http::Method::CONNECT) {
    dispatchRequest(it);
    return;
  }

  // A declared body above the server limit is refused before it is received. A smaller one is allocated at once, so
  // that its DATA frames are appended without reallocating (and copying) the body as it grows. The allocation is capped
  // by the connection receive window: a client declaring a large body without sending it allocates no more.
  if (declaredBodyTooLarge) {
    rejectTooLargeBody(it);
  } else if (declaredBodyLength != StreamRequest::kUnknownBodyLength) {
    streamReq.declaredBodyLength = declaredBodyLength;
    streamReq.bodyBuffer.reserve(
        std::min(declaredBodyLength, static_cast<std::size_t>(_connection.localSettings().connectionWindowSize)));
  }
}

void Http2ProtocolHandler::onData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  const auto it = _streams.find(streamId);
  if (it == _streams.end()) [[unlikely]] {
    log::warn("HTTP/2 DATA frame for unknown stream {}", streamId);
    return;
  }

  StreamState& state = it->second;

  // A CONNECT waiting for its target resolution: keep the data until the tunnel is established. Its receive window is
  // held meanwhile (see handleConnectRequest()), which bounds this buffer by the initial stream window.
  if (state.tunnelPending) {
    state.request->bodyBuffer.append(reinterpret_cast<const char*>(data.data()), data.size());
    state.tunnelPendingEndStream = state.tunnelPendingEndStream || endStream;
    return;
  }

  // Check if this is a CONNECT tunnel stream - forward data to upstream.
  if (state.tunnelUpstreamFd != kInvalidHandle) {
    // A failed write closes the upstream, and the stream with it: 'state' must not be used after the write.
    const NativeHandle upstreamFd = state.tunnelUpstreamFd;
    if (!data.empty() && !_tunnelBridge->writeTunnel(upstreamFd, data)) {
      // The upstream does not keep up: no more credit for the client until it wrote these bytes.
      _connection.holdStreamRecvWindow(streamId);
    }
    if (endStream) {
      // Client closed their end of the tunnel - half-close the upstream side.
      _tunnelBridge->shutdownTunnelWrite(upstreamFd);
    }
    return;
  }

  if (!state.hasRequest()) [[unlikely]] {
    log::warn("HTTP/2 DATA frame for stream {} with no active request", streamId);
    return;
  }

  StreamRequest& streamReq = *state.request;

  // The body is bounded while it is received, by the server limit: the route limit, which cannot exceed it, is
  // checked once the request is routed.
  if (streamReq.bodyBuffer.size() + data.size() > _pCtx->config.maxBodyBytes) {
    rejectTooLargeBody(it);
    return;
  }
  if (streamReq.bodyBuffer.size() + data.size() > streamReq.declaredBodyLength) {
    rejectMalformedRequest(streamId, "request body longer than its content-length");
    return;
  }

  // Accumulate body data
  streamReq.bodyBuffer.append(reinterpret_cast<const char*>(data.data()), data.size());

  // When the client attached request trailers, END_STREAM rides the trailing HEADERS block instead of the
  // final DATA frame; the body is then finalized and dispatched from onTrailersReceived.
  if (endStream) {
    finalizeRequestBodyAndDispatch(it);
  }
}

void Http2ProtocolHandler::rejectTooLargeBody(StreamsMap::iterator it) {
  const uint32_t streamId = it->first;
  HttpRequestView& request = it->second.request->request;
  (void)sendResponse(streamId, request.makeResponse(http::StatusCodePayloadTooLarge),
                     request.method() == http::Method::HEAD);
  onRequestCompleted(request, http::StatusCodePayloadTooLarge);
  // A response without body is never deferred: the stream entry is released.
  assert(!it->second.pending);
  releaseStreamAfterResponse(it);
  // The response is complete while the client still sends the request: ask it to stop, without error (RFC 9113 §8.1).
  _connection.sendRstStream(streamId, ErrorCode::NoError);
}

void Http2ProtocolHandler::rejectMalformedRequest(uint32_t streamId, std::string_view reason) {
  log::warn("Malformed HTTP/2 request on stream {}: {}", streamId, reason);
  _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
}

void Http2ProtocolHandler::finalizeRequestBodyAndDispatch(StreamsMap::iterator it) {
  const uint32_t streamId = it->first;
  StreamRequest& streamReq = *it->second.request;
  HttpRequestView& req = streamReq.request;

  // A longer body was already rejected while received.
  assert(streamReq.bodyBuffer.size() <= streamReq.declaredBodyLength);
  if (streamReq.declaredBodyLength != StreamRequest::kUnknownBodyLength &&
      streamReq.bodyBuffer.size() != streamReq.declaredBodyLength) {
    rejectMalformedRequest(streamId, "request body shorter than its content-length");
    return;
  }

  // Set body on HttpRequestView, applying any request-body decompression.
  req._body = streamReq.bodyBuffer;

  if (!req._body.empty()) {
    const auto res = HttpCodec::MaybeDecompressRequestBody(_pCtx->decompressionState, _pCtx->config.decompression, req,
                                                           streamReq.bodyBuffer, _pCtx->sharedBuffers.buf);
    if (res.message != nullptr) {
      const bool isHeadMethod = req.method() == http::Method::HEAD;
      (void)sendResponse(streamId, req.makeResponse(res.status, res.message), isHeadMethod);
      releaseStreamAfterResponse(it);
      return;
    }
  }

  // The whole body is received: let the handlers read it with body() as well as with readBody() / readBodyAsync().
  req.installAggregatedBodyBridge(streamReq.bodyContext);

  dispatchRequest(it);
}

void Http2ProtocolHandler::onTrailersReceived(StreamsMap::iterator it, DecodedHeaders& trailers, bool endStream) {
  const uint32_t streamId = it->first;
  StreamState& state = it->second;

  // Trailers are only meaningful for a stream still aggregating a normal (non-CONNECT) request, and a
  // trailing HEADERS block MUST carry END_STREAM (RFC 9113 §8.1). Anything else is a protocol error: a
  // second HEADERS block on a tunnel stream, on a stream whose request was already dispatched, or one that
  // fails to close the stream. sendRstStream fires onStreamReset, which erases the stream entry (and cleans
  // up any tunnel); the follow-up erase-by-key is a harmless no-op that keeps the intent explicit.
  // No need to add a || state.tunnelUpstreamFd != kInvalidHandle because for successfull CONNECT request,
  // state.hasRequest() == false.
  assert(state.tunnelUpstreamFd == kInvalidHandle || !state.hasRequest());
  if (!state.hasRequest() || !endStream) {
    _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
    _streams.erase(streamId);
    return;
  }

  // Pseudo-header fields are forbidden in trailers (RFC 9113 §8.1).
  if (trailers.hasPseudoHeaders()) {
    _connection.sendRstStream(streamId, ErrorCode::ProtocolError);
    _streams.erase(streamId);
    return;
  }

  // The request takes the decoded trailers over without copying them, like its headers (see onHeadersDecoded()): their
  // map is swapped, and the blocks of their storage join the storage of the request, after the bytes of its headers.
  StreamRequest& streamReq = *state.request;
  HttpRequestView& req = streamReq.request;
  req._trailers.swap(trailers.headers);
  streamReq.storage.splice(trailers.storage);

  // Trailers count toward the request head-size budget, combined with the initial headers.
  req._headSpanSize += trailers.namesAndValuesSize;

  finalizeRequestBodyAndDispatch(it);
}

void Http2ProtocolHandler::onStreamClosed(uint32_t streamId) {
  if (const auto it = _streams.find(streamId); it != _streams.end()) {
    // Clean up tunnel state for a given stream (using the consolidated StreamState).
    const auto upstreamFd = it->second.tunnelUpstreamFd;
    if (upstreamFd != kInvalidHandle) {
      _tunnelUpstreams.erase(upstreamFd);
      it->second.tunnelUpstreamFd = kInvalidHandle;

      _tunnelBridge->closeTunnel(upstreamFd);
    }
    releasePendingSend(it->second);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    orphanAsyncTask(it->second);
#endif
    recycleRequest(it->second.request);
    _streams.erase(it);
  }

  // The last stream served after the GOAWAY of the connection (see Http2Config::maxStreamsPerConnection): the
  // connection closes once its output is written.
  if (_connection.goAwayCompleted()) {
    _pConnectionState->requestDrainAndClose();
  }
}

void Http2ProtocolHandler::onWindowUpdate(uint32_t streamId, [[maybe_unused]] uint32_t increment) {
  if (streamId == 0) {
    // Connection-level window update: notify all tunnel streams. A notification may close its tunnel (its upstream
    // ended), erasing it from _tunnelUpstreams: they are notified from a copy of their upstream fds.
    if (_tunnelUpstreams.empty()) {
      return;
    }
    SmallVector<NativeHandle, 16U> upstreamFds;
    for (const auto& entry : _tunnelUpstreams) {
      upstreamFds.push_back(entry.first);
    }
    for (const NativeHandle upstreamFd : upstreamFds) {
      if (_tunnelUpstreams.contains(upstreamFd)) {
        _tunnelBridge->onTunnelWindowUpdate(upstreamFd);
      }
    }
  } else {
    // Stream-level window update
    auto it = _streams.find(streamId);
    if (it != _streams.end() && it->second.tunnelUpstreamFd != kInvalidHandle) {
      _tunnelBridge->onTunnelWindowUpdate(it->second.tunnelUpstreamFd);
    }
  }
}

template <class PendingSend>
void Http2ProtocolHandler::deferSend(StreamState& state, PendingSend&& pendingSend) {
  assert(!state.hasPendingSend());
  state.pending = _pendingWorkPool.allocateAndConstructPoolPtr(std::forward<PendingSend>(pendingSend));
  ++_nbPendingSends;
}

void Http2ProtocolHandler::releasePendingSend(const StreamState& state) noexcept {
  if (state.fileSend() != nullptr) {
    --_nbPendingSends;
  } else if (const PendingStreamingSend* pending = state.streamingSend(); pending != nullptr) {
    --_nbPendingSends;
    const std::size_t retained = pending->body.size() + pending->trailersData.size();
    assert(retained <= _deferredOutputBytes);
    _deferredOutputBytes -= retained;
  }
}

Http2ProtocolHandler::StreamRequestPtr Http2ProtocolHandler::acquireRequest() {
  if (_recycledRequests.empty()) {
    return std::make_unique<StreamRequest>();
  }
  return _recycledRequests.pop_back_val();
}

void Http2ProtocolHandler::recycleRequest(StreamRequestPtr& pRequest) {
  if (!pRequest) {
    return;
  }
  // The limit may have been lowered by a configuration update: the requests kept beyond it are reused first.
  if (_recycledRequests.size() >= _pCtx->config.http2.maxRecycledRequests) {
    pRequest.reset();
    return;
  }
  pRequest->clear();
  _recycledRequests.push_back(std::move(pRequest));
}

void Http2ProtocolHandler::releaseStreamAfterResponse(StreamsMap::iterator it) {
  assert(it != _streams.end());
  recycleRequest(it->second.request);
  if (it->second.pending) {
    it->second.requestDeadline = {};
  } else {
    const uint32_t streamId = it->first;
    _streams.erase(it);

    // Sending END_STREAM transitions a half-closed-remote stream to Closed, but send-path closure is finalized
    // explicitly so callbacks cannot invalidate protocol-handler iterators mid-send. At this point our state has
    // already been released, so close accounting and the callback are safe.
    _connection.finalizeSendClosedStream(streamId);
  }
}

std::size_t Http2ProtocolHandler::fileReadBudget(std::size_t nbFileSends) const noexcept {
  // Files are read ahead of the socket only while the output retained by the connection is under half of its limit,
  // whatever the credit granted by the client (h2load grants 1 GiB): the other streams keep room for their responses
  // (new streams are refused, and responses answered 503, at the limit). The file sends share this room, each reading
  // at least a DATA frame of the default maximum size, so that all of them progress.
  static constexpr std::size_t kMinFileRead = 16UL << 10U;  // 16 KiB
  assert(nbFileSends != 0);
  const std::size_t readAheadLimit = _pCtx->config.maxOutboundBufferBytes / 2U;
  const std::size_t retained = retainedOutboundBytes();
  const std::size_t room = retained < readAheadLimit ? readAheadLimit - retained : 0;
  return std::max(room / nbFileSends, kMinFileRead);
}

ErrorCode Http2ProtocolHandler::sendPendingFileBody(uint32_t streamId, FilePayload& pending, bool endStreamAfterBody,
                                                    std::size_t maxBytes) {
  // Largest read of the file: its bytes are sent in as many DATA frames as needed, sharing its allocation. Reading
  // frame by frame would cost a read syscall and an allocation per 16 KiB (the default maximum frame size).
  static constexpr std::size_t kMaxReadSize = 256UL << 10U;  // 256 KiB

  while (pending.length != 0 && maxBytes != 0) {
    Http2Stream* pStream = _connection.getStream(streamId);
    // Stream cannot vanish during synchronous output buffer writes - only processInput
    // (which is not re-entered) can close/reset streams.
    assert(pStream != nullptr && "stream disappeared during synchronous file send loop");

    const int32_t streamWin = pStream->sendWindow();
    const int32_t connWin = _connection.connectionSendWindow();
    if (streamWin <= 0 || connWin <= 0) {
      break;  // wait for WINDOW_UPDATE
    }

    const auto windowLimit = static_cast<std::size_t>(std::min(streamWin, connWin));
    const std::size_t chunkSize = std::min({pending.length, windowLimit, kMaxReadSize, maxBytes});

    // All min() inputs are > 0: pending.length and maxBytes (loop condition), windowLimit (checked > 0).
    assert(chunkSize != 0 && "chunkSize cannot be 0 when all inputs are positive");

    RawBytes buf(chunkSize);

    const std::size_t readCount = pending.file.readAt(std::span<std::byte>(buf.data(), chunkSize), pending.offset);
    if (readCount == 0 || readCount == File::kError) [[unlikely]] {
      log::error("HTTP/2 file payload short read at offset {}", pending.offset);
      return ErrorCode::InternalError;
    }

    buf.setSize(readCount);

    const bool lastBodyChunk = (readCount >= pending.length);
    const bool endStream = lastBodyChunk && endStreamAfterBody;

    // sendData cannot fail here: stream is valid (asserted above), flow control windows
    // are sufficient (chunkSize bounded by both), and stream state allows sending.
    [[maybe_unused]] const ErrorCode err = _connection.sendData(streamId, std::move(buf), 0UL, readCount, endStream);
    assert(err == ErrorCode::NoError && "sendData failed despite valid stream and sufficient flow control");

    pending.offset += readCount;
    pending.length -= readCount;
    maxBytes -= readCount;
  }

  return ErrorCode::NoError;
}

void Http2ProtocolHandler::flushPendingSends() {
  // Only the streams up to the last one with a pending send are visited: the scan stops once all of them were. Erasing
  // an entry while iterating moves another one to its place only if it was not visited yet, so each entry is visited
  // once.
  std::size_t nbToVisit = _nbPendingSends;
  // The file sends share the read-ahead room (the streaming sends counted among them, which only lowers their share).
  const std::size_t fileReadShare = fileReadBudget(_nbPendingSends);
  for (auto it = _streams.begin(); nbToVisit != 0;) {
    assert(it != _streams.end());
    StreamState& state = it->second;
    if (!state.hasPendingSend()) {
      ++it;
      continue;
    }
    --nbToVisit;
    const uint32_t streamId = it->first;

    // onStreamReset / onStreamClosed erase from _streams before flush runs,
    // so all entries here have a live stream.
    assert(_connection.getStream(streamId) != nullptr && "pending send references a dead stream");

    bool sent;
    if (PendingFileSend* pFileSend = state.fileSend(); pFileSend != nullptr) {
      // File + trailers is unreachable via the public HttpResponse API (trailerAddLine
      // requires an in-memory body, which file() clears), so endStreamAfterBody is always true.
      constexpr bool endStreamAfterBody = true;
      const ErrorCode err = sendPendingFileBody(streamId, pFileSend->filePayload, endStreamAfterBody, fileReadShare);
      if (err != ErrorCode::NoError) [[unlikely]] {
        --_nbPendingSends;
        it = _streams.erase(it);
        _connection.sendRstStream(streamId, err);
        continue;
      }
      sent = pFileSend->filePayload.length == 0;
    } else {
      sent = sendPendingStreamingBody(streamId, *state.streamingSend());
    }
    if (!sent) {
      ++it;
      continue;
    }

    state.pending.reset();
    --_nbPendingSends;

    // Every call site that leaves a pending send (dispatchRequest, onAsyncTaskCompleted, handleStreamingRequest)
    // immediately follows up with releaseStreamAfterResponse(it), so it->second.hasRequest() is impossible.
    // CONNECT tunnels never have a pending send: tunnel bytes flow through injectTunnelData() → _connection.sendData()
    // directly, bypassing the deferred-send machinery entirely.
    assert(!state.hasRequest() && state.tunnelUpstreamFd == kInvalidHandle);

    // If the stream has no other active state, remove the entry entirely.
    it = _streams.erase(it);
    // The deferred body completed outside frame processing (from onOutputWritten): finalize the stream's
    // connection-level lifecycle (active-stream accounting + retention). Our per-stream state is
    // already erased, so the stream-closed callback finds nothing to re-enter.
    _connection.finalizeSendClosedStream(streamId);
  }
}

bool Http2ProtocolHandler::sendPendingStreamingBody(uint32_t streamId, PendingStreamingSend& pending) {
  // Try to send buffered body data.
  while (!pending.body.empty()) {
    // Stream cannot vanish during synchronous output buffer writes.
    Http2Stream* pStream = _connection.getStream(streamId);
    assert(pStream != nullptr && "stream disappeared during synchronous streaming send loop");

    const int32_t streamWin = pStream->sendWindow();
    const int32_t connWin = _connection.connectionSendWindow();
    if (streamWin <= 0 || connWin <= 0) {
      // Flow control still blocked - wait for WINDOW_UPDATE.
      return false;
    }

    const std::size_t remaining = pending.body.size();
    const auto chunkSize =
        std::min({remaining, static_cast<std::size_t>(streamWin), static_cast<std::size_t>(connWin)});

    // All min() inputs are positive: remaining is non-zero and both windows were checked above.
    assert(chunkSize != 0 && "chunkSize cannot be 0 when all inputs are positive");

    const bool lastBodyChunk = chunkSize == remaining;
    const bool endStream = lastBodyChunk && pending.trailersData.empty();

    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
    const auto chunk = std::as_bytes(std::span<const char>(pending.body.data(), chunkSize));
    [[maybe_unused]] ErrorCode err;
    if (lastBodyChunk) {
      // The DATA frames of the last chunk take over the storage (the body bytes do not move), the previous ones copy
      // their chunk, the storage having to outlive them.
      err = _connection.sendData(streamId, std::move(pending.storage), chunk, endStream);
    } else {
      err = _connection.sendData(streamId, chunk, endStream);
    }
    pending.body.remove_prefix(chunkSize);
    assert(chunkSize <= _deferredOutputBytes);
    _deferredOutputBytes -= chunkSize;
    // sendData cannot fail: stream is valid and chunkSize is bounded by both flow-control windows.
    assert(err == ErrorCode::NoError && "sendData failed despite valid stream and sufficient flow control");
  }

  // Body fully sent. Send trailers if present.
  if (!pending.trailersData.empty()) {
    const std::size_t trailersSize = pending.trailersData.size();
    HeadersView tv(std::string_view{pending.trailersData.data(), pending.trailersData.size()});
    // sendHeaders cannot fail: stream is valid (asserted) and in correct state
    // (we just successfully sent DATA on it).
    [[maybe_unused]] const auto sendErr = _connection.sendHeaders(streamId, http::StatusCode{}, tv, /*endStream=*/true);
    assert(sendErr == ErrorCode::NoError && "sendHeaders failed for trailers on a valid stream");
    assert(trailersSize <= _deferredOutputBytes);
    _deferredOutputBytes -= trailersSize;
  }
  return true;
}

void Http2ProtocolHandler::handleStreamingRequest(StreamsMap::iterator it, const Router::RoutingResult& routingResult) {
  static constexpr bool kStreaming = true;
  const uint32_t streamId = it->first;
  StreamState& state = it->second;
  HttpRequestView& request = state.request->request;
  const bool isHeadMethod = request.method() == http::Method::HEAD;

  // Create H2 transport and writer
  const RequestPipeline requestPipeline = _pCtx->requestPipeline();
  Http2WriterTransport transport(_connection, streamId, requestPipeline, routingResult, _pCtx->dateHeader.data(),
                                 _deferredOutputBytes, _pCtx->config.maxOutboundBufferBytes,
                                 _connection.localSettings().maxStreamPendingBytes);

  HttpMessage::Options opts;
  if (_pCtx->config.addTrailerHeader) {
    opts.addTrailerHeader();
  }
  if (isHeadMethod) {
    opts.setHeadMethod();
  }
  if (!_pCtx->config.http2.sendContentLengthHeader) {
    opts.setDoNotSendContentLengthHeader();
  }
  opts.setPrepared();

  // Negotiate compression has been done in onHeadersDecoded.
  HttpResponseWriter writer(transport, request, request.responsePossibleEncoding(), _pCtx->config.compression,
                            _pCtx->compressionState, _pCtx->config.globalHeaders.fullStringWithLastSep(), opts);

  bool handlerThrew = false;
  try {
    (*routingResult.streamingHandler())(request, writer);
  } catch (const std::exception& ex) {
    handlerThrew = true;
    log::error("HTTP/2 streaming handler exception on stream {}: {}", streamId, ex.what());
  } catch (...) {
    handlerThrew = true;
    log::error("HTTP/2 streaming handler unknown exception on stream {}", streamId);
  }
  if (handlerThrew) {
    if (writer.headersPending()) {
      // Nothing was sent: answer like for any handler throwing.
      HttpResponse response = request.makeResponse(http::StatusCodeInternalServerError);
      requestPipeline.applyResponseMiddleware(request, response, routingResult.postMiddlewareRange(), kStreaming);
      finalizeAndSendResponse(it, std::move(response), routingResult.corsPolicy());
      return;
    }
    onRequestCompleted(request, http::StatusCodeInternalServerError);
    // Part of the response was sent: reset the stream, so that the client knows that it is incomplete.
    _streams.erase(it);
    _connection.sendRstStream(streamId, ErrorCode::InternalError);
    return;
  }
  if (!writer.finished()) {
    writer.end();
  }

  // Emit metrics for the streaming request (matching HTTP/1 behavior with StatusCodeOK)
  onRequestCompleted(request, http::StatusCodeOK);

  if (transport.overflowed()) {
    log::warn("HTTP/2 streaming response exceeded pending-output limit on stream {}", streamId);
    _pCtx->telemetry.counterAdd("aeronet.http2.responses.rejected_outbound_limit");
    _streams.erase(it);
    _connection.sendRstStream(streamId, ErrorCode::EnhanceYourCalm);
    return;
  }
  if (writer.failed()) {
    // The response is incomplete (a compression or send error): reset the stream, as when the handler throws after the
    // head.
    _streams.erase(it);
    _connection.sendRstStream(streamId, ErrorCode::InternalError);
    return;
  }

  // Transfer any pending data from the transport to the stream's deferred-send state.
  if (transport.hasPendingFile()) {
    PendingFileSend pendingFile;
    pendingFile.filePayload = transport.extractPendingFile();
    deferSend(state, std::move(pendingFile));
  } else if (transport.hasPendingData()) {
    RawChars pendingBuffer = transport.extractPendingBuffer();
    // Moving the buffer below does not move its bytes.
    const std::string_view pendingBody = pendingBuffer;
    RawChars pendingTrailers = transport.extractPendingTrailers();
    _deferredOutputBytes += pendingBody.size() + pendingTrailers.size();
    deferSend(state, PendingStreamingSend{
                         .storage = HttpMessageData(std::move(pendingBuffer)),
                         .body = pendingBody,
                         .trailersData = std::move(pendingTrailers),
                     });
  }

  releaseStreamAfterResponse(it);
}

void Http2ProtocolHandler::dispatchRequest(StreamsMap::iterator it) {
  const uint32_t streamId = it->first;
  HttpRequestView& request = it->second.request->request;

  // CONNECT requests establish a per-stream TCP tunnel (RFC 7540 §8.3).
  // Handle separately from normal request/response dispatch.
  if (request.method() == http::Method::CONNECT) {
    handleConnectRequest(streamId, request);
    it = _streams.find(streamId);
    if (it->second.tunnelUpstreamFd != kInvalidHandle) {
      // A successful CONNECT remains active until both sides of the tunnel close.
      recycleRequest(it->second.request);
    } else if (!it->second.tunnelPending) {
      releaseStreamAfterResponse(it);
    }
    // A pending CONNECT keeps its request until onTunnelResolved().
    return;
  }

  const bool isHeadMethod = request.method() == http::Method::HEAD;

  // Validate required pseudo-headers
  assert(!request.path().empty() && "path should have been validated in onHeadersDecoded");

  const Router::RoutingResult routingResult = _pCtx->router.match(request.method(), request.path());

  // Check path-specific HTTP/2 config
  if (routingResult.pathConfig().http2Enable == PathEntryConfig::Http2Enable::Disable) {
    [[maybe_unused]] ErrorCode err =
        sendResponse(streamId, request.makeResponse(http::StatusCodeNotFound), isHeadMethod);
    assert(err == ErrorCode::NoError && "sendResponse cannot fail for empty 404 response");
    onRequestCompleted(request, http::StatusCodeNotFound);
    releaseStreamAfterResponse(it);
    return;
  }

  // Per-route request head size limit (already clamped against the global one, see Router::clampConfigs())
  if (request.headSpanSize() > routingResult.pathConfig().maxHeaderBytes) {
    [[maybe_unused]] ErrorCode err =
        sendResponse(streamId, request.makeResponse(http::StatusCodeRequestHeaderFieldsTooLarge), isHeadMethod);
    assert(err == ErrorCode::NoError);
    onRequestCompleted(request, http::StatusCodeRequestHeaderFieldsTooLarge);
    releaseStreamAfterResponse(it);
    return;
  }

  // Per-route body size limit (already clamped against the global one, see Router::clampConfigs())
  if (request._body.size() > routingResult.pathConfig().maxBodyBytes) {
    [[maybe_unused]] ErrorCode err =
        sendResponse(streamId, request.makeResponse(http::StatusCodePayloadTooLarge), isHeadMethod);
    assert(err == ErrorCode::NoError);
    onRequestCompleted(request, http::StatusCodePayloadTooLarge);
    releaseStreamAfterResponse(it);
    return;
  }

  // Arm per-route request deadline for sweep enforcement (async/streaming handlers).
  if (routingResult.pathConfig().requestTimeout != std::chrono::milliseconds::max()) {
    it->second.requestDeadline = request.reqStart() + routingResult.pathConfig().requestTimeout;
  }

  // Handle OPTIONS and TRACE via shared protocol-agnostic code
  if (request.method() == http::Method::OPTIONS || request.method() == http::Method::TRACE) {
    const SpecialMethodConfig config{
        .tracePolicy = _pCtx->config.traceMethodPolicy,
        // From the connection: the :scheme pseudo-header is chosen by the client.
        .isTls = !request.tlsVersion().empty(),
    };
    // Note: For HTTP/2, TRACE cannot echo raw request data (no wire format available)
    // So we pass empty requestData, which will result in 405 Method Not Allowed
    HttpResponse result = ProcessSpecialMethods(request, _pCtx->router, config, routingResult.corsPolicy(), {});
    if (!result.empty()) {
      finalizeAndSendResponse(it, std::move(result), routingResult.corsPolicy());
      return;
    }
    // Not handled (e.g., not a preflight), fall through to normal processing
  }

  const bool streaming = routingResult.streamingHandler() != nullptr;
  const RequestPipeline requestPipeline = _pCtx->requestPipeline();
  const auto sendPipelineResponse = [&](HttpResponse& response) {
    requestPipeline.applyResponseMiddleware(request, response, routingResult.postMiddlewareRange(), streaming);
    finalizeAndSendResponse(it, std::move(response), routingResult.corsPolicy());
  };
  if (HttpResponse shortCircuit = requestPipeline.runRequestMiddleware(request, routingResult, streaming);
      !shortCircuit.empty()) {
    sendPipelineResponse(shortCircuit);
    return;
  }
  if (HttpResponse response = RequestPipeline::CallRequestHandler(request, routingResult); !response.empty()) {
    sendPipelineResponse(response);
    return;
  }

  if (streaming) {
    handleStreamingRequest(it, routingResult);
    return;
  }
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Returns false when the handler completed synchronously, its response being already sent.
  (void)startAsyncHandler(it, routingResult);
#endif
}

void Http2ProtocolHandler::finalizeAndSendResponse(StreamsMap::iterator it, HttpResponse&& response,
                                                   const CorsPolicy* pCorsPolicy) {
  const uint32_t streamId = it->first;
  HttpRequestView& request = it->second.request->request;
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, response);
  }
  request.prefinalizeHttpResponse(response, _pCtx->telemetry);
  response.finalizeHeadersAndBody();

  const http::StatusCode status = response.status();
  const ErrorCode err = sendResponse(streamId, std::move(response), request.method() == http::Method::HEAD);
  if (err != ErrorCode::NoError) [[unlikely]] {
    log::error("HTTP/2 failed to send response on stream {}: {}", streamId, ErrorCodeName(err));
  }

  onRequestCompleted(request, status);

  releaseStreamAfterResponse(it);
}

void Http2ProtocolHandler::answerHttpsRedirect(StreamsMap::iterator it, bool endStream) {
  const uint32_t streamId = it->first;
  HttpRequestView& request = it->second.request->request;
  const bool isHeadMethod = request.method() == http::Method::HEAD;

  // Like for HTTP/1.1: bypasses routing, the request middleware and the body.
  RawChars& urlBuf = _pCtx->sharedBuffers.buf;
  HttpResponse response = ::aeronet::internal::HttpResponseAccess::MakeUnallocated();
  if (http::BuildHttpsRedirectUrl(urlBuf, request, _pCtx->config.httpsRedirect.targetPort)) {
    response = HttpResponse(_pCtx->config.httpsRedirect.statusCode);
    response.location(std::string_view(urlBuf));
  } else {
    // No (or empty) :authority and Host header: an absolute https URL cannot be constructed.
    response = request.makeResponse(http::StatusCodeBadRequest, "Missing Host header for HTTPS redirect");
  }
  const http::StatusCode status = response.status();
  [[maybe_unused]] const ErrorCode err = sendResponse(streamId, std::move(response), isHeadMethod);
  assert(err == ErrorCode::NoError && "sendResponse cannot fail for a small redirect response");
  onRequestCompleted(request, status);
  releaseStreamAfterResponse(it);
  if (!endStream) {
    // The response is complete while the client may still send the body: ask it to stop, without error (RFC 9113 §8.1).
    _connection.sendRstStream(streamId, ErrorCode::NoError);
  }
}

// ============================
// CONNECT tunnel methods
// ============================

void Http2ProtocolHandler::handleConnectRequest(uint32_t streamId, HttpRequestView& request) {
  constexpr bool isHeadMethod = false;

  // Per RFC 7540 §8.3, CONNECT uses :authority as the target (host:port).
  // authority-form requires a numeric port (RFC 9110 §9.3.6 / RFC 3986 port = *DIGIT). Reject anything
  // else (non-numeric or > 65535) up front with 400 instead of handing it to the resolver.
  const ConnectTarget target(request.authority());
  if (target.invalid()) {
    log::warn("HTTP/2 CONNECT stream {} malformed target: {}", streamId, request.authority());
    (void)sendResponse(streamId, request.makeResponse(http::StatusCodeBadRequest, "Malformed CONNECT target"),
                       isHeadMethod);
    return;
  }

  // CONNECT is disabled unless the target is explicitly allowlisted or unrestricted access was requested with "*".
  if (!_pCtx->config.connectTargetAllowed(target.host)) {
    log::info("HTTP/2 CONNECT stream {} target {} not in allowlist", streamId, request.authority());
    (void)sendResponse(streamId, request.makeResponse(http::StatusCodeForbidden, "CONNECT target not allowed"),
                       isHeadMethod);
    return;
  }

  // Delegate TCP connection setup to the server (which owns the event loop).
  if (_tunnelBridge == nullptr) {
    assert(_fd != kInvalidHandle && "the tunnels of a stream are bound to the socket of its connection");
    _tunnelBridge = ::aeronet::internal::TunnelManager::MakeH2Bridge(*_pCtx, _fd);
  }
  const auto setup = _tunnelBridge->setupTunnel(streamId, target.host, target.port);
  switch (setup.status) {
    case ITunnelBridge::TunnelSetup::Status::Established:
      establishTunnel(streamId, setup.upstreamFd);
      log::debug("HTTP/2 CONNECT tunnel established on stream {} → {}", streamId, request.authority());
      break;
    case ITunnelBridge::TunnelSetup::Status::Pending:
      // The server calls onTunnelResolved() once the target host name is resolved. Until then, the client may only
      // send the credit it has left.
      _streams[streamId].tunnelPending = true;
      _connection.holdStreamRecvWindow(streamId);
      break;
    case ITunnelBridge::TunnelSetup::Status::Overloaded:
      log::warn("HTTP/2 CONNECT stream {} refused: too many targets being resolved", streamId);
      (void)sendResponse(
          streamId, request.makeResponse(http::StatusCodeServiceUnavailable, "Too many CONNECT targets being resolved"),
          isHeadMethod);
      break;
    default:
      assert(setup.status == ITunnelBridge::TunnelSetup::Status::Failed);
      log::warn("HTTP/2 CONNECT stream {} failed to connect to {}", streamId, request.authority());
      (void)sendResponse(streamId,
                         request.makeResponse(http::StatusCodeBadGateway, "Unable to connect to CONNECT target"),
                         isHeadMethod);
      break;
  }
}

void Http2ProtocolHandler::establishTunnel(uint32_t streamId, NativeHandle upstreamFd) {
  // Send 200 headers WITHOUT END_STREAM - the stream stays open for bidirectional DATA.
  [[maybe_unused]] ErrorCode err =
      _connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, /*endStream=*/false);
  assert(err == ErrorCode::NoError && "sendHeaders cannot fail for a just-opened CONNECT stream");

  // Track the tunnel mapping in the unified stream state.
  _streams[streamId].tunnelUpstreamFd = upstreamFd;
  _tunnelUpstreams[upstreamFd] = streamId;
}

bool Http2ProtocolHandler::isTunnelPending(uint32_t streamId) const noexcept {
  const auto it = _streams.find(streamId);
  return it != _streams.end() && it->second.tunnelPending;
}

void Http2ProtocolHandler::onTunnelResolved(uint32_t streamId, NativeHandle upstreamFd) {
  if (upstreamFd == kInvalidHandle) {
    failPendingTunnel(streamId, http::StatusCodeBadGateway, "Unable to connect to CONNECT target");
    return;
  }
  auto it = _streams.find(streamId);
  assert(it != _streams.end() && it->second.tunnelPending);
  it->second.tunnelPending = false;

  establishTunnel(streamId, upstreamFd);
  log::debug("HTTP/2 CONNECT tunnel established on stream {} → {}", streamId, it->second.request->request.authority());

  // Forward what the client sent while the target was resolved, then drop the request.
  it = _streams.find(streamId);
  StreamState& state = it->second;
  const RawChars earlyData = std::move(state.request->bodyBuffer);
  const bool clientEnded = state.tunnelPendingEndStream;
  recycleRequest(state.request);
  // The receive window, held while resolving, stays held while the upstream has not written these bytes (see
  // onTunnelUpstreamDrained()).
  if (earlyData.empty() || _tunnelBridge->writeTunnel(
                               upstreamFd, std::as_bytes(std::span<const char>(earlyData.data(), earlyData.size())))) {
    _connection.releaseStreamRecvWindow(streamId);
  }
  if (clientEnded) {
    _tunnelBridge->shutdownTunnelWrite(upstreamFd);
  }
}

void Http2ProtocolHandler::failPendingTunnel(uint32_t streamId, http::StatusCode statusCode, std::string_view reason) {
  const auto it = _streams.find(streamId);
  assert(it != _streams.end() && it->second.tunnelPending);
  it->second.tunnelPending = false;
  (void)sendResponse(streamId, it->second.request->request.makeResponse(statusCode, reason), /*isHeadMethod=*/false);
  releaseStreamAfterResponse(_streams.find(streamId));
}

void Http2ProtocolHandler::onTunnelUpstreamDrained(NativeHandle upstreamFd) {
  const auto it = _tunnelUpstreams.find(upstreamFd);
  if (it != _tunnelUpstreams.end()) {
    _connection.releaseStreamRecvWindow(it->second);
  }
}

void Http2ProtocolHandler::closeTunnelByUpstreamFd(NativeHandle upstreamFd) {
  auto it = _tunnelUpstreams.find(upstreamFd);
  if (it == _tunnelUpstreams.end()) {
    return;
  }
  const uint32_t streamId = it->second;
  auto streamIt = _streams.find(streamId);
  if (streamIt != _streams.end()) {
    streamIt->second.tunnelUpstreamFd = kInvalidHandle;
  }
  _tunnelUpstreams.erase(it);

  // Send empty DATA with END_STREAM to gracefully close the tunnel stream.
  (void)_connection.sendData(streamId, std::span<const std::byte>{}, /*endStream=*/true);
  _connection.finalizeSendClosedStream(streamId);
}

void Http2ProtocolHandler::tunnelConnectFailed(uint32_t streamId) {
  auto streamIt = _streams.find(streamId);
  if (streamIt != _streams.end() && streamIt->second.tunnelUpstreamFd != kInvalidHandle) {
    _tunnelUpstreams.erase(streamIt->second.tunnelUpstreamFd);
    streamIt->second.tunnelUpstreamFd = kInvalidHandle;
  }
  // Signal the tunnel failure per RFC 7540 §8.3 using CONNECT_ERROR.
  _connection.sendRstStream(streamId, ErrorCode::ConnectError);
}

bool Http2ProtocolHandler::isTunnelStream(uint32_t streamId) const noexcept {
  auto it = _streams.find(streamId);
  return it != _streams.end() && it->second.tunnelUpstreamFd != kInvalidHandle;
}

Http2ProtocolHandler::TunnelUpstreamsMap Http2ProtocolHandler::drainTunnelUpstreamFds() {
  auto ret = std::move(_tunnelUpstreams);
  for (auto& [streamId, state] : _streams) {
    state.tunnelUpstreamFd = kInvalidHandle;
  }
  _tunnelUpstreams.clear();
  return ret;
}

bool Http2ProtocolHandler::responseExceedsPendingLimits(uint32_t streamId, const HttpResponse& response,
                                                        bool isHeadMethod) {
  const bool hasFile = !isHeadMethod && response.hasBodyFile();
  const std::string_view body = !isHeadMethod && !hasFile ? response.bodyInMemory() : std::string_view{};
  const std::string_view trailers = !isHeadMethod ? response.trailersFlatView() : std::string_view{};

  if (!body.empty()) {
    Http2Stream* pStream = _connection.getStream(streamId);
    assert(pStream != nullptr);
    const auto windowLimit =
        static_cast<std::size_t>(std::max(std::min(pStream->sendWindow(), _connection.connectionSendWindow()), 0));
    const std::size_t deferredBodySize = body.size() - std::min(body.size(), windowLimit);
    const std::size_t deferredTrailersSize = deferredBodySize != 0 ? trailers.size() : 0;
    const auto streamLimit = _connection.localSettings().maxStreamPendingBytes;
    if (deferredBodySize > streamLimit || deferredTrailersSize > streamLimit - deferredBodySize) {
      return true;
    }
  }

  const auto limit = _pCtx->config.maxOutboundBufferBytes;
  const std::size_t retained = retainedOutboundBytes();
  if (retained > limit) {
    return true;
  }
  std::size_t available = limit - retained;
  const auto consumeBudget = [&available](std::size_t size) {
    if (size > available) {
      return false;
    }
    available -= size;
    return true;
  };

  // Raw header bytes are a conservative proxy for HPACK output and also account for the already-computed
  // response storage that exists until sendResponse returns. Global headers are encoded separately.
  const std::size_t globalHeadersSize =
      response._opts.isPrepared() ? 0 : _pCtx->config.globalHeaders.fullStringWithLastSep().size();
  if (!consumeBudget(FrameHeader::kSize) || !consumeBudget(response.headersFlatViewWithDate().size()) ||
      !consumeBudget(globalHeadersSize)) {
    return true;
  }

  if (!body.empty()) {
    const std::size_t maxFrameSize = _connection.peerSettings().maxFrameSize;
    const std::size_t frameCount = 1U + ((body.size() - 1U) / maxFrameSize);
    if (!consumeBudget(body.size()) || !consumeBudget(frameCount * FrameHeader::kSize)) {
      return true;
    }
  }
  return !trailers.empty() && (!consumeBudget(trailers.size()) || !consumeBudget(FrameHeader::kSize));
}

ErrorCode Http2ProtocolHandler::sendResponse(uint32_t streamId, HttpResponse response, bool isHeadMethod) {
  if (responseExceedsPendingLimits(streamId, response, isHeadMethod)) {
    log::warn(
        "Rejecting HTTP/2 response on stream {} at pending-output limit: response={} retained={} "
        "connection-limit={} stream-limit={}",
        streamId, response.bodyInMemory().size(), retainedOutboundBytes(), _pCtx->config.maxOutboundBufferBytes,
        _connection.localSettings().maxStreamPendingBytes);
    _pCtx->telemetry.counterAdd("aeronet.http2.responses.rejected_outbound_limit");

    const auto it = _streams.find(streamId);
    assert(it != _streams.end() && "Stream must exist when rejecting response due to pending-output limit");
    HttpRequestView& req = it->second.request->request;

    response = req.makeResponse(http::StatusCodeServiceUnavailable);
  }

  FilePayload* pFilePayload = response.filePayloadPtr();

  // Finalize Date header
  CopyCRLFDateHeader(_pCtx->dateHeader.data(), response._data.data() + response.dateHeaderStartPos());

  const ConcatenatedHeaders* pGlobalHeaders = response._opts.isPrepared() ? nullptr : &_pCtx->config.globalHeaders;

  // The trailers and body views stay valid once the response storage is released (see releaseMessageData()), as long
  // as the released storage lives.
  const std::string_view trailersFlatView = response.trailersFlatView();
  const HeadersView trailersView(trailersFlatView);
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
      pending.filePayload = std::move(*pFilePayload);

      // Pass 1 - try to send as much as possible now
      err = sendPendingFileBody(streamId, pending.filePayload, endStreamOnData, fileReadBudget(1));
      if (err != ErrorCode::NoError) {
        return err;
      }

      if (pending.filePayload.length != 0) {
        assert(!hasTrailers && "file + trailers is not supported by the current HttpResponse API");
        deferSend(_streams[streamId], std::move(pending));
      } else {
        // File payload fully sent inline. Send trailers if present.
        // Note: hasFile && hasTrailers is currently unreachable via the public HttpResponse API (trailerAddLine
        // requires an in-memory body, which file() clears), but kept for forward-compatibility if the API is relaxed in
        // the future.
        assert(!hasTrailers && "file + trailers is not supported by the current HttpResponse API");
      }
    } else {
      // The body is sent from the storage of the response (its buffer, or its captured body), handed over to the
      // connection output or to a pending send without copying it: moving the storage does not move the body bytes.
      HttpMessageData storage = response.releaseMessageData();
      Http2Stream* pStream = _connection.getStream(streamId);
      assert(pStream != nullptr && "stream vanished between sendHeaders and the body send");
      const auto windowLimit =
          static_cast<std::size_t>(std::max(std::min(pStream->sendWindow(), _connection.connectionSendWindow()), 0));
      if (bodyView.size() <= windowLimit) {
        // Note: sendData() already handles splitting into multiple frames based on peer's maxFrameSize.
        err = _connection.sendData(streamId, std::move(storage), std::as_bytes(std::span<const char>(bodyView)),
                                   endStreamOnData);
        if (err == ErrorCode::NoError && hasTrailers) {
          // sendData() copies a small body into the batched output buffer, leaving the storage here, and otherwise
          // hands it over to the output: either way, the trailers stay alive while encoded.
          err = _connection.sendHeaders(streamId, http::StatusCode{}, trailersView, true);
        }
        return err;
      }
      // A body exceeding the flow-control windows (e.g. any response larger than the peer's
      // SETTINGS_INITIAL_WINDOW_SIZE, 65535 by default) cannot be sent in one go: send what they allow now and keep the
      // remainder (plus any trailers) as a pending streaming send, flushed as WINDOW_UPDATEs arrive.
      PendingStreamingSend pendingSend{.storage = std::move(storage), .body = bodyView, .trailersData = {}};
      if (hasTrailers) {
        pendingSend.trailersData.assign(trailersFlatView);
      }
      _deferredOutputBytes += bodyView.size() + pendingSend.trailersData.size();
      [[maybe_unused]] const bool sent = sendPendingStreamingBody(streamId, pendingSend);
      assert(!sent && "a body exceeding the flow-control windows cannot be sent at once");
      deferSend(_streams[streamId], std::move(pendingSend));
      return ErrorCode::NoError;  // trailers (if any) ride with the pending send
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

bool Http2ProtocolHandler::startAsyncHandler(StreamsMap::iterator it, const Router::RoutingResult& routingResult) {
  const uint32_t streamId = it->first;
  StreamState& state = it->second;

  // Prepare the pending task entry, which owns the request while the coroutine runs (its address does not change).
  auto pendingPtr = _pendingWorkPool.allocateAndConstructPoolPtr(PendingAsyncTask{});
  PendingAsyncTask& pendingRef = std::get<PendingAsyncTask>(*pendingPtr);
  assert(state.hasRequest());
  pendingRef.streamRequest = std::move(state.request);
  HttpRequestView& req = pendingRef.streamRequest->request;

  // The path parameter names are owned by the router, which may be updated while the coroutine is suspended.
  req.pinPathParamKeys(pendingRef.pathParamKeys);

  // Store in the variant
  state.pending = std::move(pendingPtr);

  auto requestTask = RequestPipeline::CallAsyncHandler(*routingResult.sharedAsyncRequestHandler(), req);
  if (!requestTask.valid()) {
    // pendingRef is dangling once the pending task is released: use the restored request.
    HttpRequestView& restoredReq = ReleaseAsyncTask(state);
    HttpResponse response = restoredReq.makeResponse(http::StatusCodeInternalServerError, "Async handler inactive");
    _pCtx->requestPipeline().applyResponseMiddleware(restoredReq, response, routingResult.postMiddlewareRange(), false);
    finalizeAndSendResponse(it, std::move(response), routingResult.corsPolicy());
    return false;
  }

  pendingRef.task.start(std::move(requestTask), routingResult.sharedAsyncRequestHandler(), routingResult.corsPolicy(),
                        routingResult.postMiddlewareRange(),
                        _pCtx->asyncResumePoster(_fd, _pConnectionState->generation));
  pendingRef.task.bind(req);

  // Run the coroutine (it starts suspended due to initial_suspend = suspend_always) until it completes or suspends to
  // wait for deferred work. It is then resumed by resumeAsyncTaskByHandle() when the work completes.
  if (!pendingRef.task.run()) {
    log::debug("HTTP/2 async handler suspended on stream {}", streamId);
    // Completes later: the router may be updated meanwhile.
    pendingRef.task.routeMayHaveChanged = true;
    return true;
  }

  onAsyncTaskCompleted(streamId);
  return false;
}

void Http2ProtocolHandler::resumeAsyncTask(uint32_t streamId) {
  auto it = _streams.find(streamId);
  // Only called from resumeAsyncTaskByHandle which just found the entry - cannot be absent.
  auto* pAsync = it->second.asyncTask();
  assert(it != _streams.end() && pAsync != nullptr);

  if (pAsync->task.run()) {
    onAsyncTaskCompleted(streamId);
  }
}

void Http2ProtocolHandler::onAsyncTaskCompleted(uint32_t streamId) {
  auto it = _streams.find(streamId);
  assert(it != _streams.end());
  // Called from startAsyncHandler (just inserted) or resumeAsyncTask (just found) - cannot be absent.
  auto* pAsync = it->second.asyncTask();
  assert(pAsync != nullptr);

  HttpRequestView& req = pAsync->streamRequest->request;
  HttpResponse resp = pAsync->task.takeResponse(req);
  const CorsPolicy* pCorsPolicy = pAsync->task.applyResponseMiddleware(_pCtx->requestPipeline(), req, resp);

  // Move the request back to the stream and free the pending variant, so that the response can store a deferred send.
  ReleaseAsyncTask(it->second);
  finalizeAndSendResponse(it, std::move(resp), pCorsPolicy);
}

HttpRequestView& Http2ProtocolHandler::ReleaseAsyncTask(StreamState& state) noexcept {
  auto* pAsync = state.asyncTask();
  assert(pAsync != nullptr);
  state.request = std::move(pAsync->streamRequest);
  HttpRequestView& req = state.request->request;
  // Their names were copied to the task, released below.
  req._pathParams.clear();
  state.pending.reset();
  return req;
}

void Http2ProtocolHandler::orphanAsyncTask(StreamState& state) {
  if (const auto* pAsync = state.asyncTask(); pAsync != nullptr && pAsync->task.isAwaitingCallback()) {
    // Its deferred work may still use the coroutine frame and the request: keep them until it completes (the frame
    // address then also identifies the task without ambiguity, no other frame being allocated there meanwhile).
    _orphanedAsyncTasks.push_back(std::move(state.pending));
  }
}

bool Http2ProtocolHandler::resumeAsyncTaskByHandle(std::coroutine_handle<> handle) {
  auto pAsyncIt = std::ranges::find_if(_streams, [handle](const auto& pair) {
    const auto* pAsync = pair.second.asyncTask();
    return pAsync != nullptr && pAsync->task.runs(handle);
  });
  if (pAsyncIt != _streams.end()) {
    resumeAsyncTask(pAsyncIt->first);
    return true;
  }

  // The stream was closed while the work was running.
  dropAsyncTask(handle);
  return false;
}

void Http2ProtocolHandler::dropAsyncTask(std::coroutine_handle<> handle) noexcept {
  const auto isTask = [handle](const PendingWork* pPendingWork) {
    const auto* pAsync = std::get_if<PendingAsyncTask>(pPendingWork);
    return pAsync != nullptr && pAsync->task.runs(handle);
  };
  const auto orphanIt = std::ranges::find_if(
      _orphanedAsyncTasks, [&isTask](const PoolPtr<PendingWork>& pendingWork) { return isTask(pendingWork.get()); });
  if (orphanIt != _orphanedAsyncTasks.end()) {
    _orphanedAsyncTasks.erase(orphanIt);
    return;
  }
  // Task of a stream still open on a closed connection.
  for (auto& [streamId, streamState] : _streams) {
    if (streamState.pending && isTask(streamState.pending.get())) {
      streamState.pending.reset();
      return;
    }
  }
}

bool Http2ProtocolHandler::hasAsyncWorkInFlight() const noexcept {
  return !_orphanedAsyncTasks.empty() || std::ranges::any_of(_streams, [](const auto& pair) {
    const auto* pAsync = pair.second.asyncTask();
    return pAsync != nullptr && pAsync->task.isAwaitingCallback();
  });
}

#endif  // AERONET_ENABLE_ASYNC_HANDLERS

void Http2ProtocolHandler::onRequestCompleted(HttpRequestView& request, http::StatusCode status) {
  if (_pCtx->telemetry.enabled()) {
    char statusBuffer[3];
    writeStatusCode(statusBuffer, status);

    const MetricLabel labels[]{
        {"http.request.method", http::MethodToStr(request.method())},
        {"http.response.status_code", std::string_view(statusBuffer, sizeof(statusBuffer))},
    };
    const auto duration =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - request.reqStart());
    _pCtx->telemetry.counterAdd("aeronet.http2.stream.requests", 1UL, labels);
    _pCtx->telemetry.histogram("aeronet.http2.stream.duration", duration.count(), labels);
    _pCtx->telemetry.histogram("aeronet.http2.stream.request.body.bytes", static_cast<double>(request._body.size()),
                               labels);
  }

  request.end(status);
  ++_pConnectionState->requestsServed;
  ++_pCtx->stats.totalRequestsServed;
  if (_pCtx->callbacks.metrics || _pCtx->accessLog) {
    _pCtx->emitRequestMetrics(request, status);
  }
}

void Http2ProtocolHandler::initiateGoAway() {
  // Before the client preface, the connection has no stream, and a GOAWAY would make the preface be read as frames.
  if (_connection.state() == ConnectionState::AwaitingPreface) {
    _pConnectionState->requestDrainAndClose();
    return;
  }
  _connection.initiateGoAway(ErrorCode::NoError, ErrorMsg::ServerDraining);
  if (_connection.goAwayCompleted()) {
    _pConnectionState->requestDrainAndClose();
  }
}

bool Http2ProtocolHandler::sweepStreams(std::chrono::steady_clock::time_point now) {
  for (auto it = _streams.begin(); it != _streams.end();) {
    auto& streamState = it->second;
    if (streamState.requestDeadline.time_since_epoch().count() != 0 && streamState.requestDeadline < now) {
      const uint32_t streamId = it->first;
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      // The request of an async handler is owned by its task.
      auto* pAsync = streamState.asyncTask();
      HttpRequestView& request = pAsync != nullptr ? pAsync->streamRequest->request : streamState.request->request;
#else
      HttpRequestView& request = streamState.request->request;
#endif
      const bool isHeadMethod = request.method() == http::Method::HEAD;
      HttpResponse response = request.makeResponse(http::StatusCodeRequestTimeout);

      log::debug("HTTP/2 stream {} timed out (per-route request deadline exceeded)", streamId);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
      // The deferred work of the task may still be running, using its coroutine frame and its request: keep them out
      // of the stream before answering, so that nothing the response stores in the stream can destroy them.
      orphanAsyncTask(streamState);
#endif
      [[maybe_unused]] const ErrorCode err = sendResponse(streamId, std::move(response), isHeadMethod);
      assert(err == ErrorCode::NoError);
      // The 408 has no body: it does not wait for flow-control credit (which the reset below would never let come).
      assert(!streamState.hasPendingSend());

      // Erase from our map before sendRstStream, because sendRstStream triggers
      // the onStreamClosed callback which also erases from _streams — doing both
      // would double-erase and invalidate the iterator.
      it = _streams.erase(it);
      _connection.sendRstStream(streamId, ErrorCode::Cancel);
    } else {
      ++it;
    }
  }

  const auto result = _connection.checkTimeouts(now, _pConnectionState->lastActivity);
  if (result.action == Http2Connection::ProcessResult::Action::Error) [[unlikely]] {
    log::debug("HTTP/2 connection fd # {} closed: {}", _fd, ConvertProcessResultErrorMsgToSv(result.errorMsg));
    _pCtx->telemetry.counterAdd(result.errorMsg == ErrorMsg::SETTINGSTimeout
                                    ? "aeronet.connections.closed_for_http2_settings_timeout"
                                    : "aeronet.connections.closed_for_http2_ping_timeout");
    return false;
  }
  return true;
}

std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(::aeronet::internal::ServerContext& ctx,
                                                             ::aeronet::ConnectionState& connectionState,
                                                             NativeHandle fd, bool sendServerPrefaceForTls) {
  auto protocolHandler = std::make_unique<Http2ProtocolHandler>(ctx, connectionState, fd);
  if (sendServerPrefaceForTls) {
    // For TLS ALPN "h2", the server must send SETTINGS immediately after TLS handshake
    protocolHandler->connection().sendServerPreface();
  }
  return protocolHandler;
}

}  // namespace aeronet::http2
