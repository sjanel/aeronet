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

namespace aeronet {

namespace {

[[nodiscard]] bool IsRecvErr(const cmsghdr& cmsg) noexcept {
  return (cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
         (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR);
}

}  // namespace

ZeroCopyEnableResult EnableZeroCopy(NativeHandle fd) noexcept {
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

ZerocopySendResult ZeroCopyState::send(NativeHandle fd, std::string_view firstBuffer, std::string_view secondBuffer) {
  assert(!firstBuffer.empty() || !secondBuffer.empty());

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

  const auto nbWritten = ::sendmsg(fd, &msg, MSG_ZEROCOPY | MSG_NOSIGNAL);
  if (nbWritten > 0) {
    // The kernel numbered this send. A send that fails gives its sequence number back.
    ++seqHi;
    return {static_cast<std::size_t>(nbWritten), ZerocopySendStatus::Sent};
  }
  // A send of a non-empty buffer never returns 0.
  assert(nbWritten == -1);

  const int err = LastSystemError();
  if (err == error::kWouldBlock) {
    return {0, ZerocopySendStatus::WouldBlock};
  }
  if (err == error::kInterrupted) {
    return {0, ZerocopySendStatus::Interrupted};
  }
  if (err == error::kNoBufferSpace) {
    return {0, ZerocopySendStatus::NoBufferSpace};
  }
  if (error::IsNotSupported(err)) {
    return {0, ZerocopySendStatus::NotSupported};
  }
  return {0, ZerocopySendStatus::Error};
}

ZerocopyCompletions ZeroCopyState::pollCompletions(NativeHandle fd) noexcept {
  ZerocopyCompletions completions;

  // The kernel appends the address of the offender to the notification (a sockaddr_in6 at most): without room for it,
  // the control message would be truncated (MSG_CTRUNC).
  alignas(cmsghdr) char controlBuf[CMSG_SPACE(sizeof(sock_extended_err) + sizeof(sockaddr_in6))];

  while (pendingCompletions()) {
    msghdr msg{};
    msg.msg_control = controlBuf;
    msg.msg_controllen = sizeof(controlBuf);

    // MSG_ERRQUEUE reads never block: EAGAIN tells that no more notification is available. Stop on any error as well.
    if (::recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) == -1) {
      break;
    }

    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (!IsRecvErr(*cmsg)) {
        continue;
      }
      const auto* serr = reinterpret_cast<const sock_extended_err*>(CMSG_DATA(cmsg));
      if (serr->ee_origin != SO_EE_ORIGIN_ZEROCOPY || serr->ee_errno != 0) {
        continue;
      }
      // ee_info and ee_data are the sequence numbers of the first and last completed sends.
      const std::uint32_t nbSends = serr->ee_data - serr->ee_info + 1U;
      complete(serr->ee_info, serr->ee_data);
      completions.nbSends += nbSends;
      if ((serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED) != 0) {
        completions.nbCopiedSends += nbSends;
      }
    }
  }

  return completions;
}

}  // namespace aeronet
