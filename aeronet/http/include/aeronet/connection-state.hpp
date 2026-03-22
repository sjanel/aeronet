#pragma once

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/file-payload.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-metrics.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/async-handler-state.hpp"
#include "aeronet/http-response.hpp"
#endif

namespace aeronet {

struct HttpServerConfig;

#ifdef AERONET_ENABLE_OPENSSL
class TlsContext;
#endif

struct ConnectionState {
  void initializeStateNewConnection(const HttpServerConfig& config, const sockaddr_storage& peerAddress,
                                    internal::CompressionState& compressionState);

  [[nodiscard]] bool isDrainCloseRequested() const noexcept { return closeMode == CloseMode::DrainThenClose; }
  [[nodiscard]] bool isAnyCloseRequested() const noexcept { return closeMode != CloseMode::None; }
  [[nodiscard]] std::string_view clientAddress() const noexcept { return {clientAddressBuffer, clientAddressLength}; }

  [[nodiscard]] bool isTunneling() const noexcept { return peerFd != kInvalidHandle; }
  [[nodiscard]] bool isSendingFile() const noexcept { return fileSendActive; }

  [[nodiscard]] bool canCloseConnectionForDrain() const noexcept {
    return isDrainCloseRequested() && outBuffer.empty() && tunnelOrFileBuffer.empty() && !isSendingFile()
#ifdef AERONET_IO_URING
           && pendingOutBuffer.empty()
#endif
        ;
  }

  // Request to close after draining currently buffered writes (graceful half-close semantics).
  void requestDrainAndClose() {
    if (closeMode == CloseMode::None) {
      closeMode = CloseMode::DrainThenClose;
    }
  }

  ITransport::TransportResult transportRead(std::size_t chunkSize);

  ITransport::TransportResult transportWrite(std::string_view data);
  ITransport::TransportResult transportWrite(const HttpMessageData& httpResponseData);

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
  // zerocopyEnabled: if true and kTLS is active, enables MSG_ZEROCOPY on the kTLS socket.
  bool finalizeAndEmitTlsHandshakeIfNeeded(NativeHandle fd, const TlsHandshakeCallback& cb, TlsMetricsInternal& metrics,
                                           const TLSConfig& cfg);
#endif
  // Reset the connection state usable for a new connection without freeing allocated buffers.
  void reset();

  // Returns true if we should flush file now
  bool attachFilePayload(FilePayload filePayload);

  void reclaimMemoryFromOversizedBuffers();

  /// Hold the given buffer alive until all pending MSG_ZEROCOPY sends complete.
  /// MSG_ZEROCOPY pins user-space pages and the kernel DMA's from them asynchronously;
  /// freeing the buffer before the completion notification arrives causes data corruption.
  /// If no zerocopy sends are pending, the buffer is released immediately (goes out of scope).
  void holdBufferIfZerocopyPending(HttpMessageData buf);

  /// Poll the kernel error queue and release held zerocopy buffers whose sends have completed.
  void releaseCompletedZerocopyBuffers();

  struct AggregatedBodyStreamContext {
    std::string_view body;
    std::size_t offset{0};
  };

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
#ifdef AERONET_IO_URING
  // Response data queued while an async send from outBuffer is in flight (the kernel reads
  // from outBuffer until the send CQE arrives, so outBuffer cannot be appended to or moved).
  // Promoted into outBuffer and submitted when the in-flight send completes.
  HttpMessageData pendingOutBuffer;
#endif
  // Buffers sent via MSG_ZEROCOPY that must remain alive until the kernel signals
  // completion via the error queue. Without this, the allocator can reuse the freed
  // pages while the kernel is still DMA-ing from them, causing data corruption.
  vector<HttpMessageData> zerocopyPendingBuffers;
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity;
  // Timestamp of first byte of the current pending request headers.
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

  // Milliseconds since headerStartTp of last body read progress. kInactiveRelativeMs means inactive.
  uint32_t bodyLastActivityMs{kInactiveRelativeMs};
  // Milliseconds since headerStartTp of per-route handler deadline. kInactiveRelativeMs means inactive.
  uint32_t requestDeadlineMs{kInactiveRelativeMs};
  // Intrusive index in the server keep-alive deadline heap. kNoKeepAliveDeadlineIndex means unscheduled.
  uint32_t keepAliveDeadlineIndex{kNoKeepAliveDeadlineIndex};

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
  bool tlsEstablished : 1 {false};                   // true once TLS handshake completed (if TLS enabled)
  bool waitingForBody : 1 {false};  // true when awaiting missing body bytes (bodyReadTimeout enforcement)
  bool parsingHeaders : 1 {false};  // true while request headers are being received (headerStartTp to head-parsed)
  // Tunnel state: true when peerFd != -1. Use accessor isTunneling() to query.
  // True when a non-blocking connect() was issued and completion is pending (EPOLLOUT will signal).
  bool connectPending : 1 {false};
  bool shutdownWritePending : 1 {false};    // true when we should shutdown(SHUT_WR) after tunnelOrFileBuffer is drained
  bool eofReceived : 1 {false};             // true when transportRead returned 0 (EOF)
  bool corkable : 1 {false};                // true when TCP_NODELAY is active; enables TCP_CORK coalescing
  bool fileSendActive : 1 {false};          // true while a file payload is attached and in progress
  bool fileSendHeadersPending : 1 {false};  // true until response headers are flushed before file payload
  // True when this connection consumes inbound data via io_uring async recv (proactor).
  // When true, the event loop has neither registered a multishot poll nor performs synchronous
  // transport reads on this fd; instead a recv SQE is submitted, and EventDataArrived CQEs are
  // delivered to the dispatcher.
  bool usesAsyncRecv : 1 {false};
  // True when an async recv SQE is currently in flight in the kernel for this connection.
  // Used to avoid double-submission and to know when it is safe to grow inBuffer capacity.
  bool asyncRecvInFlight : 1 {false};
  // True when an async send SQE is currently in flight in the kernel for this connection.
  // While set, the kernel reads from outBuffer: outBuffer must not be mutated (no append,
  // no shrink, no move) — new response data is staged in pendingOutBuffer instead.
  bool asyncSendInFlight : 1 {false};
  // True when the connection is closed from the server's point of view but async recv/send
  // CQEs are still in flight: the kernel holds pointers into inBuffer/outBuffer, so buffer
  // release is deferred until the terminal CQEs are harvested (see releaseConnection).
  bool closePendingAsyncCqe : 1 {false};
  // Grace marker for parked connections: set the first time the maintenance sweep visits a
  // parked connection; on the second visit the socket is fully shut down so a peer that
  // stopped reading cannot pin the pending send (and its buffers) forever.
  bool closeParkSweepSeen : 1 {false};

  // Whether the connection should attempt to enable MSG_ZEROCOPY when possible.
  // Determined at accept time based on server configuration and peer/local addresses.
  bool zerocopyRequested : 1 {false};

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

  // CONNECT tunnel bridge for HTTP/2 connections.
  // Owned here so the bridge outlives the protocol handler.
  std::unique_ptr<ITunnelBridge> tunnelBridge;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  [[nodiscard]] AsyncHandlerState* pAsyncState() const noexcept { return asyncState.get(); }

  /// Allocate async state on first use from the bound pool and return it.
  AsyncHandlerState& ensureAsyncState(AsyncHandlerStatePool& asyncStatePool);

  // Async handler state is pooled by ConnectionStorage to avoid per-connection inline bloat.
  PoolPtr<AsyncHandlerState> asyncState;

#endif
};

}  // namespace aeronet
