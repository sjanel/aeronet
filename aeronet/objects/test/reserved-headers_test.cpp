#include "aeronet/reserved-headers.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/lower-ascii-key.hpp"

namespace aeronet::http {

TEST(ReservedHeadersTest, ReservedResponseHeaderBasic) {
  EXPECT_TRUE(IsReservedResponseHeader(http::ContentLength));
  EXPECT_TRUE(IsReservedResponseHeader(http::Date));
  EXPECT_TRUE(IsReservedResponseHeader(http::Connection));
  EXPECT_TRUE(IsReservedResponseHeader(http::TransferEncoding));
  EXPECT_TRUE(IsReservedResponseHeader("te"));
  EXPECT_TRUE(IsReservedResponseHeader("trailer"));
  EXPECT_TRUE(IsReservedResponseHeader("upgrade"));
}

TEST(ReservedHeadersTest, ConnectionSpecificFields) {
  for (std::string_view name : {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"}) {
    EXPECT_TRUE(IsConnectionSpecificField(name, "value")) << name;
    EXPECT_TRUE(IsConnectionSpecificField(name, "")) << name;
  }
  for (std::string_view value : {"", "gzip", "trailer", "trailerz", "trailers, gzip"}) {
    EXPECT_TRUE(IsConnectionSpecificField("te", value)) << value;
  }
  for (std::string_view value : {"trailers", "TrAiLeRs"}) {
    EXPECT_FALSE(IsConnectionSpecificField("te", value)) << value;
  }
  // Fields with the length of a connection-specific one.
  for (std::string_view name : {"tf", "referer", "set-cookie", "content-encoding", "transfer-encodinh"}) {
    EXPECT_FALSE(IsConnectionSpecificField(name, "value")) << name;
  }
}

TEST(ReservedHeadersTest, ReservedResponseHeaderRejectsUnknowns) {
  EXPECT_FALSE(IsReservedResponseHeader("x-custom-header"));
  EXPECT_FALSE(IsReservedResponseHeader("content-length-extra"));
  EXPECT_FALSE(IsReservedResponseHeader("datex"));
}

TEST(ReservedHeadersTest, ReservedResponseHeaderHandlesLongValues) {
  // Very long names must be rejected (longer than internal max)
  std::string longName(256, 'a');
  EXPECT_FALSE(IsReservedResponseHeader(LowerAsciiKey{longName}));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerBasic) {
  EXPECT_TRUE(IsForbiddenTrailerHeader("transfer-encoding"));
  EXPECT_TRUE(IsForbiddenTrailerHeader(http::ContentLength));
  EXPECT_TRUE(IsForbiddenTrailerHeader("host"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("trailer"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("te"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("set-cookie"));
  EXPECT_TRUE(IsForbiddenTrailerHeader("authorization"));
  // RFC 9110 §6.5.1: authentication, request controls and conditionals, connection-specific fields.
  for (const std::string_view name : {"proxy-authorization", "proxy-authenticate", "www-authenticate", "if-range",
                                      "max-forwards", "connection", "upgrade"}) {
    EXPECT_TRUE(IsForbiddenTrailerHeader(LowerAsciiKey{name})) << name;
  }
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerRejectsUnknowns) {
  EXPECT_FALSE(IsForbiddenTrailerHeader("x-trailer-safe"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("content-typex"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("cached"));
}

TEST(ForbiddenTrailersTest, ForbiddenTrailerHandlesLong) {
  std::string longName(512, 'z');
  EXPECT_FALSE(IsForbiddenTrailerHeader(LowerAsciiKey{longName}));
}

TEST(ReservedHeadersTest, PrefixesDoNotMatch) {
  // Ensure that headers which are prefixes of reserved names do not falsely match
  EXPECT_FALSE(IsReservedResponseHeader("con"));
  EXPECT_FALSE(IsForbiddenTrailerHeader("transf"));
}

}  // namespace aeronet::http
