#include "aeronet/unix-socket.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/sys-test-support.hpp"

namespace aeronet {

namespace fs = std::filesystem;

// Helper to create a unique temporary socket path
class UnixSocketTest : public ::testing::Test {
 protected:
  static std::string GetTempSocketPath(const std::string& suffix = "") {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static thread_local std::uniform_int_distribution<unsigned long long> dist;
    std::string path = "/tmp/aeronet-unix-socket-test-" + std::to_string(::getpid()) + "-" + std::to_string(dist(rng)) +
                       suffix + ".sock";
    return path;
  }

  static void CleanupSocket(const std::string& path) {
    if (fs::exists(path)) {
      fs::remove(path);
    }
  }
};

TEST_F(UnixSocketTest, InvalidUnixSocketTypeThrows) {
  EXPECT_THROW(
      UnixSocket(static_cast<UnixSocket::Type>(std::numeric_limits<std::underlying_type_t<UnixSocket::Type>>::max())),
      std::invalid_argument);
}

TEST_F(UnixSocketTest, ConstructorDatagramSucceeds) {
  EXPECT_NO_THROW(UnixSocket sock(UnixSocket::Type::Datagram));
  UnixSocket sock(UnixSocket::Type::Datagram);
  EXPECT_GE(sock.fd(), 0);
}

TEST_F(UnixSocketTest, ConstructorStreamSucceeds) {
  EXPECT_NO_THROW(UnixSocket sock(UnixSocket::Type::Stream));
  UnixSocket sock(UnixSocket::Type::Stream);
  EXPECT_GE(sock.fd(), 0);
}

TEST_F(UnixSocketTest, DatagramSocketIsNonBlocking) {
  UnixSocket sock(UnixSocket::Type::Datagram);
  EXPECT_GE(sock.fd(), 0);
  // Verify socket is non-blocking by attempting to recv (should return EAGAIN/EWOULDBLOCK)
  char buf[1];
  ssize_t ret = ::recv(sock.fd(), buf, 1, MSG_DONTWAIT);
  // Non-blocking socket with no data should return -1 with EAGAIN or EWOULDBLOCK
  EXPECT_EQ(-1, ret);
  int err = errno;
  EXPECT_TRUE(err == EAGAIN || err == EWOULDBLOCK);
}

TEST_F(UnixSocketTest, StreamSocketIsNonBlocking) {
  UnixSocket sock(UnixSocket::Type::Stream);
  EXPECT_GE(sock.fd(), 0);
  // Verify socket is non-blocking by checking the flag directly
  int flags = ::fcntl(sock.fd(), F_GETFL, 0);
  EXPECT_GE(flags, 0);
  EXPECT_NE(0, flags & O_NONBLOCK);
}

TEST_F(UnixSocketTest, DatagramSocketIsCloexec) {
  UnixSocket sock(UnixSocket::Type::Datagram);
  EXPECT_GE(sock.fd(), 0);
  // Verify close-on-exec flag is set
  int flags = ::fcntl(sock.fd(), F_GETFD, 0);
  EXPECT_GE(flags, 0);
  EXPECT_NE(0, flags & FD_CLOEXEC);
}

TEST_F(UnixSocketTest, StreamSocketIsCloexec) {
  UnixSocket sock(UnixSocket::Type::Stream);
  EXPECT_GE(sock.fd(), 0);
  // Verify close-on-exec flag is set
  int flags = ::fcntl(sock.fd(), F_GETFD, 0);
  EXPECT_GE(flags, 0);
  EXPECT_NE(0, flags & FD_CLOEXEC);
}

TEST_F(UnixSocketTest, ConnectDatagramSucceeds) {
  std::string socketPath = GetTempSocketPath("-dgram-server");
  CleanupSocket(socketPath);

  // Create a server socket and bind it
  UnixSocket server(UnixSocket::Type::Datagram);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));

  // Client connects
  UnixSocket client(UnixSocket::Type::Datagram);
  EXPECT_NO_THROW(client.connect(socketPath));

  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, ConnectStreamSucceeds) {
  std::string socketPath = GetTempSocketPath("-stream-server");
  CleanupSocket(socketPath);

  // Create a server socket, bind, and listen
  UnixSocket server(UnixSocket::Type::Stream);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));
  ASSERT_EQ(0, ::listen(server.fd(), 1));

  // Client connects
  UnixSocket client(UnixSocket::Type::Stream);
  EXPECT_NO_THROW(client.connect(socketPath));

  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, ConnectFailsToNonexistentSocket) {
  std::string nonexistentPath = GetTempSocketPath("-nonexistent");
  UnixSocket sock(UnixSocket::Type::Datagram);
  // Connect will call ::connect which should fail with ENOENT and return -1
  int result = sock.connect(nonexistentPath);
  EXPECT_EQ(-1, result);
  EXPECT_EQ(ENOENT, errno);
}

TEST_F(UnixSocketTest, SendDatagramSucceeds) {
  std::string socketPath = GetTempSocketPath("-dgram-send");
  CleanupSocket(socketPath);

  // Create a server socket
  UnixSocket server(UnixSocket::Type::Datagram);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));

  // Client connects and sends
  UnixSocket client(UnixSocket::Type::Datagram);
  client.connect(socketPath);
  const char* data = "hello";
  ssize_t sent = client.send(data, 5);
  // Non-blocking send in datagram mode should succeed immediately
  EXPECT_EQ(5, sent);

  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, SendDatagramZeroBytes) {
  std::string socketPath = GetTempSocketPath("-dgram-send-empty");
  CleanupSocket(socketPath);

  // Create a server socket
  UnixSocket server(UnixSocket::Type::Datagram);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));

  // Client connects and sends zero bytes
  UnixSocket client(UnixSocket::Type::Datagram);
  client.connect(socketPath);
  ssize_t sent = client.send(nullptr, 0);
  EXPECT_EQ(0, sent);

  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, SendStreamSucceeds) {
  std::string socketPath = GetTempSocketPath("-stream-send");
  CleanupSocket(socketPath);

  // Server socket
  UnixSocket server(UnixSocket::Type::Stream);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));
  ASSERT_EQ(0, ::listen(server.fd(), 1));

  // Background thread to accept connection
  std::thread acceptThread([&server]() {
    struct sockaddr_un clientAddr{};
    socklen_t len = sizeof(clientAddr);
    int clientFd = ::accept(server.fd(), reinterpret_cast<sockaddr*>(&clientAddr), &len);
    if (clientFd >= 0) {
      ::close(clientFd);
    }
  });

  // Client connects and sends
  UnixSocket client(UnixSocket::Type::Stream);
  client.connect(socketPath);
  const char* data = "stream";
  ssize_t sent = client.send(data, 6);
  EXPECT_EQ(6, sent);

  acceptThread.join();
  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, SendToClosedSocketFails) {
  std::string socketPath = GetTempSocketPath("-closed");
  CleanupSocket(socketPath);

  // Create server and accept once
  UnixSocket server(UnixSocket::Type::Stream);
  struct sockaddr_un serverAddr{};
  serverAddr.sun_family = AF_UNIX;
  std::strncpy(serverAddr.sun_path, socketPath.c_str(), sizeof(serverAddr.sun_path) - 1);
  ASSERT_EQ(0, ::bind(server.fd(), reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)));
  ASSERT_EQ(0, ::listen(server.fd(), 1));

  UnixSocket client(UnixSocket::Type::Stream);
  client.connect(socketPath);

  // Close server immediately
  ::close(server.fd());

  // Send should fail or return EPIPE
  const char* data = "test";
  ssize_t sent = client.send(data, 4);
  // Either send fails (-1) or returns less than requested due to non-blocking
  // In any case, we're testing that it handles the closed socket gracefully
  EXPECT_LT(sent, 4);

  CleanupSocket(socketPath);
}

TEST_F(UnixSocketTest, FdAccessor) {
  UnixSocket sock(UnixSocket::Type::Datagram);
  int fd = sock.fd();
  EXPECT_GE(fd, 0);
  // Verify we can use the fd directly and it's valid
  int flags = ::fcntl(fd, F_GETFD, 0);
  EXPECT_GE(flags, 0);
}

TEST_F(UnixSocketTest, MaxPathConstant) {
  // Verify the constant is defined and reasonable
  EXPECT_GT(kUnixSocketMaxPath, 0);
  // Should be at least 108 (Linux) or 104 (macOS)
  EXPECT_GE(kUnixSocketMaxPath, 100);
}

}  // namespace aeronet
