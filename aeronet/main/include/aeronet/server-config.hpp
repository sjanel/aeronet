#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aeronet {

struct ServerConfig {
  // ============================
  // Listener / socket parameters
  // ============================
  // TCP port to bind. 0 (default) lets the OS pick an ephemeral free port. After construction
  // you can retrieve the effective port via HttpServer::port().
  uint16_t port{};
  // If true, enables SO_REUSEPORT allowing multiple independent HttpServer instances (usually one per thread)
  // to bind the same (non-ephemeral) port for load distribution by the kernel. Harmless if the platform
  // or kernel does not support it (failure is logged, not fatal). Disabled by default.
  bool reusePort{false};

  // ============================
  // Request parsing & body limits
  // ============================
  // Maximum allowed size (in bytes) of the aggregate HTTP request head (request line + all headers + CRLFCRLF).
  // If exceeded while parsing, the server replies 431/400 and closes the connection. Default: 8 KiB.
  std::size_t maxHeaderBytes{8192};
  // Maximum allowed size (in bytes) of a request body (after decoding any chunked framing). Requests exceeding
  // this limit result in a 413 (Payload Too Large) style error (currently 400/413 depending on path) and closure.
  // Default: 1 MiB.
  std::size_t maxBodyBytes{1 << 20};  // 1 MiB

  // =============================================
  // Outbound buffering & backpressure management
  // =============================================
  // Upper bound (bytes) for data queued but not yet written to the client socket for a single connection.
  // Includes headers + body (streaming or aggregated). When exceeded further writes are rejected and the
  // connection marked for closure after flushing what is already queued. Default: 4 MiB per connection.
  std::size_t maxOutboundBufferBytes{4 << 20};  // 4 MiB

  // ===========================================
  // Keep-Alive / connection lifecycle controls
  // ===========================================
  // Maximum number of HTTP requests to serve over a single persistent connection before forcing close.
  // Helps cap memory use for long-lived clients and provides fairness. Default: 100.
  uint32_t maxRequestsPerConnection{100};
  // Whether HTTP/1.1 persistent connections (keep-alive) are enabled. When false, server always closes after
  // each response regardless of client headers. Default: true.
  bool enableKeepAlive{true};
  // Idle timeout for keep-alive connections (duration to wait for next request after previous response is fully
  // sent). Once exceeded the server proactively closes the connection. Default: 5000 ms.
  std::chrono::milliseconds keepAliveTimeout{std::chrono::milliseconds{5000}};

  // ===========================================
  // Slowloris / header read timeout mitigation
  // ===========================================
  // Maximum duration allowed to fully receive the HTTP request headers (request line + headers + CRLFCRLF)
  // from the moment the first byte of the request is read on a connection. If exceeded before the header
  // terminator is observed the server closes the connection (optionally could emit 408 in future). A value
  // of 0 disables this protective timeout. Default: disabled.
  std::chrono::milliseconds headerReadTimeout{std::chrono::milliseconds{0}};

  // Fluent builder style setters
  ServerConfig& withPort(uint16_t port) {  // Set explicit listening port (0 = ephemeral)
    this->port = port;
    return *this;
  }

  ServerConfig& withReusePort(bool on = true) {  // Enable/disable SO_REUSEPORT
    this->reusePort = on;
    return *this;
  }

  ServerConfig& withKeepAliveMode(bool on = true) {  // Toggle persistent connections
    this->enableKeepAlive = on;
    return *this;
  }

  ServerConfig& withMaxHeaderBytes(std::size_t maxHeaderBytes) {  // Adjust header size ceiling
    this->maxHeaderBytes = maxHeaderBytes;
    return *this;
  }

  ServerConfig& withMaxBodyBytes(std::size_t maxBodyBytes) {  // Adjust body size limit
    this->maxBodyBytes = maxBodyBytes;
    return *this;
  }

  ServerConfig& withMaxOutboundBufferBytes(std::size_t maxOutbound) {  // Adjust per-connection outbound queue cap
    this->maxOutboundBufferBytes = maxOutbound;
    return *this;
  }

  ServerConfig& withMaxRequestsPerConnection(uint32_t maxRequests) {  // Adjust request-per-connection cap
    this->maxRequestsPerConnection = maxRequests;
    return *this;
  }

  ServerConfig& withKeepAliveTimeout(std::chrono::milliseconds timeout) {  // Adjust idle keep-alive timeout
    this->keepAliveTimeout = timeout;
    return *this;
  }

  ServerConfig& withHeaderReadTimeout(std::chrono::milliseconds timeout) {  // Set slow header read timeout (0=off)
    this->headerReadTimeout = timeout;
    return *this;
  }
};

}  // namespace aeronet
