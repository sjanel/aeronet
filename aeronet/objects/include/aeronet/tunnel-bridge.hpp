#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/native-handle.hpp"

namespace aeronet {

/// Pure-virtual interface for CONNECT tunnel integration between the HTTP/2 protocol handler and the server's event
/// loop / connection manager.
///
/// The server implements a concrete bridge (TunnelManager::H2Bridge in the server module) and hands a non-owning
/// pointer to the HTTP/2 handler. This breaks the circular dependency: aeronet_http2 depends only on this interface in
/// aeronet_objects, while the main server module provides the implementation.
///
/// Thread safety: NOT thread-safe - called on the single-threaded event loop.
class ITunnelBridge {
 public:
  virtual ~ITunnelBridge() = default;

  /// Outcome of setupTunnel().
  struct TunnelSetup {
    enum class Status : uint8_t {
      Established,  // upstreamFd is the connection to the target (possibly still connecting)
      Pending,      // the target host name is resolved in the background, see Http2ProtocolHandler::onTunnelResolved()
      Failed,       // the target could not be reached
    };

    NativeHandle upstreamFd{kInvalidHandle};
    Status status{Status::Failed};
  };

  /// Set up a TCP connection to the given target host:port.
  [[nodiscard]] virtual TunnelSetup setupTunnel(uint32_t streamId, std::string_view host, uint16_t port) = 0;

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
