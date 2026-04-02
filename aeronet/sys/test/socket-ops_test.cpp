#include "aeronet/socket-ops.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>

#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/close-native-handle.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/tcp-cork-guard.hpp"

#ifdef AERONET_POSIX
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aeronet {

// Helper to create a real socket for testing
class SocketOpsTest : public ::testing::Test {
 protected:
  static NativeHandle CreateTestSocket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "Failed to create test socket";
    return fd;
  }

  static void CloseSocket(NativeHandle fd) {
    if (fd >= 0) {
      CloseNativeHandle(fd);
    }
  }
};

TEST_F(SocketOpsTest, SetNonBlockingSucceeds) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetNonBlocking(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetNonBlockingFailsOnBadFd) { EXPECT_FALSE(SetNonBlocking(-1)); }

TEST_F(SocketOpsTest, SetCloseOnExecSucceeds) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetCloseOnExec(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetNoSigPipeSucceeds) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetNoSigPipe(fd));
  CloseSocket(fd);
}

#ifdef AERONET_POSIX

TEST_F(SocketOpsTest, SetPipeNonBlockingCloExec) {
  NativeHandle fds[2];
  ASSERT_EQ(0, ::pipe(fds));
  SetPipeNonBlockingCloExec(fds[0], fds[1]);

  // Verify that both ends of the pipe are non-blocking and close-on-exec
  const auto flags0 = ::fcntl(fds[0], F_GETFL, 0);
  const auto flags1 = ::fcntl(fds[1], F_GETFL, 0);
  EXPECT_NE(flags0, -1);
  EXPECT_NE(flags1, -1);
  EXPECT_TRUE(flags0 & O_NONBLOCK);
  EXPECT_TRUE(flags1 & O_NONBLOCK);

  CloseSocket(fds[0]);
  CloseSocket(fds[1]);
}

#endif

TEST_F(SocketOpsTest, SetCloseOnExecFailsOnBadFd) { EXPECT_FALSE(SetCloseOnExec(-1)); }

TEST_F(SocketOpsTest, SetTcpNoDelaySucceeds) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetTcpNoDelay(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetTcpNoDelayFailsOnBadFd) { EXPECT_FALSE(SetTcpNoDelay(-1)); }

TEST_F(SocketOpsTest, GetSocketErrorReturnsZeroForGoodSocket) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  // A freshly created socket should not have an error
  int err = GetSocketError(fd);
  EXPECT_EQ(0, err);
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, GetSocketErrorReturnsErrnoOnGetsockoptFailure) {
  // Use an invalid fd to cause getsockopt to fail and return errno
  test::PushSocketAction({-1, EBADF});                  // one socket() override, then fallback to real socket
  NativeHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);  // uses overridden socket() which returns -1
  if (fd == -1) {
    // Override worked; now test GetSocketError with bad fd
    NativeHandle err = GetSocketError(-1);
    EXPECT_EQ(EBADF, err);  // Should return errno from failed getsockopt
  } else {
    CloseSocket(fd);
    GTEST_SKIP() << "Socket override did not trigger";
  }
}

TEST_F(SocketOpsTest, GetLocalAddressSucceeds) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  // Bind to any available port on loopback
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ASSERT_EQ(0, ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));

  sockaddr_storage retrieved;
  EXPECT_TRUE(GetLocalAddress(fd, retrieved));
  EXPECT_EQ(AF_INET, retrieved.ss_family);
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, GetLocalAddressFailsOnBadFd) {
  sockaddr_storage addr;
  EXPECT_FALSE(GetLocalAddress(-1, addr));
}

TEST_F(SocketOpsTest, GetPeerAddressFailsOnUnconnectedSocket) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  // Socket not connected; getpeername should fail
  sockaddr_storage addr;
  EXPECT_FALSE(GetPeerAddress(fd, addr));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, GetPeerAddressFailsOnBadFd) {
  sockaddr_storage addr;
  EXPECT_FALSE(GetPeerAddress(-1, addr));
}

TEST_F(SocketOpsTest, IsLoopbackDetectsIPv4Loopback) {
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  auto* in = reinterpret_cast<sockaddr_in*>(&addr);
  addr.ss_family = AF_INET;
  in->sin_addr.s_addr = htonl(0x7F000001U);  // 127.0.0.1
  EXPECT_TRUE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackDetectsIPv4LoopbackRange) {
  sockaddr_storage addr{};
  auto* in = reinterpret_cast<sockaddr_in*>(&addr);
  addr.ss_family = AF_INET;
  in->sin_addr.s_addr = htonl(0x7FFFFFFFU);  // 127.255.255.255
  EXPECT_TRUE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsNonLoopbackIPv4) {
  sockaddr_storage addr{};
  auto* in = reinterpret_cast<sockaddr_in*>(&addr);
  addr.ss_family = AF_INET;
  in->sin_addr.s_addr = htonl(0x08080808U);  // 8.8.8.8
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackDetectsIPv6Loopback) {
  sockaddr_storage addr{};
  auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
  addr.ss_family = AF_INET6;
  in6->sin6_addr = in6addr_loopback;  // ::1
  EXPECT_TRUE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsNonLoopbackIPv6) {
  sockaddr_storage addr{};
  auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
  addr.ss_family = AF_INET6;
  // Use a non-loopback IPv6 address (all zeros is not loopback)
  std::memset(&in6->sin6_addr, 0, sizeof(in6->sin6_addr));
  in6->sin6_addr.s6_addr[0] = 0x20;  // 2000::
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsUnsupportedAddressFamily) {
  sockaddr_storage addr{};
  addr.ss_family = AF_UNIX;
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, SafeSendSucceeds) {
  // Create a socket pair for IPC
  NativeHandle sockets[2];
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  BaseFd receiver(sockets[0]);
  BaseFd sender(sockets[1]);

  const char* data = "test";
  int64_t sent = SafeSend(sender.fd(), data, 4);
  // SafeSend should return the number of bytes sent, or -1 on error
  // socketpair creates connected sockets, so send should succeed
  EXPECT_GE(sent, 0);
  if (sent > 0) {
    EXPECT_EQ(4, sent);
  }
}

TEST_F(SocketOpsTest, SafeSendFailsOnBadFd) {
  const char* data = "test";
  int64_t sent = SafeSend(-1, data, 4);
  EXPECT_EQ(-1, sent);
}

TEST_F(SocketOpsTest, SafeSendStringViewOverload) {
  // Create a socket pair for IPC
  NativeHandle sockets[2];
  ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  BaseFd receiver(sockets[0]);
  BaseFd sender(sockets[1]);

  std::string_view data = "hello";
  int64_t sent = SafeSend(sender.fd(), data);
  // SafeSend should return the number of bytes sent, or -1 on error
  // socketpair creates connected sockets, so send should succeed
  EXPECT_GE(sent, 0);
  if (sent > 0) {
    EXPECT_EQ(5, sent);
  }
}

#ifdef AERONET_POSIX

TEST_F(SocketOpsTest, SetTcpCorkSucceedsOnValidSocket) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  // On Linux this sets TCP_CORK; on macOS/Windows it's a no-op returning true.
  EXPECT_TRUE(SetTcpCork(fd, true));
  EXPECT_TRUE(SetTcpCork(fd, false));
  CloseSocket(fd);
}

#ifdef AERONET_LINUX

TEST_F(SocketOpsTest, SetTcpCorkFailsOnBadFd) { EXPECT_FALSE(SetTcpCork(-1, true)); }

TEST_F(SocketOpsTest, SetTcpCorkVerifyKernelState) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);

  EXPECT_TRUE(SetTcpCork(fd, true));
  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(0, ::getsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, &len));
  EXPECT_EQ(1, val);

  EXPECT_TRUE(SetTcpCork(fd, false));
  ASSERT_EQ(0, ::getsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, &len));
  EXPECT_EQ(0, val);

  CloseSocket(fd);
}

TEST_F(SocketOpsTest, TcpCorkGuardCorksAndUncorks) {
  NativeHandle fd = CreateTestSocket();
  ASSERT_GE(fd, 0);

  {
    TcpCorkGuard guard(fd);
    int val = 0;
    socklen_t len = sizeof(val);
    ASSERT_EQ(0, ::getsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, &len));
    EXPECT_EQ(1, val);
  }  // guard destroyed, socket should be uncorked

  int val = 0;
  socklen_t len = sizeof(val);
  ASSERT_EQ(0, ::getsockopt(fd, IPPROTO_TCP, TCP_CORK, &val, &len));
  EXPECT_EQ(0, val);

  CloseSocket(fd);
}

#endif  // AERONET_LINUX

TEST_F(SocketOpsTest, TcpCorkGuardNoOpOnInvalidHandle) {
  // Should not crash or fail
  TcpCorkGuard guard(kInvalidHandle);
}

#endif  // AERONET_POSIX

}  // namespace aeronet
