#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/native-handle.hpp"
#include "aeronet/transport-result.hpp"

namespace aeronet {

// Result of enabling zerocopy on a socket.
enum class ZeroCopyEnableResult : std::uint8_t {
  Enabled,       // SO_ZEROCOPY successfully set
  NotSupported,  // Kernel or socket type doesn't support zerocopy
  Error,         // setsockopt failed
};

// Result of a zerocopy send operation.
enum class ZerocopySendResult : std::uint8_t {
  Sent,          // Data sent with zerocopy
  SentWithCopy,  // Data sent but zerocopy fell back to copy (small payload or unsupported)
  WouldBlock,    // EAGAIN/EWOULDBLOCK - socket not ready
  Error,         // Fatal error
};

// Tracks in-flight zerocopy buffers waiting for completion notification.
// The kernel delivers completion via the socket error queue with SO_EE_ORIGIN_ZEROCOPY.
struct ZeroCopyState {
  [[nodiscard]] bool pendingCompletions() const noexcept { return seqLo < seqHi; }

  [[nodiscard]] bool enabled() const noexcept { return seqLo != ~0U; }

  void setEnabled(bool enabled) noexcept { seqLo = static_cast<uint32_t>(enabled) - 1U; }

#ifdef AERONET_LINUX
  // Attempts a zerocopy send when eligible, for one or two buffers. Returns:
  //  - a TransportResult if the caller should return immediately (success, would-block, or fatal error)
  //  - std::nullopt if the caller should fall through to the regular (non-zerocopy) write path
  //    (zerocopy disabled/too small, interrupted, or transient kNoBufferSpace)
  TransportResult tryZerocopySend(NativeHandle fd, std::uint32_t minBytesForZerocopy, std::string_view firstBuffer,
                                  std::string_view secondBuffer = {});

  /// Poll the socket error queue for zerocopy completion notifications.
  /// Call this before reusing buffers that were sent with zerocopy.
  /// This is non-blocking and drains all available completions.
  ///
  /// @param fd The socket file descriptor
  /// @param state Zerocopy tracking state (updated with completed ranges)
  /// @return Number of completions processed (may be 0 if none ready)
  std::size_t pollZeroCopyCompletions(NativeHandle fd) noexcept;
#endif

  // Sequence number range tracking completions from the kernel error queue.
  // lo..hi defines the range of outstanding zerocopy sends.
  std::uint32_t seqLo{~0U};
  std::uint32_t seqHi{0};
};

#ifdef AERONET_LINUX

/// Enable MSG_ZEROCOPY on a TCP socket. Call once after socket creation.
/// Returns the result of the operation.
ZeroCopyEnableResult EnableZeroCopy(NativeHandle fd) noexcept;

#else
// Non-Linux stubs - zerocopy is Linux-specific

inline ZeroCopyEnableResult EnableZeroCopy([[maybe_unused]] NativeHandle fd) noexcept {
  return ZeroCopyEnableResult::NotSupported;
}

#endif

}  // namespace aeronet
