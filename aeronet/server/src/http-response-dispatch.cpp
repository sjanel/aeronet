#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/connection.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/keep-alive-deadline-queue.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-cork-guard.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy.hpp"
#include "tunnel-manager.hpp"
#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {

namespace {

// Appends 'data' (only its head for a file response, whose body is sent separately) after the output still buffered in
// 'state', waiting for the socket to become writable.
void AppendToOutBuffer(ConnectionState& state, HttpMessageData data, bool headOnly) {
  // Room for the new head, so that appending it below does not reallocate a copy of outBuffer again.
  state.prepareOutBufferAppend(data.firstBuffer().size());
  if (headOnly) {
    state.outBuffer.append(data.firstBuffer());
  } else {
    state.outBuffer.append(std::move(data));
  }
}

}  // namespace

TransportResult SingleHttpServer::writeMessage(ConnectionIt cnxIt, const HttpMessageData& data,
                                               [[maybe_unused]] bool& zerocopySent) {
  ConnectionState& state = _connections.connectionState(cnxIt);
#ifdef AERONET_LINUX
  if (state.zerocopyEnabled && data.remainingSize() >= _config.zerocopyMinBytes &&
      _connections.now >= _zerocopy.retryTime) {
    // A buffer written with MSG_ZEROCOPY is retained until the completion of its sends. The retained bytes of a
    // connection are bounded by maxZerocopyPendingBytes, a single larger payload being allowed when nothing is
    // retained.
    const std::size_t retainedBytes = state.zerocopyBuffers.bytes();
    if (retainedBytes != 0 && retainedBytes + data.retainedSize() > _config.maxZerocopyPendingBytes) {
      ++_stats.zerocopyRetainLimitFallbacks;
    } else {
      PlainTransport* plain = state.transport.get<PlainTransport>();
      assert(plain != nullptr);
      const auto [bytesSent, status] = plain->writeZerocopy(data.firstBuffer(), data.secondBuffer());
      if (status == ZerocopySendStatus::Sent) {
        zerocopySent = true;
        ++_stats.zerocopySends;
        _stats.zerocopyBytes += bytesSent;
        // Watched until their completion, even when keep-alive does not apply (see watchZerocopySends()).
        if (!internal::KeepAliveDeadlineQueue::contains(state)) {
          _keepAliveDeadlines.upsert(state, cnxIt->fd(), _connections.now + _config.keepAliveTimeout);
        }
        return {bytesSent, TransportHint::None};
      }
      if (status == ZerocopySendStatus::WouldBlock) {
        return {0, TransportHint::WriteReady};
      }
      if (status == ZerocopySendStatus::Error) {
        return {0, TransportHint::Error};
      }
      if (status == ZerocopySendStatus::NoBufferSpace) {
        // Locked memory (RLIMIT_MEMLOCK, for the whole user) or socket option memory (net.core.optmem_max) exhausted:
        // copy for a while instead of failing each zerocopy send.
        ++_stats.zerocopyNoBufferSpaceFallbacks;
        _zerocopy.retryTime = _connections.now + kZerocopyNoBufferSpaceBackoff;
        if (!_zerocopy.noBufferSpaceLogged) {
          _zerocopy.noBufferSpaceLogged = true;
          log::warn(
              "MSG_ZEROCOPY send failed with ENOBUFS (RLIMIT_MEMLOCK or net.core.optmem_max too low?): copying for {} "
              "ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(kZerocopyNoBufferSpaceBackoff).count());
        }
      }
      // Nothing was sent (out of buffer space, interrupted, not supported): copy instead.
    }
  }
#endif
  return state.transportWrite(data);
}

#ifdef AERONET_LINUX
ZerocopyCompletions SingleHttpServer::pollZerocopyCompletions(ConnectionState& state) {
  const ZerocopyCompletions completions = state.pollZerocopyCompletions();
  _stats.zerocopyCopiedSends += completions.nbCopiedSends;
  return completions;
}
#endif

SingleHttpServer::LoopAction SingleHttpServer::processSpecialMethods(ConnectionIt& cnxIt, std::size_t consumedBytes,
                                                                     const CorsPolicy* pCorsPolicy) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;

  // Handle OPTIONS and TRACE via shared protocol-agnostic code
  if (request.method() == http::Method::OPTIONS || request.method() == http::Method::TRACE) {
    const SpecialMethodConfig config{
        .tracePolicy = _config.traceMethodPolicy,
        .isTls = !request.tlsVersion().empty(),
    };

    // For TRACE, we need to pass the raw request data
    const std::string_view requestData = (request.method() == http::Method::TRACE)
                                             ? std::string_view(state.inBuffer.data(), consumedBytes)
                                             : std::string_view{};

    std::optional<HttpResponse> result = ProcessSpecialMethods(request, _router, config, pCorsPolicy, requestData);
    if (result) {
      finalizeAndSendResponseForHttp1(cnxIt, std::move(*result), pCorsPolicy);
      return LoopAction::Continue;
    }
    // Not handled (e.g., not a preflight), fall through to normal processing
    return LoopAction::Nothing;
  }

  // CONNECT requires protocol-specific handling (TCP tunnel setup)
  if (request.method() == http::Method::CONNECT) {
    return internal::TunnelManager::Of(*this).startHttp1Tunnel(cnxIt, consumedBytes);
  }

  return LoopAction::Nothing;
}

void SingleHttpServer::finalizeAndSendResponseForHttp1(ConnectionIt cnxIt, HttpResponse&& resp,
                                                       const CorsPolicy* pCorsPolicy) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  if (pCorsPolicy != nullptr) {
    (void)pCorsPolicy->applyToResponse(request, resp);
  }

  // Clear per-route request deadline and headerStartTp now that a response is being sent.
  // Resetting headerStartTp to epoch allows transportRead to re-arm it for the next keep-alive request.
  clearRequestDeadline(state);
  state.headerStartTp = {};

  request.prefinalizeHttpResponse(resp, _telemetry);

  const bool keepAlive =
      request.isKeepAliveForHttp1(_config.enableKeepAlive, _config.maxRequestsPerConnection, _lifecycle.isRunning());

  const auto respStatusCode = resp.status();

  HttpResponse::Options opts;

  if (!keepAlive) {
    opts.setClose();
  }
  if (_config.addTrailerHeader) {
    opts.addTrailerHeader();
  }
  if (request.method() == http::Method::HEAD) {
    opts.setHeadMethod();
  }

  queueData(cnxIt, resp.finalizeForHttp1(_dateHeader.data(), request.version(), opts, &_config.globalHeaders,
                                         _config.minCapturedBodySize));

  if (!keepAlive) {
    // Always request drain+close for non-keep-alive connections. The actual close only triggers once outBuffer is empty
    // (checked by canCloseConnectionForDrain), so it is safe to call this before a large response finishes flushing.
    state.requestDrainAndClose();
  }
  // End the span after response is finalized
  endRequest(request, respStatusCode, state.requestsServed > 0);
}

void SingleHttpServer::queueData(ConnectionIt cnxIt, HttpMessageData httpResponseData) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.isAnyCloseRequested()) {
    return;
  }

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

  if (!state.outBuffer.empty()) {
    // Previous output still waits for the socket to become writable: queue this data after it.
    AppendToOutBuffer(state, std::move(httpResponseData), haveFilePayload);
    _stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
    const std::size_t remainingSize = state.outBuffer.remainingSize();
    _stats.maxConnectionOutboundBuffer.updateMax(remainingSize);
    if (remainingSize > _config.maxOutboundBufferBytes) {
      state.requestDrainAndClose();
    }
    if (haveFilePayload) {
      // Sent by flushFilePayload() once the buffered output, ending with this response head, is written.
      [[maybe_unused]] const bool sendNow = state.attachFilePayload(std::move(filePayload));
      assert(!sendNow);
    }
    flushOutbound(cnxIt);
    return;
  }

  // Cork the socket to coalesce header + body into fewer TCP segments.
  // The guard uncorks on scope exit, flushing any accumulated data.
  const TcpCorkGuard corkGuard(state.corkable ? cnxIt->fd() : kInvalidHandle);

  // Plain TCP path: try immediate write optimization
  bool zerocopySent = false;
  const auto [written, want] = writeMessage(cnxIt, httpResponseData, zerocopySent);
  if (want == TransportHint::Error) {
    // A failed write sent nothing with MSG_ZEROCOPY: the data can be released.
    state.requestDrainAndClose();
    return;
  }
  if (written == bufferedSz) {
    _stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
    _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
#ifdef AERONET_LINUX
    if (zerocopySent) {
      state.retainZerocopyBuffer(std::move(httpResponseData));
    }
#endif
    if (haveFilePayload && state.attachFilePayload(std::move(filePayload))) {
      flushFilePayload(cnxIt);
    }
    return;
  }
  // partial write, capture the buffer in the connection state
  httpResponseData.addOffset(static_cast<std::size_t>(written));
  assert(!state.outBufferZerocopySent);
  state.outBuffer = std::move(httpResponseData);
  state.outBufferZerocopySent = zerocopySent;
  _stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);

  const std::size_t remainingSize = state.outBuffer.remainingSize();
  _stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
  _stats.maxConnectionOutboundBuffer.updateMax(remainingSize);
  if (remainingSize > _config.maxOutboundBufferBytes) {
    state.requestDrainAndClose();
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
}

void SingleHttpServer::flushOutbound(ConnectionIt cnxIt) {
  ++_stats.flushCycles;
  TransportHint want = TransportHint::None;
  ConnectionState& state = _connections.connectionState(cnxIt);

  const NativeHandle fd = cnxIt->fd();

  // Cork the socket to coalesce buffered writes into fewer TCP segments.
  const TcpCorkGuard corkGuard(state.corkable ? fd : kInvalidHandle);

  while (!state.outBuffer.empty()) {
    bool zerocopySent = false;
    const auto [written, stepWant] = writeMessage(cnxIt, state.outBuffer, zerocopySent);
    state.outBufferZerocopySent = state.outBufferZerocopySent || zerocopySent;
    want = stepWant;
    _stats.totalBytesWrittenFlush += written;
    if (want == TransportHint::Error) {
      const auto savedErr = LastSystemError();
      log::error("send/transportWrite failed fd # {} err={}", fd, savedErr);
      state.requestDrainAndClose();
      // Previous zerocopy sends may still read it, the socket staying open until they complete.
      state.discardOutBuffer();
    } else if (written > 0) {
      if (written == state.outBuffer.remainingSize()) {
        state.discardOutBuffer();
        break;
      }
      state.outBuffer.addOffset(written);
      continue;
    }
    // Nothing written: the socket is not writable. Wait for the writable event (enabled below) instead of retrying,
    // which would block the whole event loop for as long as the peer does not read.
    break;
  }
  if (state.protocolHandler != nullptr) {
    auto& fragments = _sharedBuffers.sv;
    while (state.protocolHandler->hasPendingOutput()) {
      state.protocolHandler->getPendingOutputFragments(fragments);
      assert(!fragments.empty());

      const auto [written, stepWant] = state.transportWrite(fragments);
      want = stepWant;
      _stats.totalBytesWrittenFlush += written;
      if (written != 0) {
        state.protocolHandler->onOutputWritten(written);
      }
      if (want == TransportHint::Error) [[unlikely]] {
        const auto savedErr = LastSystemError();
        log::error("gather transportWrite failed fd # {} err={}", fd, savedErr);
        state.protocolHandler->discardPendingOutput();
        state.requestDrainAndClose();
        break;
      }
      if (want != TransportHint::None || written == 0) {
        break;
      }
    }
  }

  if (state.hasPendingOutput() && !state.waitingWritable && !enableWritableInterest(cnxIt)) {
    return;
  }

  if (state.outBuffer.empty() && state.fileSendHeadersPending) {
    state.fileSendHeadersPending = false;
  }

  // Determine if we can drop EPOLLOUT: only when no buffered data AND no handshake wantWrite pending.
  if (state.isSendingFile()) {
    flushFilePayload(cnxIt);
  } else if (!state.hasPendingOutput() && state.waitingWritable &&
             (state.tlsEstablished || state.transport.handshakeDone()) &&
             (!state.isTunneling() || state.tunnelOrFileBuffer.empty()) && disableWritableInterest(cnxIt) &&
             state.isAnyCloseRequested()) {
    return;
  }
  resumeInputIfOutputDrained(fd, state);
  // Clear writable interest if no buffered data and transport no longer needs write progress.
  // (We do not call handshakePending() here because ConnStateInternal does not expose it; transport has that.)
  if (!state.hasPendingOutput() && !state.isSendingFile() &&
      (!state.isTunneling() || state.tunnelOrFileBuffer.empty())) {
    const bool transportNeedsWrite = !state.tlsEstablished && want == TransportHint::WriteReady;
    if (transportNeedsWrite) {
      if (!state.waitingWritable && !enableWritableInterest(cnxIt)) {
        return;  // failure logged
      }
    } else if (state.waitingWritable) {
      disableWritableInterest(cnxIt);
    }
  }
}

void SingleHttpServer::resumeInputIfOutputDrained(NativeHandle fd, ConnectionState& state) {
  // Whatever drained the output (a writable event, a flush before reading, the maintenance retry of a missed writable
  // event), no read event reports the input left by its backpressure.
  if (state.inputBlockedByOutput && !state.hasPendingOutput() && !state.isSendingFile()) {
    state.inputBlockedByOutput = false;
    if (!state.isAnyCloseRequested()) {
      deferInput(fd, state);
    }
  }
}

bool SingleHttpServer::flushUserSpaceTlsBuffer(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  if (state.tunnelOrFileBuffer.empty()) {
    return false;
  }

  // Loop to drain the TLS buffer until it's empty or we would block (edge-triggered epoll requirement)
  for (;;) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);

    if (want == TransportHint::Error) [[unlikely]] {
      state.requestDrainAndClose();
      state.fileSendActive = false;
      state.tunnelOrFileBuffer.clear();
      return false;
    }

    state.tunnelOrFileBuffer.erase_front(written);
    // Note: fileSend.offset and fileSend.remaining were already updated in transportFile when the data was read.
    // Do NOT update them again here or we'll double-count and prematurely mark the transfer complete.
    _stats.totalBytesWrittenFlush += written;

    // If buffer is now empty, we're done
    if (state.tunnelOrFileBuffer.empty()) {
      if (state.fileSend.filePayload.length == 0) {
        state.fileSendActive = false;
      }
      return false;
    }

    // If we would block or transport needs write progress, enable writable interest and return
    if (want == TransportHint::WriteReady || written == 0) {
      if (!state.waitingWritable) {
        enableWritableInterest(cnxIt);
      }
      // Do NOT set fileSendActive = false here: tunnelOrFileBuffer is still non-empty.
      // Clearing active would cause flushOutbound to disable writable interest before the
      // remaining TLS bytes are actually written, producing truncated responses.
      return true;
    }

    // Otherwise, continue the loop to write more
  }
}

void SingleHttpServer::flushFilePayload(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);

  if (state.fileSendHeadersPending) {
    if (!state.outBuffer.empty()) {
      return;
    }
    state.fileSendHeadersPending = false;
  }

  // Only treat the send as fully complete when both the file is exhausted AND any
  // user-space TLS staging buffer has been drained.  Clearing tunnelOrFileBuffer here
  // while data is still pending would drop the last chunk of the response and produce
  // truncated bodies on macOS / any platform without kTLS.
  if (state.fileSend.filePayload.length == 0 && state.tunnelOrFileBuffer.empty()) {
    state.fileSendActive = false;
    return;
  }

  if (!state.transport.handshakeDone()) {
    return;
  }

  // Determine if this is a TLS connection and whether kTLS is active.
  // With kTLS enabled, the kernel handles encryption for sendfile() directly.
  // Without kTLS, we must pread() into a buffer and SSL_write() (user-space TLS).
  bool userSpaceTls = false;
#ifdef AERONET_ENABLE_OPENSSL
  if (auto* tlsTr = state.transport.get<TlsTransport>()) {
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

    if (state.fileSend.filePayload.length == 0) {
      state.fileSendActive = false;
      state.tunnelOrFileBuffer.clear();
      return;
    }

    const auto res = state.transportFile(cnxIt->fd(), userSpaceTls);
    switch (res.code) {
      case ConnectionState::FileResult::Code::Read:
        // Read case: data read from file into buffer; now try to write it immediately.
        if (userSpaceTls && flushUserSpaceTlsBuffer(cnxIt)) {
          // Attempt to flush immediately; if it blocks/fails, we'll resume on next writable.
          return;  // Would block, wait for next writable event
        }
        // Successfully flushed, continue loop to read more
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
      case ConnectionState::FileResult::Code::WouldBlock:
        if (res.enableWritable && !state.waitingWritable) {
          // The helper reports WouldBlock; enable writable interest so we can resume later.
          enableWritableInterest(cnxIt);

          // Edge-triggered epoll fix: immediately retry ONCE after enabling writable interest.
          // If the socket became writable between sendfile() returning EAGAIN and epoll_ctl(),
          // we would miss the edge. This immediate retry catches that case.
          const auto retryRes = state.transportFile(cnxIt->fd(), userSpaceTls);
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
        // Would block, wait for next writable event
        return;
      default:
        assert(res.code == ConnectionState::FileResult::Code::Error);
        return;  // Error, stop
    }
  }
}

}  // namespace aeronet
