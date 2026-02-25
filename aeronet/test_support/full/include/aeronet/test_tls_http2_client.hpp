#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/test_tls_client.hpp"

namespace aeronet::test {

/// Lightweight HTTP/2 over TLS client for end-to-end testing.
///
/// Features:
///  * Automatic TLS connection with ALPN "h2"
///  * HTTP/2 connection preface and SETTINGS exchange
///  * Simple request/response helpers
///  * Support for multiple concurrent streams
///
/// Usage:
///   TlsHttp2Client client(server.port());
///   ASSERT_TRUE(client.isConnected());
///   auto response = client.get("/hello");
///   EXPECT_EQ(response.statusCode, 200);
///
/// Not intended for production usage; simplified for testing.
class TlsHttp2Client {
 public:
  /// HTTP/2 response for test assertions.
  struct Response {
    int statusCode{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// Find a header value by name (case-insensitive).
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
  };

  /// Constructor - connects to server and completes HTTP/2 handshake.
  /// @param port Server port
  /// @param config Optional HTTP/2 configuration
  explicit TlsHttp2Client(uint16_t port, Http2Config config = {});

  TlsHttp2Client(const TlsHttp2Client&) = delete;
  TlsHttp2Client(TlsHttp2Client&&) noexcept = delete;
  TlsHttp2Client& operator=(const TlsHttp2Client&) = delete;
  TlsHttp2Client& operator=(TlsHttp2Client&&) noexcept = delete;

  ~TlsHttp2Client();

  /// Check if HTTP/2 connection is established.
  [[nodiscard]] bool isConnected() const noexcept;

  /// Get the negotiated ALPN protocol (should be "h2").
  [[nodiscard]] std::string_view negotiatedAlpn() const noexcept;

  /// Perform a simple GET request.
  /// @param path Request path (e.g., "/hello")
  /// @param extraHeaders Additional headers to send
  /// @return Response with status code, headers, and body
  Response get(std::string_view path, const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

  /// Perform a POST request with body.
  /// @param path Request path
  /// @param body Request body
  /// @param contentType Content-Type header value
  /// @param extraHeaders Additional headers
  /// @return Response with status code, headers, and body
  Response post(std::string_view path, std::string_view body, std::string_view contentType = "application/octet-stream",
                const std::vector<std::pair<std::string, std::string>>& extraHeaders = {});

  /// Send a custom request.
  /// @param method HTTP method (GET, POST, etc.)
  /// @param path Request path
  /// @param headers Request headers (pseudo-headers :method, :path, :scheme, :authority are added automatically)
  /// @param body Optional request body
  /// @return Response with status code, headers, and body
  Response request(std::string_view method, std::string_view path,
                   const std::vector<std::pair<std::string, std::string>>& headers = {}, std::string_view body = {});

  /// Perform a CONNECT request to establish a tunnel.
  /// @param authority Target host:port
  /// @param headers Additional headers
  /// @return Stream ID of the established tunnel, or 0 on failure
  uint32_t connect(std::string_view authority, const std::vector<std::pair<std::string, std::string>>& headers = {});

  /// Send data on an established tunnel stream.
  /// @param streamId Stream ID returned by connect()
  /// @param data Data to send
  /// @param endStream Whether this is the last data frame
  /// @return True if successful
  bool sendTunnelData(uint32_t streamId, std::span<const std::byte> data, bool endStream = false);

  /// Wait for data on a tunnel stream.
  /// @param streamId Stream ID
  /// @param timeout Maximum time to wait
  /// @return Received data, or empty on timeout/error
  std::vector<std::byte> receiveTunnelData(uint32_t streamId,
                                           std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /// Get the underlying HTTP/2 connection for advanced testing.
  [[nodiscard]] http2::Http2Connection& connection() noexcept { return *_http2Connection; }
  [[nodiscard]] const http2::Http2Connection& connection() const noexcept { return *_http2Connection; }

 private:
  /// Write data to TLS connection.
  bool writeAll(std::span<const std::byte> data);

  /// Read and process HTTP/2 frames.
  /// @param timeout Maximum time to wait for response
  /// @return True if successful, false on timeout or error
  bool processFrames(std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

  /// Wait for a specific stream to receive a complete response.
  bool waitForResponse(uint32_t streamId, std::chrono::milliseconds timeout = std::chrono::milliseconds{5000},
                       bool waitForComplete = true);

  /// Build and send a request on a new stream.
  uint32_t sendRequest(std::string_view method, std::string_view path,
                       const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body);

  /// Per-stream response data accumulator.
  struct StreamResponse {
    Response response;
    bool headersReceived{false};
    bool complete{false};
  };

  uint16_t _port;
  TlsClient _tlsClient;
  std::unique_ptr<http2::Http2Connection> _http2Connection;
  bool _connected{false};
  uint32_t _nextStreamId{1};  // Client streams are odd-numbered

  std::vector<std::byte> _pendingInput;
  std::size_t _pendingOffset{0};

  // Responses indexed by stream ID
  std::map<uint32_t, StreamResponse> _streamResponses;
};

}  // namespace aeronet::test
