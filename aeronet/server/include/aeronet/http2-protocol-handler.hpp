#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

#include "aeronet/cors-policy.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-event-sink.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/async-handler-state.hpp"
#endif

namespace aeronet {

struct ConnectionState;

namespace internal {
struct ServerContext;
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
/// The handler is installed after ALPN negotiates "h2", or when a cleartext client sends the HTTP/2 connection
/// preface directly (h2c prior knowledge).
/// The server then routes all I/O through this handler instead of HTTP/1.1 parsing.
///
/// Thread safety: NOT thread-safe. Executes on the single-threaded event loop.
class Http2ProtocolHandler final : public IProtocolHandler, private EventSink {
 public:
  /// Create an HTTP/2 protocol handler.
  /// @param ctx The server context: configuration, request pipeline shared with HTTP/1.1 (through which the requests
  ///        are routed and answered), states shared by the connections, statistics and metrics. It must outlive the
  ///        handler.
  /// @param connectionState The connection owning the handler, to which its requests belong (client address, TLS
  ///        information). It must outlive the handler.
  /// @param fd The socket of the connection, which identifies it to the server (CONNECT tunnels, async handlers).
  /// @param tunnelBridge Bridge to the server for the CONNECT tunnels of the streams, owned by the handler. Without
  ///        one, the first CONNECT request creates the one forwarding to the tunnels of the server context.
  Http2ProtocolHandler(::aeronet::internal::ServerContext& ctx, ::aeronet::ConnectionState& connectionState,
                       NativeHandle fd, std::unique_ptr<ITunnelBridge> tunnelBridge = {});

  Http2ProtocolHandler(const Http2ProtocolHandler&) = delete;
  Http2ProtocolHandler& operator=(const Http2ProtocolHandler&) = delete;

  Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept;
  Http2ProtocolHandler& operator=(Http2ProtocolHandler&&) noexcept;

  ~Http2ProtocolHandler() override;

  // ============================
  // IProtocolHandler interface
  // ============================

  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::Http2; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> data) override;

  [[nodiscard]] std::span<const std::byte> getPendingOutput() const noexcept override {
    return _connection.getPendingOutput();
  }

  void getPendingOutputFragments(vector<std::string_view>& fragments) const override {
    _connection.getPendingOutputFragments(fragments);
  }

  [[nodiscard]] std::size_t pendingOutputSize() const noexcept override { return _connection.pendingOutputSize(); }

  void onOutputWritten(std::size_t bytesWritten) override {
    _connection.onOutputWritten(bytesWritten);
    if (_nbPendingSends != 0 && !_connection.hasPendingOutput()) {
      flushPendingSends();
    }
  }

  void discardPendingOutput() noexcept override { _connection.discardPendingOutput(); }

  void onTransportClosing() override {
    // Close all active tunnel upstream connections before clearing state.
    for (const auto& [streamId, state] : _streams) {
      if (state.tunnelUpstreamFd != kInvalidHandle) {
        _tunnelBridge->closeTunnel(state.tunnelUpstreamFd);
      }
    }
    _tunnelUpstreams.clear();

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    for (auto& [streamId, state] : _streams) {
      orphanAsyncTask(state);
    }
#endif
    _streams.clear();
    _deferredOutputBytes = 0;
    _nbPendingSends = 0;

    // Detach callbacks to avoid generating new outbound frames while the transport is closing.
    _connection.setEventSink(nullptr);
  }

  // ============================
  // HTTP/2 specific
  // ============================

  /// Get the underlying HTTP/2 connection for advanced usage.
  [[nodiscard]] Http2Connection& connection() noexcept { return _connection; }

  /// Inject data received from an upstream tunnel fd into the corresponding HTTP/2 stream.
  /// Called by the server when an upstream fd becomes readable.
  /// @return ErrorCode::NoError on success, or an error if the stream is gone.
  [[nodiscard]] ErrorCode injectTunnelData(uint32_t streamId, std::span<const std::byte> data) {
    return _connection.sendData(streamId, data, /*endStream=*/false);
  }

  /// Notify the handler that a tunnel upstream fd was closed externally (e.g., upstream EOF).
  /// Sends an empty DATA frame with END_STREAM to gracefully close the tunnel stream.
  void closeTunnelByUpstreamFd(NativeHandle upstreamFd);

  /// Tell whether the CONNECT request of the stream waits for the resolution of its target host name
  /// (see ITunnelBridge::TunnelSetup::Status::Pending).
  [[nodiscard]] bool isTunnelPending(uint32_t streamId) const noexcept;

  /// Complete the CONNECT request of a stream for which isTunnelPending() is true, now that its target host name was
  /// resolved: answers 200 and starts tunneling with upstreamFd, or answers 502 when it is kInvalidHandle.
  void onTunnelResolved(uint32_t streamId, NativeHandle upstreamFd);

  /// Answer the CONNECT request of a stream for which isTunnelPending() is true with an error (its target could not be
  /// reached, or was not resolved in time).
  void failPendingTunnel(uint32_t streamId, http::StatusCode statusCode, std::string_view reason);

  /// The upstream of a tunnel wrote all the bytes buffered for it, after ITunnelBridge::writeTunnel() returned false:
  /// the client stream gets flow-control credit again.
  void onTunnelUpstreamDrained(NativeHandle upstreamFd);

  /// Notify the handler that the async connect for a tunnel stream's upstream fd failed.
  /// Sends RST_STREAM with CONNECT_ERROR on the stream.
  void tunnelConnectFailed(uint32_t streamId);

  /// Check if a given stream is a CONNECT tunnel.
  [[nodiscard]] bool isTunnelStream(uint32_t streamId) const noexcept;

  using TunnelUpstreamsMap = flat_hash_map<NativeHandle, uint32_t>;

  /// The upstream fds of the tunnels of this connection, with their stream.
  [[nodiscard]] const TunnelUpstreamsMap& tunnelUpstreams() const noexcept { return _tunnelUpstreams; }

  /// Drain all tunnel upstream fds and clear internal tunnel maps.
  /// Returns the list of upstream fds that must be closed by the caller.
  /// Used during connection teardown to avoid recursive closeConnection calls.
  [[nodiscard]] TunnelUpstreamsMap drainTunnelUpstreamFds();

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Resume a pending async task identified by its coroutine handle.
  /// Called by the server when an async callback fires for this connection.
  /// The coroutine of a task whose stream was closed meanwhile is destroyed instead (it was kept for its work).
  /// @return true if a matching task was found and resumed, false otherwise.
  bool resumeAsyncTaskByHandle(std::coroutine_handle<> handle);

  /// Destroy the async task identified by its coroutine handle without resuming it: its connection was closed while its
  /// deferred work was running.
  void dropAsyncTask(std::coroutine_handle<> handle) noexcept override;

  /// Tells whether deferred work started by a coroutine of this connection is still running.
  [[nodiscard]] bool hasAsyncWorkInFlight() const noexcept override;
#endif

  /// Check per-stream request deadlines and send 408 + RST_STREAM for expired ones.
  /// Called during the server's periodic sweep of idle connections.
  void sweepStreams(std::chrono::steady_clock::time_point now);

 private:
  /// Per-stream request state during aggregation.
  /// Allocated on the heap, so that it never moves while handled (the body bridge of the request points to
  /// bodyContext), and recycled for the next streams (see acquireRequest()), which reuse the capacity of its maps and
  /// buffers.
  struct StreamRequest {
    /// declaredBodyLength of a request without content-length.
    static constexpr std::size_t kUnknownBodyLength = std::numeric_limits<std::size_t>::max();

    StreamRequest() noexcept = default;

    StreamRequest(const StreamRequest&) = delete;
    StreamRequest(StreamRequest&&) noexcept = delete;
    StreamRequest& operator=(const StreamRequest&) = delete;
    StreamRequest& operator=(StreamRequest&&) noexcept = delete;

    ~StreamRequest();

    /// Make it a new request for the next stream, keeping the memory of its maps and buffers unless it grew large.
    void clear() noexcept;

    HttpRequestView request;
    RawChars bodyBuffer;
    // The content-length of the request, which its DATA frames must match (RFC 9113 §8.1.1).
    std::size_t declaredBodyLength{kUnknownBodyLength};
    // The decoded bytes of the headers and of the trailers of the request, taken over from the HPACK decoder.
    DecodedHeaders::Storage storage{DecodedHeaders::kStorageInitialCapacity};
    // Read position of the body for readBody() / hasMoreBody().
    HttpRequestView::AggregatedBodyContext bodyContext;
  };

  using StreamRequestPtr = std::unique_ptr<StreamRequest>;

  struct PendingFileSend {
    FilePayload filePayload;
  };

  /// In-memory body data (and trailers) waiting for flow-control credit.
  struct PendingStreamingSend {
    // Owns the memory of body: the storage of a response (possibly with its captured body), or a streaming buffer.
    HttpMessageData storage;
    std::string_view body;  // Body bytes not sent yet, in storage
    RawChars trailersData;  // Trailer lines (may be empty)
  };

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Per-stream async handler state for coroutines that suspend (e.g., co_await deferWork).
  struct PendingAsyncTask {
    // Path parameter names of the request, owned by the router until copied here (see startAsyncHandler()).
    RawChars pathParamKeys;
    // The request, owned by the task while the coroutine runs: an orphaned task keeps it for its deferred work.
    StreamRequestPtr streamRequest;
    // Declared last, destroyed first: the coroutine may use the request.
    AsyncHandlerTask task;
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
    [[nodiscard]] bool hasRequest() const noexcept { return request != nullptr; }

    [[nodiscard]] PendingFileSend* fileSend() const noexcept {
      return pending ? std::get_if<PendingFileSend>(pending.get()) : nullptr;
    }

    [[nodiscard]] PendingStreamingSend* streamingSend() const noexcept {
      return pending ? std::get_if<PendingStreamingSend>(pending.get()) : nullptr;
    }

    /// Whether the response body waits for flow-control credit (a PendingFileSend or a PendingStreamingSend).
    [[nodiscard]] bool hasPendingSend() const noexcept { return fileSend() != nullptr || streamingSend() != nullptr; }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
    [[nodiscard]] PendingAsyncTask* asyncTask() const noexcept {
      return pending ? std::get_if<PendingAsyncTask>(pending.get()) : nullptr;
    }
#endif

    StreamRequestPtr request;
    PoolPtr<PendingWork> pending;
    NativeHandle tunnelUpstreamFd{kInvalidHandle};  ///< Valid if this is a CONNECT tunnel stream
    /// True while the target host name of the CONNECT request is resolved. The DATA received meanwhile is kept in
    /// request.bodyBuffer, and tunnelPendingEndStream tells whether the client already ended its side.
    bool tunnelPending{false};
    bool tunnelPendingEndStream{false};
    /// Per-route handler deadline. Epoch means no per-route deadline active.
    std::chrono::steady_clock::time_point requestDeadline;
  };

  using StreamsMap = flat_hash_map<uint32_t, StreamState>;

  void setupCallbacks() { _connection.setEventSink(this); }

  void onHeadersDecoded(uint32_t streamId, DecodedHeaders& decodedHeaders, bool endStream) override;
  void onData(uint32_t streamId, std::span<const std::byte> data, bool endStream) override;
  void onStreamReset([[maybe_unused]] uint32_t streamId, [[maybe_unused]] ErrorCode errorCode) override {}
  void onStreamClosed(uint32_t streamId) override;
  void onGoAway([[maybe_unused]] uint32_t lastStreamId, [[maybe_unused]] ErrorCode errorCode,
                [[maybe_unused]] std::string_view debugData) override {}
  void onWindowUpdate(uint32_t streamId, [[maybe_unused]] uint32_t increment) override;

  [[nodiscard]] std::size_t retainedOutboundBytes() const noexcept {
    return _connection.pendingOutputSize() + _deferredOutputBytes;
  }

  /// Keeps the rest of the response body of a stream, sent as flow-control credit is granted (see flushPendingSends()).
  template <class PendingSend>
  void deferSend(StreamState& state, PendingSend&& pendingSend);

  /// Forgets the pending send of a stream about to be erased, if any.
  void releasePendingSend(const StreamState& state) noexcept;

  [[nodiscard]] bool responseExceedsPendingLimits(uint32_t streamId, const HttpResponse& response, bool isHeadMethod);

  /// Handle a trailing HEADERS block on an already-open request stream: request trailers (RFC 9113 §8.1).
  void onTrailersReceived(StreamsMap::iterator it, DecodedHeaders& trailers, bool endStream);

  /// A request for a new stream: a recycled one if any.
  [[nodiscard]] StreamRequestPtr acquireRequest();

  /// Release the request of a stream (if any), kept for the next streams up to Http2Config::maxRecycledRequests.
  void recycleRequest(StreamRequestPtr& pRequest);

  /// Answer 413 to a request whose body exceeds the server limit, while the client is still sending it, then reset the
  /// stream (NO_ERROR) so that the client stops sending it.
  void rejectTooLargeBody(StreamsMap::iterator it);

  /// Reset the stream of a malformed request with PROTOCOL_ERROR (RFC 9113 §8.1.1), which releases its state.
  void rejectMalformedRequest(uint32_t streamId, std::string_view reason);

  /// Install the aggregated request body (applying request-body decompression) and dispatch the request.
  /// Shared by the DATA-END_STREAM path and the trailing-HEADERS (trailers) path.
  void finalizeRequestBodyAndDispatch(StreamsMap::iterator it);

  /// Continue the pending sends of the streams, as far as the flow-control windows allow.
  void flushPendingSends();

  /// The most bytes of their file that each of 'nbFileSends' file sends may read now (see the definition).
  [[nodiscard]] std::size_t fileReadBudget(std::size_t nbFileSends) const noexcept;

  /// Read and send up to 'maxBytes' of the file of a response, as far as the flow-control windows allow.
  [[nodiscard]] ErrorCode sendPendingFileBody(uint32_t streamId, FilePayload& pending, bool endStreamAfterBody,
                                              std::size_t maxBytes);

  /// Send the buffered body (then the trailers) of a pending streaming send as far as the flow-control windows allow.
  /// Returns true once all of it is sent.
  bool sendPendingStreamingBody(uint32_t streamId, PendingStreamingSend& pending);

  /// Release request state after a response, retaining it only while deferred output remains.
  void releaseStreamAfterResponse(StreamsMap::iterator it);

  /// Call the streaming handler of the route of the request of a stream.
  void handleStreamingRequest(StreamsMap::iterator it, const Router::RoutingResult& routingResult);

  /// Route a completed request and answer it through the request pipeline (see RequestPipeline).
  void dispatchRequest(StreamsMap::iterator it);

  /// Answer the request of a stream with its redirect to https (see HttpServerConfig::httpsRedirect), before its body
  /// is received: the stream is then reset (NO_ERROR) if the client did not end it.
  void answerHttpsRedirect(StreamsMap::iterator it, bool endStream);

  /// Apply the CORS policy of the route to the response to the request of a stream, finalize and send it, then release
  /// the stream.
  void finalizeAndSendResponse(StreamsMap::iterator it, HttpResponse&& response, const CorsPolicy* pCorsPolicy);

  /// Handle a CONNECT request: validate target, set up tunnel, send 200 response.
  void handleConnectRequest(uint32_t streamId, HttpRequestView& request);

  // Answer 200 to the CONNECT request of the stream, which tunnels with upstreamFd from now on.
  void establishTunnel(uint32_t streamId, NativeHandle upstreamFd);

  /// Emit metrics, account the request in the server statistics and end its trace span.
  void onRequestCompleted(HttpRequestView& request, http::StatusCode status);

  /// Send an HTTP response on a stream.
  ErrorCode sendResponse(uint32_t streamId, HttpResponse response, bool isHeadMethod);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  /// Start an async handler for a stream. Returns true if the handler was started
  /// asynchronously (response will be sent later), false if it completed synchronously
  /// (response already sent).
  bool startAsyncHandler(StreamsMap::iterator it, const Router::RoutingResult& routingResult);

  /// Resume a pending async task's coroutine after suspension.
  void resumeAsyncTask(uint32_t streamId);

  /// Called when an async task completes: finalize and send the response.
  void onAsyncTaskCompleted(uint32_t streamId);

  /// Keeps the async task of a stream about to be erased if its coroutine waits for deferred work (which may use it).
  void orphanAsyncTask(StreamState& state);

  /// Moves the request of the async task of a stream back to the stream, and destroys the task.
  static void ReleaseAsyncTask(StreamState& state) noexcept;
#endif

  Http2Connection _connection;

  /// Requests of closed streams, reused by the next ones (see recycleRequest()).
  vector<StreamRequestPtr> _recycledRequests;

  /// Pool for PendingWork allocations (avoids repeated heap alloc/free cycles).
  ObjectPool<PendingWork> _pendingWorkPool;

  /// Unified per-stream state map (replaces _streamRequests, _pendingFileSends,
  /// _pendingStreamingSends, _pendingAsyncTasks, and _tunnelStreams).
  StreamsMap _streams;

  // Bytes owned by flow-control-deferred in-memory response bodies and trailers across all streams.
  std::size_t _deferredOutputBytes{0};

  ::aeronet::internal::ServerContext* _pCtx;
  // Connection owning this handler.
  ::aeronet::ConnectionState* _pConnectionState;

  // Reverse tunnel map: upstream fd → stream ID (needed for closeTunnelByUpstreamFd).
  TunnelUpstreamsMap _tunnelUpstreams;  // upstreamFd → streamId

  // Null until the first CONNECT request (unless given at construction).
  std::unique_ptr<ITunnelBridge> _tunnelBridge;

  NativeHandle _fd;

  // Number of streams with a pending send (see StreamState::hasPendingSend()): the flushes look for them only if any.
  uint32_t _nbPendingSends{0};

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Async tasks of closed streams whose deferred work is still running: destroyed when it completes.
  vector<PoolPtr<PendingWork>> _orphanedAsyncTasks;
#endif
};

/// Factory function for creating HTTP/2 protocol handlers (see Http2ProtocolHandler for the parameters).
/// @param sendServerPrefaceForTls If true, sends SETTINGS immediately (for TLS ALPN "h2").
///        For h2c (cleartext), this should be false as server waits for client preface first.
/// @return Unique pointer to the created handler
std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(::aeronet::internal::ServerContext& ctx,
                                                             ::aeronet::ConnectionState& connectionState,
                                                             NativeHandle fd, bool sendServerPrefaceForTls);

}  // namespace http2
}  // namespace aeronet
