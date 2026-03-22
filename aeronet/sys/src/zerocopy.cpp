#include "aeronet/zerocopy.hpp"

#ifdef AERONET_IO_URING
#include <liburing.h>
#endif
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>  // NOLINT(misc-include-cleaner) used by iovec

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/system-error.hpp"

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

int64_t ZerocopySend(int fd, std::string_view data, ZeroCopyState& state) noexcept {
  assert(state.enabled());

  // Use sendmsg with MSG_ZEROCOPY for large payloads
  // MSG_ZEROCOPY tells the kernel to DMA from user pages directly
  // NOLINTNEXTLINE(misc-include-cleaner)
  iovec iov{const_cast<char*>(data.data()), data.size()};

  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  const auto sent = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
  if (sent > 0) {
    // Track the pending completion - kernel will notify via error queue.
    // The kernel assigns monotonically increasing sequence numbers starting from 0;
    // seqHi tracks the next expected sequence so PollZeroCopyCompletions can determine
    // when all outstanding sends have completed.
    ++state.seqHi;
  }

  return static_cast<int64_t>(sent);
}

int64_t ZerocopySend(int fd, std::string_view firstBuf, std::string_view secondBuf, ZeroCopyState& state) noexcept {
  // Use sendmsg with MSG_ZEROCOPY for large payloads
  // MSG_ZEROCOPY tells the kernel to DMA from user pages directly
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  iovec iov[]{{const_cast<char*>(firstBuf.data()), firstBuf.size()},
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(secondBuf.data()), secondBuf.size()}};

  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = secondBuf.empty() ? 1 : 2;

  const auto sent = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
  if (sent > 0) {
    ++state.seqHi;
  }

  return static_cast<int64_t>(sent);
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
    const auto ret = ::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (ret == -1) {
      // EAGAIN/EWOULDBLOCK means no more completions available
      if (errno == error::kWouldBlock) {
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

#ifdef AERONET_IO_URING
namespace {
// Wait for send completion + optional buffer notification CQE.
// Returns bytes sent or -1 on error (errno set).
int64_t WaitSendZcCompletion(struct io_uring* ring) {
  struct io_uring_cqe* cqe = nullptr;
  int waitRet = ::io_uring_wait_cqe(ring, &cqe);
  if (waitRet < 0) [[unlikely]] {
    errno = -waitRet;
    return -1;
  }
  const int res = cqe->res;
  const bool hasMore = (cqe->flags & IORING_CQE_F_MORE) != 0;
  ::io_uring_cqe_seen(ring, cqe);

  if (res < 0) {
    // If send failed, still consume the notification CQE if flagged.
    if (hasMore) {
      cqe = nullptr;
      (void)::io_uring_wait_cqe(ring, &cqe);
      if (cqe != nullptr) {
        ::io_uring_cqe_seen(ring, cqe);
      }
    }
    errno = -res;
    return -1;
  }

  // Wait for IORING_CQE_F_NOTIF (buffer release notification).
  if (hasMore) {
    cqe = nullptr;
    waitRet = ::io_uring_wait_cqe(ring, &cqe);
    if (cqe != nullptr) {
      ::io_uring_cqe_seen(ring, cqe);
    }
  }

  return static_cast<int64_t>(res);
}
}  // namespace

int64_t IoUringSendZc(void* ioRing, NativeHandle fd, std::string_view data) noexcept {
  auto* ring = static_cast<struct io_uring*>(ioRing);
  auto* sqe = ::io_uring_get_sqe(ring);
  if (sqe == nullptr) [[unlikely]] {
    errno = EAGAIN;
    return -1;
  }
  ::io_uring_prep_send_zc(sqe, fd, data.data(), data.size(), MSG_NOSIGNAL, 0);
  ::io_uring_sqe_set_data64(sqe, 0);
  const int submitted = ::io_uring_submit(ring);
  if (submitted < 0) [[unlikely]] {
    errno = -submitted;
    return -1;
  }
  return WaitSendZcCompletion(ring);
}

int64_t IoUringSendZc(void* ioRing, NativeHandle fd, std::string_view firstBuf, std::string_view secondBuf) noexcept {
  auto* ring = static_cast<struct io_uring*>(ioRing);
  auto* sqe = ::io_uring_get_sqe(ring);
  if (sqe == nullptr) [[unlikely]] {
    errno = EAGAIN;
    return -1;
  }
  // NOLINTNEXTLINE(misc-include-cleaner)
  iovec iov[]{{const_cast<char*>(firstBuf.data()), firstBuf.size()},
              // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
              {const_cast<char*>(secondBuf.data()), secondBuf.size()}};
  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = secondBuf.empty() ? 1 : 2;
  ::io_uring_prep_sendmsg_zc(sqe, fd, &msg, MSG_NOSIGNAL);
  ::io_uring_sqe_set_data64(sqe, 0);
  const int submitted = ::io_uring_submit(ring);
  if (submitted < 0) [[unlikely]] {
    errno = -submitted;
    return -1;
  }
  return WaitSendZcCompletion(ring);
}
#endif

}  // namespace aeronet
