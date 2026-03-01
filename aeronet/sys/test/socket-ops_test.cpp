#include "aeronet/socket-ops.hpp"

#include <gtest/gtest.h>
#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>

#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/base-fd.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet {

// Helper to create a real socket for testing
class SocketOpsTest : public ::testing::Test {
 protected:
  static int CreateTestSocket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0) << "Failed to create test socket";
    return fd;
  }

  static void CloseSocket(int fd) {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

TEST_F(SocketOpsTest, SetNonBlockingSucceeds) {
  int fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetNonBlocking(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetNonBlockingFailsOnBadFd) { EXPECT_FALSE(SetNonBlocking(-1)); }

TEST_F(SocketOpsTest, SetCloseOnExecSucceeds) {
  int fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetCloseOnExec(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetCloseOnExecFailsOnBadFd) { EXPECT_FALSE(SetCloseOnExec(-1)); }

TEST_F(SocketOpsTest, SetTcpNoDelaySucceeds) {
  int fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  EXPECT_TRUE(SetTcpNoDelay(fd));
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, SetTcpNoDelayFailsOnBadFd) { EXPECT_FALSE(SetTcpNoDelay(-1)); }

TEST_F(SocketOpsTest, GetSocketErrorReturnsZeroForGoodSocket) {
  int fd = CreateTestSocket();
  ASSERT_GE(fd, 0);
  // A freshly created socket should not have an error
  int err = GetSocketError(fd);
  EXPECT_EQ(0, err);
  CloseSocket(fd);
}

TEST_F(SocketOpsTest, GetSocketErrorReturnsErrnoOnGetsockoptFailure) {
  // Use an invalid fd to cause getsockopt to fail and return errno
  test::PushSocketAction({-1, EBADF});         // one socket() override, then fallback to real socket
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);  // uses overridden socket() which returns -1
  if (fd == -1) {
    // Override worked; now test GetSocketError with bad fd
    int err = GetSocketError(-1);
    EXPECT_EQ(EBADF, err);  // Should return errno from failed getsockopt
  } else {
    CloseSocket(fd);
    GTEST_SKIP() << "Socket override did not trigger";
  }
}

TEST_F(SocketOpsTest, GetLocalAddressSucceeds) {
  int fd = CreateTestSocket();
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
  int fd = CreateTestSocket();
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
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  auto* in = reinterpret_cast<sockaddr_in*>(&addr);
  addr.ss_family = AF_INET;
  in->sin_addr.s_addr = htonl(0x7FFFFFFFU);  // 127.255.255.255
  EXPECT_TRUE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsNonLoopbackIPv4) {
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  auto* in = reinterpret_cast<sockaddr_in*>(&addr);
  addr.ss_family = AF_INET;
  in->sin_addr.s_addr = htonl(0x08080808U);  // 8.8.8.8
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackDetectsIPv6Loopback) {
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
  addr.ss_family = AF_INET6;
  in6->sin6_addr = in6addr_loopback;  // ::1
  EXPECT_TRUE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsNonLoopbackIPv6) {
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  auto* in6 = reinterpret_cast<sockaddr_in6*>(&addr);
  addr.ss_family = AF_INET6;
  // Use a non-loopback IPv6 address (all zeros is not loopback)
  std::memset(&in6->sin6_addr, 0, sizeof(in6->sin6_addr));
  in6->sin6_addr.s6_addr[0] = 0x20;  // 2000::
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, IsLoopbackRejectsUnsupportedAddressFamily) {
  sockaddr_storage addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.ss_family = AF_UNIX;
  EXPECT_FALSE(IsLoopback(addr));
}

TEST_F(SocketOpsTest, SafeSendSucceeds) {
  // Create a socket pair for IPC
  int sockets[2];
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
  int sockets[2];
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

}  // namespace aeronet
