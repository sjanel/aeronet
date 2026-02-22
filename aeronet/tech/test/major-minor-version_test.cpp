#include "aeronet/major-minor-version.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string_view>

using namespace aeronet;

// helper prefix storage with static lifetime for template parameter
static constexpr char kHttpPrefix[] = "HTTP/";

using HttpVer = MajorMinorVersion<kHttpPrefix>;

TEST(MajorMinorVersion, ParseValid) {
  HttpVer vers{};
  std::string_view str = "HTTP/1.1";
  vers = HttpVer{str};
  EXPECT_EQ(vers.major(), 1);
  EXPECT_EQ(vers.minor(), 1);
}

TEST(MajorMinorVersion, ParseInvalidPrefix) {
  HttpVer vers{};
  std::string_view str = "NOTHTTP/1.1";
  vers = HttpVer{str};
  EXPECT_EQ(vers, HttpVer{});
}

TEST(MajorMinorVersion, ParseInvalidFormat) {
  HttpVer vers{};
  std::string_view s1 = "HTTP/1";  // missing minor
  vers = HttpVer{s1};
  EXPECT_EQ(vers, HttpVer{});

  std::string_view s2 = "HTTP/114";  // no dot
  vers = HttpVer{s2};
  EXPECT_EQ(vers, HttpVer{});

  std::string_view s3 = "HTTP/1.y";  // non-numeric minor
  vers = HttpVer{s3};
  EXPECT_EQ(vers, HttpVer{});

  std::string_view s4 = "HTTP/11.0";  // major > 9
  vers = HttpVer{s4};
  EXPECT_EQ(vers, HttpVer{});

  std::string_view s5 = "HTTP/1.10";  // minor > 9
  vers = HttpVer{s5};
  EXPECT_EQ(vers, HttpVer{});

  std::string_view s6 = "HTTP/0.1";  // major == 0
  vers = HttpVer{s6};
  EXPECT_EQ(vers, HttpVer{});
}

TEST(MajorMinorVersion, StrAndCompare) {
  HttpVer vers1{1, 0};
  HttpVer vers2{1, 1};
  HttpVer vers3{2, 0};

  // compare operators
  EXPECT_LT(vers1, vers2);
  EXPECT_LT(vers2, vers3);
  EXPECT_NE(vers1, vers2);
}

TEST(MajorMinorVersion, WriteFull) {
  HttpVer vers{1, 1};
  char buf[HttpVer::kStrLen + 1] = {};
  char* endPtr = vers.writeFull(buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(endPtr - buf)), "HTTP/1.1");

  HttpVer vers2{2, 0};
  endPtr = vers2.writeFull(buf);
  EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(endPtr - buf)), "HTTP/2.0");
}

TEST(MajorMinorVersion, Str) {
  HttpVer vers{1, 3};
  const auto strArr = vers.str();
  EXPECT_EQ(std::string_view(strArr.data(), strArr.size()), "HTTP/1.3");
}

TEST(MajorMinorVersion, InvalidVersion) {
  HttpVer vers{};
  EXPECT_FALSE(vers.isValid());

  HttpVer vers2{10, 0};  // major > 9
  EXPECT_FALSE(vers2.isValid());

  HttpVer vers3{1, 10};  // minor > 9
  EXPECT_FALSE(vers3.isValid());

  HttpVer vers4{0, 140};  // major == 0
  EXPECT_FALSE(vers4.isValid());
}

TEST(MajorMinorVersion, ValidVersion) {
  HttpVer vers{1, 1};
  EXPECT_TRUE(vers.isValid());

  HttpVer vers2{9, 9};
  EXPECT_TRUE(vers2.isValid());
}
