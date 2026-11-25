#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class EventFd;

}  // namespace aeronet

namespace aeronet::internal {

/// A host name to resolve with a HostResolver, owned jointly by its requester and by the resolver thread while it
/// resolves it. A requester keeping more state for it derives its own type from it.
struct HostResolution {
  HostResolution(std::string_view host, uint16_t port);

  /// Host to resolve, null terminated for getaddrinfo.
  [[nodiscard]] std::string_view host() const noexcept { return {hostBuffer.data(), hostBuffer.size() - 1U}; }

  RawChars32 hostBuffer;
  /// Resolved addresses, nullptr when the resolution failed. Written by the resolver thread before it publishes the
  /// resolution.
  AddrInfoPtr addresses;
  uint16_t port;
};

/// Resolves host names off an event loop: getaddrinfo blocks until the resolver answers, which can take seconds.
/// Resolutions run on a few background threads, created on demand. The event loop collects the resolutions done with
/// takeResolved() once notified.
///
/// Destroying the resolver never waits for a resolution in progress: its thread finishes it, drops the result, and
/// exits.
class HostResolver {
 public:
  /// Maximum number of resolutions running concurrently.
  static constexpr uint32_t kMaxThreads = 4;

  /// 'wakeupFd' is signaled from a resolver thread each time a resolution is done, to wake up the event loop. It is
  /// never signaled anymore once the resolver is destroyed.
  explicit HostResolver(const EventFd& wakeupFd);

  HostResolver(const HostResolver&) = delete;
  HostResolver(HostResolver&&) = delete;
  HostResolver& operator=(const HostResolver&) = delete;
  HostResolver& operator=(HostResolver&&) = delete;

  ~HostResolver();

  /// Queue the resolution of the host of 'resolution'.
  void resolve(std::shared_ptr<HostResolution> resolution);

  /// Drop 'resolution' if it is still queued. A resolution in progress cannot be interrupted: its thread completes it,
  /// and takeResolved() returns it, for the caller to ignore.
  void cancel(const HostResolution& resolution);

  /// Tell whether resolutions done are waiting to be collected.
  [[nodiscard]] bool hasResolved() const noexcept;

  /// Move the resolutions done to the end of 'out'.
  void takeResolved(vector<std::shared_ptr<HostResolution>>& out);

 private:
  struct Shared;

  static void ResolveLoop(const std::shared_ptr<Shared>& shared);

  std::shared_ptr<Shared> _shared;
};

}  // namespace aeronet::internal
