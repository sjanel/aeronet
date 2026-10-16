#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/native-handle.hpp"

namespace aeronet {

// Result of enabling zerocopy on a socket.
enum class ZeroCopyEnableResult : std::uint8_t {
  Enabled,       // SO_ZEROCOPY successfully set
  NotSupported,  // Kernel or socket type doesn't support zerocopy
  Error,         // setsockopt failed
};

// Outcome of a send attempted with MSG_ZEROCOPY.
enum class ZerocopySendStatus : std::uint8_t {
  Sent,        // Bytes were sent: the kernel may read their buffers until the completion of the send
  WouldBlock,  // Socket send buffer full
  Error,       // Fatal socket error
  // Nothing was sent: the bytes can be sent without MSG_ZEROCOPY instead.
  Interrupted,    // Interrupted by a signal
  NoBufferSpace,  // ENOBUFS: socket option memory (net.core.optmem_max) or locked memory (RLIMIT_MEMLOCK) exhausted
  NotSupported,   // The socket does not support MSG_ZEROCOPY
};

struct ZerocopySendResult {
  std::size_t bytesSent{0};
  ZerocopySendStatus status{ZerocopySendStatus::Sent};
};

// Completion notifications of zerocopy sends, read from the error queue of a socket.
struct ZerocopyCompletions {
  // Number of zerocopy sends reported as completed.
  std::uint32_t nbSends{0};
  // Number of them whose data the kernel copied anyway (SO_EE_CODE_ZEROCOPY_COPIED): local peer (loopback, other
  // network namespace of the host), device without scatter-gather...
  std::uint32_t nbCopiedSends{0};
};

// MSG_ZEROCOPY sends of a TCP socket, and the tracking of their completion.
// The kernel numbers the zerocopy sends of a socket from 0 (modulo 2^32), and reports their completion in the error
// queue of the socket as ranges of sequence numbers, once it no longer reads their buffers. Ranges usually arrive in
// order, but not always (retransmissions, socket teardown): completedBefore() is then a lower bound, exact again once
// all the sends completed.
struct ZeroCopyState {
  // Whether some zerocopy sends were not completed yet.
  [[nodiscard]] bool pendingCompletions() const noexcept { return nbCompleted != seqHi; }

  // Sequence number of the next zerocopy send. The buffers of the sends issued so far may be read by the kernel until
  // completedBefore(nextSeq()) returns true.
  [[nodiscard]] std::uint32_t nextSeq() const noexcept { return seqHi; }

  // Whether all the zerocopy sends numbered before 'seqEnd' completed.
  [[nodiscard]] bool completedBefore(std::uint32_t seqEnd) const noexcept {
    return static_cast<std::int32_t>(seqLo - seqEnd) >= 0;
  }

  // Records the completion of the sends numbered from 'first' to 'last' (included), as reported by the kernel.
  void complete(std::uint32_t first, std::uint32_t last) noexcept {
    nbCompleted += last - first + 1U;
    if (first == seqLo) {
      seqLo = last + 1U;
    }
    if (nbCompleted == seqHi) {
      // Each send completes once: all of them did, including those reported before an earlier one.
      seqLo = seqHi;
    }
  }

  // Maximum number of buffers of a send.
  static constexpr std::size_t kMaxBuffers = 64;

#ifdef AERONET_LINUX
  // Sends 'buffers' in order with MSG_ZEROCOPY (non-blocking). At most kMaxBuffers buffers, not all empty (the empty
  // ones are skipped).
  ZerocopySendResult send(NativeHandle fd, std::span<const std::string_view> buffers);

  // Reads the completion notifications available in the error queue of the socket (non-blocking), stopping once all
  // the sends completed.
  ZerocopyCompletions pollCompletions(NativeHandle fd) noexcept;
#endif

  // All the sends numbered before seqLo completed (a lower bound while completions arrive out of order).
  std::uint32_t seqLo{0};
  // Number of zerocopy sends issued, modulo 2^32: sequence number of the next one.
  std::uint32_t seqHi{0};
  // Number of zerocopy sends completed, modulo 2^32.
  std::uint32_t nbCompleted{0};
};

#ifdef AERONET_LINUX

/// Enable MSG_ZEROCOPY on a TCP socket: sets SO_ZEROCOPY, inherited by the sockets accepted from a listening socket.
ZeroCopyEnableResult EnableZeroCopy(NativeHandle fd) noexcept;

#else
// Non-Linux stubs - zerocopy is Linux-specific

inline ZeroCopyEnableResult EnableZeroCopy([[maybe_unused]] NativeHandle fd) noexcept {
  return ZeroCopyEnableResult::NotSupported;
}

#endif

}  // namespace aeronet
