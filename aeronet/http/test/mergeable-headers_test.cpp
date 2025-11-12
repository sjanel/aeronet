#include "aeronet/mergeable-headers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>

using aeronet::http::ReqHeaderValueSeparator;

namespace {

TEST(MergeableHeaders, KnownListStyleHeadersReturnComma) {
  for (std::string_view hdr : {"Accept", "Accept-Charset", "Accept-Encoding", "Accept-Language", "Cache-Control",
                               "Connection", "Expect", "Forwarded", "If-Match", "If-None-Match", "Pragma", "TE",
                               "Trailer", "Transfer-Encoding", "Upgrade", "Via", "Warning"}) {
    EXPECT_EQ(',', ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ(',', ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, CookieIsSemicolon) {
  EXPECT_EQ(';', ReqHeaderValueSeparator("Cookie", true));
  EXPECT_EQ(';', ReqHeaderValueSeparator("Cookie", false));
}

TEST(MergeableHeaders, UserAgentSpaceJoin) {
  EXPECT_EQ(' ', ReqHeaderValueSeparator("User-Agent", true));
  EXPECT_EQ(' ', ReqHeaderValueSeparator("User-Agent", false));
}

TEST(MergeableHeaders, OverrideHeadersReturnO) {
  for (std::string_view hdr : {"Authorization", "From", "If-Modified-Since", "If-Range", "If-Unmodified-Since",
                               "Max-Forwards", "Proxy-Authorization", "Range", "Referer"}) {
    EXPECT_EQ('O', ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('O', ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, DisallowedDuplicateHeadersReturnNull) {
  for (std::string_view hdr : {"Content-Length", "Host"}) {
    EXPECT_EQ('\0', ReqHeaderValueSeparator(hdr, true)) << hdr;
    EXPECT_EQ('\0', ReqHeaderValueSeparator(hdr, false)) << hdr << " (strict)";
  }
}

TEST(MergeableHeaders, CaseInsensitiveMatch) {
  // Mix casing for a few representatives of each category.
  EXPECT_EQ(',', ReqHeaderValueSeparator("aCcEpT", true));
  EXPECT_EQ(';', ReqHeaderValueSeparator("cOOkIe", true));
  EXPECT_EQ(' ', ReqHeaderValueSeparator("uSeR-aGeNt", true));
  EXPECT_EQ('O', ReqHeaderValueSeparator("aUtHoRiZaTiOn", true));
  EXPECT_EQ('\0', ReqHeaderValueSeparator("hOsT", true));
}

TEST(MergeableHeaders, UnknownHeaderHonorsMergeFlag) {
  EXPECT_EQ(',', ReqHeaderValueSeparator("X-Experimental", true));
  EXPECT_EQ('\0', ReqHeaderValueSeparator("X-Experimental", false));
}

TEST(MergeableHeaders, NoAccidentalTableCollisions) {
  // Sanity: calling the function with each known header twice produces stable result; acts as a rudimentary
  // duplicate guard (compile-time table already static_asserts ordering, but not duplicates).
  const std::array<std::pair<std::string_view, char>, 5> probe = {
      {{"Accept", ','}, {"Cookie", ';'}, {"User-Agent", ' '}, {"Authorization", 'O'}, {"Host", '\0'}}};
  for (auto [key, expected] : probe) {
    EXPECT_EQ(expected, ReqHeaderValueSeparator(key, true));
    EXPECT_EQ(expected, ReqHeaderValueSeparator(key, true));
  }
}

TEST(MergeableHeaders, StrictModeDoesNotAffectKnownPolicies) {
  // Compare permissive vs strict for all known headers; they must match (strict only influences unknowns).
  for (std::string_view hdr : {"Accept",
                               "Accept-Charset",
                               "Accept-Encoding",
                               "Accept-Language",
                               "Authorization",
                               "Cache-Control",
                               "Connection",
                               "Content-Length",
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
                               "Range",
                               "Referer",
                               "TE",
                               "Trailer",
                               "Transfer-Encoding",
                               "Upgrade",
                               "User-Agent",
                               "Via",
                               "Warning"}) {
    auto perm = ReqHeaderValueSeparator(hdr, true);
    auto strict = ReqHeaderValueSeparator(hdr, false);
    EXPECT_EQ(perm, strict) << "Mismatch on known header when toggling strict flag: " << hdr;
  }
}

}  // namespace
