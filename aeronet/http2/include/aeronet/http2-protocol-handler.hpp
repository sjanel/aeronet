#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "aeronet/file.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/tunnel-bridge.hpp"

namespace aeronet {

struct ConnectionState;
struct HttpServerConfig;

namespace internal {
struct ResponseCompressionState;
struct RequestDecompressionState;
}  // namespace internal
class Router;

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
    }
  }

  void initiateClose() override { _connection.initiateGoAway(ErrorCode::NoError); }

  void onTransportClosing() override {
    // Close all active tunnel upstream connections before clearing state.
    for (const auto& [streamId, upstreamFd] : _tunnelStreams) {
      _tunnelBridge->closeTunnel(upstreamFd);
    }
    _tunnelStreams.clear();
    _tunnelUpstreams.clear();

    _streamRequests.clear();
    _pendingFileSends.clear();

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

  /// Install a tunnel bridge for CONNECT tunnel management.
  /// The bridge must outlive this handler (typically owned by ConnectionState).
  void setTunnelBridge(ITunnelBridge* bridge) noexcept { _tunnelBridge = bridge; }

  /// Inject data received from an upstream tunnel fd into the corresponding HTTP/2 stream.
  /// Called by the server when an upstream fd becomes readable.
  /// @return ErrorCode::NoError on success, or an error if the stream is gone.
  [[nodiscard]] ErrorCode injectTunnelData(uint32_t streamId, std::span<const std::byte> data);

  /// Notify the handler that a tunnel upstream fd was closed externally (e.g., upstream EOF).
  /// Sends an empty DATA frame with END_STREAM to gracefully close the tunnel stream.
  void closeTunnelByUpstreamFd(int upstreamFd);

  /// Notify the handler that the async connect for a tunnel stream's upstream fd failed.
  /// Sends RST_STREAM with CONNECT_ERROR on the stream.
  void tunnelConnectFailed(uint32_t streamId);

  /// Check if a given stream is a CONNECT tunnel.
  [[nodiscard]] bool isTunnelStream(uint32_t streamId) const noexcept;

  using TunnelStreamsMap = flat_hash_map<uint32_t, int>;
  using TunnelUpstreamsMap = flat_hash_map<int, uint32_t>;

  /// Drain all tunnel upstream fds and clear internal tunnel maps.
  /// Returns the list of upstream fds that must be closed by the caller.
  /// Used during connection teardown to avoid recursive closeConnection calls.
  [[nodiscard]] TunnelUpstreamsMap drainTunnelUpstreamFds();

 private:
  /// Per-stream request state during aggregation.
  struct StreamRequest {
    HttpRequest request;
    RawChars bodyBuffer;
    std::unique_ptr<char[]> headerStorage;  // Storage for header name/value strings
  };

  using StreamRequestsMap = flat_hash_map<uint32_t, StreamRequest>;

  void setupCallbacks();
  void onHeadersDecodedReceived(uint32_t streamId, const HeadersViewMap& headers, bool endStream);
  void onDataReceived(uint32_t streamId, std::span<const std::byte> data, bool endStream);
  void onStreamClosed(uint32_t streamId);
  void onStreamReset(uint32_t streamId, ErrorCode errorCode);

  struct PendingFileSend {
    File file;
    std::size_t offset = 0;
    std::size_t remaining = 0;
    RawChars trailersData;
    HeadersView trailersView;
  };

  using PendingFileSendsMap = flat_hash_map<uint32_t, PendingFileSend>;

  void flushPendingFileSends();
  [[nodiscard]] ErrorCode sendPendingFileBody(uint32_t streamId, PendingFileSend& pending, bool endStreamAfterBody);

  /// Dispatch a completed request to the dispatcher and send response.
  void dispatchRequest(StreamRequestsMap::iterator it);

  /// Handle a CONNECT request: validate target, set up tunnel, send 200 response.
  void handleConnectRequest(uint32_t streamId, HttpRequest& request);

  /// Clean up tunnel state for a given stream.
  void cleanupTunnel(uint32_t streamId);

  // Creates an HTTP/2 request dispatcher that routes HTTP/2 requests through the unified Router.
  // The dispatcher receives an HttpRequest (already populated with HTTP/2 fields) and dispatches
  // to the appropriate handler (sync, async, or streaming).
  HttpResponse reply(HttpRequest& request);

  /// Send an HTTP response on a stream.
  ErrorCode sendResponse(uint32_t streamId, HttpResponse response, bool isHeadMethod);

  Http2Connection _connection;

  Router* _pRouter;

  // Request state per stream
  StreamRequestsMap _streamRequests;

  // File payload streaming state per stream (flow-control aware)
  PendingFileSendsMap _pendingFileSends;
  RawChars _fileSendBuffer;

  HttpServerConfig* _pServerConfig;
  internal::ResponseCompressionState* _pCompressionState;
  internal::RequestDecompressionState* _pDecompressionState;
  RawChars* _pTmpBuffer;
  tracing::TelemetryContext* _pTelemetryContext;

  // CONNECT tunnel state: maps stream IDs to upstream fds (and reverse).
  TunnelStreamsMap _tunnelStreams;      // streamId → upstreamFd
  TunnelUpstreamsMap _tunnelUpstreams;  // upstreamFd → streamId

  ITunnelBridge* _tunnelBridge{nullptr};
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
