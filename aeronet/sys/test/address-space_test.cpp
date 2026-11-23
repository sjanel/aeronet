#include "aeronet/address-space.hpp"

#include <gtest/gtest.h>

#ifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include <initializer_list>
#include <string_view>

#include "aeronet/socket-ops.hpp"

namespace aeronet {
namespace {

struct Case {
  std::string_view address;
  AddressSpace space;
};

constexpr AddressSpace kLoopback = AddressSpace::Loopback;
constexpr AddressSpace kLocal = AddressSpace::Local;
constexpr AddressSpace kPublic = AddressSpace::Public;

// The bounds of each block of the table of the Local Network Access specification, and the addresses next to them.
constexpr Case kAddresses[]{
    {"127.0.0.1", kLoopback},
    {"127.0.0.0", kLoopback},
    {"127.255.255.255", kLoopback},
    {"126.255.255.255", kPublic},
    {"128.0.0.0", kPublic},
    {"10.0.0.0", kLocal},
    {"10.255.255.255", kLocal},
    {"9.255.255.255", kPublic},
    {"11.0.0.0", kPublic},
    {"100.64.0.0", kLocal},
    {"100.127.255.255", kLocal},
    {"100.63.255.255", kPublic},
    {"100.128.0.0", kPublic},
    {"172.16.0.0", kLocal},
    {"172.31.255.255", kLocal},
    {"172.15.255.255", kPublic},
    {"172.32.0.0", kPublic},
    {"192.168.0.0", kLocal},
    {"192.168.255.255", kLocal},
    {"192.167.255.255", kPublic},
    {"192.169.0.0", kPublic},
    {"198.18.0.0", kLoopback},
    {"198.19.255.255", kLoopback},
    {"198.17.255.255", kPublic},
    {"198.20.0.0", kPublic},
    {"169.254.169.254", kLocal},
    {"169.254.0.0", kLocal},
    {"169.254.255.255", kLocal},
    {"169.253.255.255", kPublic},
    {"169.255.0.0", kPublic},
    {"0.0.0.0", kLoopback},
    {"0.0.0.1", kLocal},
    {"0.255.255.255", kLocal},
    {"1.0.0.0", kPublic},
    {"8.8.8.8", kPublic},
    {"203.0.113.7", kPublic},
    {"255.255.255.255", kPublic},
    {"::1", kLoopback},
    {"::", kLoopback},
    {"::2", kPublic},
    {"::7f00:1", kPublic},  // IPv4-compatible (deprecated), not listed
    {"fc00::", kLocal},
    {"fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kLocal},
    {"fbff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kPublic},
    {"fe00::", kPublic},
    {"fe7f:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kPublic},
    {"fe80::1", kLocal},
    {"febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kLocal},
    {"fec0::1", kLocal},
    {"feff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kLocal},
    {"ff02::1", kPublic},
    {"2001:db8::1", kLocal},
    {"2001:db8:ffff:ffff:ffff:ffff:ffff:ffff", kLocal},
    {"2001:db7:ffff:ffff:ffff:ffff:ffff:ffff", kPublic},
    {"2001:db9::", kPublic},
    {"2002:db8::1", kPublic},  // 6to4, not listed
    {"2001:cb8::1", kPublic},
    {"3fff::1", kLocal},
    {"3fff:fff:ffff:ffff:ffff:ffff:ffff:ffff", kLocal},
    {"3fff:1000::", kPublic},
    {"3ffe:ffff:ffff:ffff:ffff:ffff:ffff:ffff", kPublic},
    {"2606:4700::1111", kPublic},
    {"::ffff:127.0.0.1", kLoopback},
    {"::ffff:0.0.0.0", kLoopback},
    {"::ffff:10.1.2.3", kLocal},
    {"::ffff:8.8.8.8", kPublic},
    {"::fffe:127.0.0.1", kPublic},
    {"64:ff9b::127.0.0.1", kLoopback},
    {"64:ff9b::169.254.169.254", kLocal},
    {"64:ff9b::8.8.8.8", kPublic},
    {"64:ff9b:1::127.0.0.1", kPublic},
};

TEST(AddressSpaceTest, AddressesOfTheTableOfTheSpecification) {
  for (const auto& [address, space] : kAddresses) {
    EXPECT_EQ(HostAddressSpace(address), space) << address;
    sockaddr_storage storage{};
    ASSERT_TRUE(ParseAddress(address, storage)) << address;
    EXPECT_EQ(AddressSpaceOf(reinterpret_cast<const sockaddr&>(storage)), space) << address;
  }
}

// The IPv4 addresses in the forms that browsers (WHATWG URL) and inet_aton() read, but not inet_pton(): a forward proxy
// resolving the host with getaddrinfo() connects to them.
constexpr Case kOtherIpv4Forms[]{
    {"127.1", kLoopback},
    {"127.0.1", kLoopback},
    {"2130706433", kLoopback},
    {"0x7f000001", kLoopback},
    {"0x7F.0.0.1", kLoopback},
    {"0X7f.0.0.1", kLoopback},
    {"0177.0.0.1", kLoopback},
    {"0x7f.1", kLoopback},
    {"127.0.0.1.", kLoopback},
    {"127.1.", kLoopback},
    {"0", kLoopback},
    {"0x", kLoopback},
    {"00", kLoopback},
    {"0x0000000000000000000000001", kLocal},  // 0.0.0.1
    {"10.1", kLocal},
    {"010.0.0.1", kPublic},  // octal: 8.0.0.1
    {"169.254.43518", kLocal},
    {"169.16689662", kLocal},
    {"0xa9fea9fe", kLocal},
    {"0xC0.0xA8.0.1", kLocal},
    {"0300.0250.0.1", kLocal},
    {"8.8.8.8.", kPublic},
    {"0x8.0x8.0x8.0x8", kPublic},
    {"134744072", kPublic},
    {"4294967295", kPublic},
    {"0xffffffff", kPublic},
    {"255.255.65535", kPublic},
    {"255.16777215", kPublic},
};

TEST(AddressSpaceTest, OtherFormsOfIpv4Addresses) {
  for (const auto& [address, space] : kOtherIpv4Forms) {
    EXPECT_EQ(HostAddressSpace(address), space) << address;
  }
}

// A host ending in a number is an IPv4 address for browsers, which reject it when it is not a valid one: a resolver
// might still read it as an address, it is taken as the host itself.
TEST(AddressSpaceTest, InvalidAddressesAreLoopback) {
  for (const std::string_view host : std::initializer_list<std::string_view>{
           "256.0.0.1",
           "1.256.0.1",
           "1.2.256.1",
           "1.2.3.256",
           "1.2.65536",
           "1.16777216",
           "4294967296",
           "0x100000000",
           "99999999999999",
           "1.2.3.4.5",
           "1.2.3.4.5.",
           "1..2",
           ".1",
           "example.123",
           "example.1.2.3.4",
           "example.0x1f",
           "example.0x",
           "09",
           "1.08",
           "0x1g.1",
           "1.2.3.0x100",
           "a.b.1",
           std::string_view("1.2.3.4\0.5", 10),
           // A colon makes an IPv6 address.
           "::ffff:127.1",
           "1:2",
           ":",
           "fe80::1::2%eth0",
       }) {
    EXPECT_EQ(HostAddressSpace(host), AddressSpace::Loopback) << host;
  }
}

TEST(AddressSpaceTest, OrderFromTheMostPrivate) {
  EXPECT_LT(AddressSpace::Loopback, AddressSpace::Local);
  EXPECT_LT(AddressSpace::Local, AddressSpace::Public);
}

TEST(AddressSpaceTest, OtherFamiliesAreLoopback) {
  sockaddr_storage storage{};
  storage.ss_family = AF_UNIX;
  EXPECT_EQ(AddressSpaceOf(reinterpret_cast<const sockaddr&>(storage)), AddressSpace::Loopback);
}

TEST(AddressSpaceTest, HostNames) {
  EXPECT_EQ(HostAddressSpace("localhost"), AddressSpace::Loopback);
  EXPECT_EQ(HostAddressSpace("LocalHost"), AddressSpace::Loopback);
  EXPECT_EQ(HostAddressSpace("localhost."), AddressSpace::Loopback);
  EXPECT_EQ(HostAddressSpace("app.localhost"), AddressSpace::Loopback);
  EXPECT_EQ(HostAddressSpace("a.b.LOCALHOST."), AddressSpace::Loopback);

  EXPECT_EQ(HostAddressSpace("notlocalhost"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("localhost.example.com"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("example.com"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("intranet"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace(""), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("."), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace(std::string_view("10.0.0.1\0x", 10)), AddressSpace::Public);
  // A last label that is not a number: a domain name.
  EXPECT_EQ(HostAddressSpace("1.2.3.a"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("127.0.0.1x"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("0x7g"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("127.0.0.0xg"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("1e3"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("127.0.0.1-"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("1.2.3.-1"), AddressSpace::Public);
  EXPECT_EQ(HostAddressSpace("8.8.8.8.."), AddressSpace::Public);
}

TEST(AddressSpaceTest, ZoneIdentifiersAreIgnored) {
  EXPECT_EQ(HostAddressSpace("fe80::1%eth0"), AddressSpace::Local);
  EXPECT_EQ(HostAddressSpace("::1%lo"), AddressSpace::Loopback);
}

}  // namespace
}  // namespace aeronet
