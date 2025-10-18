#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "accept-encoding-negotiation.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "connection-state.hpp"
#include "connection.hpp"
#include "log.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "tcp-connector.hpp"
#include "timedef.hpp"
#include "transport.hpp"

namespace aeronet {

HttpServer::LoopAction HttpServer::processSpecialMethods(ConnectionMapIt& cnxIt, const HttpRequest& req,
                                                         std::size_t consumedBytes,
                                                         std::chrono::steady_clock::time_point reqStart) {
  switch (req.method()) {
    case http::Method::OPTIONS:
      // OPTIONS * request (target="*") should return an Allow header listing supported methods.
      if (req.path() == "*") {
        HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
        // Compute allowed methods for server (root/global) - use router.allowedMethods("*")
        http::MethodBmp allowed = _router.allowedMethods("*");

        // Build Allow header value by iterating known methods
        RawChars allowVal;
        for (http::MethodIdx methodIdx = 0; methodIdx < http::kNbMethods; ++methodIdx) {
          if (http::isMethodSet(allowed, methodIdx)) {
            if (!allowVal.empty()) {
              allowVal.push_back(',');
            }
            allowVal.append(http::toMethodStr(http::fromMethodIdx(methodIdx)));
          }
        }
        resp.customHeader(http::Allow, allowVal).contentType(http::ContentTypeTextPlain);
        finalizeAndSendResponse(cnxIt, req, std::move(resp), consumedBytes, reqStart);
        return LoopAction::Continue;
      }
      break;
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
          allowTrace = req.tlsVersion().empty();
          break;
        case HttpServerConfig::TraceMethodPolicy::Disabled:
        default:
          allowTrace = false;
          break;
      }
      if (allowTrace) {
        // Reconstruct the request head from HttpRequest
        std::string_view reqDataEchoed(cnxIt->second.inBuffer.data(), consumedBytes);

        HttpResponse resp(http::StatusCodeOK, http::ReasonOK);
        resp.customHeader(http::ContentType, "message/http");
        resp.body(reqDataEchoed);
        finalizeAndSendResponse(cnxIt, req, std::move(resp), consumedBytes, reqStart);
        return LoopAction::Continue;
      }
      // TRACE disabled -> Method Not Allowed
      HttpResponse resp(http::StatusCodeMethodNotAllowed, http::ReasonMethodNotAllowed);
      resp.contentType(http::ContentTypeTextPlain).body(resp.reason());
      finalizeAndSendResponse(cnxIt, req, std::move(resp), consumedBytes, reqStart);
      return LoopAction::Continue;
    }
    case http::Method::CONNECT: {
      // CONNECT: establish a TCP tunnel to target (host:port). On success reply 200 and
      // proxy bytes bidirectionally between client and upstream.
      // Parse authority form in req.path() (host:port)
      const std::string_view target = req.path();
      const auto colonPos = target.find(':');
      if (colonPos == std::string_view::npos) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Malformed CONNECT target");
        return LoopAction::Break;
      }
      const std::string_view host(target.begin(), target.begin() + static_cast<size_t>(colonPos));
      const std::string_view portStr(target.data() + colonPos + 1, static_cast<size_t>(target.size() - colonPos - 1));

      // Enforce CONNECT allowlist if present
      if (!_config.connectAllowlist.empty() &&
          std::ranges::find(_config.connectAllowlist, host) == _config.connectAllowlist.end()) {
        emitSimpleError(cnxIt, http::StatusCodeForbidden, true, "CONNECT target not allowed");
        return LoopAction::Break;
      }

      // Use helper to resolve and initiate a non-blocking connect. The helper
      // returns a ConnectResult with an owned BaseFd and flags indicating
      // whether the connect is pending or failed.
      ConnectResult cres = ConnectTCP(cnxIt->second.inBuffer.data(), host, portStr);
      if (cres.failure) {
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Unable to resolve CONNECT target");
        return LoopAction::Break;
      }

      int upstreamFd = cres.cnx.fd();
      // Register upstream in event loop for edge-triggered reads and writes so we can detect
      // completion of non-blocking connect (EPOLLOUT) as well as incoming data.
      if (!_eventLoop.add(upstreamFd, EPOLLIN | EPOLLOUT | EPOLLET)) {
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Failed to register upstream fd");
        return LoopAction::Break;
      }
      // Insert upstream connection state. Inserting may rehash and invalidate the
      // caller's iterator; save the client's fd and re-resolve the client iterator
      // after emplacing.
      const int clientFd = cnxIt->first.fd();
      auto [upIt, inserted] = _connStates.emplace(std::move(cres.cnx), ConnectionState{});
      if (!inserted) {
        log::error("TCP connection ConnectionState fd # {} already exists, should not happen", upstreamFd);
        _eventLoop.del(upstreamFd);
        // Try to re-find client to report error; if not found, just return Break.
        emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Upstream connection tracking failed");
        return LoopAction::Break;
      }
      // Set upstream transport to plain (no TLS)
      upIt->second.transport = std::make_unique<PlainTransport>(upstreamFd);

      // If the connector indicated the connect is still in progress on this
      // non-blocking socket, mark state so the event loop's writable handler
      // can check SO_ERROR and surface failures. Use the connector's flag
      // rather than relying on errno here (errno may have been overwritten).

      // Reply 200 Connection Established to client
      // Since cnxIt is passed by reference we will update it here so the caller need not re-find.
      cnxIt = _connStates.find(clientFd);
      if (cnxIt == _connStates.end()) {
        throw std::runtime_error("Should not happen - Client connection vanished after upstream insertion");
      }

      HttpResponse resp(http::StatusCodeOK, "Connection Established");
      resp.contentType(http::ContentTypeTextPlain);
      finalizeAndSendResponse(cnxIt, req, std::move(resp), consumedBytes, reqStart);

      // Enter tunneling mode: link peer fds
      cnxIt->second.peerFd = upstreamFd;
      upIt->second.peerFd = cnxIt->first.fd();
      upIt->second.connectPending = cres.connectPending;

      // From now on, both connections bypass HTTP parsing; we simply proxy bytes. We'll rely on handleReadableClient
      // to read from each side and forward to the other by writing into the peer's transport directly.
      // Erase any partially parsed buffers for the client (we already replied)
      cnxIt->second.inBuffer.clear();
      upIt->second.inBuffer.clear();
      return LoopAction::Continue;
    }
    default:
      break;
  }
  return LoopAction::Nothing;
}

void HttpServer::finalizeAndSendResponse(ConnectionMapIt cnxIt, const HttpRequest& req, HttpResponse&& resp,
                                         std::size_t consumedBytes, std::chrono::steady_clock::time_point reqStart) {
  ConnectionState& state = cnxIt->second;
  ++state.requestsServed;
  bool keepAlive =
      _config.enableKeepAlive && state.requestsServed < _config.maxRequestsPerConnection && _lifecycle.isRunning();
  if (keepAlive) {
    std::string_view connVal = req.headerValueOrEmpty(http::Connection);
    if (connVal.empty()) {
      // Default is keep-alive for HTTP/1.1, close for HTTP/1.0
      keepAlive = req.version() == http::HTTP_1_1;
    } else if (CaseInsensitiveEqual(connVal, http::close)) {
      keepAlive = false;
    }
  }

  bool isHead = (req.method() == http::Method::HEAD);
  if (!isHead && !resp.userProvidedContentEncoding()) {
    const CompressionConfig& compressionConfig = _config.compression;
    std::string_view encHeader = req.headerValueOrEmpty(http::AcceptEncoding);
    auto [encoding, reject] = _encodingSelector.negotiateAcceptEncoding(encHeader);
    // If the client explicitly forbids identity (identity;q=0) and we have no acceptable
    // alternative encodings to offer, emit a 406 per RFC 9110 Section 12.5.3 guidance.
    if (reject) {
      resp.statusCode(http::StatusCodeNotAcceptable)
          .reason(http::ReasonNotAcceptable)
          .contentType(http::ContentTypeTextPlain)
          .body("No acceptable content-coding available");
    }
    // Apply size threshold for non-streaming (buffered) responses: if body below minBytes skip compression.
    else if (encoding != Encoding::none && resp.body().size() < compressionConfig.minBytes) {
      encoding = Encoding::none;
    }
    // Approximate allowlist check
    if (encoding != Encoding::none && !compressionConfig.contentTypeAllowlist.empty()) {
      std::string_view contentType = req.headerValueOrEmpty(http::ContentType);
      if (std::ranges::none_of(compressionConfig.contentTypeAllowlist,
                               [contentType](std::string_view str) { return contentType.starts_with(str); })) {
        encoding = Encoding::none;
      }
    }
    if (encoding != Encoding::none) {
      auto& encoder = _encoders[static_cast<size_t>(encoding)];
      if (encoder) {
        auto out = encoder->encodeFull(compressionConfig.encoderChunkSize, resp.body());
        resp.customHeader(http::ContentEncoding, GetEncodingStr(encoding));
        if (compressionConfig.addVaryHeader) {
          resp.customHeader(http::Vary, http::AcceptEncoding);
        }
        resp.body(out);
      }
    }
  }

  queueData(cnxIt, resp.finalizeAndStealData(req.version(), SysClock::now(), keepAlive, _config.globalHeaders, isHead,
                                             _config.minCapturedBodySize));

  state.inBuffer.erase_front(consumedBytes);
  if (!keepAlive && state.outBuffer.empty()) {
    state.requestDrainAndClose();
  }
  if (_metricsCb) {
    RequestMetrics metrics;
    metrics.method = req.method();
    metrics.path = req.path();
    metrics.status = resp.statusCode();
    metrics.bytesIn = req.body().size();
    metrics.reusedConnection = state.requestsServed > 0;
    metrics.duration = std::chrono::steady_clock::now() - reqStart;
    _metricsCb(metrics);
  }
}

bool HttpServer::queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData) {
  ConnectionState& state = cnxIt->second;

  const auto totalSz = httpResponseData.remainingSize();

  if (state.outBuffer.empty()) {
    // Plain TCP path: try immediate write optimization
    TransportHint want;
    const std::size_t written = state.transportWrite(httpResponseData, want);
    switch (want) {
      case TransportHint::Error:
        state.requestImmediateClose();
        return false;
      case TransportHint::ReadReady:
        [[fallthrough]];
      case TransportHint::WriteReady:
        [[fallthrough]];
      case TransportHint::None:
        if (std::cmp_equal(written, totalSz)) {
          _stats.totalBytesQueued += static_cast<uint64_t>(totalSz);
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
  _stats.totalBytesQueued += static_cast<uint64_t>(totalSz);
  _stats.maxConnectionOutboundBuffer = std::max(_stats.maxConnectionOutboundBuffer, remainingSize);
  if (remainingSize > _config.maxOutboundBufferBytes) {
    state.requestImmediateClose();
  }
  if (!state.waitingWritable) {
    if (HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLOUT | EPOLLET,
                                          "enable writable buffered path", _stats)) {
      state.waitingWritable = true;
      ++_stats.deferredWriteEvents;
    }
  }

  // If we buffered data, try flushing it immediately
  if (!state.outBuffer.empty()) {
    flushOutbound(cnxIt);
  }

  return true;
}

void HttpServer::flushOutbound(ConnectionMapIt cnxIt) {
  ++_stats.flushCycles;
  TransportHint want = TransportHint::None;
  ConnectionState& state = cnxIt->second;
  const int fd = cnxIt->first.fd();
  while (!state.outBuffer.empty()) {
    const std::size_t written = state.transportWrite(state.outBuffer, want);
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

  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.outBuffer.empty() && state.waitingWritable && (state.tlsEstablished || state.transport->handshakeDone()) &&
      HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLET,
                                        "disable writable flushOutbound drop EPOLLOUT", _stats)) {
    state.waitingWritable = false;
    if (state.isAnyCloseRequested()) {
      return;
    }
  }
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (state.outBuffer.empty()) {
    bool transportNeedsWrite = (!state.tlsEstablished && want == TransportHint::WriteReady);
    if (transportNeedsWrite) {
      if (!state.waitingWritable) {
        if (!HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLOUT | EPOLLET,
                                               "enable writable flushOutbound transportNeedsWrite", _stats)) {
          return;  // failure logged
        }
        state.waitingWritable = true;
      }
    } else if (state.waitingWritable) {
      state.waitingWritable = false;
      HttpServer::ModWithCloseOnFailure(_eventLoop, cnxIt, EPOLLIN | EPOLLET,
                                        "disable writable flushOutbound transport no longer needs", _stats);
    }
  }
}

}  // namespace aeronet
