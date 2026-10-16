#include "aeronet/zerocopy.hpp"

#include <gtest/gtest.h>
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>

#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/system-error.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

namespace {

bool IsZeroCopyEnabled(int fd) noexcept {
  int optVal = 0;
  socklen_t optLen = sizeof(optVal);  // NOLINT(misc-include-cleaner)
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::getsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &optVal, &optLen) == -1) {
    return false;
  }
  return optVal != 0;
}

// Both ends of a connected AF_UNIX socket pair. Its sends and error queue reads are driven by the syscall overrides.
class SocketPair {
 public:
  SocketPair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0) {
      _ends[0] = BaseFd(fds[0]);
      _ends[1] = BaseFd(fds[1]);
    }
  }

  [[nodiscard]] int fd() const noexcept { return _ends[0].fd(); }

 private:
  BaseFd _ends[2];
};

using ErrQueueGuard = test::QueueResetGuard<test::KeyedActionQueue<int, test::ErrQueueNotification>>;
using IoActionGuard = test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>>;

// A state with 'nbSends' zerocopy sends issued, none completed.
ZeroCopyState WithSendsInFlight(uint32_t nbSends) {
  ZeroCopyState state;
  state.seqHi = nbSends;
  return state;
}

}  // namespace

TEST(ZeroCopyTest, EnableZerocopyOnTcpSocket) {
  // Create a TCP socket (zerocopy is only supported on TCP)
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0) << "socket() failed: " << std::strerror(errno);
  BaseFd guard(fd);

  const auto result = EnableZeroCopy(fd);
  // Zerocopy may or may not be supported depending on kernel version and configuration
  EXPECT_TRUE(result == ZeroCopyEnableResult::Enabled || result == ZeroCopyEnableResult::NotSupported)
      << "Unexpected result: " << static_cast<int>(result);

  if (result == ZeroCopyEnableResult::Enabled) {
    EXPECT_TRUE(IsZeroCopyEnabled(fd));

    // Enabling again should return Enabled
    const auto secondResult = EnableZeroCopy(fd);
    EXPECT_EQ(secondResult, ZeroCopyEnableResult::Enabled);
  }
}

TEST(ZeroCopyTest, EnableZerocopyOnUdpReturnNotSupported) {
  // UDP sockets may not support zerocopy (kernel dependent)
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0) << "socket() failed: " << std::strerror(errno);
  BaseFd guard(fd);

  // UDP zerocopy support varies by kernel - just verify we get a valid result
  const auto result = EnableZeroCopy(fd);
  EXPECT_TRUE(result == ZeroCopyEnableResult::Enabled || result == ZeroCopyEnableResult::NotSupported ||
              result == ZeroCopyEnableResult::Error);
}

TEST(ZeroCopyTest, IsZerocopyEnabledReturnsFalseOnNewSocket) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0);
  BaseFd guard(fd);

  // New socket should not have zerocopy enabled
  EXPECT_FALSE(IsZeroCopyEnabled(fd));
}

TEST(ZeroCopyTest, IsZerocopyEnabledReturnsFalseOnInvalidFd) { EXPECT_FALSE(IsZeroCopyEnabled(-1)); }

#ifdef AERONET_LINUX
TEST(ZeroCopyTest, EnableZerocopyReturnsNotSupportedOnEnoprotopt) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0);
  BaseFd guard(fd);

  // Simulate kernel/socket type not supporting zerocopy
  test::PushSetsockoptAction({-1, ENOPROTOOPT});

  const auto result = EnableZeroCopy(fd);
  EXPECT_EQ(result, ZeroCopyEnableResult::NotSupported);
}

TEST(ZeroCopyTest, EnableZerocopyReturnsNotSupportedOnEopnotsupp) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0);
  BaseFd guard(fd);

  // Simulate kernel/socket type not supporting zerocopy
  test::PushSetsockoptAction({-1, EOPNOTSUPP});

  const auto result = EnableZeroCopy(fd);
  EXPECT_EQ(result, ZeroCopyEnableResult::NotSupported);
}

TEST(ZeroCopyTest, EnableZerocopyReturnsErrorOnOtherErrno) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  ASSERT_GE(fd, 0);
  BaseFd guard(fd);

  // Simulate a different failure (permission denied)
  test::PushSetsockoptAction({-1, EACCES});

  const auto result = EnableZeroCopy(fd);
  EXPECT_EQ(result, ZeroCopyEnableResult::Error);
}
#endif  // AERONET_LINUX

// ---------------------------------------------------------------------------
// Tracking of the completions
// ---------------------------------------------------------------------------

TEST(ZeroCopyStateTest, NothingPendingInitially) {
  const ZeroCopyState state;
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_EQ(state.nextSeq(), 0U);
  EXPECT_TRUE(state.completedBefore(state.nextSeq()));
}

TEST(ZeroCopyStateTest, InOrderCompletionsAdvanceTheWatermark) {
  ZeroCopyState state = WithSendsInFlight(3);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_FALSE(state.completedBefore(1));

  state.complete(0, 0);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(1));
  EXPECT_FALSE(state.completedBefore(2));

  // The kernel coalesces the notifications of consecutive sends into one range.
  state.complete(1, 2);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(3));
  EXPECT_EQ(state.nbCompleted, 3U);
}

TEST(ZeroCopyStateTest, OutOfOrderCompletionKeepsEarlierSendPending) {
  // The completion of send 1 may arrive before the one of send 0 (retransmission of send 0): send 0 is still in flight.
  ZeroCopyState state = WithSendsInFlight(2);
  state.complete(1, 1);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_FALSE(state.completedBefore(1));
  EXPECT_FALSE(state.completedBefore(2));

  state.complete(0, 0);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(2));
  EXPECT_EQ(state.seqLo, 2U);
}

TEST(ZeroCopyStateTest, WatermarkIsALowerBoundWhileCompletionsAreOutOfOrder) {
  ZeroCopyState state = WithSendsInFlight(4);
  state.complete(2, 3);
  EXPECT_FALSE(state.completedBefore(1));

  state.complete(0, 0);
  EXPECT_TRUE(state.completedBefore(1));
  // Sends 2 and 3 completed, but only send 1 tells when the watermark can pass them.
  EXPECT_FALSE(state.completedBefore(2));
  EXPECT_TRUE(state.pendingCompletions());

  state.complete(1, 1);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(4));
}

TEST(ZeroCopyStateTest, SequenceNumbersWrapAround) {
  static constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  ZeroCopyState state;
  state.seqLo = kMax - 1U;
  state.nbCompleted = kMax - 1U;
  state.seqHi = 1U;  // sends kMax - 1, kMax and 0 in flight
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_FALSE(state.completedBefore(kMax));

  state.complete(kMax - 1U, kMax);
  EXPECT_TRUE(state.completedBefore(kMax));
  EXPECT_TRUE(state.completedBefore(0));
  EXPECT_FALSE(state.completedBefore(1));
  EXPECT_TRUE(state.pendingCompletions());

  state.complete(0, 0);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(1));
}

#ifdef AERONET_LINUX

// ---------------------------------------------------------------------------
// Sends
// ---------------------------------------------------------------------------

TEST(ZeroCopySendTest, SuccessfulSendIsNumbered) {
  SocketPair pair;
  const std::string head(4, 'H');
  const std::string body(4096, 'B');

  test::SetSendmsgActions(pair.fd(), {
                                         IoAction{static_cast<int64_t>(head.size() + body.size()), 0},
                                         IoAction{static_cast<int64_t>(body.size() / 2), 0},
                                     });
  IoActionGuard guard(test::g_sendmsg_actions);

  ZeroCopyState state;
  const std::string_view headAndBody[]{head, body};
  auto res = state.send(pair.fd(), headAndBody);
  EXPECT_EQ(res.status, ZerocopySendStatus::Sent);
  EXPECT_EQ(res.bytesSent, head.size() + body.size());
  EXPECT_EQ(state.nextSeq(), 1U);
  EXPECT_TRUE(state.pendingCompletions());

  // A partial send of a single buffer is a send too.
  const std::string_view bodyOnly[]{{}, body};
  res = state.send(pair.fd(), bodyOnly);
  EXPECT_EQ(res.status, ZerocopySendStatus::Sent);
  EXPECT_EQ(res.bytesSent, body.size() / 2);
  EXPECT_EQ(state.nextSeq(), 2U);
}

TEST(ZeroCopySendTest, FailedSendsAreNotNumbered) {
  struct Case {
    int err;
    ZerocopySendStatus status;
  };
  for (const Case testCase : {
           Case{error::kWouldBlock, ZerocopySendStatus::WouldBlock},
           Case{error::kInterrupted, ZerocopySendStatus::Interrupted},
           Case{error::kNoBufferSpace, ZerocopySendStatus::NoBufferSpace},
           Case{EOPNOTSUPP, ZerocopySendStatus::NotSupported},
           Case{error::kBrokenPipe, ZerocopySendStatus::Error},
       }) {
    SocketPair pair;
    test::SetSendmsgActions(pair.fd(), {IoAction{-1, testCase.err}});
    IoActionGuard guard(test::g_sendmsg_actions);

    ZeroCopyState state;
    const std::string_view data[]{"data"};
    const auto res = state.send(pair.fd(), data);
    EXPECT_EQ(res.status, testCase.status) << "errno " << testCase.err;
    EXPECT_EQ(res.bytesSent, 0U);
    // The kernel gives the sequence number of a failed send back.
    EXPECT_EQ(state.nextSeq(), 0U);
    EXPECT_FALSE(state.pendingCompletions());
  }
}

// ---------------------------------------------------------------------------
// Completion notifications
// ---------------------------------------------------------------------------

TEST(ZeroCopyPollTest, NoErrorQueueReadWithoutSendsInFlight) {
  SocketPair pair;
  ErrQueueGuard guard(test::g_errqueue_notifications);
  test::PushErrQueueNotification(pair.fd(), {.first = 0, .last = 0});

  ZeroCopyState state;
  const auto completions = state.pollCompletions(pair.fd());
  EXPECT_EQ(completions.nbSends, 0U);
  EXPECT_EQ(test::g_errqueue_notifications.size(pair.fd()), 1U);
}

TEST(ZeroCopyPollTest, ReadsNotificationsUntilAllSendsCompleted) {
  SocketPair pair;
  ErrQueueGuard guard(test::g_errqueue_notifications);
  test::PushErrQueueNotification(pair.fd(), {.first = 0, .last = 0});
  test::PushErrQueueNotification(pair.fd(), {.first = 1, .last = 2});
  // Not read: all the sends completed before.
  test::PushErrQueueNotification(pair.fd(), {.first = 3, .last = 3});

  ZeroCopyState state = WithSendsInFlight(3);
  const auto completions = state.pollCompletions(pair.fd());
  EXPECT_EQ(completions.nbSends, 3U);
  EXPECT_EQ(completions.nbCopiedSends, 0U);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_EQ(test::g_errqueue_notifications.size(pair.fd()), 1U);
}

TEST(ZeroCopyPollTest, StopsOnEmptyErrorQueue) {
  SocketPair pair;
  ErrQueueGuard notificationsGuard(test::g_errqueue_notifications);
  IoActionGuard actionsGuard(test::g_recvmsg_actions);
  test::PushErrQueueNotification(pair.fd(), {.first = 0, .last = 0});
  test::g_recvmsg_actions.setActions(pair.fd(), {IoAction{-1, error::kWouldBlock}});

  ZeroCopyState state = WithSendsInFlight(3);
  const auto completions = state.pollCompletions(pair.fd());
  EXPECT_EQ(completions.nbSends, 1U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(1));
  EXPECT_FALSE(state.completedBefore(2));
}

TEST(ZeroCopyPollTest, OutOfOrderNotificationsKeepEarlierSendsPending) {
  SocketPair pair;
  ErrQueueGuard notificationsGuard(test::g_errqueue_notifications);
  IoActionGuard actionsGuard(test::g_recvmsg_actions);
  test::PushErrQueueNotification(pair.fd(), {.first = 1, .last = 1});
  test::g_recvmsg_actions.setActions(pair.fd(), {IoAction{-1, error::kWouldBlock}});

  ZeroCopyState state = WithSendsInFlight(2);
  EXPECT_EQ(state.pollCompletions(pair.fd()).nbSends, 1U);
  // Send 0 may still be read by the kernel (its retransmission): its buffers must be kept.
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_FALSE(state.completedBefore(1));

  test::PushErrQueueNotification(pair.fd(), {.first = 0, .last = 0});
  EXPECT_EQ(state.pollCompletions(pair.fd()).nbSends, 1U);
  EXPECT_FALSE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(2));
}

TEST(ZeroCopyPollTest, CountsSendsCopiedByTheKernel) {
  SocketPair pair;
  ErrQueueGuard guard(test::g_errqueue_notifications);
  test::PushErrQueueNotification(pair.fd(), {.first = 0, .last = 1, .code = SO_EE_CODE_ZEROCOPY_COPIED});
  test::PushErrQueueNotification(pair.fd(), {.first = 2, .last = 2});

  ZeroCopyState state = WithSendsInFlight(3);
  const auto completions = state.pollCompletions(pair.fd());
  EXPECT_EQ(completions.nbSends, 3U);
  EXPECT_EQ(completions.nbCopiedSends, 2U);
  EXPECT_FALSE(state.pendingCompletions());
}

TEST(ZeroCopyPollTest, IgnoresOtherErrorQueueMessages) {
  SocketPair pair;
  ErrQueueGuard notificationsGuard(test::g_errqueue_notifications);
  IoActionGuard actionsGuard(test::g_recvmsg_actions);
  const int fd = pair.fd();
  // Not a zerocopy notification
  test::PushErrQueueNotification(fd, {.first = 0, .last = 0, .origin = SO_EE_ORIGIN_LOCAL});
  // An error, not a completion
  test::PushErrQueueNotification(fd, {.first = 0, .last = 0, .errnoValue = ECONNRESET});
  // Unknown control message types
  test::PushErrQueueNotification(fd, {.first = 0, .last = 0, .type = 0});
  test::PushErrQueueNotification(fd, {.first = 0, .last = 0, .level = SOL_IPV6, .type = 0});
  // No control message at all
  test::PushErrQueueNotification(fd, {.controlMessage = false});
  // The notification of an IPv6 socket
  test::PushErrQueueNotification(fd, {.first = 0, .last = 0, .level = SOL_IPV6, .type = IPV6_RECVERR});
  test::g_recvmsg_actions.setActions(fd, {IoAction{-1, error::kWouldBlock}});

  ZeroCopyState state = WithSendsInFlight(2);
  const auto completions = state.pollCompletions(fd);
  EXPECT_EQ(completions.nbSends, 1U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_TRUE(state.completedBefore(1));
  EXPECT_EQ(test::g_errqueue_notifications.size(fd), 0U);
}

TEST(ZeroCopyPollTest, StopsOnErrorQueueReadFailure) {
  SocketPair pair;
  IoActionGuard guard(test::g_recvmsg_actions);
  test::g_recvmsg_actions.setActions(pair.fd(), {IoAction{-1, EBADF}});

  ZeroCopyState state = WithSendsInFlight(1);
  EXPECT_EQ(state.pollCompletions(pair.fd()).nbSends, 0U);
  EXPECT_TRUE(state.pendingCompletions());
}

// ---------------------------------------------------------------------------
// Real kernel
// ---------------------------------------------------------------------------

// The server sets SO_ZEROCOPY on its listening socket only: the accepted sockets inherit it. Their sends to a loopback
// peer are reported copied, through notifications whose control message carries the address of the offender.
TEST(ZeroCopyKernelTest, AcceptedSocketInheritsZerocopyAndReportsLoopbackCopies) {
  BaseFd listener(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(listener);
  if (EnableZeroCopy(listener.fd()) != ZeroCopyEnableResult::Enabled) {
    GTEST_SKIP() << "SO_ZEROCOPY not supported by this kernel";
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(listener.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_EQ(::listen(listener.fd(), 1), 0);
  socklen_t addrLen = sizeof(addr);  // NOLINT(misc-include-cleaner)
  ASSERT_EQ(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&addr), &addrLen), 0);

  BaseFd client(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_EQ(::connect(client.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  BaseFd accepted(::accept(listener.fd(), nullptr, nullptr));
  ASSERT_TRUE(accepted);
  EXPECT_TRUE(IsZeroCopyEnabled(accepted.fd()));

  PlainTransport transport(accepted.fd());
  const std::string head = "HEAD";
  const std::string body(64UL * 1024, 'Z');
  const std::string tail = "TAIL";
  // A gather send: the empty buffer is skipped.
  const std::string_view buffers[]{head, {}, body, tail};
  const auto [bytesSent, status] = transport.writeZerocopy(buffers);
  ASSERT_EQ(status, ZerocopySendStatus::Sent);
  ASSERT_EQ(bytesSent, head.size() + body.size() + tail.size());
  EXPECT_EQ(transport.zerocopy().nextSeq(), 1U);

  std::string received(bytesSent, '\0');
  for (std::size_t pos = 0; pos < received.size();) {
    const auto ret = ::recv(client.fd(), received.data() + pos, received.size() - pos, 0);
    ASSERT_GT(ret, 0);
    pos += static_cast<std::size_t>(ret);
  }
  EXPECT_EQ(received, head + body + tail);

  ZerocopyCompletions completions;
  for (int attempt = 0; attempt < 200 && transport.zerocopy().pendingCompletions(); ++attempt) {
    const auto polled = transport.pollZerocopyCompletions();
    completions.nbSends += polled.nbSends;
    completions.nbCopiedSends += polled.nbCopiedSends;
    if (transport.zerocopy().pendingCompletions()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
  }
  EXPECT_FALSE(transport.zerocopy().pendingCompletions());
  EXPECT_EQ(completions.nbSends, 1U);
  // Data looped to a local socket is always copied.
  EXPECT_EQ(completions.nbCopiedSends, 1U);
}

#endif  // AERONET_LINUX

}  // namespace aeronet
