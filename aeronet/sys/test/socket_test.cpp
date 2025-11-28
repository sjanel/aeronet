#include "aeronet/socket.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>  // NOLINT(misc-include-cleaner) used by socket class

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace aeronet {

TEST(Socket, Nominal) {
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
  first.bindAndListen(false, false, port);
  Socket second(Socket::Type::Stream);
  EXPECT_FALSE(second.tryBind(false, false, port));
  second.close();
  first.close();
}

TEST(Socket, BindAndListenUpdatesPort) {
  Socket sock(Socket::Type::Stream);
  uint16_t port = 0;
  EXPECT_NO_THROW(sock.bindAndListen(false, false, port));
  EXPECT_NE(0, port);
  sock.close();
}

TEST(Socket, BindAndListenThrowsWhenPortInUse) {
  Socket first(Socket::Type::Stream);
  uint16_t port = 0;
  first.bindAndListen(false, false, port);
  Socket second(Socket::Type::Stream);
  EXPECT_THROW(second.bindAndListen(false, false, port), std::system_error);
  second.close();
  first.close();
}

}  // namespace aeronet