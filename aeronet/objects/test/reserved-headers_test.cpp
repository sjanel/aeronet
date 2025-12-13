#include "aeronet/reserved-headers.hpp"

#include <gtest/gtest.h>

#include <string>

namespace aeronet::http {

TEST(ReservedHeadersTest, ReservedResponseHeaderBasic) {
  EXPECT_TRUE(IsReservedResponseHeader("content-length"));
  EXPECT_TRUE(IsReservedResponseHeader("date"));
  EXPECT_TRUE(IsReservedResponseHeader("connection"));
  EXPECT_TRUE(IsReservedResponseHeader("transfer-encoding"));
  EXPECT_TRUE(IsReservedResponseHeader("te"));
  EXPECT_TRUE(IsReservedResponseHeader("trailer"));
  EXPECT_TRUE(IsReservedResponseHeader("upgrade"));
}

TEST(ReservedHeadersTest, ReservedResponseHeaderCaseInsensitive) {
  EXPECT_TRUE(IsReservedResponseHeader("Content-Length"));
  EXPECT_TRUE(IsReservedResponseHeader("DaTe"));
  EXPECT_TRUE(IsReservedResponseHeader("TrAnSfEr-EnCoDiNg"));
}

TEST(ReservedHeadersTest, ReservedResponseHeaderRejectsUnknowns) {
  EXPECT_FALSE(IsReservedResponseHeader("x-custom-header"));
  EXPECT_FALSE(IsReservedResponseHeader("content-length-extra"));
  EXPECT_FALSE(IsReservedResponseHeader("datex"));
}

TEST(ReservedHeadersTest, ReservedResponseHeaderHandlesEmptyAndLong) {
  EXPECT_FALSE(IsReservedResponseHeader(""));
  // Very long names must be rejected (longer than internal max)
  std::string longName(256, 'a');
  EXPECT_FALSE(IsReservedResponseHeader(longName));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerBasic) {
  EXPECT_TRUE(IsForbiddenTrailerHeader("transfer-encoding"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("content-length"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("host"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("trailer"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("te"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("set-cookie"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("authorization"));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerCaseInsensitive) {
  EXPECT_TRUE(IsForbiddenTrailerHeader("Transfer-Encoding"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("Content-Length"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("SET-COOKIE"));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerRejectsUnknowns) {
  EXPECT_FALSE(IsForbiddenTrailerHeader("x-trailer-safe"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("content-typex"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("cached"));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerHandlesEmptyAndLong) {
  EXPECT_FALSE(IsForbiddenTrailerHeader(""));
  std::string longName(512, 'Z');
  EXPECT_FALSE(IsForbiddenTrailerHeader(longName));
}

TEST(ReservedHeadersTest, PrefixesDoNotMatch) {
  // Ensure that headers which are prefixes of reserved names do not falsely match
  EXPECT_FALSE(IsReservedResponseHeader("con"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("transf"));
}

}  // namespace aeronet::http
