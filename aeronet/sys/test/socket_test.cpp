#include "aeronet/socket.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>  // NOLINT(misc-include-cleaner) used by socket class

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

#define AERONET_WANT_SOCKET_OVERRIDES

#include "aeronet/sys-test-support.hpp"

namespace aeronet {

TEST(Socket, NominalExplicitClose) {
  Socket sock(Socket::Type::Stream);
  EXPECT_TRUE(sock);
  EXPECT_GE(sock.fd(), 0);
  sock.close();
  EXPECT_FALSE(sock);
}

TEST(Socket, Invalid) {
  Socket::Type invalidType;
  std::memset(&invalidType, 255, sizeof(Socket::Type));
  EXPECT_THROW(Socket{invalidType}, std::invalid_argument);
}

TEST(Socket, TryBindReturnsFalseWhenPortIsTaken) {
  Socket first(Socket::Type::Stream);
  uint16_t port = 0;
  first.bindAndListen(false, port);
  Socket second(Socket::Type::Stream);
  EXPECT_FALSE(second.tryBind(false, port));
  second.close();
  first.close();
}

TEST(Socket, BindAndListenUpdatesPort) {
  Socket sock(Socket::Type::Stream);
  uint16_t port = 0;
  EXPECT_NO_THROW(sock.bindAndListen(false, port));
  EXPECT_NE(0, port);
  sock.close();
}

TEST(Socket, BindAndListenThrowsWhenPortInUse) {
  Socket first(Socket::Type::Stream);
  uint16_t port = 0;
  first.bindAndListen(false, port);
  Socket second(Socket::Type::Stream);
  EXPECT_THROW(second.bindAndListen(false, port), std::system_error);
  second.close();
  first.close();
}

TEST(Socket, ConstructorThrowsWhenSocketCreationFails) {
  test::PushSocketAction({-1, EMFILE});  // EMFILE: too many open files
  EXPECT_THROW(Socket{Socket::Type::Stream}, std::system_error);
}

TEST(Socket, TryBindThrowsWhenSetsockoptReuseAddrFails) {
  Socket sock(Socket::Type::Stream);
  test::PushSetsockoptAction({-1, EACCES});  // EACCES: permission denied
  EXPECT_THROW((void)sock.tryBind(false, 0), std::system_error);
  sock.close();
}

TEST(Socket, TryBindThrowsWhenSetsockoptReusePortFails) {
  Socket sock(Socket::Type::Stream);
  // First setsockopt succeeds (SO_REUSEADDR), second one (SO_REUSEPORT) fails
  test::PushSetsockoptAction({0, 0});        // SO_REUSEADDR succeeds
  test::PushSetsockoptAction({-1, EACCES});  // SO_REUSEPORT fails
  EXPECT_THROW((void)sock.tryBind(true, 0), std::system_error);
  sock.close();
}

TEST(Socket, BindAndListenThrowsWhenListenFails) {
  Socket sock(Socket::Type::Stream);
  uint16_t port = 0;
  // Bind succeeds, but listen fails
  test::PushBindAction({0, 0});              // bind succeeds
  test::PushListenAction({-1, EADDRINUSE});  // listen fails
  EXPECT_THROW(sock.bindAndListen(false, port), std::system_error);
  sock.close();
}

TEST(Socket, BindAndListenThrowsWhenGetsocknameFails) {
  Socket sock(Socket::Type::Stream);
  uint16_t port = 0;  // port 0 means ephemeral; getsockname will be called
  // Bind succeeds, listen succeeds, but getsockname fails
  test::PushBindAction({0, 0});               // bind succeeds
  test::PushListenAction({0, 0});             // listen succeeds
  test::PushGetsocknameAction({-1, EACCES});  // getsockname fails
  EXPECT_THROW(sock.bindAndListen(false, port), std::system_error);
  sock.close();
}

}  // namespace aeronet
