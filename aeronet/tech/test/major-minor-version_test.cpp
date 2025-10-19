#include "major-minor-version.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string_view>

using namespace aeronet;

// helper prefix storage with static lifetime for template parameter
static constexpr char kHttpPrefix[] = "HTTP/";

using HttpVer = MajorMinorVersion<kHttpPrefix, uint8_t>;

TEST(MajorMinorVersion, ParseValid) {
  HttpVer vers{};
  const char *str = "HTTP/1.1";
  EXPECT_TRUE(parseVersion<kHttpPrefix>(str, str + std::strlen(str), vers));
  EXPECT_EQ(vers.major, 1);
  EXPECT_EQ(vers.minor, 1);
}

TEST(MajorMinorVersion, ParseInvalidPrefix) {
  HttpVer vers{};
  const char *str = "NOTHTTP/1.1";
  EXPECT_FALSE(parseVersion<kHttpPrefix>(str, str + std::strlen(str), vers));
}

TEST(MajorMinorVersion, ParseInvalidFormat) {
  HttpVer vers{};
  const char *s1 = "HTTP/1";  // missing minor
  EXPECT_FALSE(parseVersion<kHttpPrefix>(s1, s1 + std::strlen(s1), vers));

  const char *s2 = "HTTP/x.y";  // non-numeric
  EXPECT_FALSE(parseVersion<kHttpPrefix>(s2, s2 + std::strlen(s2), vers));
}

TEST(MajorMinorVersion, StrAndCompare) {
  HttpVer vers1{1, 0};
  HttpVer vers2{1, 1};
  HttpVer vers3{2, 0};

  // compare operators
  EXPECT_LT(vers1, vers2);
  EXPECT_LT(vers2, vers3);
  EXPECT_NE(vers1, vers2);

  // str() produces a FixedCapacityVector that can be wrapped in string_view
  auto sv = vers1.str();
  std::string_view sview(sv);
  EXPECT_EQ(sview, "HTTP/1.0");
}
