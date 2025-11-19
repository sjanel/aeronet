#include "aeronet/socket.hpp"

#include <gtest/gtest.h>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

TEST(Socket, Nominal) {
  aeronet::Socket sock(SOCK_STREAM);
  EXPECT_TRUE(sock);
  EXPECT_GE(sock.fd(), 0);
  sock.close();
  EXPECT_FALSE(sock);
}

TEST(Socket, CreationFailureThrows) { EXPECT_THROW(aeronet::Socket(-1), std::system_error); }