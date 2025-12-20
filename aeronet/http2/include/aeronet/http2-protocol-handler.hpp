#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

struct ConnectionState;
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
  Http2ProtocolHandler(const Http2Config& config, Router& router);

  // Disable copy
  Http2ProtocolHandler(const Http2ProtocolHandler&) = delete;
  Http2ProtocolHandler& operator=(const Http2ProtocolHandler&) = delete;

  // Enable move
  Http2ProtocolHandler(Http2ProtocolHandler&&) noexcept;
  Http2ProtocolHandler& operator=(Http2ProtocolHandler&&) noexcept;

  ~Http2ProtocolHandler() override;

  // ============================
  // IProtocolHandler interface
  // ============================

  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::Http2; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> data,
                                                   ::aeronet::ConnectionState& state) override;

  [[nodiscard]] bool hasPendingOutput() const noexcept override;

  [[nodiscard]] std::span<const std::byte> getPendingOutput() override;

  void onOutputWritten(std::size_t bytesWritten) override;

  void initiateClose() override;

  void onTransportClosing() override;

  // ============================
  // HTTP/2 specific
  // ============================

  /// Get the underlying HTTP/2 connection for advanced usage.
  [[nodiscard]] Http2Connection& connection() noexcept { return _connection; }

 private:
  /// Per-stream request state during aggregation.
  struct StreamRequest {
    HttpRequest request;
    RawChars bodyBuffer;
    RawChars headerStorage;  // Storage for header name/value strings
    bool headersReceived{false};
  };

  void setupCallbacks();
  void onHeadersReceived(uint32_t streamId, const HeaderProvider& headerProvider, bool endStream);
  void onDataReceived(uint32_t streamId, std::span<const std::byte> data, bool endStream);
  void onStreamClosed(uint32_t streamId);
  void onStreamReset(uint32_t streamId, ErrorCode errorCode);

  /// Dispatch a completed request to the dispatcher and send response.
  void dispatchRequest(uint32_t streamId);

  // Creates an HTTP/2 request dispatcher that routes HTTP/2 requests through the unified Router.
  // The dispatcher receives an HttpRequest (already populated with HTTP/2 fields) and dispatches
  // to the appropriate handler (sync, async, or streaming).
  HttpResponse reply(HttpRequest& request);

  /// Send an HTTP response on a stream.
  ErrorCode sendResponse(uint32_t streamId, HttpResponse response);

  Http2Connection _connection;

  Router* _pRouter;

  // Request state per stream
  flat_hash_map<uint32_t, StreamRequest> _streamRequests;
};

/// Factory function for creating HTTP/2 protocol handlers.
/// @param config HTTP/2 configuration
/// @param dispatcher Callback that dispatches HttpRequest to handlers
/// @param sendServerPrefaceForTls If true, sends SETTINGS immediately (for TLS ALPN "h2").
///        For h2c (cleartext), this should be false as server waits for client preface first.
/// @return Unique pointer to the created handler
std::unique_ptr<IProtocolHandler> CreateHttp2ProtocolHandler(const Http2Config& config, Router& router,
                                                             bool sendServerPrefaceForTls = false);

}  // namespace http2
}  // namespace aeronet
