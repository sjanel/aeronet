#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/host-resolver.hpp"
#include "aeronet/native-handle.hpp"

namespace aeronet::internal {

/// A CONNECT tunnel waiting for the resolution of its target host name.
/// Owned jointly by the event loop and by the resolver thread while it resolves it.
struct PendingTunnel : HostResolution {
  PendingTunnel(std::string_view targetHost, uint16_t targetPort, NativeHandle client, uint32_t stream,
                std::size_t requestBytes, std::chrono::steady_clock::time_point expiry)
      : HostResolution(targetHost, targetPort),
        consumedBytes(requestBytes),
        deadline(expiry),
        clientFd(client),
        streamId(stream) {}

  /// Size of the input buffer of the client connection up to the end of the HTTP/1.1 CONNECT request (the requests
  /// served before it included), consumed once answered.
  std::size_t consumedBytes;
  /// The CONNECT request is answered 504 if its target is not resolved by then.
  std::chrono::steady_clock::time_point deadline;
  NativeHandle clientFd;
  /// HTTP/2 stream of the CONNECT request, 0 for an HTTP/1.1 tunnel.
  uint32_t streamId;
  /// Set by the event loop when the client connection closed before the resolution completed.
  bool cancelled{false};
};

}  // namespace aeronet::internal
