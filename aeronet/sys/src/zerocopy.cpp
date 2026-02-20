#include "aeronet/zerocopy.hpp"

#ifdef __linux__

#include <linux/errqueue.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string_view>

namespace aeronet {

ZeroCopyEnableResult EnableZeroCopy(int fd) noexcept {
  // Try to enable SO_ZEROCOPY
  int optVal = 1;
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &optVal, sizeof(optVal)) == -1) {
    // ENOPROTOOPT means the kernel or socket type doesn't support zerocopy
    if (errno == ENOPROTOOPT || errno == EOPNOTSUPP) {
      return ZeroCopyEnableResult::NotSupported;
    }
    return ZeroCopyEnableResult::Error;
  }

  return ZeroCopyEnableResult::Enabled;
}

ssize_t ZerocopySend(int fd, std::string_view data, ZeroCopyState& state) noexcept {
  assert(state.enabled());

  // Use sendmsg with MSG_ZEROCOPY for large payloads
  // MSG_ZEROCOPY tells the kernel to DMA from user pages directly
  // NOLINTNEXTLINE(misc-include-cleaner)
  iovec iov{const_cast<char*>(data.data()), data.size()};

  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  const ssize_t sent = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
  if (sent > 0) {
    // Track the pending completion - kernel will notify via error queue.
    // The kernel assigns monotonically increasing sequence numbers starting from 0;
    // seqHi tracks the next expected sequence so PollZeroCopyCompletions can determine
    // when all outstanding sends have completed.
    ++state.seqHi;
  }

  return sent;
}

ssize_t ZerocopySend(int fd, std::string_view firstBuf, std::string_view secondBuf, ZeroCopyState& state) noexcept {
  // Use sendmsg with MSG_ZEROCOPY for large payloads
  // MSG_ZEROCOPY tells the kernel to DMA from user pages directly
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  iovec iov[]{{const_cast<char*>(firstBuf.data()), firstBuf.size()},
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(secondBuf.data()), secondBuf.size()}};

  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = secondBuf.empty() ? 1 : 2;

  const ssize_t sent = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
  if (sent > 0) {
    ++state.seqHi;
  }

  return sent;
}

std::size_t PollZeroCopyCompletions(int fd, ZeroCopyState& state) noexcept {
  if (!state.pendingCompletions()) {
    return 0;
  }

  std::size_t completions = 0;

  // Buffer for recvmsg to read error queue entries
  // We need space for the extended error structure plus its associated data
  char controlBuf[CMSG_SPACE(sizeof(sock_extended_err) + sizeof(std::uint32_t))];

  msghdr msg{};
  for (;;) {
    msg.msg_control = controlBuf;
    msg.msg_controllen = sizeof(controlBuf);

    // MSG_ERRQUEUE reads from the socket error queue (where zerocopy completions arrive)
    // MSG_DONTWAIT ensures we don't block if no completions are ready
    const ssize_t ret = ::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (ret == -1) {
      // EAGAIN/EWOULDBLOCK means no more completions available
      static_assert(EAGAIN == EWOULDBLOCK);
      if (errno == EAGAIN) {
        break;
      }
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
          state.seqLo = serr->ee_data + 1;
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

#endif  // __linux__
