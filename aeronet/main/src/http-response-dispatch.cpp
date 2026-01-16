#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/event.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-prefinalize.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {

SingleHttpServer::LoopAction SingleHttpServer::processSpecialMethods(ConnectionMapIt& cnxIt, std::size_t consumedBytes,
                                                                     const CorsPolicy* pCorsPolicy) {
  HttpRequest& request = cnxIt->second->request;

  // Handle OPTIONS and TRACE via shared protocol-agnostic code
  if (request.method() == http::Method::OPTIONS || request.method() == http::Method::TRACE) {
    const SpecialMethodConfig config{
        .tracePolicy = _config.traceMethodPolicy,
        .isTls = !request.tlsVersion().empty(),
    };

    // For TRACE, we need to pass the raw request data
    const std::string_view requestData = (request.method() == http::Method::TRACE)
                                             ? std::string_view(cnxIt->second->inBuffer.data(), consumedBytes)
                                             : std::string_view{};

    auto result = ProcessSpecialMethods(request, _router, config, pCorsPolicy, requestData);
    if (result) {
      finalizeAndSendResponseForHttp1(cnxIt, std::move(*result), consumedBytes, pCorsPolicy);
      return LoopAction::Continue;
    }
    // Not handled (e.g., not a preflight), fall through to normal processing
    return LoopAction::Nothing;
  }

  // CONNECT requires protocol-specific handling (TCP tunnel setup)
  if (request.method() == http::Method::CONNECT) {
    return processConnectMethod(cnxIt, consumedBytes, pCorsPolicy);
  }

  return LoopAction::Nothing;
}

SingleHttpServer::LoopAction SingleHttpServer::processConnectMethod(ConnectionMapIt& cnxIt, std::size_t consumedBytes,
                                                                    const CorsPolicy* pCorsPolicy) {
  HttpRequest& request = cnxIt->second->request;

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
  if (!connectAllowList.empty() && !connectAllowList.containsCI(host)) {
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
  if (!_eventLoop.add(EventLoop::EventFd{upstreamFd, EventIn | EventOut | EventRdHup | EventEt})) [[unlikely]] {
    emitSimpleError(cnxIt, http::StatusCodeBadGateway, true, "Failed to register upstream fd");
    return LoopAction::Break;
  }

  // Insert upstream connection state. Inserting may rehash and invalidate the
  // caller's iterator; save the client's fd and re-resolve the client iterator
  // after emplacing.
  const int clientFd = cnxIt->first.fd();
  auto [upIt, inserted] = _connections.emplace(std::move(cres.cnx));
  // Note: Duplicate fd for a newly connected upstream socket indicates a library bug - the kernel
  // assigns unique fds for each socket(), and we remove closed connections before their fd can
  // be reused. Using assert to document this invariant.
  assert(inserted && "Duplicate upstream fd indicates library bug - connection not properly removed");

  // Set upstream transport to plain (no TLS)
  upIt->second->transport = std::make_unique<PlainTransport>(upstreamFd);

  // If the connector indicated the connect is still in progress on this
  // non-blocking socket, mark state so the event loop's writable handler
  // can check SO_ERROR and surface failures. Use the connector's flag
  // rather than relying on errno here (errno may have been overwritten).

  // Reply 200 Connection Established to client
  // Since cnxIt is passed by reference we will update it here so the caller need not re-find.
  // Note: The client connection cannot vanish during upstream insertion - map rehash only relocates
  // existing entries, it never removes them. We use assert to document this invariant.
  cnxIt = _connections.active.find(clientFd);
  assert(cnxIt != _connections.active.end() && "Client connection cannot vanish during map rehash");

  finalizeAndSendResponseForHttp1(cnxIt, HttpResponse("Connection Established"), consumedBytes, pCorsPolicy);

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

void SingleHttpServer::finalizeAndSendResponseForHttp1(ConnectionMapIt cnxIt, HttpResponse&& resp,
                                                       std::size_t consumedBytes, const CorsPolicy* pCorsPolicy) {
  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, resp);
  }

  // TODO: metrics should also be updated in HTTP/2
  ++state.requestsServed;
  ++_stats.totalRequestsServed;

  const bool isHead = (request.method() == http::Method::HEAD);
  internal::PrefinalizeHttpResponse(request, resp, isHead, _compression, _config);

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

  const auto respStatusCode = resp.status();

  queueData(cnxIt, resp.finalizeForHttp1(SysClock::now(), request.version(), !keepAlive, _config.globalHeaders, isHead,
                                         _config.minCapturedBodySize));

  state.inBuffer.erase_front(consumedBytes);
  if (!keepAlive && state.outBuffer.empty()) {
    state.requestDrainAndClose();
  }
  if (_callbacks.metrics) {
    emitRequestMetrics(request, respStatusCode, request.body().size(), state.requestsServed > 0);
  }

  // End the span after response is finalized
  request.end(respStatusCode);
}

bool SingleHttpServer::queueData(ConnectionMapIt cnxIt, HttpResponseData httpResponseData) {
  ConnectionState& state = *cnxIt->second;

  // Extract file payload early so we can move the File once and avoid double-moves
  // when the response writes immediately.
  auto* pFilePayload = httpResponseData.getIfFilePayload();
  FilePayload filePayload;
  bool haveFilePayload = false;
  if (pFilePayload != nullptr) {
    filePayload = std::move(*pFilePayload);
    haveFilePayload = true;
  }

  const std::uint64_t extraQueuedBytes = haveFilePayload ? filePayload.length : 0;

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
          if (haveFilePayload && state.attachFilePayload(std::move(filePayload))) {
            flushFilePayload(cnxIt);
          }
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

  if (haveFilePayload && state.attachFilePayload(std::move(filePayload))) {
    flushFilePayload(cnxIt);
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

  if (state.isSendingFile()) {
    flushFilePayload(cnxIt);
  }
  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  else if (state.outBuffer.empty() && state.waitingWritable &&
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

bool SingleHttpServer::flushUserSpaceTlsBuffer(ConnectionMapIt cnxIt) {
  ConnectionState& state = *cnxIt->second;
  if (state.tunnelOrFileBuffer.empty()) {
    return false;
  }

  // Loop to drain the TLS buffer until it's empty or we would block (edge-triggered epoll requirement)
  for (;;) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);

    if (want == TransportHint::Error) [[unlikely]] {
      state.requestImmediateClose();
      state.fileSend.active = false;
      state.tunnelOrFileBuffer.clear();
      return false;
    }

    state.tunnelOrFileBuffer.erase_front(written);
    // Note: fileSend.offset and fileSend.remaining were already updated in transportFile when the data was read.
    // Do NOT update them again here or we'll double-count and prematurely mark the transfer complete.
    _stats.totalBytesWrittenFlush += written;

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

  // Determine if this is a TLS connection and whether kTLS is active.
  // With kTLS enabled, the kernel handles encryption for sendfile() directly.
  // Without kTLS, we must pread() into a buffer and SSL_write() (user-space TLS).
  bool userSpaceTls = false;
#ifdef AERONET_ENABLE_OPENSSL
  if (auto* tlsTr = dynamic_cast<TlsTransport*>(state.transport.get())) {
    // kTLS enabled: kernel handles encryption, use sendfile() directly
    // kTLS not enabled: must use pread() + SSL_write() (user-space TLS fallback)
    userSpaceTls = !tlsTr->isKtlsSendEnabled();
  }
#endif

  // Loop to drain file payload while we can make progress (edge-triggered epoll requires this)
  for (;;) {
    if (userSpaceTls && flushUserSpaceTlsBuffer(cnxIt)) {
      // Pending TLS bytes were not fully flushed (would block or error); return and wait for next writable event.
      return;
    }

    if (state.fileSend.remaining == 0) {
      state.fileSend.active = false;
      state.tunnelOrFileBuffer.clear();
      return;
    }

    const auto res = state.transportFile(cnxIt->first.fd(), userSpaceTls);
    switch (res.code) {
      case ConnectionState::FileResult::Code::Read:
        // Read case: data read from file into buffer; now try to write it immediately.
        if (userSpaceTls) {
          // Attempt to flush immediately; if it blocks/fails, we'll resume on next writable.
          if (flushUserSpaceTlsBuffer(cnxIt)) {
            return;  // Would block, wait for next writable event
          }
          // Successfully flushed, continue loop to read more
        }
        break;
      case ConnectionState::FileResult::Code::Sent:
        _stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(res.bytesDone);
#ifdef AERONET_ENABLE_OPENSSL
        if (!userSpaceTls) {
          _tls.metrics.ktlsSendBytes += static_cast<std::uint64_t>(res.bytesDone);
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
          const auto retryRes = state.transportFile(cnxIt->first.fd(), userSpaceTls);
          if (retryRes.code == ConnectionState::FileResult::Code::Sent) {
            _stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(retryRes.bytesDone);
#ifdef AERONET_ENABLE_OPENSSL
            if (!userSpaceTls) {
              _tls.metrics.ktlsSendBytes += static_cast<std::uint64_t>(retryRes.bytesDone);
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
