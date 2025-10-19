#include "concatenated-strings.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace aeronet;

TEST(ConcatenatedStrings, BasicAccess) {
  ConcatenatedStrings<3> cs({"alpn", "cipher", "tls1.3"});
  EXPECT_EQ(cs[0], "alpn");
  EXPECT_EQ(cs[1], "cipher");
  EXPECT_EQ(cs[2], "tls1.3");
}

TEST(ConcatenatedStrings, DefaultConstructedEmpty) {
  ConcatenatedStrings<3> info;
  EXPECT_EQ(info[0], std::string_view());
  EXPECT_EQ(info[1], std::string_view());
  EXPECT_EQ(info[2], std::string_view());
}

TEST(ConcatenatedStrings, ParameterizedStoresAndReturns) {
  ConcatenatedStrings<3> info({"h2", "TLS_AES_128_GCM_SHA256", "TLSv1.3"});
  EXPECT_EQ(info[0], "h2");
  EXPECT_EQ(info[1], "TLS_AES_128_GCM_SHA256");
  EXPECT_EQ(info[2], "TLSv1.3");
}

TEST(ConcatenatedStrings, LongStringsAreHandled) {
  std::string alpn(1000, 'A');
  std::string cipher(500, 'B');
  std::string version(200, 'C');
  ConcatenatedStrings<3> info({alpn, cipher, version});
  EXPECT_EQ(info[0], std::string_view(alpn));
  EXPECT_EQ(info[1], std::string_view(cipher));
  EXPECT_EQ(info[2], std::string_view(version));
}

TEST(ConcatenatedStrings, CopyAndAssign) {
  ConcatenatedStrings<2> src({"proto", "cipher"});
  ConcatenatedStrings<2> copyInfo = src;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copyInfo[0], "proto");
  EXPECT_EQ(copyInfo[1], "cipher");

  ConcatenatedStrings<2> dst;
  dst = src;  // copy assign
  EXPECT_EQ(dst[0], "proto");
  EXPECT_EQ(dst[1], "cipher");
}