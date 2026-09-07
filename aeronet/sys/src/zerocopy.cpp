#include "aeronet/zerocopy.hpp"

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string_view>

// Ensure timespec is defined before including linux/errqueue.h
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec

#include "aeronet/native-handle.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/transport-result.hpp"

namespace aeronet {

ZeroCopyEnableResult EnableZeroCopy(int fd) noexcept {
  // Try to enable SO_ZEROCOPY
  int optVal = 1;
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &optVal, sizeof(optVal)) == -1) {
    // ENOPROTOOPT means the kernel or socket type doesn't support zerocopy
    if (errno == ENOPROTOOPT || error::IsNotSupported(errno)) {
      return ZeroCopyEnableResult::NotSupported;
    }
    return ZeroCopyEnableResult::Error;
  }

  return ZeroCopyEnableResult::Enabled;
}

TransportResult ZeroCopyState::tryZerocopySend(NativeHandle fd, std::uint32_t minBytesForZerocopy,
                                               std::string_view firstBuffer, std::string_view secondBuffer) {
  const std::size_t size = firstBuffer.size() + secondBuffer.size();
  assert(size > 0);

  TransportResult res{};

  if (!enabled() || size < minBytesForZerocopy) {
    return res;
  }

  assert(minBytesForZerocopy > 0);

  // Drain pending completion notifications before issuing a new zerocopy send.
  // Prevents the kernel error queue from growing unbounded, avoids kNoBufferSpace, and releases pinned pages promptly
  // - critical for virtual devices (veth in K8s).
  pollZeroCopyCompletions(fd);

  assert(enabled());

  iovec iov[2];  // NOLINT(misc-include-cleaner) it's in sys/uio.h (included)

  msghdr msg{};
  msg.msg_iov = iov;

  iov[0] = {const_cast<char*>(firstBuffer.data()), firstBuffer.size()};

  if (secondBuffer.empty()) {
    msg.msg_iovlen = 1;
  } else {
    iov[1] = {const_cast<char*>(secondBuffer.data()), secondBuffer.size()};
    msg.msg_iovlen = 2;
  }

  // Use sendmsg with MSG_ZEROCOPY for large payloads. MSG_ZEROCOPY tells the kernel to DMA from user pages directly
  const auto nbWritten = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);

  if (nbWritten <= 0) {
    assert(nbWritten != 0);
    // consider failure - normally, a decent system cannot return 0 for a successful call of a non-empty buffer send.
    const int zcErr = LastSystemError();

    if (error::IsNotSupported(zcErr)) {
      // Disable zerocopy for this transport
      setEnabled(false);
    } else if (zcErr == error::kWouldBlock) {
      res.want = TransportHint::WriteReady;
    } else if (zcErr == error::kInterrupted || zcErr == error::kNoBufferSpace) {
      // Interrupted, or kernel can't pin more pages (transient) - fall through to regular write path.
    } else {
      res.want = TransportHint::Error;
    }
  } else {
    // Track the pending completion - kernel will notify via error queue.
    // The kernel assigns monotonically increasing sequence numbers starting from 0; seqHi tracks the next expected
    // sequence so pollZeroCopyCompletions can determine when all outstanding sends have completed.
    ++seqHi;

    res.bytesProcessed = static_cast<std::size_t>(nbWritten);
  }

  return res;
}

std::size_t ZeroCopyState::pollZeroCopyCompletions(int fd) noexcept {
  std::size_t completions = 0;

  if (!pendingCompletions()) {
    return completions;
  }

  // Buffer for recvmsg to read error queue entries
  // We need space for the extended error structure plus its associated data
  char controlBuf[CMSG_SPACE(sizeof(sock_extended_err) + sizeof(std::uint32_t))];

  msghdr msg{};
  for (;;) {
    msg.msg_control = controlBuf;
    msg.msg_controllen = sizeof(controlBuf);

    // MSG_ERRQUEUE reads from the socket error queue (where zerocopy completions arrive)
    // MSG_DONTWAIT ensures we don't block if no completions are ready
    const auto ret = ::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (ret == -1) {
      // EAGAIN/EWOULDBLOCK means no more completions available
      // Other errors: stop polling but don't treat as fatal
      break;
    }

    // Parse the control message to find zerocopy completion info
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    while (cm != nullptr) {
      if ((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) ||
          (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR)) {
        auto* serr = reinterpret_cast<sock_extended_err*>(CMSG_DATA(cm));

        if (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY) {
          // Update completion tracking
          // serr->ee_info is the sequence number of the first completed send
          // serr->ee_data is the sequence number of the last completed send
          seqLo = serr->ee_data + 1;
          ++completions;

          // serr->ee_code indicates whether the kernel actually used zerocopy:
          // SO_EE_CODE_ZEROCOPY_COPIED: kernel fell back to copying (still valid completion)
          // 0: true zerocopy was used
          // Either way, the buffer can now be reused.
        }
      }

      cm = CMSG_NXTHDR(&msg, cm);
    }
  }

  return completions;
}

}  // namespace aeronet
