#include "aeronet/connection-state.hpp"

#ifdef AERONET_POSIX
#include <sys/socket.h>
#endif

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "aeronet/file-payload.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sendfile.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/transport-result.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"
#include "aeronet/zerocopy.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake.hpp"
#include "aeronet/tls-metrics.hpp"
#include "aeronet/tls-transport.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#endif

namespace aeronet {

void ConnectionState::initializeStateNewConnection(const HttpServerConfig& config, const sockaddr_storage& peerAddress,
                                                   CompressionState& compressionState) {
  request.init(config, compressionState);
  const bool isIpAddress = peerAddress.ss_family == AF_INET || peerAddress.ss_family == AF_INET6;
  if (isIpAddress) {
    static constexpr uint8_t kClientAddressLengthMask =
        static_cast<uint8_t>((1U << kFormattedAddressCapacityNbBits) - 1U);
    const uint8_t formattedLen =
        FormatAddress(peerAddress, clientAddressBuffer, static_cast<uint8_t>(sizeof(clientAddressBuffer)));
    assert(formattedLen <= kClientAddressLengthMask);
    // clientAddressLength is a 6-bit bitfield: mask explicitly to make the narrowing conversion intentional.
    clientAddressLength = static_cast<uint8_t>(formattedLen & kClientAddressLengthMask);
  } else {
    clientAddressLength = 0;
  }

  // Decide per-connection zerocopy preference at accept time (the server also requires a plain socket accepted from a
  // listener with SO_ZEROCOPY).
  switch (config.zerocopyMode) {
    case ZerocopyMode::Disabled:
      zerocopyEnabled = false;
      zerocopyStopOnCopy = false;
      break;
    case ZerocopyMode::Enabled:
      zerocopyEnabled = true;
      zerocopyStopOnCopy = false;
      break;
    default:
      assert(config.zerocopyMode == ZerocopyMode::Opportunistic);
      // The kernel copies the data sent to a local peer anyway. Loopback peers are known in advance; we don't bother
      // checking the local address since a server bound to a non-loopback IP while the peer is on loopback is not a
      // realistic scenario worth optimizing for. Other local peers (another network namespace of the host: containers
      // of the same node) are known once the kernel reports a copied send.
      zerocopyEnabled = isIpAddress && !IsLoopback(peerAddress);
      zerocopyStopOnCopy = true;
      break;
  }
}

TransportResult ConnectionState::transportRead(const HttpServerConfig& config, std::size_t chunkSize) {
  inBuffer.ensureAvailableCapacityExponential(chunkSize);

  const auto result = transport.read(inBuffer.data() + inBuffer.size(), chunkSize);
  inBuffer.addSize(result.bytesProcessed);
  if (result.bytesProcessed != 0) {
    static_assert(std::bit_width(HttpServerConfig::kMaxReadChunkBytes) < static_cast<int>(1U << kReadSizeShiftNbBits));
    static constexpr uint8_t kReadSizeShiftMask = (1U << kReadSizeShiftNbBits) - 1U;
    readSizeShift = static_cast<uint8_t>(config.adaptReadSizeShift(readSizeShift, chunkSize, result.bytesProcessed) &
                                         kReadSizeShiftMask);
  }
  return result;
}

bool ConnectionState::tunnelTransportWrite(NativeHandle fd) {
  const auto [written, want] = transportWrite(tunnelOrFileBuffer);
  if (want == TransportHint::Error) [[unlikely]] {
    // Fatal error writing tunnel data: close this connection
    return false;
  }
  tunnelOrFileBuffer.erase_front(written);
  // If still has data, keep EPOLLOUT registered
  if (!tunnelOrFileBuffer.empty()) {
    return true;
  }
  if (shutdownWritePending) {
    if (!ShutdownWrite(fd)) {
      log::warn("Failed to shutdown write for fd # {}", fd);
      return false;
    }
    shutdownWritePending = false;
  }
  // Tunnel buffer drained: fall through to normal flushOutbound handling
  return true;
}

ConnectionState::FileResult ConnectionState::transportFile(NativeHandle clientFd, bool tlsFlow) {
  // Kernel sendfile(2): use a large chunk to minimize syscalls.  The kernel transfers
  // directly from page-cache to socket buffer, so a large value just means fewer
  // transitions to/from kernel mode. A too small value would cause excessive syscalls and reduce throughput, especially
  // for large files.
  static constexpr std::size_t kSendfileChunk = 2UL << 20U;  // 2 MiB

  // TLS (pread) path: we read into a user-space buffer that then gets encrypted and
  // written via the transport. A much smaller chunk avoids allocating a huge buffer
  // and prevents deadlocks when the peer socket buffer is smaller than the chunk
  // (common in unit tests with blocking socketpairs, but also in real deployments
  // where TCP send-buffer may be ~128-256 KB).
  static constexpr std::size_t kTlsReadChunk = 128UL << 10U;  // 128 KiB

  const std::size_t chunkLimit = tlsFlow ? kTlsReadChunk : kSendfileChunk;
  const std::size_t maxBytes = std::min(fileSend.filePayload.length, chunkLimit);

  int64_t result;
  if (tlsFlow) {
    tunnelOrFileBuffer.ensureAvailableCapacityExponential(maxBytes);
    result =
        ReadOffset(fileSend.filePayload.file.fd(), tunnelOrFileBuffer.data(), maxBytes, fileSend.filePayload.offset);
  } else {
    result =
        Sendfile(clientFd, static_cast<int>(fileSend.filePayload.file.fd()), fileSend.filePayload.offset, maxBytes);
  }
  FileResult res{static_cast<std::size_t>(result), tlsFlow ? FileResult::Code::Read : FileResult::Code::Sent, tlsFlow};

  if (result == -1) [[unlikely]] {
    res.bytesDone = 0;
#ifdef AERONET_POSIX
    const int errnoVal = LastSystemError();
    switch (errnoVal) {
      case error::kWouldBlock:
        res.enableWritable = true;
        [[fallthrough]];
      case error::kInterrupted:
        if (!tlsFlow || fileSend.filePayload.length != 0) {
          res.code = FileResult::Code::WouldBlock;
        }
        return res;
      default:
        res.code = FileResult::Code::Error;
        // ECONNRESET / error::kBrokenPipe / ECONNABORTED are normal peer-close events (client closed before
        // transfer finished). Downgrade to debug to avoid flooding logs during high concurrency.
        if ((errnoVal == error::kConnectionReset || errnoVal == error::kBrokenPipe ||
             errnoVal == error::kConnectionAborted)) {
          log::debug("{} peer closed during {} sendfile fd # {} err={} msg={}", tlsFlow ? "pread" : "sendfile",
                     tlsFlow ? "TLS" : "plain", clientFd, errnoVal, SystemErrorMessage(errnoVal));
        } else {
          log::error("{} failed during {} sendfile fd # {} err={} msg={}", tlsFlow ? "pread" : "sendfile",
                     tlsFlow ? "TLS" : "plain", clientFd, errnoVal, SystemErrorMessage(errnoVal));
        }
        // The output queued behind the file must not follow its truncated body, read as the rest of it.
        clearBuffers();
        return res;
    }
#elifdef AERONET_WINDOWS
    const int errVal = static_cast<int>(::GetLastError());
    if (errVal == error::kWouldBlock) {
      res.enableWritable = true;
      res.code = FileResult::Code::WouldBlock;
      return res;
    }
    res.code = FileResult::Code::Error;
    log::error("{} failed during {} sendfile fd # {} err={}", tlsFlow ? "ReadFile" : "TransmitFile",
               tlsFlow ? "TLS" : "plain", clientFd, errVal);
    // The output queued behind the file must not follow its truncated body, read as the rest of it.
    clearBuffers();
    return res;
#endif
  }
  if (tlsFlow) {
    tunnelOrFileBuffer.setSize(res.bytesDone);

    // Update file send offsets according to bytes read.
    fileSend.filePayload.offset += res.bytesDone;
    fileSend.filePayload.length -= res.bytesDone;

    if (tunnelOrFileBuffer.empty() && fileSend.filePayload.length == 0) {
      fileSendActive = false;
    }
  } else {
    // Successful transfer: update state based on the modified offset
    if (result > 0) {
      fileSend.filePayload.length -= static_cast<std::size_t>(result);
    } else {  // 0
      // sendfile() returning 0 with a non-blocking socket typically means the socket would block.
      // Treat it as WouldBlock to enable writable interest and wait for the socket to be ready.
      res.code = FileResult::Code::WouldBlock;
      res.enableWritable = true;
    }
    if (fileSend.filePayload.length == 0) {
      fileSendActive = false;
      tunnelOrFileBuffer.clear();
    }
  }
  return res;
}

void ConnectionState::installAggregatedBodyBridge() { request.installAggregatedBodyBridge(bodyStreamContext); }

#ifdef AERONET_ENABLE_OPENSSL
bool ConnectionState::finalizeAndEmitTlsHandshakeIfNeeded(NativeHandle fd, const TlsHandshakeCallback& cb,
                                                          TlsMetricsInternal& metrics, const TLSConfig& cfg) {
  auto* tlsTr = transport.get<TlsTransport>();
  if (tlsTr == nullptr) {
    return false;
  }

  auto* ssl = tlsTr->rawSsl();

  tlsInfo =
      FinalizeTlsHandshake(ssl, fd, cfg.logHandshake, tlsHandshakeEventEmitted, cb, tlsInfo.handshakeStart, metrics);

  // SelectAlpn should abort the handshake (SSL_TLSEXT_ERR_ALERT_FATAL) before we reach here.
  assert(!tlsHandshakeObserver.alpnStrictMismatch && "ALPN strict mismatch should have aborted the handshake earlier");

  const auto ktlsMode = cfg.ktlsMode;
  if (ktlsMode != TLSConfig::KtlsMode::Disabled) {
    // Attempt to enable kTLS send offload. The transport caches the result so
    // isKtlsSendEnabled() can be queried later without re-checking the BIO.
    const auto application = MaybeEnableKtlsSend(tlsTr->enableKtlsSend(), fd, ktlsMode, metrics);
    if (application == KtlsApplication::CloseConnection) {
      requestDrainAndClose();
    }
  }

  return true;
}
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
AsyncHandlerState& ConnectionState::ensureAsyncState(AsyncHandlerStatePool& asyncStatePool) {
  if (!asyncState) {
    asyncState = asyncStatePool.allocateAndConstructPoolPtr();
  }
  return *asyncState;
}
#endif

void ConnectionState::reset() {
  // In order to avoid retaining large buffers in cached ConnectionState objects,
  // we shrink (before clear, otherwise it would free all memory) and clear them before reuse.
  const auto shrinkAndClear = [](auto& buffer) {
    buffer.shrink_to_fit();
    buffer.clear();
  };
  shrinkAndClear(inBuffer);
  shrinkAndClear(bodyAndTrailersBuffer);
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  asyncState.reset();
#endif
  shrinkAndClear(tunnelOrFileBuffer);

  request.shrinkAndMaybeClear();

  shrinkAndClear(outBuffer);
  // Never written, so not read by the kernel.
  outQueue.clear();
  outQueue.shrink_to_fit();
  // The socket is closed at this point, and the kernel no longer reads the zerocopy buffers: SingleHttpServer only
  // closes a connection gracefully once its zerocopy sends completed, and takes them over when resetting it (see
  // SingleHttpServer::releaseConnection()).
  shrinkAndClear(zerocopyBuffers);
  zerocopyStallCheckQueuedBytes = kNoZerocopyStallCheck;
  // no need to clear request, it's built from scratch from initTrySetHead
  bodyStreamContext = {};
  transport.reset();
  lastActivity = {};
  headerStartTp = {};
  bodyLastActivityMs = kInactiveRelativeMs;
  requestDeadlineMs = kInactiveRelativeMs;
  keepAliveDeadlineIndex = kNoKeepAliveDeadlineIndex;
  parsingHeaders = false;
  peerFd = kInvalidHandle;
  peerStreamId = 0;
  requestsServed = 0;
  trailerLen = 0;
  clientAddressLength = 0;
  readSizeShift = 0;
  closeMode = CloseMode::None;
  waitingWritable = false;
  tlsEstablished = false;
  waitingForBody = false;
  readPaused = false;
  inputDeferred = false;
  inputBlockedByOutput = false;
  relayPausedByBacklog = false;
  expectationAnswered = false;
  connectPending = false;
  tunnelResolving = false;
  shutdownWritePending = false;
  eofReceived = false;
  corkable = false;
  zerocopyEnabled = false;
  zerocopyStopOnCopy = false;
  outBufferZerocopySent = false;
  tlsInfo = {};
#ifdef AERONET_ENABLE_OPENSSL
  tlsHandshakeObserver = {};
  tlsHandshakeEventEmitted = false;
  tlsContextKeepAlive.reset();
  tlsHandshakeInFlight = false;
#endif
  fileSend = {};
  // A connection may close in the middle of a file transfer (peer gone): its next user must not look busy sending it.
  fileSendActive = false;
  fileSendHeadersPending = false;

  // Reset protocol handler (e.g., WebSocket, HTTP/2)
  protocolHandler.reset();
  protocol = ProtocolType::Http11;
}

bool ConnectionState::encryptsOutputInUserSpace() const noexcept {
#ifdef AERONET_ENABLE_OPENSSL
  const auto* tlsTr = transport.get<TlsTransport>();
  return tlsTr != nullptr && !tlsTr->isKtlsSendEnabled();
#else
  return false;
#endif
}

bool ConnectionState::needsCork([[maybe_unused]] std::size_t nbBytes, bool severalParts) const noexcept {
  if (!corkable) {
    return false;
  }
  if (severalParts) {
    return true;
  }
  // Otherwise a single write (plain socket or kTLS: one gather write, the kernel building the TLS records), unless
  // OpenSSL encrypts more than one record.
#ifdef AERONET_ENABLE_OPENSSL
  return nbBytes > TlsTransport::kMaxRecordPlainSize && encryptsOutputInUserSpace();
#else
  return false;
#endif
}

bool ConnectionState::flushNeedsCork() const noexcept {
  if (!corkable) {
    return false;
  }
  const bool hasHttp1Output = !outBuffer.empty() || !outQueue.empty();
  const std::size_t handlerBytes = protocolHandler != nullptr ? protocolHandler->pendingOutputSize() : 0;
  // Written apart: the file being sent from the output around it (the head of its response, the queued output), the
  // queued output from the files of its responses (it is gathered up to the first one), and the protocol handler output
  // from the HTTP/1 output ahead of it (its 101 Switching Protocols response).
  const bool severalParts = (hasHttp1Output && (isSendingFile() || handlerBytes != 0)) ||
                            std::ranges::any_of(outQueue.entries(), &HttpMessageData::hasFilePayload);
  std::size_t nbBytes = bufferedOutputBytes() + handlerBytes;
  if (isSendingFile()) {
    nbBytes += fileSend.filePayload.length + tunnelOrFileBuffer.size();
  }
  return needsCork(nbBytes, severalParts);
}

bool ConnectionState::attachFilePayload(FilePayload filePayload) {
  assert(!fileSendActive && "Cannot attach file payload while a previous file send is still active");
  fileSend.filePayload = std::move(filePayload);
  fileSendActive = fileSend.filePayload.length > 0;
  fileSendHeadersPending = !outBuffer.empty();
  // Don't enable writable interest here - let flushFilePayload do it when it actually blocks.
  // Enabling it prematurely (when the socket is already writable) causes us to miss the edge
  // in edge-triggered epoll mode.
  return isSendingFile() && !fileSendHeadersPending;
}

void ConnectionState::reclaimMemoryFromOversizedBuffers() {
  // Reclaim memory from oversized buffers between keep-alive requests.
  // These buffers grow via ensureAvailableCapacityExponential during I/O but never shrink
  // on their own - capacity is retained across requests on long-lived connections.
  // shrink_to_fit halves capacity when utilization is < 25%, avoiding aggressive reallocation
  // of live data while progressively reclaiming unused memory.

  // bodyAndTrailersBuffer: grows to accommodate decompressed request bodies (up to maxBodyBytes).
  // Safe to clear - body data has been consumed by the handler.
  // inBuffer: grows during transportRead to hold pipelined/accumulated request data.
  // Cannot clear - may contain a partial next request. shrink_to_fit alone is safe.
  // Both hold the request body (and the inBuffer the request head of a waiting body) of a running async handler, or of
  // an abandoned one whose deferred work still runs: they must not move then.
  // The inBuffer also holds the CONNECT request whose target is being resolved, answered (and logged) from its views
  // once the resolution completes: it must not move either.
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  if (!asyncState || (!asyncState->active && !asyncState->handle)) {
    bodyAndTrailersBuffer.shrink_to_fit();
    bodyAndTrailersBuffer.clear();
    if (!tunnelResolving) {
      inBuffer.shrink_to_fit();
    }
  }
#else
  bodyAndTrailersBuffer.shrink_to_fit();
  bodyAndTrailersBuffer.clear();
  if (!tunnelResolving) {
    inBuffer.shrink_to_fit();
  }
#endif

  // outBuffer: grows when TCP writes can't keep up and responses queue. Not while zerocopy sends may read it.
  if (!outBufferZerocopySent) {
    outBuffer.shrink_to_fit();
  }

  // outQueue: released once empty, as the common case does not use it.
  outQueue.shrink_to_fit();

  // zerocopyBuffers: holds one entry per written buffer still read by the kernel.
  zerocopyBuffers.shrink_to_fit();
}

#ifdef AERONET_LINUX
ZerocopyCompletions ConnectionState::pollZerocopyCompletions() noexcept {
  PlainTransport* plain = transport.get<PlainTransport>();
  if (plain == nullptr || !plain->zerocopy().pendingCompletions()) {
    return {};
  }
  const ZerocopyCompletions completions = plain->pollZerocopyCompletions();
  if (completions.nbCopiedSends != 0 && zerocopyStopOnCopy) {
    // The kernel copies the data of this connection anyway (local peer, device without scatter-gather): MSG_ZEROCOPY
    // would only add the cost of pinning the pages and of the notifications.
    zerocopyEnabled = false;
  }
  zerocopyBuffers.releaseCompleted(plain->zerocopy());
  if (!plain->zerocopy().pendingCompletions()) {
    outBufferZerocopySent = false;
  }
  return completions;
}

void ConnectionState::retainZerocopyBuffer(HttpMessageData data) {
  const PlainTransport* plain = transport.get<PlainTransport>();
  assert(plain != nullptr);
  zerocopyBuffers.retain(std::move(data), plain->zerocopy().nextSeq());
}
#endif

void ConnectionState::discardOutBuffer() {
#ifdef AERONET_LINUX
  if (outBufferZerocopySent) {
    retainZerocopyBuffer(std::move(outBuffer));
    outBufferZerocopySent = false;
  }
#endif
  outBuffer.clear();
}

bool ConnectionState::queueOutput(HttpMessageData data) {
  // outBuffer can only be appended to when it is the last output: no queued entry nor file payload after it.
  if (outQueue.empty() && !isSendingFile() && !outBufferZerocopySent && OutputQueue::CanMerge(outBuffer, data)) {
    outBuffer.append(std::move(data));
    return true;
  }
  return outQueue.push(std::move(data));
}

void ConnectionState::dequeueOutput() {
  assert(outBuffer.empty() && !outBufferZerocopySent && !isSendingFile());
  outBuffer = outQueue.pop();
  if (FilePayload* pFilePayload = outBuffer.getIfFilePayload()) {
    // Sent once outBuffer, the head of its response, is written.
    [[maybe_unused]] const bool sendNow = attachFilePayload(std::move(*pFilePayload));
    assert(!sendNow);
  }
}

ConnectionState::GatheredOutput ConnectionState::gatherOutput(vector<std::string_view>& buffers,
                                                              std::size_t maxBuffers) const {
  assert(!isSendingFile());
  buffers.clear();
  const auto gather = [&buffers](const HttpMessageData& data) {
    for (const std::string_view buffer : {data.firstBuffer(), data.secondBuffer()}) {
      if (!buffer.empty()) {
        buffers.push_back(buffer);
      }
    }
  };
  gather(outBuffer);
  GatheredOutput gathered{outBuffer.remainingSize(), outBuffer.retainedSize(), 1};
  for (const HttpMessageData& entry : outQueue.entries()) {
    if (buffers.size() + 2 > maxBuffers || entry.hasFilePayload()) {
      break;
    }
    gather(entry);
    gathered.size += entry.remainingSize();
    gathered.retainedSize += entry.retainedSize();
    ++gathered.nbMessages;
  }
  return gathered;
}

void ConnectionState::outputWritten(std::size_t written, bool zerocopySent) {
  outBufferZerocopySent = outBufferZerocopySent || zerocopySent;
  std::size_t entrySize = outBuffer.remainingSize();
  if (written < entrySize) {
    outBuffer.addOffset(written);
    return;
  }
  written -= entrySize;
  discardOutBuffer();
  while (written != 0) {
    // The next entries, gathered after outBuffer: none of them has a file payload, sent apart.
    HttpMessageData entry = outQueue.pop();
    assert(!entry.hasFilePayload());
    entrySize = entry.remainingSize();
    if (written < entrySize) {
      entry.addOffset(written);
      outBuffer = std::move(entry);
      outBufferZerocopySent = zerocopySent;
      return;
    }
    written -= entrySize;
#ifdef AERONET_LINUX
    if (zerocopySent) {
      retainZerocopyBuffer(std::move(entry));
    }
#endif
  }
}

void ConnectionState::clearBuffers() {
  discardOutBuffer();
  outQueue.clear();
  // The file to send after outBuffer as well: a drain close would wait for it otherwise.
  fileSendActive = false;
  fileSendHeadersPending = false;
  if (protocolHandler != nullptr) {
    protocolHandler->discardPendingOutput();
  }
  tunnelOrFileBuffer.clear();
  requestDrainAndClose();
}

}  // namespace aeronet
