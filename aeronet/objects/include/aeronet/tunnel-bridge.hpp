#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/platform.hpp"

namespace aeronet {

/// Pure-virtual interface for CONNECT tunnel integration between the HTTP/2
/// protocol handler and the server's event loop / connection manager.
///
/// The server implements a concrete bridge (e.g. H2TunnelBridge inside
/// SingleHttpServer) and hands a non-owning pointer to the HTTP/2 handler.
/// This breaks the circular dependency: aeronet_http2 depends only on this
/// interface in aeronet_objects, while the main server module provides the
/// implementation.
///
/// Thread safety: NOT thread-safe — called on the single-threaded event loop.
class ITunnelBridge {
 public:
  virtual ~ITunnelBridge() = default;

  /// Set up a TCP connection to the given target host:port.
  /// @return The upstream fd on success, kInvalidHandle on failure.
  [[nodiscard]] virtual NativeHandle setupTunnel(uint32_t streamId, std::string_view host, std::string_view port) = 0;

  /// Write data to an upstream tunnel fd. The server handles buffering and EPOLLOUT.
  virtual void writeTunnel(NativeHandle upstreamFd, std::span<const std::byte> data) = 0;

  /// Half-close the upstream tunnel fd (shutdown write side).
  virtual void shutdownTunnelWrite(NativeHandle upstreamFd) = 0;

  /// Close and deregister an upstream tunnel fd.
  virtual void closeTunnel(NativeHandle upstreamFd) = 0;

  /// Notify the server that a WINDOW_UPDATE was received for a tunnel stream,
  /// allowing the server to resume forwarding buffered upstream data.
  virtual void onTunnelWindowUpdate(NativeHandle upstreamFd) = 0;
};

}  // namespace aeronet
