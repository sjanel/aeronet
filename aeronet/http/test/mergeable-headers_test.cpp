#include "aeronet/mergeable-headers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"

namespace aeronet {

TEST(MergeableHeaders, KnownListStyleHeadersReturnComma) {
  static constexpr std::string_view kHeaders[] = {
      "accept",
      "accept-charset",
      http::AcceptEncoding,
      "accept-language",
      http::CacheControl,
      http::Connection,
      "expect",
      "forwarded",
      "if-match",
      "if-none-match",
      "pragma",
      http::TE,
      "trailer",
      http::TransferEncoding,
      http::Upgrade,
      "via",
      "warning",
  };
  for (std::string_view hdr : kHeaders) {
    EXPECT_EQ(',', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ(',', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, CookieIsSemicolon) {
  EXPECT_EQ(';', http::ReqHeaderValueSeparator("cookie", true));
  EXPECT_EQ(';', http::ReqHeaderValueSeparator("cookie", false));
}

TEST(MergeableHeaders, UserAgentSpaceJoin) {
  EXPECT_EQ(' ', http::ReqHeaderValueSeparator("user-agent", true));
  EXPECT_EQ(' ', http::ReqHeaderValueSeparator("user-agent", false));
}

TEST(MergeableHeaders, OverrideHeadersReturnO) {
  for (std::string_view hdr : {
           "authorization",
           "from",
           "if-modified-since",
           "if-range",
           "if-unmodified-since",
           "max-forwards",
           "proxy-authorization",
           "range",
           "referer",
       }) {
    EXPECT_EQ('O', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('O', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, DisallowedDuplicateHeadersReturnNull) {
  for (std::string_view hdr : {"content-length", "host"}) {
    EXPECT_EQ('\0', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('\0', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, UnknownHeaderHonorsMergeFlag) {
  EXPECT_EQ(',', http::ReqHeaderValueSeparator("X-Experimental", true));
  EXPECT_EQ('\0', http::ReqHeaderValueSeparator("X-Experimental", false));
}

TEST(MergeableHeaders, NoAccidentalTableCollisions) {
  // Sanity: calling the function with each known header twice produces stable result; acts as a rudimentary
  // duplicate guard (compile-time table already static_asserts ordering, but not duplicates).
  static constexpr std::array<std::pair<std::string_view, char>, 5> probe{
      {{"accept", ','}, {"cookie", ';'}, {"user-agent", ' '}, {"authorization", 'O'}, {"host", '\0'}},
  };
  for (auto [key, expected] : probe) {
    EXPECT_EQ(expected, http::ReqHeaderValueSeparator(key, true));
    EXPECT_EQ(expected, http::ReqHeaderValueSeparator(key, true));
  }
}

TEST(MergeableHeaders, StrictModeDoesNotAffectKnownPolicies) {
  // Compare permissive vs strict for all known headers; they must match (strict only influences unknowns).
  static constexpr std::string_view kHeaders[]{
      "accept",
      "accept-charset",
      http::AcceptEncoding,
      "accept-language",
      "authorization",
      http::CacheControl,
      http::Connection,
      http::ContentLength,
      "cookie",
      "expect",
      "forwarded",
      "from",
      "host",
      "if-match",
      "if-modified-since",
      "if-none-match",
      "if-range",
      "if-unmodified-since",
      "max-forwards",
      "pragma",
      "proxy-authorization",
      http::Range,
      "referer",
      "te",
      "trailer",
      "transfer-encoding",
      "upgrade",
      "user-agent",
      "via",
      "warning",
  };
  for (std::string_view hdr : kHeaders) {
    auto perm = http::ReqHeaderValueSeparator(hdr, true);
    auto strict = http::ReqHeaderValueSeparator(hdr, false);
    EXPECT_EQ(perm, strict) << "Mismatch on known header when toggling strict flag: " << hdr;
  }
}

}  // namespace aeronet