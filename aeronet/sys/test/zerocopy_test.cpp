#include "aeronet/zerocopy.hpp"

#include <gtest/gtest.h>
#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#define AERONET_WANT_READ_WRITE_OVERRIDES
#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/zerocopy-mode.hpp"

namespace aeronet {

namespace {

bool IsZeroCopyEnabled(int fd) noexcept {
  int optVal = 0;
  socklen_t optLen = sizeof(optVal);
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::getsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &optVal, &optLen) == -1) {
    return false;
  }
  return optVal != 0;
}

constexpr uint32_t kZeroCopyMinPayloadSize = 1024;  // Minimum size for zerocopy sends in tests

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

TEST(ZeroCopyTest, PollZerocopyCompletionsReturnsZeroWhenNoPending) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0UL);

  // Should return 0 when no completions pending
  EXPECT_EQ(PollZeroCopyCompletions(sv[0], state), 0U);
}

TEST(ZeroCopyTest, AllZerocopyCompletedLogic) {
  ZeroCopyState state(0UL);

  // Initially no pending completions
  EXPECT_FALSE(state.pendingCompletions());

  // With pending completions but same seqLo/seqHi
  state.seqLo = 5;
  state.seqHi = 5;
  EXPECT_FALSE(state.pendingCompletions());

  // With pending completions and different seq numbers
  state.seqHi = 10;
  EXPECT_TRUE(state.pendingCompletions());
}

#ifdef __linux__
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

#endif  // __linux__

// PollZeroCopyCompletions tests moved to zerocopy_completions_test.cpp

// Tests for PlainTransport zerocopy integration
TEST(PlainTransportZeroCopy, EnableZerocopyOnTransport) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  PlainTransport transport(sv[0], ZerocopyMode::Opportunistic, kZeroCopyMinPayloadSize);

  // Initially zerocopy should not be enabled
  EXPECT_FALSE(transport.isZerocopyEnabled());
  EXPECT_FALSE(transport.hasZerocopyPending());

#ifdef __linux__
  // No pending completions on new transport
  EXPECT_FALSE(transport.hasZerocopyPending());
  EXPECT_EQ(transport.pollZerocopyCompletions(), 0U);
#endif
}

TEST(PlainTransportZeroCopy, WriteStillWorksWithZerocopyEnabled) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  // Write should still work regardless
  const std::string testData = "Hello, zerocopy world!";
  auto result = transport.write(testData);
  EXPECT_EQ(result.bytesProcessed, testData.size());
  EXPECT_EQ(result.want, TransportHint::None);

  // Verify data was received
  std::string recvBuf(testData.size(), '\0');
  EXPECT_EQ(::recv(sv[1], recvBuf.data(), recvBuf.size(), 0), static_cast<ssize_t>(testData.size()));
  EXPECT_EQ(recvBuf, testData);
}

TEST(PlainTransportZeroCopy, LargeWriteWorksWithZerocopyEnabled) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  // Set socket buffer sizes to allow large writes
  int sndbuf = 256 * 1024;
  int rcvbuf = 256 * 1024;
  // NOLINTNEXTLINE(misc-include-cleaner)
  ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  // NOLINTNEXTLINE(misc-include-cleaner)
  ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  // Ensure the constructor's setsockopt(SO_ZEROCOPY, ...) is treated as successful
  test::PushSetsockoptAction({0, 0});
  uint32_t minBytesForZerocopy = 1024;  // Set low threshold for testing
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  // Create a payload larger than the zerocopy threshold
  const std::string largeData(minBytesForZerocopy + 1024, 'Z');
  auto result = transport.write(largeData);
  EXPECT_EQ(result.bytesProcessed, largeData.size());
  EXPECT_EQ(result.want, TransportHint::None);

  // Verify data was received
  std::string recvBuf(largeData.size(), '\0');
  std::size_t totalRecv = 0;
  while (totalRecv < largeData.size()) {
    const ssize_t rc = ::recv(sv[1], recvBuf.data() + totalRecv, recvBuf.size() - totalRecv, 0);
    ASSERT_GT(rc, 0) << "recv failed with errno=" << errno;
    totalRecv += static_cast<std::size_t>(rc);
  }
  EXPECT_EQ(recvBuf, largeData);
}

TEST(PlainTransportZeroCopy, TwoBufWriteStillWorksWithZerocopyEnabled) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  uint32_t minBytesForZerocopy = 1024;  // Set low threshold for testing
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  // Two-buffer write (scatter-gather) should still work
  const std::string head = "HEAD:";
  const std::string body = "BODY-DATA";
  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, head.size() + body.size());
  EXPECT_EQ(result.want, TransportHint::None);

  // Verify data was received correctly
  std::string recvBuf(head.size() + body.size(), '\0');
  EXPECT_EQ(::recv(sv[1], recvBuf.data(), recvBuf.size(), 0), static_cast<ssize_t>(recvBuf.size()));
  EXPECT_EQ(recvBuf, head + body);
}

TEST(PlainTransportZeroCopy, DisableZerocopyWorks) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  uint32_t minBytesForZerocopy = 1024;  // Set low threshold for testing
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  // then disable zerocopy
  transport.disableZerocopy();

  EXPECT_FALSE(transport.isZerocopyEnabled());
}

#ifdef __linux__

// Tests for transport zerocopy path using mocked sendmsg

TEST(PlainTransportZeroCopy, ZerocopySendSuccessPathWithMockedSendmsg) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  // Set up large socket buffers
  int sndbuf = 256 * 1024;
  int rcvbuf = 256 * 1024;
  ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  // Create large payload to trigger zerocopy path
  const std::size_t payloadSize = kZeroCopyMinPayloadSize + 1024;
  const std::string largeData(payloadSize, 'X');

  // Mock sendmsg to return full payload sent (simulating successful zerocopy)
  test::SetSendmsgActions(sv[0], {IoAction{static_cast<ssize_t>(payloadSize), 0}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);

  // Verify zerocopy was marked as pending (since send succeeded)
  EXPECT_TRUE(transport.hasZerocopyPending());
}

TEST(PlainTransportZeroCopy, ZerocopySendEAGAINReturnsWriteReady) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::size_t payloadSize = kZeroCopyMinPayloadSize + 1024;
  const std::string largeData(payloadSize, 'Y');

  // Mock sendmsg to return EAGAIN (kernel buffer full)
  test::SetSendmsgActions(sv[0], {IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::WriteReady);
}

TEST(PlainTransportZeroCopy, ZerocopySendEINTRFallsBackToRegularWrite) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  // Set up large socket buffers
  int sndbuf = 256 * 1024;
  int rcvbuf = 256 * 1024;
  ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::size_t payloadSize = kZeroCopyMinPayloadSize + 1024;
  const std::string largeData(payloadSize, 'Z');

  // Mock sendmsg to return EINTR (signal interrupted), which should fall through to regular write
  test::SetSendmsgActions(sv[0], {IoAction{-1, EINTR}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  // Should succeed via regular write fallback
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);

  // Read back the data to verify it was sent via regular write
  std::string recvBuf(payloadSize, '\0');
  std::size_t totalRecv = 0;
  while (totalRecv < payloadSize) {
    const ssize_t rc = ::recv(sv[1], recvBuf.data() + totalRecv, recvBuf.size() - totalRecv, 0);
    ASSERT_GT(rc, 0) << "recv failed with errno=" << errno;
    totalRecv += static_cast<std::size_t>(rc);
  }
  EXPECT_EQ(recvBuf, largeData);
}

TEST(PlainTransportZeroCopy, ZerocopySendOtherErrorReturnsError) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::size_t payloadSize = kZeroCopyMinPayloadSize + 1024;
  const std::string largeData(payloadSize, 'E');

  // Mock sendmsg to return a fatal error (EPIPE - broken pipe)
  test::SetSendmsgActions(sv[0], {IoAction{-1, EPIPE}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

TEST(PlainTransportZeroCopy, ZerocopySendPartialWriteReturnsPartialBytes) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::size_t payloadSize = kZeroCopyMinPayloadSize + 1024;
  const std::string largeData(payloadSize, 'P');

  // Mock sendmsg to return partial write (only half the data)
  const ssize_t partialBytes = static_cast<ssize_t>(payloadSize / 2);
  test::SetSendmsgActions(sv[0], {IoAction{partialBytes, 0}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  EXPECT_EQ(result.bytesProcessed, static_cast<std::size_t>(partialBytes));
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_TRUE(transport.hasZerocopyPending());
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufSuccessPathWithMockedSendmsg) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::string head(4, 'H');
  const std::string body(kZeroCopyMinPayloadSize + 64, 'B');
  const std::size_t payloadSize = head.size() + body.size();

  // Mock sendmsg to return full payload sent (simulating successful zerocopy)
  test::SetSendmsgActions(sv[0], {IoAction{static_cast<ssize_t>(payloadSize), 0}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_TRUE(transport.hasZerocopyPending());
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufEAGAINReturnsWriteReady) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::string head(4, 'H');
  const std::string body(kZeroCopyMinPayloadSize + 64, 'B');

  // Mock sendmsg to return EAGAIN (kernel buffer full)
  test::SetSendmsgActions(sv[0], {IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::WriteReady);
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufEINTRFallsBackToWritev) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::string head(4, 'H');
  const std::string body(kZeroCopyMinPayloadSize + 64, 'B');
  const std::size_t payloadSize = head.size() + body.size();

  // Mock sendmsg to return EINTR (signal interrupted), which should fall through to regular writev
  test::SetSendmsgActions(sv[0], {IoAction{-1, EINTR}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);

  // Read back the data to verify it was sent via regular writev
  std::string recvBuf(payloadSize, '\0');
  std::size_t totalRecv = 0;
  while (totalRecv < payloadSize) {
    const ssize_t rc = ::recv(sv[1], recvBuf.data() + totalRecv, recvBuf.size() - totalRecv, 0);
    ASSERT_GT(rc, 0) << "recv failed with errno=" << errno;
    totalRecv += static_cast<std::size_t>(rc);
  }
  EXPECT_EQ(recvBuf, head + body);
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufOtherErrorReturnsError) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, kZeroCopyMinPayloadSize);

  const std::string head(4, 'H');
  const std::string body(kZeroCopyMinPayloadSize + 64, 'B');

  // Mock sendmsg to return a fatal error (EPIPE - broken pipe)
  test::SetSendmsgActions(sv[0], {IoAction{-1, EPIPE}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, 0U);
  EXPECT_EQ(result.want, TransportHint::Error);
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufPartialWriteReturnsPartialBytes) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  const uint32_t minBytesForZerocopy = 1024;
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  const std::string head(4, 'H');
  const std::string body(minBytesForZerocopy + 64, 'B');
  const std::size_t payloadSize = head.size() + body.size();

  // Mock sendmsg to return partial write (only half the data)
  const ssize_t partialBytes = static_cast<ssize_t>(payloadSize / 2);
  test::SetSendmsgActions(sv[0], {IoAction{partialBytes, 0}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, static_cast<std::size_t>(partialBytes));
  EXPECT_EQ(result.want, TransportHint::None);
  EXPECT_TRUE(transport.hasZerocopyPending());
}

TEST(PlainTransportZeroCopy, ConstructorWarnsWhenEnableFails) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  for (auto zerocopyMode : {ZerocopyMode::Enabled, ZerocopyMode::Opportunistic}) {
    // Simulate setsockopt(SO_ZEROCOPY) failing (NotSupported or Error)
    test::PushSetsockoptAction({-1, ENOPROTOOPT});

    // Construct transport requesting zerocopy enabled; constructor should attempt to enable
    const uint32_t minBytesForZerocopy = 1024;
    PlainTransport transport(sv[0], zerocopyMode, minBytesForZerocopy);

    // Zerocopy should not be enabled due to the simulated failure
    EXPECT_FALSE(transport.isZerocopyEnabled());
  }
}

TEST(PollZeroCopyCompletionsTest, HandlesEagainAndKeepsPending) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 0;
  state.seqHi = 10;

  test::g_recvmsg_actions.setActions(sv[0], {IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_recvmsg_actions);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
}

TEST(PollZeroCopyCompletionsTest, HandlesOtherErrnoAndKeepsPending) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 1;
  state.seqHi = 5;

  test::g_recvmsg_actions.setActions(sv[0], {IoAction{-1, EINTR}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_recvmsg_actions);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
}

TEST(PollZeroCopyCompletionsTest, ParsesZerocopyCompletion) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 0;
  state.seqHi = 43;

  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_recvmsg_actions);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 1U);
  EXPECT_EQ(state.seqLo, 43U);
  EXPECT_FALSE(state.pendingCompletions());
}

TEST(PollZeroCopyCompletionsTest, ParsesIpv6ZerocopyCompletion) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 0;
  state.seqHi = 43;

  // mode: first value = 6 -> IPv6; second value = 1 -> keep zerocopy origin
  test::g_recvmsg_modes.setActions(sv[0], {6, 1});
  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guardA(test::g_recvmsg_actions);
  test::QueueResetGuard<test::KeyedActionQueue<int, int>> guardB(test::g_recvmsg_modes);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 1U);
  EXPECT_EQ(state.seqLo, 43U);
  EXPECT_FALSE(state.pendingCompletions());
}

TEST(PollZeroCopyCompletionsTest, IgnoresNonZerocopyOrigin) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 7;
  state.seqHi = 10;

  // mode: single value 2 -> non-zerocopy origin
  test::g_recvmsg_modes.setActions(sv[0], {2});
  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guardA(test::g_recvmsg_actions);
  test::QueueResetGuard<test::KeyedActionQueue<int, int>> guardB(test::g_recvmsg_modes);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_EQ(state.seqLo, 7U);
}

TEST(PollZeroCopyCompletionsTest, IgnoresUnknownControlMessage) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 2;
  state.seqHi = 5;

  // mode: 7 => set cmsg_type to non-IP_RECVERR (causes continue path)
  test::g_recvmsg_modes.setActions(sv[0], {7, 1});
  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guardA(test::g_recvmsg_actions);
  test::QueueResetGuard<test::KeyedActionQueue<int, int>> guardB(test::g_recvmsg_modes);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_EQ(state.seqLo, 2U);
}

TEST(PollZeroCopyCompletionsTest, SkipsWhenNoControlMessage) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 4;
  state.seqHi = 10;

  // mode: 8 => do not populate control message (CMSG_FIRSTHDR should be nullptr)
  test::g_recvmsg_modes.setActions(sv[0], {8});
  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guardA(test::g_recvmsg_actions);
  test::QueueResetGuard<test::KeyedActionQueue<int, int>> guardB(test::g_recvmsg_modes);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_EQ(state.seqLo, 4U);
}

TEST(PollZeroCopyCompletionsTest, IgnoresIpv6WithWrongType) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  ZeroCopyState state(0U);
  state.seqLo = 9;
  state.seqHi = 20;

  // mode: 9 => SOL_IPV6 but cmsg_type != IPV6_RECVERR (should continue)
  test::g_recvmsg_modes.setActions(sv[0], {9});
  test::g_recvmsg_actions.setActions(sv[0], {IoAction{0, 0}, IoAction{-1, EAGAIN}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guardA(test::g_recvmsg_actions);
  test::QueueResetGuard<test::KeyedActionQueue<int, int>> guardB(test::g_recvmsg_modes);

  const auto comps = PollZeroCopyCompletions(sv[0], state);
  EXPECT_EQ(comps, 0U);
  EXPECT_TRUE(state.pendingCompletions());
  EXPECT_EQ(state.seqLo, 9U);
}

TEST(PlainTransportZeroCopy, ZerocopySendENOBUFSFallsBackToRegularWrite) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  // Set up large socket buffers
  int sndbuf = 256 * 1024;
  int rcvbuf = 256 * 1024;
  ::setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  ::setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  test::PushSetsockoptAction({0, 0});
  const uint32_t minBytesForZerocopy = 1024;
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  const std::size_t payloadSize = minBytesForZerocopy + 1024;
  const std::string largeData(payloadSize, 'N');

  // Mock sendmsg to return ENOBUFS (kernel cannot pin more pages for zerocopy).
  // This is a transient condition — the transport must fall through to regular write.
  test::SetSendmsgActions(sv[0], {IoAction{-1, ENOBUFS}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(largeData);
  // Should succeed via regular write fallback, NOT return Error.
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);

  // Read back the data to verify it was sent via regular write
  std::string recvBuf(payloadSize, '\0');
  std::size_t totalRecv = 0;
  while (totalRecv < payloadSize) {
    const ssize_t rc = ::recv(sv[1], recvBuf.data() + totalRecv, recvBuf.size() - totalRecv, 0);
    ASSERT_GT(rc, 0) << "recv failed with errno=" << errno;
    totalRecv += static_cast<std::size_t>(rc);
  }
  EXPECT_EQ(recvBuf, largeData);
}

TEST(PlainTransportZeroCopy, ZerocopySendTwoBufENOBUFSFallsBackToWritev) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  BaseFd guard0(sv[0]);
  BaseFd guard1(sv[1]);

  test::PushSetsockoptAction({0, 0});
  const uint32_t minBytesForZerocopy = 1024;
  PlainTransport transport(sv[0], ZerocopyMode::Enabled, minBytesForZerocopy);

  const std::string head(4, 'H');
  const std::string body(minBytesForZerocopy + 64, 'B');
  const std::size_t payloadSize = head.size() + body.size();

  // Mock sendmsg to return ENOBUFS (kernel cannot pin more pages for zerocopy).
  // This is a transient condition — the transport must fall through to regular writev.
  test::SetSendmsgActions(sv[0], {IoAction{-1, ENOBUFS}});
  test::QueueResetGuard<test::KeyedActionQueue<int, IoAction>> guard(test::g_sendmsg_actions);

  auto result = transport.write(head, body);
  EXPECT_EQ(result.bytesProcessed, payloadSize);
  EXPECT_EQ(result.want, TransportHint::None);

  // Read back the data to verify it was sent via regular writev
  std::string recvBuf(payloadSize, '\0');
  std::size_t totalRecv = 0;
  while (totalRecv < payloadSize) {
    const ssize_t rc = ::recv(sv[1], recvBuf.data() + totalRecv, recvBuf.size() - totalRecv, 0);
    ASSERT_GT(rc, 0) << "recv failed with errno=" << errno;
    totalRecv += static_cast<std::size_t>(rc);
  }
  EXPECT_EQ(recvBuf, head + body);
}

#endif  // __linux__

}  // namespace aeronet
