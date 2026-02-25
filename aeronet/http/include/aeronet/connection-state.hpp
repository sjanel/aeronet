#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tls-info.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-metrics.hpp"
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>
#include <functional>
#include <optional>

#include "aeronet/http-response.hpp"
#endif

namespace aeronet {

struct HttpServerConfig;

#ifdef AERONET_ENABLE_OPENSSL
class TlsContext;
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
class CorsPolicy;
#endif

struct ConnectionState {
  void initializeStateNewConnection(const HttpServerConfig& config, int cnxFd,
                                    internal::ResponseCompressionState& compressionState);

  [[nodiscard]] bool isImmediateCloseRequested() const noexcept { return closeMode == CloseMode::Immediate; }
  [[nodiscard]] bool isDrainCloseRequested() const noexcept { return closeMode == CloseMode::DrainThenClose; }
  [[nodiscard]] bool isAnyCloseRequested() const noexcept { return closeMode != CloseMode::None; }

  [[nodiscard]] bool isTunneling() const noexcept { return peerFd != -1; }
  [[nodiscard]] bool isSendingFile() const noexcept { return fileSend.active; }

  [[nodiscard]] bool canCloseConnectionForDrain() const noexcept {
    return isDrainCloseRequested() && outBuffer.empty() && tunnelOrFileBuffer.empty() && !isSendingFile();
  }

  [[nodiscard]] bool canCloseImmediately() const noexcept {
    return isImmediateCloseRequested() || canCloseConnectionForDrain();
  }

  // Request to close immediately (abort outstanding buffered writes).
  void requestImmediateClose() { closeMode = CloseMode::Immediate; }

  // Request to close after draining currently buffered writes (graceful half-close semantics).
  void requestDrainAndClose() {
    if (closeMode == CloseMode::None) {
      closeMode = CloseMode::DrainThenClose;
    }
  }

  ITransport::TransportResult transportRead(std::size_t chunkSize);

  ITransport::TransportResult transportWrite(std::string_view data);
  ITransport::TransportResult transportWrite(const HttpResponseData& httpResponseData);

  // Result of a kernel sendfile operation performed on this connection's fileSend state.
  struct FileResult {
    enum class Code : uint8_t { Read, Sent, WouldBlock, Error };

    std::size_t bytesDone{0};
    Code code{Code::Sent};
    // When code == WouldBlock, indicates the caller should enable writable interest
    // (true for EAGAIN/EWOULDBLOCK, false for EINTR).
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
  FileResult transportFile(int clientFd, bool tlsFlow);

  // Helper to set up request body streaming bridges for aggregated body reading.
  void installAggregatedBodyBridge();

#ifdef AERONET_ENABLE_OPENSSL
  // Finalize TLS handshake (if this transport is TLS) and emit the handshake event.
  // Returns true if a TLS transport was finalized (caller may perform transport-specific book-keeping).
  // zerocopyEnabled: if true and kTLS is active, enables MSG_ZEROCOPY on the kTLS socket.
  bool finalizeAndEmitTlsHandshakeIfNeeded(int fd, const TlsHandshakeCallback& cb, TlsMetricsInternal& metrics,
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
  void holdBufferIfZerocopyPending(HttpResponseData buf);

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
  HttpRequest request;
  AggregatedBodyStreamContext bodyStreamContext;
  // pending outbound data not yet written
  HttpResponseData outBuffer;
  // Buffers sent via MSG_ZEROCOPY that must remain alive until the kernel signals
  // completion via the error queue. Without this, the allocator can reuse the freed
  // pages while the kernel is still DMA-ing from them, causing data corruption.
  vector<HttpResponseData> zerocopyPendingBuffers;
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
  // Timestamp of first byte of the current pending request headers (buffer not yet containing full CRLFCRLF).
  // Reset when a complete request head is parsed. If std::chrono::steady_clock::time_point{} (epoch) -> inactive.
  std::chrono::steady_clock::time_point headerStartTp;     // default epoch value means no header timing active
  std::chrono::steady_clock::time_point bodyLastActivity;  // timestamp of last body progress while waiting
  // Tunnel support: when a connection is acting as a tunnel endpoint, peerFd holds the
  // file descriptor of the other side (upstream or client).
  int peerFd{-1};
  uint32_t requestsServed{0};
  // Length of trailer headers in bodyAndTrailersBuffer (0 if no trailers).
  // Trailers occupy [bodyAndTrailersBuffer.size() - trailerLen, bodyAndTrailersBuffer.size()).
  uint32_t trailerLen{0};

  TLSInfo tlsInfo;

  // Connection close lifecycle.
  enum class CloseMode : uint8_t { None, DrainThenClose, Immediate };

  CloseMode closeMode{CloseMode::None};
  bool waitingWritable{false};  // EPOLLOUT registered
  bool tlsEstablished{false};   // true once TLS handshake completed (if TLS enabled)
  bool waitingForBody{false};   // true when awaiting missing body bytes (bodyReadTimeout enforcement)
  // Tunnel state: true when peerFd != -1. Use accessor isTunneling() to query.
  // True when a non-blocking connect() was issued and completion is pending (EPOLLOUT will signal).
  bool connectPending{false};

  // Current protocol type. Http11 by default, changes after successful upgrade.
  ProtocolType protocol{ProtocolType::Http11};

  // Whether the connection should attempt to enable MSG_ZEROCOPY when possible.
  // Determined at accept time based on server configuration and peer/local addresses.
  bool zerocopyRequested{false};

#ifdef AERONET_ENABLE_OPENSSL
  // Observability / attribution for handshake failures.
  // Populated by OpenSSL callbacks via SSL ex_data (see tls-handshake-observer).
  TlsHandshakeObserver tlsHandshakeObserver;

  // Ensures the TLS handshake event callback is emitted at most once per connection.
  bool tlsHandshakeEventEmitted{false};

  // True while the TLS handshake for this connection is in-flight and counted against
  // concurrency limits.
  bool tlsHandshakeInFlight{false};

  // Keep the TLS context alive for as long as this connection's SSL/handshake may reference
  // callback user pointers (ALPN/SNI). This is required for safe hot-reload of TLS contexts.
  std::shared_ptr<const TlsContext> tlsContextKeepAlive;
#endif
  struct FileSendState {
    File file;
    bool active{false};
    bool headersPending{false};
    std::size_t offset{0};
    std::size_t remaining{0};
  };

  FileSendState fileSend;

  // Protocol handler for upgraded connections (WebSocket, HTTP/2).
  // nullptr when using default HTTP/1.1 processing (most connections).
  // When set, the server routes data through this handler instead of HTTP parsing.
  std::unique_ptr<IProtocolHandler> protocolHandler;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  struct AsyncHandlerState {
    AsyncHandlerState() = default;

    enum class AwaitReason : uint8_t { None, WaitingForBody, WaitingForCallback };

    void clear();

    std::coroutine_handle<> handle;
    // stable storage for the current request head when async body progress is needed
    RawChars headBuffer;
    AwaitReason awaitReason{AwaitReason::None};
    bool active{false};
    bool needsBody{false};
    bool usesSharedDecompressedBody{false};
    bool isChunked{false};
    bool expectContinue{false};
    std::size_t consumedBytes{0};
    const CorsPolicy* corsPolicy{nullptr};
    const void* responseMiddleware{nullptr};
    std::size_t responseMiddlewareCount{0};
    std::optional<HttpResponse> pendingResponse;
    // Callback to post async work completion to the server's event loop.
    // Set by the server when dispatching an async handler.
    std::function<void(std::coroutine_handle<>, std::function<void()>)> postCallback;
  } asyncState;
#endif
};

}  // namespace aeronet