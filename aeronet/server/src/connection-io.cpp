#include "aeronet/internal/connection-io.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/tcp-cork-guard.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy.hpp"
#include "tunnel-manager.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet::internal {

namespace {

// Maximum number of buffers gathered in a write of the HTTP/1 output (up to 2 per message: its head and its payload).
constexpr std::size_t kMaxGatheredBuffers = ZeroCopyState::kMaxBuffers;

}  // namespace

// ============================================================================
// Output
// ============================================================================

TransportResult ConnectionIo::writeMessage(ConnectionIt cnxIt, const HttpMessageData& data, bool& zerocopySent) {
  const std::string_view buffers[]{data.firstBuffer(), data.secondBuffer()};
  return writeMessages(cnxIt, buffers, {data.remainingSize(), data.retainedSize(), 1}, zerocopySent);
}

TransportResult ConnectionIo::writeMessages(ConnectionIt cnxIt, std::span<const std::string_view> buffers,
                                            [[maybe_unused]] const ConnectionState::GatheredOutput& output,
                                            [[maybe_unused]] bool& zerocopySent) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
#ifdef AERONET_LINUX
  const HttpServerConfig& config = ctx.config;
  if (state.zerocopyEnabled && output.size >= config.zerocopyMinBytes && ctx.connections.now >= _zerocopy.retryTime) {
    // A buffer written with MSG_ZEROCOPY is retained until the completion of its sends. The retained buffers of a
    // connection are bounded by maxZerocopyPendingBytes, each counting for at least zerocopyMinBytes, a single larger
    // write being allowed when nothing is retained.
    if (!state.zerocopyBuffers.canRetain(output.nbMessages, output.retainedSize, config.zerocopyMinBytes,
                                         config.maxZerocopyPendingBytes)) {
      ++ctx.stats.zerocopyRetainLimitFallbacks;
    } else {
      PlainTransport* plain = state.transport.get<PlainTransport>();
      assert(plain != nullptr);
      const auto [bytesSent, status] = plain->writeZerocopy(buffers);
      if (status == ZerocopySendStatus::Sent) {
        zerocopySent = true;
        ++ctx.stats.zerocopySends;
        ctx.stats.zerocopyBytes += bytesSent;
        // Watched until their completion, even when keep-alive does not apply (see
        // ConnectionMaintenance::watchZerocopySends()).
        ctx.maintenance.armDeadlineIfUnset(state, cnxIt->fd(), ctx.connections.now + config.keepAliveTimeout);
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
        ++ctx.stats.zerocopyNoBufferSpaceFallbacks;
        _zerocopy.retryTime = ctx.connections.now + kZerocopyNoBufferSpaceBackoff;
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
  return state.transportWrite(buffers);
}

#ifdef AERONET_LINUX
ZerocopyCompletions ConnectionIo::pollZerocopyCompletions(ConnectionState& state) {
  const ZerocopyCompletions completions = state.pollZerocopyCompletions();
  _pCtx->stats.zerocopyCopiedSends += completions.nbCopiedSends;
  return completions;
}

void ConnectionIo::releaseResetZerocopyBuffers(std::chrono::steady_clock::time_point now) {
  auto& buffers = _zerocopy.resetBuffers;
  const auto firstKept = std::ranges::find_if(
      buffers, [now](const ZerocopyRuntime::ResetBuffer& buffer) { return now < buffer.releaseTime; });
  buffers.erase(buffers.begin(), firstKept);
}

void ConnectionIo::waitForResetZerocopyBuffers() {
  if (!_zerocopy.resetBuffers.empty()) {
    std::this_thread::sleep_until(_zerocopy.resetBuffers.back().releaseTime);
    _zerocopy.resetBuffers.clear();
  }
}
#endif

void ConnectionIo::queueData(ConnectionIt cnxIt, HttpMessageData httpResponseData) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  if (state.isAnyCloseRequested()) {
    return;
  }

  auto* pFilePayload = httpResponseData.getIfFilePayload();
  const std::uint64_t extraQueuedBytes = pFilePayload != nullptr ? pFilePayload->length : 0;

  const auto bufferedSz = httpResponseData.remainingSize();

  // The response to a request followed by others in the input is written with theirs (see batchOutputOf()), unless it
  // has a file or would make the batch too large: the batch is then written along with it.
  if (&state == _pOutputBatch && pFilePayload == nullptr && !state.waitingWritable && !state.isSendingFile() &&
      state.bufferedOutputBytes() + bufferedSz <= kMaxBatchedOutputBytes) {
    if (state.outBuffer.empty() && state.outQueue.empty()) {
      assert(!state.outBufferZerocopySent);
      state.outBuffer = std::move(httpResponseData);
    } else {
      (void)state.queueOutput(std::move(httpResponseData));
    }
    ctx.stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz);
    return;
  }

  if (state.hasPendingOutput() || state.isSendingFile()) {
    // Previous output still waits for the socket to become writable: queue this data (its file payload included)
    // after it.
    const bool merged = state.queueOutput(std::move(httpResponseData));
    ctx.stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
    const std::size_t bufferedBytes = state.bufferedOutputBytes();
    ctx.stats.maxConnectionOutboundBuffer.updateMax(bufferedBytes);
    if (bufferedBytes > ctx.config.maxOutboundBufferBytes) {
      state.requestDrainAndClose();
    }
    // While the socket is full (its writable event awaited), small data merged into the waiting output leaves the write
    // to that event: retrying it would most often fail, for each event of a stream. A new queued entry (at most every
    // OutputQueue::kMaxMergedSize bytes) retries it, so that a handler writing a long response without returning to
    // the event loop keeps it flowing out as the peer reads.
    if (!merged || !state.waitingWritable) {
      flushOutbound(cnxIt);
    }
    return;
  }

  // Extract file payload early so we can move the File once and avoid double-moves
  // when the response writes immediately.
  FilePayload filePayload;
  const bool haveFilePayload = pFilePayload != nullptr;
  if (haveFilePayload) {
    filePayload = std::move(*pFilePayload);
  }

  // Cork the socket to coalesce the writes of the response (its head then its file, or several TLS records) into fewer
  // TCP segments. The guard uncorks on scope exit, flushing any accumulated data.
  const TcpCorkGuard corkGuard(state.needsCork(bufferedSz, haveFilePayload) ? cnxIt->fd() : kInvalidHandle);

  // Plain TCP path: try immediate write optimization
  bool zerocopySent = false;
  const auto [written, want] = writeMessage(cnxIt, httpResponseData, zerocopySent);
  if (want == TransportHint::Error) {
    // A failed write sent nothing with MSG_ZEROCOPY: the data can be released.
    state.requestDrainAndClose();
    return;
  }
  if (written == bufferedSz) {
    ctx.stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
    ctx.stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);
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
  ctx.stats.totalBytesWrittenImmediate += static_cast<uint64_t>(written);

  const std::size_t remainingSize = state.outBuffer.remainingSize();
  ctx.stats.totalBytesQueued += static_cast<uint64_t>(bufferedSz + extraQueuedBytes);
  ctx.stats.maxConnectionOutboundBuffer.updateMax(remainingSize);
  if (remainingSize > ctx.config.maxOutboundBufferBytes) {
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

std::size_t ConnectionIo::writeOutBuffer(ConnectionIt cnxIt, TransportHint& want) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  bool zerocopySent = false;
  TransportResult result;
  if (state.outQueue.empty() || state.isSendingFile()) {
    result = writeMessage(cnxIt, state.outBuffer, zerocopySent);
  } else {
    auto& buffers = ctx.sharedBuffers.sv;
    const ConnectionState::GatheredOutput output = state.gatherOutput(buffers, kMaxGatheredBuffers);
    result = writeMessages(cnxIt, buffers, output, zerocopySent);
  }
  want = result.want;
  ctx.stats.totalBytesWrittenFlush += result.bytesProcessed;
  if (want == TransportHint::Error) {
    log::error("send/transportWrite failed fd # {} err={}", cnxIt->fd(), LastSystemError());
    // Drops the output (outBuffer being retained if previous zerocopy sends may still read it, the socket staying open
    // until they complete) and closes.
    state.clearBuffers();
  } else if (result.bytesProcessed != 0) {
    state.outputWritten(result.bytesProcessed, zerocopySent);
  }
  return result.bytesProcessed;
}

void ConnectionIo::flushOutbound(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ++ctx.stats.flushCycles;
  TransportHint want = TransportHint::None;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);

  const NativeHandle fd = cnxIt->fd();

  // Cork the socket to coalesce the writes of the flush into fewer TCP segments.
  const TcpCorkGuard corkGuard(state.flushNeedsCork() ? fd : kInvalidHandle);

  // The HTTP/1 output, in order: outBuffer, the file sent after it (if any), then the queued entries.
  for (;;) {
    if (!state.outBuffer.empty()) {
      const std::size_t written = writeOutBuffer(cnxIt, want);
      if (want == TransportHint::Error) {
        break;
      }
      if (!state.outBuffer.empty()) {
        if (written == 0 || want != TransportHint::None) {
          // The socket is not writable (anymore). Wait for the writable event (enabled below) instead of retrying,
          // which would block the whole event loop for as long as the peer does not read.
          break;
        }
        continue;
      }
    }
    if (state.isSendingFile()) {
      flushFilePayload(cnxIt);
      if (state.isSendingFile()) {
        break;
      }
    }
    if (state.outQueue.empty()) {
      break;
    }
    state.dequeueOutput();
  }
  if (state.protocolHandler != nullptr && state.outBuffer.empty() && state.outQueue.empty()) {
    // The output of the protocol handler follows the HTTP/1 one (its 101 Switching Protocols response).
    auto& fragments = ctx.sharedBuffers.sv;
    while (state.protocolHandler->hasPendingOutput()) {
      state.protocolHandler->getPendingOutputFragments(fragments);
      assert(!fragments.empty());

      const auto [written, stepWant] = state.transportWrite(fragments);
      want = stepWant;
      ctx.stats.totalBytesWrittenFlush += written;
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
  if (!state.isSendingFile() && !state.hasPendingOutput() && state.waitingWritable &&
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

void ConnectionIo::resumeInputIfOutputDrained(NativeHandle fd, ConnectionState& state) {
  // Whatever drained the output (a writable event, a flush before reading, the maintenance retry of a missed writable
  // event), no read event reports the input left by its backpressure.
  if (state.inputBlockedByOutput && !state.hasPendingOutput() && !state.isSendingFile()) {
    state.inputBlockedByOutput = false;
    if (!state.isAnyCloseRequested()) {
      deferInput(fd, state);
    }
  }
  // Same for the tunnel relays toward an HTTP/2 connection, stopped by its output (those toward an HTTP/1.1 tunnel
  // endpoint resume once its tunnel buffer is written, see TunnelManager::handleWritable()). They resume below half of
  // the limit that stopped them: the other streams of the connection could keep its output from ever being empty.
  if (state.relayPausedByBacklog && state.protocolHandler != nullptr &&
      state.protocolHandler->pendingOutputSize() < _pCtx->config.maxOutboundBufferBytes / 2) {
    assert(_pCtx->tunnels != nullptr);
    _pCtx->tunnels->resumeRelaysTowards(_pCtx->connections.iterator(fd));
  }
}

bool ConnectionIo::flushUserSpaceTlsBuffer(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  if (state.tunnelOrFileBuffer.empty()) {
    return false;
  }

  // Loop to drain the TLS buffer until it's empty or we would block (edge-triggered epoll requirement)
  for (;;) {
    const auto [written, want] = state.transportWrite(state.tunnelOrFileBuffer);

    if (want == TransportHint::Error) [[unlikely]] {
      // Drops the rest of the file (not read anymore) and the output queued behind it, and closes.
      state.clearBuffers();
      return true;
    }

    state.tunnelOrFileBuffer.erase_front(written);
    // Note: fileSend.offset and fileSend.remaining were already updated in transportFile when the data was read.
    // Do NOT update them again here or we'll double-count and prematurely mark the transfer complete.
    _pCtx->stats.totalBytesWrittenFlush += written;

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

void ConnectionIo::flushFilePayload(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);

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

  // With kTLS enabled, the kernel handles encryption for sendfile() directly.
  // Without kTLS, we must pread() into a buffer and SSL_write() (user-space TLS).
  const bool userSpaceTls = state.encryptsOutputInUserSpace();

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
        ctx.stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(res.bytesDone);
#ifdef AERONET_ENABLE_OPENSSL
        if (!userSpaceTls) {
          ctx.tls.metrics().ktlsSendBytes += static_cast<std::uint64_t>(res.bytesDone);
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
            ctx.stats.totalBytesWrittenFlush += static_cast<std::uint64_t>(retryRes.bytesDone);
#ifdef AERONET_ENABLE_OPENSSL
            if (!userSpaceTls) {
              ctx.tls.metrics().ktlsSendBytes += static_cast<std::uint64_t>(retryRes.bytesDone);
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

// ============================================================================
// Writable interest
// ============================================================================

bool ConnectionIo::enableWritableInterest(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  assert(!state.waitingWritable);
  if (ctx.eventLoop.mod(EventLoop::EventFd{cnxIt->fd(), EventIn | EventOut | EventRdHup | EventEt})) [[likely]] {
    state.waitingWritable = true;
    ++_nbWritableConnections;
    ++ctx.stats.deferredWriteEvents;
    return true;
  }
  // Cannot register for EPOLLOUT so buffered data will never be flushed.
  // Clear the write buffers so canCloseConnectionForDrain() can proceed;
  // without this, tunnel connections get stuck forever (exempt from keep-alive reaping).
  ++ctx.stats.epollModFailures;
  state.clearBuffers();
  return false;
}

bool ConnectionIo::disableWritableInterest(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  assert(state.waitingWritable);
  forgetWritableInterest(state);
  if (ctx.eventLoop.mod(EventLoop::EventFd{cnxIt->fd(), EventIn | EventRdHup | EventEt})) [[likely]] {
    return true;
  }
  ++ctx.stats.epollModFailures;
  state.clearBuffers();
  return false;
}

// ============================================================================
// Input
// ============================================================================

void ConnectionIo::deferInput(NativeHandle fd, ConnectionState& state) {
  // No wakeup needed: the next poll does not wait while there are deferred inputs (see SingleHttpServer::eventLoop()).
  if (!state.inputDeferred) {
    state.inputDeferred = true;
    _pendingReadFds.push_back(fd);
  }
}

// ============================================================================
// Close
// ============================================================================

void ConnectionIo::closeConnection(ConnectionIt cnxIt) {
  if (!startLingeringClose(cnxIt)) {
    closeConnectionNow(cnxIt);
  }
}

bool ConnectionIo::startLingeringClose(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  // The request answered last starts the input buffer until it is entirely read: an empty one (the common case) leaves
  // nothing to wait for. A connection still waiting for writable events (a TLS read wanting to write) closes right
  // away: a level-triggered poller (Windows) would keep reporting it writable while it lingers.
  if (state.inBuffer.empty() || !state.canCloseConnectionForDrain() || state.waitingWritable || state.eofReceived ||
      state.protocolHandler != nullptr || state.isTunneling() || ctx.config.lingeringCloseTimeout.count() == 0) {
    return false;
  }
  const NativeHandle fd = cnxIt->fd();
#ifdef AERONET_ENABLE_OPENSSL
  // Its close_notify alert ends the TLS stream before the TCP one.
  if (auto* pTlsTransport = state.transport.get<TlsTransport>(); pTlsTransport != nullptr) {
    pTlsTransport->shutdown();
  }
#endif
  if (!ShutdownWrite(fd)) {
    log::debug("shutdown of the write side failed for fd # {} err={}, closing it", fd, LastSystemError());
    return false;
  }
  log::debug("fd # {} lingers before its close", fd);
  ctx.maintenance.forgetConnectionMaintenance(state);
  state.closeMode = ConnectionState::CloseMode::Linger;
  state.inBuffer.clear();
  releaseServedInBuffer(state);
  ctx.maintenance.armDeadline(state, fd, ctx.connections.now + ctx.config.lingeringCloseTimeout);
  ctx.telemetry.counterAdd("aeronet.connections.lingering_closes");
  return true;
}

CloseStatus ConnectionIo::discardLingeringInput(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  lendInBuffer(state);
  CloseStatus closeStatus = CloseStatus::Keep;
  std::size_t bytesRead = 0;
  while (true) {
    const auto [count, want] =
        state.transportRead(ctx.config, ctx.config.computeReadChunkSize(bytesRead, state.readSizeShift));
    state.inBuffer.clear();
    bytesRead += count;
    if (want != TransportHint::None) {
      // A failed read ends it, as a TLS transport wanting to write: its write side is shut down.
      if (want != TransportHint::ReadReady) {
        closeStatus = CloseStatus::Close;
      }
      break;
    }
    if (count == 0) {
      closeStatus = CloseStatus::Close;
      break;
    }
    if (ctx.config.fairnessBudgetExhausted(bytesRead)) {
      deferInput(cnxIt->fd(), state);
      break;
    }
  }
  releaseServedInBuffer(state);
  ctx.telemetry.counterAdd("aeronet.bytes.read", static_cast<uint64_t>(bytesRead));
  return closeStatus;
}

void ConnectionIo::closeConnectionNow(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const auto cfd = cnxIt->fd();

  log::debug("closeConnection called for fd # {}", cfd);

  ctx.maintenance.forgetConnectionMaintenance(ctx.connections.connectionState(cnxIt));
  if (ctx.tunnels != nullptr) {
    ctx.tunnels->closeTunnelsOf(cnxIt);
    // Releasing the other ends of its tunnels may have moved the entries of the connection storage (Windows).
    cnxIt = ctx.connections.iterator(cfd);
  }
  releaseConnection(cnxIt);
}

void ConnectionIo::releaseConnection(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  const auto fd = cnxIt->fd();
  ctx.eventLoop.del(fd);
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  // The input buffer of the event loop may be lent to it: a close does not always come at the end of a read.
  releaseServedInBuffer(state);
#ifdef AERONET_LINUX
  // After a graceful close, the kernel would keep sending from the buffers of the zerocopy sends still in flight: abort
  // such a connection (reset), so that the kernel drops its send queue. Drain closes wait for the completions instead
  // (see ConnectionState::canCloseConnectionForDrain()).
  pollZerocopyCompletions(state);
  if (state.hasZerocopySendsInFlight()) {
    log::debug("Resetting fd # {} with zerocopy sends still in flight", fd);
    if (!SetAbortiveClose(fd)) [[unlikely]] {
      log::error("setsockopt(SO_LINGER) failed for fd # {} err={}", fd, LastSystemError());
    }
    // The packets already handed to the network device still reference the buffers for a short while. A fresh clock
    // read: the connection may be closed out of the event loop (server stop), where the cached time is not updated.
    const auto releaseTime = std::chrono::steady_clock::now() + kZerocopyResetGracePeriod;
    state.zerocopyBuffers.extract([this, releaseTime](HttpMessageData data) {
      _zerocopy.resetBuffers.push_back(ZerocopyRuntime::ResetBuffer{std::move(data), releaseTime});
    });
    if (state.outBufferZerocopySent) {
      _zerocopy.resetBuffers.push_back(ZerocopyRuntime::ResetBuffer{std::move(state.outBuffer), releaseTime});
      state.outBufferZerocopySent = false;
    }
    ++ctx.stats.zerocopyResetConnections;
    ctx.telemetry.counterAdd("aeronet.connections.aborted_with_zerocopy_in_flight");
  }
#endif
#ifdef AERONET_ENABLE_OPENSSL
  ctx.connections.recycleOrRelease(cnxIt, ctx.config.maxCachedConnections, ctx.config.tls.enabled,
                                   ctx.tls.handshakesInFlight());
#else
  ctx.connections.recycleOrRelease(cnxIt, ctx.config.maxCachedConnections);
#endif
}

void ConnectionIo::closeAllConnections() {
  ConnectionStorage& connections = _pCtx->connections;
#ifdef AERONET_WINDOWS
  while (!connections.empty()) {
    closeConnectionNow(connections.begin());
  }
#else
  for (auto cnxIt = connections.begin(); cnxIt != connections.end(); ++cnxIt) {
    if (*cnxIt) {
      closeConnectionNow(cnxIt);
    }
  }
#endif
  // Without connections, no tunnel is left: the resolver threads exit once their current resolution completes.
  _pCtx->tunnels.reset();
}

}  // namespace aeronet::internal
