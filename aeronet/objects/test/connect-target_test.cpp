#include "aeronet/connect-target.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace aeronet {

namespace {

void ExpectTarget(std::string_view authority, std::string_view host, uint16_t port) {
  const ConnectTarget target(authority);
  ASSERT_FALSE(target.invalid()) << authority;
  EXPECT_EQ(target.host, host) << authority;
  EXPECT_EQ(target.port, port) << authority;
}

void ExpectInvalid(std::string_view authority) {
  const ConnectTarget target(authority);
  EXPECT_TRUE(target.invalid()) << authority;
}

}  // namespace

TEST(ConnectTargetTest, HostNameAndIpv4) {
  ExpectTarget("example.com:443", "example.com", 443);
  ExpectTarget("10.0.0.12:8080", "10.0.0.12", 8080);
  ExpectTarget("h:1", "h", 1);
  ExpectTarget("h:65535", "h", 65535);
}

TEST(ConnectTargetTest, Ipv6InBrackets) {
  ExpectTarget("[::1]:443", "::1", 443);
  ExpectTarget("[2001:db8::7]:8443", "2001:db8::7", 8443);
}

TEST(ConnectTargetTest, InvalidTargets) {
  ExpectInvalid("");
  ExpectInvalid("example.com");
  ExpectInvalid("example.com:");
  ExpectInvalid(":443");
  ExpectInvalid("example.com:https");
  ExpectInvalid("example.com:0");
  ExpectInvalid("example.com:65536");
  ExpectInvalid("example.com:443x");
  ExpectInvalid("example.com:-1");
  ExpectInvalid("example.com: 443");
  // IPv6 addresses must be enclosed in brackets.
  ExpectInvalid("::1:443");
  ExpectInvalid("[::1]");
  ExpectInvalid("[::1]443");
  ExpectInvalid("[::1:443");
  ExpectInvalid("[]:443");
}

}  // namespace aeronet
