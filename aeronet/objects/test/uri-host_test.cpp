#include "aeronet/uri-host.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace aeronet::http {

TEST(UriHost, IPv4Address) {
  for (std::string_view addr : {
           "0.0.0.0",
           "1.2.3.4",
           "10.0.0.12",
           "192.168.255.1",
           "255.255.255.255",
           "99.100.249.250",
       }) {
    EXPECT_TRUE(IsValidIPv4Address(addr)) << addr;
  }
  for (std::string_view addr : {
           "",
           "1",
           "1.2.3",
           "1.2.3.4.5",
           "1.2.3.",
           ".1.2.3",
           "1..2.3",
           "256.0.0.1",
           "1.2.3.300",
           "01.2.3.4",
           "1.2.3.00",
           "1234.1.1.1",
           "a.b.c.d",
           "1.2.3.4 ",
           " 1.2.3.4",
           "1.2.3.-4",
           "1.2.3.0x1",
           "1.2.3.4:80",
       }) {
    EXPECT_FALSE(IsValidIPv4Address(addr)) << addr;
  }
}

TEST(UriHost, IPv6Address) {
  for (std::string_view addr : {
           "::",
           "::1",
           "1::",
           "1::8",
           "fe80::1",
           "FFFF::abcd",
           "2001:db8::7",
           "0000:0000::0",
           "1:2:3:4:5:6:7:8",
           "1:2:3:4:5:6:7::",
           "::2:3:4:5:6:7:8",
           "1::3:4:5:6:7:8",
           "1:2:3::5:6:7:8",
           "::ffff:192.0.2.1",
           "1:2:3:4:5:6:1.2.3.4",
           "::1.2.3.4",
           "1::1.2.3.4",
           "1:2:3:4:5::1.2.3.4",
       }) {
    EXPECT_TRUE(IsValidIPv6Address(addr)) << addr;
  }
  for (std::string_view addr : {
           "",
           ":",
           ":::",
           ":::1",
           "1:",
           ":1",
           "1::2:",
           "1:2:3:4:5:6:7",
           "1:2:3:4:5:6:7:8:9",
           "1:2:3:4:5:6:7:8::",
           "::1:2:3:4:5:6:7:8",
           "1::2::3",
           "::1::",
           "12345::",
           "g::1",
           "1:2:3:4:5:6:7:1.2.3.4",
           "1:2:3:4:5:6::1.2.3.4",
           "1.2.3.4",
           "::1.2.3.4:1",
           "1.2.3.4::",
           "::256.1.1.1",
           "::1.2.3",
           "::1 ",
           " ::1",
           "[::1]",
           "fe80::1%25eth0",
           "1:2:3:4:5:6:7:8:",
       }) {
    EXPECT_FALSE(IsValidIPv6Address(addr)) << addr;
  }
}

TEST(UriHost, RegName) {
  for (std::string_view host : {
           "a",
           "example.com",
           "EXAMPLE.Com",
           "my_host-1",
           "a~b",
           "%41b",
           "a%2Fb",
           "%ff",
           "a!$&'()*+,;=",
           "10.0.0.1",
           "999.1.1.1",
           "-",
           ".",
       }) {
    EXPECT_TRUE(IsValidRegName(host)) << host;
  }
  for (std::string_view host : {
           "",    "a b",  "a/b",  "a@b",  "a:b", "a%4", "a%",   "%",   "a%zz", "a%g1", "[::1]",       "a#b",
           "a?b", "a\\b", "a\"b", "a<b>", "a[b", "a]b", "a{b}", "a|b", "a^b",  "a`b",  "caf\xC3\xA9",
       }) {
    EXPECT_FALSE(IsValidRegName(host)) << host;
  }
}

// RFC 3986 §3.2.2: a reg-name is made of unreserved characters, sub-delims and pct-encoded octets only.
TEST(UriHost, RegNameCharacters) {
  static constexpr std::string_view kAllowed =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~!$&'()*+,;=";
  for (int byte = 0; byte < 256; ++byte) {
    const char ch = static_cast<char>(byte);
    const std::string host{'a', ch, 'b'};
    EXPECT_EQ(IsValidRegName(host), kAllowed.contains(ch)) << "byte " << byte;
    EXPECT_EQ(IsValidHostAndPort(host + ":80"), kAllowed.contains(ch)) << "byte " << byte;
  }
}

TEST(UriHost, HostAndPort) {
  for (std::string_view authority : {
           "h",
           "h:80",
           "h:",
           "h:00080",
           "h:123456789",
           "example.com:8080",
           "1.2.3.4:80",
           "%41:1",
           "my_host:1",
           "[::1]",
           "[::1]:8080",
           "[::1]:",
           "[2001:db8::7]:443",
           "[::ffff:1.2.3.4]:80",
           "[v1.x]",
           "[V1A.a:b]:1",
           "[v9.a!$&'()*+,;=]",
           "[vF.-._~]",
       }) {
    EXPECT_TRUE(IsValidHostAndPort(authority)) << authority;
  }
  for (std::string_view authority : {
           "",
           ":",
           ":80",
           "a b/c@d",
           "evil.example/x",
           "evil.example/x:80",
           "a@b",
           "user@h",
           "user:pw@h:80",
           "h:8x",
           "h:80:90",
           "h: 80",
           "h:-1",
           " h",
           "h ",
           "h\t",
           "h?x",
           "h#x",
           "h\\x",
           "[::1",
           "[::1]x",
           "[::1]:8x",
           "[::1]]",
           "[::1]/x",
           "[]",
           "[]:80",
           "[1.2.3.4]",
           "[::1%25eth0]",
           "[fe80::1::2]",
           "[v.x]",
           "[v1.]",
           "[v1]",
           "[vx.a]",
           "[v1.a/b]",
           "[v1.a@b]",
           "[[::1]]",
           "::1",
           "::1:80",
           "1:2::3",
       }) {
    EXPECT_FALSE(IsValidHostAndPort(authority)) << authority;
  }
}

TEST(UriHost, HostFieldValue) {
  EXPECT_TRUE(IsValidHostFieldValue(""));  // RFC 9110 §7.2: for a target URI without authority
  EXPECT_TRUE(IsValidHostFieldValue("example.com"));
  EXPECT_TRUE(IsValidHostFieldValue("[::1]:8080"));
  EXPECT_FALSE(IsValidHostFieldValue(":8080"));
  EXPECT_FALSE(IsValidHostFieldValue("a b/c@d"));
  EXPECT_FALSE(IsValidHostFieldValue("evil.example/x"));
}

TEST(UriHost, LongValues) {
  std::string host(65536, 'a');
  EXPECT_TRUE(IsValidRegName(host));
  EXPECT_TRUE(IsValidHostAndPort(host + ":65535"));
  host[40000] = '/';
  EXPECT_FALSE(IsValidRegName(host));
  EXPECT_FALSE(IsValidHostAndPort(host));

  std::string encoded;
  for (int i = 0; i < 10000; ++i) {
    encoded.append("%2a");
  }
  EXPECT_TRUE(IsValidRegName(encoded));
  encoded.pop_back();
  EXPECT_FALSE(IsValidRegName(encoded));

  std::string manyPieces;
  for (int i = 0; i < 10000; ++i) {
    manyPieces.append("ffff:");
  }
  manyPieces.append("ffff");
  EXPECT_FALSE(IsValidIPv6Address(manyPieces));
  EXPECT_FALSE(IsValidHostAndPort('[' + manyPieces + "]:80"));
}

}  // namespace aeronet::http
