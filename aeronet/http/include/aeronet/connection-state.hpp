#pragma once

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/file-payload.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/output-queue.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-buffers.hpp"
#include "aeronet/zerocopy.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-metrics.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/object-pool.hpp"
#endif

namespace aeronet {

struct HttpServerConfig;

#ifdef AERONET_ENABLE_OPENSSL
class TlsContext;
#endif

struct ConnectionState {
  void initializeStateNewConnection(const HttpServerConfig& config, const sockaddr_storage& peerAddress,
                                    CompressionState& compressionState);

  [[nodiscard]] bool isDrainCloseRequested() const noexcept { return closeMode == CloseMode::DrainThenClose; }
  [[nodiscard]] bool isAnyCloseRequested() const noexcept { return closeMode != CloseMode::None; }
  [[nodiscard]] std::string_view clientAddress() const noexcept { return {clientAddressBuffer, clientAddressLength}; }

  [[nodiscard]] bool isTunneling() const noexcept { return peerFd != kInvalidHandle; }
  [[nodiscard]] bool isSendingFile() const noexcept { return fileSendActive; }

  [[nodiscard]] bool hasPendingOutput() const noexcept {
    return !outBuffer.empty() || !outQueue.empty() ||
           (protocolHandler != nullptr && protocolHandler->hasPendingOutput());
  }

  // Size of the HTTP/1 output not written yet (the files to send excluded).
  [[nodiscard]] std::size_t bufferedOutputBytes() const noexcept {
    return outBuffer.remainingSize() + outQueue.bytes();
  }

  // Tells whether the output of this connection is encrypted by OpenSSL (TLS without kTLS send), which writes each of
  // its TLS records on its own.
  [[nodiscard]] bool encryptsOutputInUserSpace() const noexcept;

  // Tells whether a flush of 'nbBytes' of output, written in several parts if 'severalParts' (like a response head then
  // its file), should be wrapped in TCP_CORK (see corkable): only when it most likely takes several writes, that it
  // coalesces into full TCP segments. A single write gains nothing from it, at the cost of two setsockopt syscalls.
  [[nodiscard]] bool needsCork(std::size_t nbBytes, bool severalParts) const noexcept;

  // needsCork() for a flush of all the pending output: the HTTP/1 output with its files, then the protocol handler one.
  [[nodiscard]] bool flushNeedsCork() const noexcept;

  // Tells whether a requested drain close can proceed. Besides buffered output, it waits for the completion of the
  // zerocopy sends: after close, the kernel would still send from their buffers, released with the connection.
  [[nodiscard]] bool canCloseConnectionForDrain() const noexcept {
    return isDrainCloseRequested() && !hasPendingOutput() && tunnelOrFileBuffer.empty() && !isSendingFile() &&
           !hasZerocopySendsInFlight();
  }

  // Tells whether some zerocopy sends were not completed yet, as of their last poll (pollZerocopyCompletions()).
  // Always false when zerocopy is not used.
  [[nodiscard]] bool hasZerocopySendsInFlight() const noexcept {
    const PlainTransport* plain = transport.get<PlainTransport>();
    return plain != nullptr && plain->zerocopy().pendingCompletions();
  }

  // Request to close after draining currently buffered writes (graceful half-close semantics).
  void requestDrainAndClose() {
    if (closeMode == CloseMode::None) {
      closeMode = CloseMode::DrainThenClose;
    }
  }

  // Reads up to chunkSize bytes (see HttpServerConfig::computeReadChunkSize()) into inBuffer, then adapts the read size
  // of the connection to the bytes read.
  TransportResult transportRead(const HttpServerConfig& config, std::size_t chunkSize);

  // A write never marks the TLS handshake as established, even when it completes it: only
  // SingleHttpServer::finalizeTlsHandshakeIfReady() does, after recording the negotiated parameters and setting up the
  // protocol selected by ALPN.
  TransportResult transportWrite(std::string_view data) { return transport.write(data); }

  TransportResult transportWrite(std::span<const std::string_view> buffers) { return transport.write(buffers); }

  // Return true if success, false if fatal error.
  bool tunnelTransportWrite(NativeHandle fd);

  // Result of a kernel sendfile operation performed on this connection's fileSend state.
  struct FileResult {
    enum class Code : uint8_t { Read, Sent, WouldBlock, Error };

    std::size_t bytesDone{0};
    Code code{Code::Sent};
    // When code == WouldBlock, indicates the caller should enable writable interest
    // (true for error::kWouldBlock, false for error::kInterrupted).
    bool enableWritable{false};
  };

  // if tls is false:
  //   Attempt to send up to maxChunk bytes from the currently tracked file via the kernel
  //   sendfile(2) syscall. The method updates fileSend.offset and fileSend.remaining on
  //   successful transfers. It does NOT modify EPOLL interest; the caller should consult
  //   the returned SendfileResult and invoke enableWritableInterest/disableWritableInterest
  //   as appropriate.
  // if tls is true:
  //   Read up to `maxBytes` from the tracked file into `tunnelOrFileBuffer`. The method
  //   will not request EPOLL changes or log; it simply fills the buffer and returns a
  //   structured result so callers can decide on logging/closing/enabling writable interest.
  FileResult transportFile(NativeHandle clientFd, bool tlsFlow);

  // Helper to set up request body streaming bridges for aggregated body reading.
  void installAggregatedBodyBridge();

#ifdef AERONET_ENABLE_OPENSSL
  // Finalize TLS handshake (if this transport is TLS) and emit the handshake event.
  // Returns true if a TLS transport was finalized (caller may perform transport-specific book-keeping).
  bool finalizeAndEmitTlsHandshakeIfNeeded(NativeHandle fd, const TlsHandshakeCallback& cb, TlsMetricsInternal& metrics,
                                           const TLSConfig& cfg);
#endif
  // Reset the connection state usable for a new connection without freeing allocated buffers.
  void reset();

  // Returns true if we should flush file now
  bool attachFilePayload(FilePayload filePayload);

  void reclaimMemoryFromOversizedBuffers();

#ifdef AERONET_LINUX
  /// Reads the completion notifications of the zerocopy sends, and releases the buffers no longer read by the kernel.
  /// Once the kernel reports a copy, stops using MSG_ZEROCOPY if zerocopyStopOnCopy is set.
  ZerocopyCompletions pollZerocopyCompletions() noexcept;

  /// Retains 'data', written (in part) with MSG_ZEROCOPY, until the completion of the zerocopy sends issued so far:
  /// the kernel reads it until then. Freeing it earlier would let the kernel send whatever reuses its memory.
  void retainZerocopyBuffer(HttpMessageData data);
#endif

  /// Empties outBuffer, written or dropped: retains its storage if zerocopy sends may still read it.
  void discardOutBuffer();

  /// Queues 'data' (its file payload included) behind the HTTP/1 output not written yet. Neither 'data' nor that output
  /// is copied, except small ones (see OutputQueue::CanMerge()): 'data' is then appended to the last of them, unless
  /// zerocopy sends may read it. Returns true if 'data' was appended to the output ahead of it, false if queued on its
  /// own.
  bool queueOutput(HttpMessageData data);

  /// Continues the HTTP/1 output, outBuffer and its file payload being written, with the first queued entry: moved to
  /// outBuffer, its file payload being attached to be sent after it.
  void dequeueOutput();

  struct GatheredOutput {
    std::size_t size;          // bytes of the gathered buffers
    std::size_t retainedSize;  // bytes of their messages (see HttpMessageData::retainedSize())
    std::size_t nbMessages;    // number of their messages
  };

  /// Gathers in 'buffers' (cleared first) the non-empty unsent buffers of outBuffer, then of the queued entries written
  /// with it in a single write: up to the first one with a file payload (sent before what follows it), and within
  /// 'maxBuffers' buffers. outBuffer must have no file payload attached.
  GatheredOutput gatherOutput(vector<std::string_view>& buffers, std::size_t maxBuffers) const;

  /// Records that the first 'written' bytes of outBuffer, followed by the first queued entries (gathered in the same
  /// write), were written, with MSG_ZEROCOPY if 'zerocopySent'. The written buffers are released, or retained while
  /// zerocopy sends may read them, and the entry written in part becomes outBuffer.
  void outputWritten(std::size_t written, bool zerocopySent);

  /// Drops the output not written yet (outBuffer being retained if zerocopy sends may read it, the queued entries, the
  /// file being sent and the output of the protocol handler), then requests a drain close.
  void clearBuffers();

  using AggregatedBodyStreamContext = HttpRequestView::AggregatedBodyContext;

  // accumulated input raw data
  RawChars inBuffer;
  // decoded body + optional trailer headers (RFC 7230 §4.1.2)
  RawChars bodyAndTrailersBuffer;
  // Buffer used for tunneling raw bytes when peer is not writable, or for send file buffer (they are both mutually
  // exclusive).
  RawChars tunnelOrFileBuffer;
  // per-connection request object reused across dispatches
  HttpRequestView request;
  AggregatedBodyStreamContext bodyStreamContext;
  // pending outbound data not yet written
  HttpMessageData outBuffer;
  // HTTP/1 output queued behind outBuffer (and the file sent after it), produced while it waits for the socket.
  OutputQueue outQueue;
  // Written buffers still read by the kernel for zerocopy sends in flight.
  ZerocopyBuffers zerocopyBuffers;
  Transport transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity;
  // Start of the header read timer of the current HTTP/1.1 request: the accept of the connection for its first request,
  // the first bytes of the next ones (see SingleHttpServer::startHeaderReadTimer()).
  // Kept alive for the full request lifecycle as the reference point for bodyLastActivityMs / requestDeadlineMs.
  // Epoch means inactive (no request in progress).
  std::chrono::steady_clock::time_point headerStartTp;

  TLSInfo tlsInfo;

  // Tunnel support: when a connection is acting as a tunnel endpoint, peerFd holds the
  // file descriptor of the other side (upstream or client).
  NativeHandle peerFd{kInvalidHandle};
  // HTTP/2 CONNECT tunnel: when non-zero, this upstream connection is paired with a specific
  // HTTP/2 stream on the peer (HTTP/2 client) connection. Zero for HTTP/1.1 tunnels.
  uint32_t peerStreamId{0};
  uint32_t requestsServed{0};
  // Length of trailer headers in bodyAndTrailersBuffer (0 if no trailers).
  // Trailers occupy [bodyAndTrailersBuffer.size() - trailerLen, bodyAndTrailersBuffer.size()).
  uint32_t trailerLen{0};

  /// Sentinel value for bodyLastActivityMs / requestDeadlineMs indicating no active timestamp.
  static constexpr uint32_t kInactiveRelativeMs = static_cast<uint32_t>(-1);
  static constexpr uint32_t kNoKeepAliveDeadlineIndex = static_cast<uint32_t>(-1);
  /// Sentinel value for zerocopyStallCheckQueuedBytes when no stall check was made.
  static constexpr uint32_t kNoZerocopyStallCheck = static_cast<uint32_t>(-1);

  // Milliseconds since headerStartTp of last body read progress. kInactiveRelativeMs means inactive.
  uint32_t bodyLastActivityMs{kInactiveRelativeMs};
  // Milliseconds since headerStartTp of per-route handler deadline. kInactiveRelativeMs means inactive.
  uint32_t requestDeadlineMs{kInactiveRelativeMs};
  // Intrusive index in the server keep-alive deadline heap. kNoKeepAliveDeadlineIndex means unscheduled.
  uint32_t keepAliveDeadlineIndex{kNoKeepAliveDeadlineIndex};
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  uint32_t generation{0};
#endif

  // Bytes of the socket send queue at the previous check that the zerocopy sends in flight progress, while the
  // connection waits for their completion (see SingleHttpServer::watchZerocopySends()). kNoZerocopyStallCheck if none.
  uint32_t zerocopyStallCheckQueuedBytes{kNoZerocopyStallCheck};

  char clientAddressBuffer[kFormattedAddressCapacity];

  // Connection close lifecycle.
  enum class CloseMode : uint8_t { None, DrainThenClose };

  static constexpr uint8_t kFormattedAddressCapacityNbBits =
      static_cast<uint8_t>(std::bit_width(kFormattedAddressCapacity));

  uint8_t clientAddressLength : kFormattedAddressCapacityNbBits{0};

  // Pack small lifecycle/protocol state into one allocation unit to reduce
  // per-connection footprint.
  CloseMode closeMode : 1 {CloseMode::None};
  ProtocolType protocol : 2 {ProtocolType::Http11};  // Current protocol type.
  bool waitingWritable : 1 {false};                  // EPOLLOUT registered
  bool tlsEstablished : 1 {false};                   // true once the TLS handshake completed and was finalized
  bool waitingForBody : 1 {false};  // true when awaiting missing body bytes (bodyReadTimeout enforcement)
  bool parsingHeaders : 1 {false};  // true while request headers are being received (headerStartTp to head-parsed)
  // Tunnel state: true when peerFd != -1. Use accessor isTunneling() to query.
  // True when a non-blocking connect() was issued and completion is pending (EPOLLOUT will signal).
  bool connectPending : 1 {false};
  // True while the target host name of the HTTP/1.1 CONNECT request in inBuffer is resolved in the background: the
  // connection reads nothing until the tunnel is established (or refused).
  bool tunnelResolving : 1 {false};
  bool shutdownWritePending : 1 {false};    // true when we should shutdown(SHUT_WR) after tunnelOrFileBuffer is drained
  bool eofReceived : 1 {false};             // true when transportRead returned 0 (EOF)
  bool corkable : 1 {false};                // true when TCP_NODELAY is active; enables TCP_CORK coalescing
  bool fileSendActive : 1 {false};          // true while a file payload is attached and in progress
  bool fileSendHeadersPending : 1 {false};  // true until response headers are flushed before file payload

  // Whether the large writes of the responses use MSG_ZEROCOPY (plain sockets only, Linux).
  // Decided at accept time from the server configuration and the peer address.
  bool zerocopyEnabled : 1 {false};
  // Whether zerocopyEnabled is cleared once the kernel reports that it copied the data of a zerocopy send anyway
  // (local peer, device without scatter-gather): ZerocopyMode::Opportunistic.
  bool zerocopyStopOnCopy : 1 {false};
  // Whether some bytes of outBuffer were written with MSG_ZEROCOPY by sends not completed yet: outBuffer must then
  // neither be modified nor freed (see discardOutBuffer()).
  bool outBufferZerocopySent : 1 {false};

  // True when socket bytes may have been left unread while an HTTP/1 async handler was running (see
  // SingleHttpServer::handleReadableClient()): they must be read once it completes, no new event reporting them.
  bool readPaused : 1 {false};

  // True while the connection waits in SingleHttpServer::_pendingReadFds for its input to be resumed.
  bool inputDeferred : 1 {false};

  // True when the input processing stopped on output backpressure, leaving input in inBuffer or in the transport: no
  // read event reports it, so it is resumed once the output drained, whatever drained it (see
  // SingleHttpServer::flushOutbound).
  bool inputBlockedByOutput : 1 {false};

  // Tunnel backpressure: true when a relay toward this connection stopped reading its source because of the bytes this
  // connection has not written yet (its tunnel buffer, or its HTTP/2 output): it is resumed once they are written (see
  // internal::TunnelManager::resumeRelaysTowards()).
  bool relayPausedByBacklog : 1 {false};

  // True when the expectations (Expect header) of the request at the head of inBuffer were already answered: its head
  // is parsed again each time a part of its body is received, but interim responses must be sent once.
  bool expectationAnswered : 1 {false};

  static constexpr uint8_t kReadSizeShiftNbBits = 5;

  // Number of times the read size of the connection was doubled from HttpServerConfig::minReadChunkBytes (see
  // HttpServerConfig::adaptReadSizeShift()).
  uint8_t readSizeShift : kReadSizeShiftNbBits{0};

#ifdef AERONET_ENABLE_OPENSSL
  // Ensures the TLS handshake event callback is emitted at most once per connection.
  bool tlsHandshakeEventEmitted : 1 {false};

  // True while the TLS handshake for this connection is in-flight and counted against
  // concurrency limits.
  bool tlsHandshakeInFlight : 1 {false};

  // Observability / attribution for handshake failures.
  // Populated by OpenSSL callbacks via SSL ex_data (see tls-handshake-observer).
  TlsHandshakeObserver tlsHandshakeObserver;

  // Keep the TLS context alive for as long as this connection's SSL/handshake may reference
  // callback user pointers (ALPN/SNI). This is required for safe hot-reload of TLS contexts.
  std::shared_ptr<const TlsContext> tlsContextKeepAlive;
#endif

  struct FileSendState {
    FilePayload filePayload;
  };

  FileSendState fileSend;

  // Protocol handler for upgraded connections (WebSocket, HTTP/2).
  // nullptr when using default HTTP/1.1 processing (most connections).
  // When set, the server routes data through this handler instead of HTTP parsing.
  std::unique_ptr<IProtocolHandler> protocolHandler;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  [[nodiscard]] AsyncHandlerState* pAsyncState() const noexcept { return asyncState.get(); }

  // Tells whether deferred work (deferWork()) is still running for a coroutine of this connection: its frame and its
  // request must then be kept until the work completes, even if the connection is closed.
  [[nodiscard]] bool hasAsyncWorkInFlight() const noexcept {
    return (asyncState && asyncState->isAwaitingCallback()) ||
           (protocolHandler != nullptr && protocolHandler->hasAsyncWorkInFlight());
  }

  /// Allocate async state on first use from the bound pool and return it.
  AsyncHandlerState& ensureAsyncState(AsyncHandlerStatePool& asyncStatePool);

  // Async handler state is pooled by ConnectionStorage to avoid per-connection inline bloat.
  PoolPtr<AsyncHandlerState> asyncState;
#endif
};

}  // namespace aeronet
