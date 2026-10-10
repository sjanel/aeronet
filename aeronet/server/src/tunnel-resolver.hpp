#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

/// A CONNECT tunnel waiting for the resolution of its target host name.
/// Owned jointly by the event loop and by the resolver thread while it resolves it.
struct PendingTunnel {
  PendingTunnel(std::string_view targetHost, uint16_t targetPort, NativeHandle client, uint32_t stream,
                std::size_t requestBytes);

  /// Target host, null terminated for getaddrinfo.
  [[nodiscard]] std::string_view host() const noexcept { return {hostBuffer.data(), hostBuffer.size() - 1U}; }

  RawChars32 hostBuffer;
  /// Resolved addresses, nullptr when the resolution failed. Written by the resolver thread before it publishes the
  /// tunnel as resolved.
  AddrInfoPtr addresses;
  /// Size of the HTTP/1.1 CONNECT request in the input buffer of the client connection, consumed once answered.
  std::size_t consumedBytes;
  NativeHandle clientFd;
  /// HTTP/2 stream of the CONNECT request, 0 for an HTTP/1.1 tunnel.
  uint32_t streamId;
  uint16_t port;
  /// Set by the event loop when the client connection closed before the resolution completed.
  bool cancelled{false};
};

/// Resolves CONNECT target host names off the event loop: getaddrinfo blocks until the resolver answers, which can
/// take seconds. Resolutions run on a few background threads, created on demand. The event loop collects the resolved
/// tunnels with takeResolved() once notified.
///
/// Destroying the resolver never waits for a resolution in progress: its thread finishes it, drops the result, and
/// exits.
class TunnelResolver {
 public:
  /// Maximum number of resolutions running concurrently.
  static constexpr uint32_t kMaxThreads = 4;

  /// 'notify' is called from a resolver thread each time a tunnel was resolved: it must wake up the event loop. It is
  /// never called anymore once the resolver is destroyed.
  explicit TunnelResolver(std::function<void()> notify);

  TunnelResolver(const TunnelResolver&) = delete;
  TunnelResolver(TunnelResolver&&) = delete;
  TunnelResolver& operator=(const TunnelResolver&) = delete;
  TunnelResolver& operator=(TunnelResolver&&) = delete;

  ~TunnelResolver();

  /// Queue the resolution of the host of 'pending'.
  void resolve(std::shared_ptr<PendingTunnel> pending);

  /// Tell whether resolved tunnels are waiting to be collected.
  [[nodiscard]] bool hasResolved() const noexcept;

  /// Move the resolved tunnels into 'out' (cleared first).
  void takeResolved(vector<std::shared_ptr<PendingTunnel>>& out);

 private:
  struct Shared;

  static void ResolveLoop(const std::shared_ptr<Shared>& shared);

  std::shared_ptr<Shared> _shared;
};

}  // namespace aeronet::internal
