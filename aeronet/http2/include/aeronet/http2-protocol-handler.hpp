#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <variant>

#include "aeronet/cors-policy.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/tunnel-bridge.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/request-task.hpp"
#endif

namespace aeronet {

struct ConnectionState;
struct HttpServerConfig;

namespace internal {
struct ResponseCompressionState;
struct RequestDecompressionState;
}  // namespace internal

namespace http2 {

/// HTTP/2 protocol handler implementing IProtocolHandler.
///
/// This handler bridges the HTTP/2 protocol implementation to the aeronet server
/// infrastructure. It manages:
/// - HTTP/2 connection state machine
/// - Incoming request aggregation per stream
/// - Response encoding and sending
/// - Flow control integration
///
/// Usage:
/// The handler is installed after ALPN negotiates "h2" or after h2c upgrade.
/// The server then routes all I/O through this handler instead of HTTP/1.1 parsing.
///
/// Thread safety: NOT thread-safe. Executes on the single-threaded event loop.
class Http2ProtocolHandler final : public IProtocolHandler {
 public:
  /// Create an HTTP/2 protocol handler with a request dispatcher.
  /// @param config HTTP/2 configuration
  /// @param dispatcher Callback that dispatches an HttpRequest to handlers and returns a response
  Http2ProtocolHandler(const Http2Config& config, Router& router, HttpServerConfig& serverConfig,
                       internal::ResponseCompressionState& compressionState,
                       internal::RequestDecompressionState& decompressionState,
                       tracing::TelemetryContext& telemetryContext, RawChars& tmpBuffer);

  Http2ProtocolHandler(const Http2ProtocolHandler&) = delete;
  Http2ProtocolHandler& operator=(const Http2ProtocolHandler&) = delete;

  Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept;
  Http2ProtocolHandler& operator=(Http2ProtocolHandler&&) noexcept;

  ~Http2ProtocolHandler() override;

  // ============================
  // IProtocolHandler interface
  // ============================

  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::Http2; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> data,
                                                   ::aeronet::ConnectionState& state) override;

  [[nodiscard]] bool hasPendingOutput() const noexcept override { return _connection.hasPendingOutput(); }

  [[nodiscard]] std::span<const std::byte> getPendingOutput() override { return _connection.getPendingOutput(); }

  void onOutputWritten(std::size_t bytesWritten) override {
    _connection.onOutputWritten(bytesWritten);
    if (!_connection.hasPendingOutput()) {
      flushPendingFileSends();
      flushPendingStreamingSends();
    }
  }

  void initiateClose() override { _connection.initiateGoAway(ErrorCode::NoError); }

  void onTransportClosing() override {
    // Close all active tunnel upstream connections before clearing state.
    for (const auto& [streamId, state] : _streams) {
      if (state.tunnelUpstreamFd != kInvalidHandle) {
        _tunnelBridge->closeTunnel(state.tunnelUpstreamFd);
      }
    }
    _tunnelUpstreams.clear();

    _streams.clear();

    // Detach callbacks to avoid generating new outbound frames while the transport is closing.
    _connection.setOnHeadersDecoded({});
    _connection.setOnData({});
    _connection.setOnStreamReset({});
    _connection.setOnStreamClosed({});
    _connection.setOnGoAway({});
  }

  // ============================
  // HTTP/2 specific
  // ============================

  /// Get the underlying HTTP/2 connection for advanced usage.
  [[nodiscard]] Http2Connection& connection() noexcept { return _connection; }

  /// Callback invoked after each HTTP/2 request is completed (response sent).
  /// Parameters: request reference, response status code.
  using RequestCompletionCallback = std::function<void(const HttpRequest&, http::StatusCode)>;

  /// Install a completion callback for per-request metrics and tracing.
  void setRequestCompletionCallback(RequestCompletionCallback cb) noexcept {
    _requestCompletionCallback = std::move(cb);
  }

  /// Install a tunnel bridge for CONNECT tunnel management.
  /// The bridge must outlive this handler (typically owned by ConnectionState).
  void setTunnelBridge(ITunnelBridge* bridge) noexcept { _tunnelBridge = bridge; }

  /// Inject data received from an upstream tunnel fd into the corresponding HTTP/2 stream.
  /// Called by the server when an upstream fd becomes readable.
  /// @return ErrorCode::NoError on success, or an error if the stream is gone.
  [[nodiscard]] ErrorCode injectTunnelData(uint32_t streamId, std::span<const std::byte> data);

  /// Notify the handler that a tunnel upstream fd was closed externally (e.g., upstream EOF).
  /// Sends an empty DATA frame with END_STREAM to gracefully close the tunnel stream.
  void closeTunnelByUpstreamFd(NativeHandle upstreamFd);

  /// Notify the handler that the async connect for a tunnel stream's upstream fd failed.
  /// Sends RST_STREAM with CONNECT_ERROR on the stream.
  void tunnelConnectFailed(uint32_t streamId);

  /// Check if a given stream is a CONNECT tunnel.
  [[nodiscard]] bool isTunnelStream(uint32_t streamId) const noexcept;

  using TunnelUpstreamsMap = flat_hash_map<NativeHandle, uint32_t>;

  /// Drain all tunnel upstream fds and clear internal tunnel maps.
  /// Returns the list of upstream fds that must be closed by the caller.
  /// Used during connection teardown to avoid recursive closeConnection calls.
  [[nodiscard]] TunnelUpstreamsMap drainTunnelUpstreamFds();

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Install a callback for posting deferred async work completion to the server's event loop.
  /// The callback receives a coroutine handle and optional pre-resume work function.
  /// Must be called before any async handlers are dispatched.
  using AsyncPostCallbackFn = std::function<void(std::coroutine_handle<>, std::function<void()>)>;
  void setAsyncPostCallback(AsyncPostCallbackFn fn) noexcept { _asyncPostCallback = std::move(fn); }

  /// Resume a pending async task identified by its coroutine handle.
  /// Called by the server when an async callback fires for this connection.
  /// @return true if a matching task was found and resumed, false otherwise.
  bool resumeAsyncTaskByHandle(std::coroutine_handle<> handle);
#endif

  /// Check per-stream request deadlines and send 408 + RST_STREAM for expired ones.
  /// Called during the server's periodic sweep of idle connections.
  void sweepStreams(std::chrono::steady_clock::time_point now);

 private:
  /// Per-stream request state during aggregation.
  struct StreamRequest {
    HttpRequest request;
    RawChars bodyBuffer;
    std::unique_ptr<char[]> headerStorage;  // Storage for header name/value strings. nullptr = inactive.
  };

  struct PendingFileSend {
    FilePayload filePayload;
    RawChars trailersData;
    HeadersView trailersView;
  };

  /// Buffered streaming body data when flow-control windows are exhausted.
  struct PendingStreamingSend {
    RawChars buffer;        // Remaining body data
    std::size_t offset{0};  // How much of buffer has been sent
    RawChars trailersData;  // Trailer lines (may be empty)
  };

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Per-stream async handler state for coroutines that suspend (e.g., co_await deferWork).
  struct PendingAsyncTask {
    RequestTask<HttpResponse> task;
    StreamRequest streamRequest;  // Owns the HttpRequest and header storage
    const CorsPolicy* pCorsPolicy{};
    const ResponseMiddleware* pResponseMiddleware{};
    uint32_t responseMiddlewareCount{0};
    bool isHead{false};
    bool suspended{false};  // Set to true when the coroutine suspends
  };

  using PendingWork = std::variant<PendingFileSend, PendingStreamingSend, PendingAsyncTask>;
#else
  using PendingWork = std::variant<PendingFileSend, PendingStreamingSend>;
#endif

  /// Consolidated per-stream state.
  /// All per-stream state lives in a single struct stored in one map.
  /// Cold pending work uses a single heap-allocated variant to keep inline size small.
  struct StreamState {
    /// Whether there is an active request being aggregated (headers received).
    [[nodiscard]] bool hasRequest() const noexcept { return request.headerStorage != nullptr; }

    [[nodiscard]] PendingFileSend* fileSend() const noexcept {
      return pending ? std::get_if<PendingFileSend>(pending.get()) : nullptr;
    }

    [[nodiscard]] PendingStreamingSend* streamingSend() const noexcept {
      return pending ? std::get_if<PendingStreamingSend>(pending.get()) : nullptr;
    }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    [[nodiscard]] PendingAsyncTask* asyncTask() const noexcept {
      return pending ? std::get_if<PendingAsyncTask>(pending.get()) : nullptr;
    }
#endif

    StreamRequest request;
    PoolPtr<PendingWork> pending;
    NativeHandle tunnelUpstreamFd{kInvalidHandle};  ///< Valid if this is a CONNECT tunnel stream
    /// Per-route handler deadline. Epoch means no per-route deadline active.
    std::chrono::steady_clock::time_point requestDeadline;
  };

  using StreamsMap = flat_hash_map<uint32_t, StreamState>;

  void setupCallbacks();
  void onHeadersDecodedReceived(uint32_t streamId, const HeadersViewMap& headers, bool endStream);
  void onDataReceived(uint32_t streamId, std::span<const std::byte> data, bool endStream);
  void onStreamClosed(uint32_t streamId);
  void onStreamReset(uint32_t streamId, ErrorCode errorCode);

  void flushPendingFileSends();
  [[nodiscard]] ErrorCode sendPendingFileBody(uint32_t streamId, FilePayload& pending, bool endStreamAfterBody);

  void flushPendingStreamingSends();

  /// Dispatch a streaming handler request on an HTTP/2 stream.
  void handleStreamingRequest(StreamsMap::iterator it, const StreamingHandler& handler, const CorsPolicy* pCorsPolicy,
                              std::span<const ResponseMiddleware> responseMiddleware);

  /// Dispatch a completed request to the dispatcher and send response.
  void dispatchRequest(StreamsMap::iterator it);

  /// Handle a CONNECT request: validate target, set up tunnel, send 200 response.
  void handleConnectRequest(uint32_t streamId, HttpRequest& request);

  /// Clean up tunnel state for a given stream (using the consolidated StreamState).
  void cleanupTunnel(StreamsMap::iterator it);

  // Creates an HTTP/2 request dispatcher that routes HTTP/2 requests through the unified Router.
  // The dispatcher receives an HttpRequest (already populated with HTTP/2 fields) and dispatches
  // to the appropriate handler (sync, async, or streaming).
  HttpResponse reply(HttpRequest& request, const Router::RoutingResult& routingResult);

  /// Emit metrics and end trace span for a completed request.
  void onRequestCompleted(HttpRequest& request, http::StatusCode status);

  /// Send an HTTP response on a stream.
  ErrorCode sendResponse(uint32_t streamId, HttpResponse response, bool isHeadMethod);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Start an async handler for a stream. Returns true if the handler was started
  /// asynchronously (response will be sent later), false if it completed synchronously
  /// (response already sent).
  bool startAsyncHandler(StreamsMap::iterator it, const AsyncRequestHandler& handler, const CorsPolicy* pCorsPolicy,
                         std::span<const ResponseMiddleware> responseMiddleware);

  /// Resume a pending async task's coroutine after suspension.
  void resumeAsyncTask(uint32_t streamId);

  /// Called when an async task completes: finalize and send the response.
  void onAsyncTaskCompleted(uint32_t streamId);
#endif

  bool applyRequestMiddleware(HttpRequest& request, uint32_t streamId, bool isHead, bool streaming,
                              const Router::RoutingResult& routingResult);

  Http2Connection _connection;

  Router* _pRouter;

  /// Pool for PendingWork allocations (avoids repeated heap alloc/free cycles).
  ObjectPool<PendingWork> _pendingWorkPool;

  /// Unified per-stream state map (replaces _streamRequests, _pendingFileSends,
  /// _pendingStreamingSends, _pendingAsyncTasks, and _tunnelStreams).
  StreamsMap _streams;

  RawChars _fileSendBuffer;

  HttpServerConfig* _pServerConfig;
  internal::ResponseCompressionState* _pCompressionState;
  internal::RequestDecompressionState* _pDecompressionState;
  RawChars* _pTmpBuffer;
  tracing::TelemetryContext* _pTelemetryContext;

  // Reverse tunnel map: upstream fd → stream ID (needed for closeTunnelByUpstreamFd).
  TunnelUpstreamsMap _tunnelUpstreams;  // upstreamFd → streamId

  ITunnelBridge* _tunnelBridge{nullptr};
  RequestCompletionCallback _requestCompletionCallback;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Callback to post async work completion to the server's event loop.
  AsyncPostCallbackFn _asyncPostCallback;
#endif
};

/// Factory function for creating HTTP/2 protocol handlers.
/// @param config HTTP/2 configuration
/// @param dispatcher Callback that dispatches HttpRequest to handlers
/// @param sendServerPrefaceForTls If true, sends SETTINGS immediately (for TLS ALPN "h2").
///        For h2c (cleartext), this should be false as server waits for client preface first.
/// @return Unique pointer to the created handler
std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(const Http2Config& config, Router& router,
                                                             HttpServerConfig& serverConfig,
                                                             internal::ResponseCompressionState& compressionState,
                                                             internal::RequestDecompressionState& decompressionState,
                                                             tracing::TelemetryContext& telemetryContext,
                                                             RawChars& tmpBuffer, bool sendServerPrefaceForTls = false);

}  // namespace http2
}  // namespace aeronet
