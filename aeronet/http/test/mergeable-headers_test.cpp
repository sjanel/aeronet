#include "aeronet/mergeable-headers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"

namespace aeronet {

namespace {

TEST(MergeableHeaders, KnownListStyleHeadersReturnComma) {
  static constexpr std::string_view kHeaders[] = {
      "Accept",
      "Accept-Charset",
      http::AcceptEncoding,
      "Accept-Language",
      http::CacheControl,
      http::Connection,
      "Expect",
      "Forwarded",
      "If-Match",
      "If-None-Match",
      "Pragma",
      http::TE,
      "Trailer",
      http::TransferEncoding,
      http::Upgrade,
      "Via",
      "Warning",
  };
  for (std::string_view hdr : kHeaders) {
    EXPECT_EQ(',', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ(',', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, CookieIsSemicolon) {
  EXPECT_EQ(';', http::ReqHeaderValueSeparator("Cookie", true));
  EXPECT_EQ(';', http::ReqHeaderValueSeparator("Cookie", false));
}

TEST(MergeableHeaders, UserAgentSpaceJoin) {
  EXPECT_EQ(' ', http::ReqHeaderValueSeparator("User-Agent", true));
  EXPECT_EQ(' ', http::ReqHeaderValueSeparator("User-Agent", false));
}

TEST(MergeableHeaders, OverrideHeadersReturnO) {
  for (std::string_view hdr : {"Authorization", "From", "If-Modified-Since", "If-Range", "If-Unmodified-Since",
                               "Max-Forwards", "Proxy-Authorization", "Range", "Referer"}) {
    EXPECT_EQ('O', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('O', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, DisallowedDuplicateHeadersReturnNull) {
  for (std::string_view hdr : {"Content-Length", "Host"}) {
    EXPECT_EQ('\0', http::ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('\0', http::ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, CaseInsensitiveMatch) {
  // Mix casing for a few representatives of each category.
  EXPECT_EQ(',', http::ReqHeaderValueSeparator("aCcEpT", true));
  EXPECT_EQ(';', http::ReqHeaderValueSeparator("cOOkIe", true));
  EXPECT_EQ(' ', http::ReqHeaderValueSeparator("uSeR-aGeNt", true));
  EXPECT_EQ('O', http::ReqHeaderValueSeparator("aUtHoRiZaTiOn", true));
  EXPECT_EQ('\0', http::ReqHeaderValueSeparator("hOsT", true));
}

TEST(MergeableHeaders, UnknownHeaderHonorsMergeFlag) {
  EXPECT_EQ(',', http::ReqHeaderValueSeparator("X-Experimental", true));
  EXPECT_EQ('\0', http::ReqHeaderValueSeparator("X-Experimental", false));
}

TEST(MergeableHeaders, NoAccidentalTableCollisions) {
  // Sanity: calling the function with each known header twice produces stable result; acts as a rudimentary
  // duplicate guard (compile-time table already static_asserts ordering, but not duplicates).
  static constexpr std::array<std::pair<std::string_view, char>, 5> probe = {
      {{"Accept", ','}, {"Cookie", ';'}, {"User-Agent", ' '}, {"Authorization", 'O'}, {"Host", '\0'}}};
  for (auto [key, expected] : probe) {
    EXPECT_EQ(expected, http::ReqHeaderValueSeparator(key, true));
    EXPECT_EQ(expected, http::ReqHeaderValueSeparator(key, true));
  }
}

TEST(MergeableHeaders, StrictModeDoesNotAffectKnownPolicies) {
  // Compare permissive vs strict for all known headers; they must match (strict only influences unknowns).
  static constexpr std::string_view kHeaders[]{"Accept",
                                               "Accept-Charset",
                                               http::AcceptEncoding,
                                               "Accept-Language",
                                               "Authorization",
                                               http::CacheControl,
                                               http::Connection,
                                               http::ContentLength,
                                               "Cookie",
                                               "Expect",
                                               "Forwarded",
                                               "From",
                                               "Host",
                                               "If-Match",
                                               "If-Modified-Since",
                                               "If-None-Match",
                                               "If-Range",
                                               "If-Unmodified-Since",
                                               "Max-Forwards",
                                               "Pragma",
                                               "Proxy-Authorization",
                                               http::Range,
                                               "Referer",
                                               "TE",
                                               "Trailer",
                                               "Transfer-Encoding",
                                               "Upgrade",
                                               "User-Agent",
                                               "Via",
                                               "Warning"};
  for (std::string_view hdr : kHeaders) {
    auto perm = http::ReqHeaderValueSeparator(hdr, true);
    auto strict = http::ReqHeaderValueSeparator(hdr, false);
    EXPECT_EQ(perm, strict) << "Mismatch on known header when toggling strict flag: " << hdr;
  }
}

}  // namespace
}  // namespace aeronet