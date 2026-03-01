#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string_view>

#include "aeronet/platform.hpp"

#ifdef __linux__
// Ensure timespec is defined before including linux/errqueue.h
#include <linux/errqueue.h>
#include <sys/socket.h>

#endif

namespace aeronet {

// Result of enabling zerocopy on a socket.
enum class ZeroCopyEnableResult : std::uint8_t {
  Enabled,       // SO_ZEROCOPY successfully set
  NotSupported,  // Kernel or socket type doesn't support zerocopy
  Error          // setsockopt failed
};

// Result of a zerocopy send operation.
enum class ZerocopySendResult : std::uint8_t {
  Sent,          // Data sent with zerocopy
  SentWithCopy,  // Data sent but zerocopy fell back to copy (small payload or unsupported)
  WouldBlock,    // EAGAIN/EWOULDBLOCK - socket not ready
  Error          // Fatal error
};

// Tracks in-flight zerocopy buffers waiting for completion notification.
// The kernel delivers completion via the socket error queue with SO_EE_ORIGIN_ZEROCOPY.
struct ZeroCopyState {
  [[nodiscard]] bool pendingCompletions() const noexcept { return seqLo < seqHi; }

  [[nodiscard]] bool enabled() const noexcept { return seqLo != ~0U; }

  void setEnabled(bool enabled) noexcept { seqLo = static_cast<uint32_t>(enabled) - 1U; }

  // Sequence number range tracking completions from the kernel error queue.
  // lo..hi defines the range of outstanding zerocopy sends.
  std::uint32_t seqLo{~0U};
  std::uint32_t seqHi{0};
};

#ifdef __linux__

/// Enable MSG_ZEROCOPY on a TCP socket. Call once after socket creation.
/// Returns the result of the operation.
ZeroCopyEnableResult EnableZeroCopy(int fd) noexcept;

/// Perform a zerocopy send if conditions are met (large payload, zerocopy enabled).
/// Returns the number of bytes sent, or -1 on error.
/// On success with zerocopy, sets completionPending = true indicating the buffer must not be modified/freed.
/// The caller must poll for completion via PollZeroCopyCompletion before reusing the buffer.
///
/// Automatically falls back to regular send for small payloads or when zerocopy is not enabled.
///
/// @param fd The socket file descriptor
/// @param data The data to send
/// @param state Zerocopy tracking state (updated on success)
/// @return Bytes sent (>=0) or -1 on error (check errno)
[[nodiscard]] ssize_t ZerocopySend(int fd, std::string_view data, ZeroCopyState& state) noexcept;

/// Perform a zerocopy send for two buffers if conditions are met (large payload, zerocopy enabled).
/// Returns the number of bytes sent, or -1 on error.
/// On success with zerocopy, sets completionPending = true indicating the buffers must not be modified/freed.
/// The caller must poll for completion via PollZeroCopyCompletion before reusing the buffers.
///
/// Automatically falls back to regular send for small payloads or when zerocopy is not enabled.
///
/// @param fd The socket file descriptor
/// @param firstBuf The first buffer to send
/// @param secondBuf The second buffer to send
/// @param state Zerocopy tracking state (updated on success)
/// @return Bytes sent (>=0) or -1 on error (check errno)
[[nodiscard]] ssize_t ZerocopySend(int fd, std::string_view firstBuf, std::string_view secondBuf,
                                   ZeroCopyState& state) noexcept;

/// Poll the socket error queue for zerocopy completion notifications.
/// Call this before reusing buffers that were sent with zerocopy.
/// This is non-blocking and drains all available completions.
///
/// @param fd The socket file descriptor
/// @param state Zerocopy tracking state (updated with completed ranges)
/// @return Number of completions processed (may be 0 if none ready)
std::size_t PollZeroCopyCompletions(int fd, ZeroCopyState& state) noexcept;

#else
// Non-Linux stubs - zerocopy is Linux-specific

inline ZeroCopyEnableResult EnableZeroCopy([[maybe_unused]] NativeHandle fd) noexcept {
  return ZeroCopyEnableResult::NotSupported;
}

[[nodiscard]] inline bool IsZeroCopyEnabled([[maybe_unused]] NativeHandle fd) noexcept { return false; }

[[nodiscard]] inline ssize_t ZerocopySend([[maybe_unused]] NativeHandle fd, [[maybe_unused]] std::string_view data,
                                          [[maybe_unused]] ZeroCopyState& state) noexcept {
  return -1;
}

[[nodiscard]] inline ssize_t ZerocopySend([[maybe_unused]] NativeHandle fd, [[maybe_unused]] std::string_view firstBuf,
                                          [[maybe_unused]] std::string_view secondBuf,
                                          [[maybe_unused]] ZeroCopyState& state) noexcept {
  return -1;
}

inline std::size_t PollZeroCopyCompletions([[maybe_unused]] NativeHandle fd,
                                           [[maybe_unused]] ZeroCopyState& state) noexcept {
  return 0;
}

#endif

}  // namespace aeronet
