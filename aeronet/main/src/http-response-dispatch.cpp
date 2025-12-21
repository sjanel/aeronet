#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/template-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {

SingleHttpServer::LoopAction SingleHttpServer::processSpecialMethods(ConnectionMapIt& cnxIt, std::size_t consumedBytes,
                                                                     const CorsPolicy* pCorsPolicy) {
  HttpRequest& request = cnxIt->second->request;
  switch (request.method()) {
    case http::Method::OPTIONS: {
      // OPTIONS * request (target="*") should return an Allow header listing supported methods.
      const auto buildAllowHeader = [](http::MethodBmp mask) {
        RawChars32 allowValue;
        for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
          if (!http::IsMethodIdxSet(mask, methodIdx)) {
            continue;
          }
          if (!allowValue.empty()) {
            allowValue.push_back(',');
          }
          allowValue.append(http::MethodIdxToStr(methodIdx));
        }
        return allowValue;
      };

      if (request.path() == "*") {
        HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
        const http::MethodBmp allowed = _router.allowedMethods("*");
        auto allowVal = buildAllowHeader(allowed);
        if (!allowVal.empty()) {
          resp.header(http::Allow, allowVal);
        }
        finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
        return LoopAction::Continue;
      }

      const auto routeMethods = _router.allowedMethods(request.path());
      if (pCorsPolicy != nullptr) {
        auto preflight = pCorsPolicy->handlePreflight(request, routeMethods);
        switch (preflight.status) {
          case CorsPolicy::PreflightResult::Status::NotPreflight:
            break;
          case CorsPolicy::PreflightResult::Status::Allowed:
            finalizeAndSendResponse(cnxIt, std::move(preflight.response), consumedBytes, pCorsPolicy);
            return LoopAction::Continue;
          case CorsPolicy::PreflightResult::Status::OriginDenied: {
            HttpResponse resp(http::StatusCodeForbidden, http::ReasonForbidden);
            resp.body(http::ReasonForbidden);
            finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
            return LoopAction::Continue;
          }
          case CorsPolicy::PreflightResult::Status::MethodDenied: {
            HttpResponse resp(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
            resp.body(resp.reason());
            const auto allowedForPath = buildAllowHeader(routeMethods);
            if (!allowedForPath.empty()) {
              resp.header(http::Allow, allowedForPath);
            }
            finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
            return LoopAction::Continue;
          }
          case CorsPolicy::PreflightResult::Status::HeadersDenied: {
            HttpResponse resp(http::StatusCodeForbidden, http::ReasonForbidden);
            resp.body(http::ReasonForbidden);
            finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
            return LoopAction::Continue;
          }
        }
      }

      break;
    }
    case http::Method::TRACE: {
      // TRACE: echo the received request message as the body with Content-Type: message/http
      // Respect configured TracePolicy. Default: Disabled.
      bool allowTrace;
      switch (_config.traceMethodPolicy) {
        case HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS:
          allowTrace = true;
          break;
        case HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly:
          // If this request arrived over TLS, disallow TRACE
          allowTrace = request.tlsVersion().empty();
          break;
        case HttpServerConfig::TraceMethodPolicy::Disabled:
        default:
          allowTrace = false;
          break;
      }
      if (allowTrace) {
        // Reconstruct the request head from HttpRequest
        std::string_view reqDataEchoed(cnxIt->second->inBuffer.data(), consumedBytes);

        HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
        resp.body(reqDataEchoed, http::ContentTypeMessageHttp);
        finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
        return LoopAction::Continue;
      }
      // TRACE disabled -> Method Not Allowed
      HttpResponse resp(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
      resp.body(resp.reason());
      finalizeAndSendResponse(cnxIt, std::move(resp), consumedBytes, pCorsPolicy);
      return LoopAction::Continue;
    }
    case http::Method::CONNECT: {
      // CONNECT: establish a TCP tunnel to target (host:port). On success reply 200 and
      // proxy bytes bidirectionally between client and upstream.
      // Parse authority form in request.path() (host:port)
      const std::string_view target = request.path();
      const auto colonPos = target.find(':');
      if (colonPos == std::string_view::npos) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Malformed CONNECT target");
        return LoopAction::Break;
      }
      const std::string_view host(target.begin(), target.begin() + static_cast<size_t>(colonPos));
      const std::string_view portStr(target.data() + colonPos + 1, static_cast<size_t>(target.size() - colonPos - 1));

      // Enforce CONNECT allowlist if present
      const auto& connectAllowList = _config.connectAllowlist();
      if (!connectAllowList.empty() && !connectAllowList.contains(host)) {
        emitSimpleError(cnxIt, http::StatusCodeForbidden, true, "CONNECT target not allowed");
        return LoopAction::Break;
      }

      // Use helper to resolve and initiate a non-blocking connect. The helper
      // returns a ConnectResult with an owned BaseFd and flags indicating
      // whether the connect is pending or failed.
      char* data = cnxIt->second->inBuffer.data();
      ConnectResult cres = ConnectTCP(std::span<char>(data + (host.data() - data), host.size()),
                                      std::span<char>(data + (portStr.data() - data), portStr.size()));
      if (cres.failure) {
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Unable to resolve CONNECT target");
        return LoopAction::Break;
      }

      int upstreamFd = cres.cnx.fd();
      // Register upstream in event loop for edge-triggered reads and writes so we can detect
      // completion of non-blocking connect (EPOLLOUT) as well as incoming data.
      if (!_eventLoop.add(EventLoop::EventFd{upstreamFd, EventIn | EventOut | EventEt})) {
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Failed to register upstream fd");
        return LoopAction::Break;
      }

      // Insert upstream connection state. Inserting may rehash and invalidate the
      // caller's iterator; save the client's fd and re-resolve the client iterator
      // after emplacing.
      const int clientFd = cnxIt->first.fd();
      auto [upIt, inserted] = _activeConnectionsMap.emplace(std::move(cres.cnx), getNewConnectionState());
      if (!inserted) {
        log::error("TCP connection ConnectionState fd # {} already exists, should not happen", upstreamFd);
        _eventLoop.del(upstreamFd);
        // Try to re-find client to report error; if not found, just return Break.
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Upstream connection tracking failed");
        return LoopAction::Break;
      }
      // Set upstream transport to plain (no TLS)
      upIt->second->transport = std::make_unique<PlainTransport>(upstreamFd);

      // If the connector indicated the connect is still in progress on this
      // non-blocking socket, mark state so the event loop's writable handler
      // can check SO_ERROR and surface failures. Use the connector's flag
      // rather than relying on errno here (errno may have been overwritten).

      // Reply 200 Connection Established to client
      // Since cnxIt is passed by reference we will update it here so the caller need not re-find.
      cnxIt = _activeConnectionsMap.find(clientFd);
      if (cnxIt == _activeConnectionsMap.end()) {
        throw std::runtime_error("Should not happen - Client connection vanished after upstream insertion");
      }

      finalizeAndSendResponse(cnxIt, HttpResponse(http::StatusCodeOK, "Connection Established"), consumedBytes,
                              pCorsPolicy);

      // Enter tunneling mode: link peer fds
      cnxIt->second->peerFd = upstreamFd;
      upIt->second->peerFd = cnxIt->first.fd();
      upIt->second->connectPending = cres.connectPending;

      // From now on, both connections bypass HTTP parsing; we simply proxy bytes. We'll rely on handleReadableClient
      // to read from each side and forward to the other by writing into the peer's transport directly.
      // Erase any partially parsed buffers for the client (we already replied)
      cnxIt->second->inBuffer.clear();
      upIt->second->inBuffer.clear();
      return LoopAction::Continue;
    }
    default:
      break;
  }
  return LoopAction::Nothing;
}

void SingleHttpServer::tryCompressResponse(const HttpRequest& request, HttpResponse& resp) {
  const CompressionConfig& compressionConfig = _config.compression;
  if (resp.body().size() < compressionConfig.minBytes) {
    return;
  }
  const std::string_view encHeader = request.headerValueOrEmpty(http::AcceptEncoding);
  auto [encoding, reject] = _encodingSelector.negotiateAcceptEncoding(encHeader);
  // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
  // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
  if (reject) {
    resp.status(http::StatusCodeNotAcceptable, http::ReasonNotAcceptable)
        .body("No acceptable content-coding available");
  }
  if (encoding == Encoding::none) {
    return;
  }

  if (!compressionConfig.contentTypeAllowList.empty()) {
    std::string_view contentType = request.headerValueOrEmpty(http::ContentType);
    if (!compressionConfig.contentTypeAllowList.contains(contentType)) {
      return;
    }
  }

  if (resp.headerValue(http::ContentEncoding)) {
    return;
  }

  // We will compress the response body.
  // Depending on where the body is stored, the compression will be made at the final destination of the emitted data

  // First, write the needed headers.
  // We can use addHeader for Content-Encoding instead of header because at this point,
  // we know that the user did not provide content encoding.
  resp.addHeader(http::ContentEncoding, GetEncodingStr(encoding));
  if (compressionConfig.addVaryHeader) {
    // Use appendHeaderValue so we preserve any existing Vary values and append
    // Accept-Encoding (comma-separated) when appropriate.
    resp.appendHeaderValue(http::Vary, http::AcceptEncoding);
  }

  auto& encoder = _encoders[static_cast<std::size_t>(encoding)];
  auto* pExternPayload = resp.externPayloadPtr();
  // If the external payload exists, we can compress directly into the HttpResponse internal buffer.
  // We don't need to care about the trailers because, if present, they are necessarily appended to the body buffer.
  if (pExternPayload != nullptr) {
    // compress only the body portion of the external payload (do NOT compress trailers).
    // Determine body length inside external payload. If _trailerPos!=0 it marks the start of trailers
    // relative to the body start inside the external payload; otherwise the entire payload is body.
    const auto externView = pExternPayload->view();
    const auto externTrailers = resp.externalTrailers(*pExternPayload);
    const std::string_view externBody(externView.data(), externView.size() - externTrailers.size());
    const auto oldSize = resp._data.size();

    encoder->encodeFull(externTrailers.size(), externBody, resp._data);

    const std::size_t compressedBodyLen = resp._data.size() - oldSize;

    // If there are trailers in the external payload, append them (uncompressed) to the inline buffer
    // and update _trailerPos to point to the start of trailers relative to the (now-compressed) body.
    if (!externTrailers.empty()) {
      resp._data.append(externTrailers);
      resp._trailerPos = compressedBodyLen;  // trailers now start at compressedBodyLen relative to body
    }

    // Mark as inline and drop the external payload
    resp._payloadVariant = {};
  } else {
    // compress from inline body to external buffer
    const auto internalTrailers = resp.internalTrailers();
    RawChars out;
    encoder->encodeFull(internalTrailers.size(), resp.body(), out);

    if (resp._trailerPos != 0) {
      resp._trailerPos = out.size();
      out.append(internalTrailers);
    }

    // If there are trailers appended to the inline buffer, copy them into the captured payload (uncompressed)
    // so they are not lost when we remove the internal body+trailers from _data.
    // Keep the original content type: remove inline body+trailers from internal buffer
    resp._data.setSize(resp._data.size() - resp.internalBodyAndTrailersLen());
    resp._payloadVariant = HttpPayload(std::move(out));
  }
}

void SingleHttpServer::finalizeAndSendResponse(ConnectionMapIt cnxIt, HttpResponse&& resp, std::size_t consumedBytes,
                                               const CorsPolicy* pCorsPolicy) {
  const auto respStatusCode = resp.status();

  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  request._ownerState = nullptr;
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, resp);
  }

  ++state.requestsServed;
  ++_stats.totalRequestsServed;

  // keep-alive logic
  bool keepAlive =
      _config.enableKeepAlive && state.requestsServed < _config.maxRequestsPerConnection && _lifecycle.isRunning();
  if (keepAlive) {
    const std::string_view connVal = request.headerValueOrEmpty(http::Connection);
    if (connVal.empty()) {
      // Default is keep-alive for HTTP/1.1, close for HTTP/1.0
      keepAlive = request.version() == http::HTTP_1_1;
    } else if (CaseInsensitiveEqual(connVal, http::close)) {
      keepAlive = false;
    }
  }

  const bool isHead = (request.method() == http::Method::HEAD);
  if (!isHead) {
    if (respStatusCode == http::StatusCodeNotFound && resp.body().empty()) {
      resp.body(k404NotFoundTemplate2, http::ContentTypeTextHtml);
    }
    tryCompressResponse(request, resp);
  }

  queueFormattedHttp1Response(cnxIt, resp.finalizeForHttp1(request.version(), SysClock::now(), !keepAlive,
                                                           _config.globalHeaders, isHead, _config.minCapturedBodySize));

  state.inBuffer.erase_front(consumedBytes);
  if (!keepAlive && state.outBuffer.empty()) {
    state.requestDrainAndClose();
  }
  if (_metricsCb) {
    emitRequestMetrics(request, respStatusCode, request.body().size(), state.requestsServed > 0);
  }

  // End the span after response is finalized
  request.end(respStatusCode);
}

bool SingleHttpServer::queueFormattedHttp1Response(ConnectionMapIt cnxIt,
                                                   HttpResponse::FormattedHttp1Response prepared) {
  const bool hasFile = prepared.fileLength > 0;
  const std::uint64_t fileBytes = hasFile ? prepared.fileLength : 0;

  if (!queueData(cnxIt, std::move(prepared.data), fileBytes)) {
    return false;
  }

  if (hasFile) {
    ConnectionState& state = *cnxIt->second;
    state.fileSend.file = std::move(prepared.file);
    state.fileSend.offset = prepared.fileOffset;
    state.fileSend.remaining = prepared.fileLength;
    state.fileSend.active = state.fileSend.remaining > 0;
    state.fileSend.headersPending = !state.outBuffer.empty();
    if (state.isSendingFile()) {
      // Don't enable writable interest here - let flushFilePayload do it when it actually blocks.
      // Enabling it prematurely (when the socket is already writable) causes us to miss the edge
      // in edge-triggered epoll mode.
      if (!state.fileSend.headersPending) {
        flushFilePayload(cnxIt);
      }
    }
  }
  return true;
}

bool SingleHttpServer::queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData,
                                 std::uint64_t extraQueuedBytes) {
  ConnectionState& state = *cnxIt->second;

  const auto bufferedSz = httpResponseData.remainingSize();

  if (state.outBuffer.empty()) {
    // Plain TCP path: try immediate write optimization
    const auto [written, want] = state.transportWrite(httpResponseData);
    switch (want) {
      case TransportHint::Error:
        state.requestImmediateClose();
        return false;
      case TransportHint::ReadReady:
        [[fallthrough]];
      case TransportHint::WriteReady:
        [[fallthrough]];
      case TransportHint::None:
        if (written == bufferedSz) {
          _stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
          _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
          return true;
        }
        // partial write, capture the buffer in the connection state
        httpResponseData.addOffset(static_cast<std::size_t>(written));
        state.outBuffer = std::move(httpResponseData);
        _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
        break;
    }
  } else {
    state.outBuffer.append(std::move(httpResponseData));
  }

  const std::size_t remainingSize = state.outBuffer.remainingSize();
  _stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, remainingSize);
  if (remainingSize > _config.maxOutboundBufferBytes) {
    state.requestImmediateClose();
  }
  if (!state.waitingWritable) {
    enableWritableInterest(cnxIt);
  }

  // If we buffered data, try flushing it immediately
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }

  return true;
}

void SingleHttpServer::flushOutbound(ConnectionMapIt cnxIt) {
  ++_stats.flushCycles;
  TransportHint want = TransportHint::None;
  ConnectionState& state = *cnxIt->second;
  const int fd = cnxIt->first.fd();
  while (!state.outBuffer.empty()) {
    const auto [written, stepWant] = state.transportWrite(state.outBuffer);
    want = stepWant;
    _stats.totalBytesWrittenFlush += written;
    switch (want) {
      case TransportHint::Error: {
        auto savedErr = errno;
        log::error("send/transportWrite failed fd # {} errno={} msg={}", fd, savedErr, std::strerror(savedErr));
        state.requestImmediateClose();
        state.outBuffer.clear();
        break;
      }
      case TransportHint::ReadReady:
        [[fallthrough]];
      case TransportHint::WriteReady:
        [[fallthrough]];
      case TransportHint::None:
        if (written > 0) {
          if (written == state.outBuffer.remainingSize()) {
            state.outBuffer.clear();
            break;
          }
          state.outBuffer.addOffset(written);
          continue;
        }
        break;
    }
  }

  if (state.outBuffer.empty() && state.fileSend.headersPending) {
    state.fileSend.headersPending = false;
  }

  flushFilePayload(cnxIt);
  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.outBuffer.empty() && !state.isSendingFile() && state.waitingWritable &&
      (state.tlsEstablished || state.transport->handshakeDone())) {
    if (disableWritableInterest(cnxIt)) {
      if (state.isAnyCloseRequested()) {
        return;
      }
    }
  }
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (state.outBuffer.empty() && !state.isSendingFile()) {
    bool transportNeedsWrite = (!state.tlsEstablished && want == TransportHint::WriteReady);
    if (transportNeedsWrite) {
      if (!state.waitingWritable) {
        if (!enableWritableInterest(cnxIt)) {
          return;  // failure logged
        }
      }
    } else if (state.waitingWritable) {
      disableWritableInterest(cnxIt);
    }
  }
}

bool SingleHttpServer::flushPendingTunnelOrFileBuffer(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  if (state.tunnelOrFileBuffer.empty()) {
    return false;
  }

  // Loop to drain the TLS buffer until it's empty or we would block (edge-triggered epoll requirement)
  for (;;) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);

    if (want == TransportHint::Error) {
      state.requestImmediateClose();
      state.fileSend.active = false;
      state.tunnelOrFileBuffer.clear();
      return false;
    }

    if (written > 0) {
      state.tunnelOrFileBuffer.erase_front(written);
      // Note: fileSend.offset and fileSend.remaining were already updated in transportFile when the data was read.
      // Do NOT update them again here or we'll double-count and prematurely mark the transfer complete.
      _stats.totalBytesWrittenFlush += written;
    }

    // If buffer is now empty, we're done
    if (state.tunnelOrFileBuffer.empty()) {
      if (state.fileSend.remaining == 0) {
        state.fileSend.active = false;
      }
      return false;
    }

    // If we would block or transport needs write progress, enable writable interest and return
    if (want == TransportHint::WriteReady || written == 0) {
      if (!state.waitingWritable) {
        enableWritableInterest(cnxIt);
      }
      if (state.fileSend.remaining == 0) {
        state.fileSend.active = false;
      }
      return true;
    }

    // Otherwise, continue the loop to write more
  }
}

void SingleHttpServer::flushFilePayload(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  if (!state.isSendingFile()) {
    return;
  }

  if (state.fileSend.headersPending) {
    if (!state.outBuffer.empty()) {
      return;
    }
    state.fileSend.headersPending = false;
  }

  if (state.fileSend.remaining == 0) {
    state.fileSend.active = false;
    state.tunnelOrFileBuffer.clear();
    return;
  }

  if (!state.transport->handshakeDone()) {
    return;
  }

#ifdef AERONET_ENABLE_OPENSSL
  const bool tlsTransport = _config.tls.enabled && dynamic_cast<TlsTransport*>(state.transport.get()) != nullptr;
#else
  static constexpr bool tlsTransport = false;
#endif

#ifdef AERONET_ENABLE_KTLS
  const bool ktlsSend = tlsTransport && state.ktlsSendEnabled;
#else
  static constexpr bool ktlsSend = false;
#endif

  const bool tlsFlow = tlsTransport && !ktlsSend;

  // Loop to drain file payload while we can make progress (edge-triggered epoll requires this)
  for (;;) {
    if (tlsFlow && flushPendingTunnelOrFileBuffer(cnxIt)) {
      // Pending TLS bytes were not fully flushed (would block or error); return and wait for next writable event.
      return;
    }

    if (state.fileSend.remaining == 0) {
      state.fileSend.active = false;
      state.tunnelOrFileBuffer.clear();
      return;
    }

    const auto res = state.transportFile(cnxIt->first.fd(), tlsFlow);
    switch (res.code) {
      case ConnectionState::FileResult::Code::Read:
        // Read case: data read from file into buffer; now try to write it immediately.
        if (tlsFlow) {
          // Attempt to flush immediately; if it blocks/fails, we'll resume on next writable.
          if (flushPendingTunnelOrFileBuffer(cnxIt)) {
            return;  // Would block, wait for next writable event
          }
          // Successfully flushed, continue loop to read more
        }
        break;
      case ConnectionState::FileResult::Code::Sent:
        _stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(res.bytesDone);
#ifdef AERONET_ENABLE_KTLS
        if (ktlsSend) {
          _stats.ktlsSendBytes += static_cast<std::uint64_t>(res.bytesDone);
        }
#endif
        // Continue loop to send more
        break;
      case ConnectionState::FileResult::Code::Error:
        return;  // Error, stop
      case ConnectionState::FileResult::Code::WouldBlock:
        if (res.enableWritable && !state.waitingWritable) {
          // The helper reports WouldBlock; enable writable interest so we can resume later.
          enableWritableInterest(cnxIt);

          // Edge-triggered epoll fix: immediately retry ONCE after enabling writable interest.
          // If the socket became writable between sendfile() returning EAGAIN and epoll_ctl(),
          // we would miss the edge. This immediate retry catches that case.
          const auto retryRes = state.transportFile(cnxIt->first.fd(), tlsFlow);
          if (retryRes.code == ConnectionState::FileResult::Code::Sent) {
            _stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(retryRes.bytesDone);
#ifdef AERONET_ENABLE_KTLS
            if (ktlsSend) {
              _stats.ktlsSendBytes += static_cast<std::uint64_t>(retryRes.bytesDone);
            }
#endif
            // Socket was writable, continue the loop to send more
            break;
          }
        }
        return;  // Would block, wait for next writable event
      default:
        std::unreachable();
    }
  }
}

}  // namespace aeronet
