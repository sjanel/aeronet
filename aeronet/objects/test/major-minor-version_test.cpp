
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "aeronet/http-version.hpp"

namespace aeronet {

TEST(MajorMinorVersion, ParseValid) {
  http::Version vers{};
  std::string_view str = "HTTP/1.1";
  vers = http::Version{str};
  EXPECT_EQ(vers.major(), 1);
  EXPECT_EQ(vers.minor(), 1);
}

TEST(MajorMinorVersion, ParseInvalidPrefix) {
  http::Version vers{};
  std::string_view str = "NOTHTTP/1.1";
  vers = http::Version{str};
  EXPECT_EQ(vers, http::Version{});
}

TEST(MajorMinorVersion, ParseInvalidFormat) {
  http::Version vers{};
  std::string_view s1 = "HTTP/1";  // missing minor
  vers = http::Version{s1};
  EXPECT_EQ(vers, http::Version{});

  std::string_view s2 = "HTTP/114";  // no dot
  vers = http::Version{s2.data(), s2.size()};
  EXPECT_EQ(vers, http::Version{});

  std::string_view s3 = "HTTP/1.y";  // non-numeric minor
  vers = http::Version{s3};
  EXPECT_EQ(vers, http::Version{});

  std::string_view s4 = "HTTP/11.0";  // major > 9
  vers = http::Version{s4};
  EXPECT_EQ(vers, http::Version{});

  std::string_view s5 = "HTTP/1.10";  // minor > 9
  vers = http::Version{s5};
  EXPECT_EQ(vers, http::Version{});
}

TEST(MajorMinorVersion, StrAndCompare) {
  http::Version vers1{1, 0};
  http::Version vers2{1, 1};
  http::Version vers3{2, 0};

  // compare operators
  EXPECT_LT(vers1, vers2);
  EXPECT_LT(vers2, vers3);
  EXPECT_NE(vers1, vers2);
}

TEST(MajorMinorVersion, WriteFull) {
  http::Version vers0{static_cast<uint8_t>(0), 9};
  char buf[http::Version::kStrLen + 1]{};
  char* endPtr = vers0.writeFull(buf);
  EXPECT_TRUE(vers0.isValid());
  EXPECT_EQ(vers0.major(), 0);
  EXPECT_EQ(vers0.minor(), 9);
  std::string_view sv(buf, static_cast<std::size_t>(endPtr - buf));
  EXPECT_EQ(sv, "HTTP/0.9");

  http::Version vers1{1, 1};
  endPtr = vers1.writeFull(buf);
  EXPECT_EQ(sv, "HTTP/1.1");

  http::Version vers2{2, 0};
  endPtr = vers2.writeFull(buf);
  EXPECT_EQ(sv, "HTTP/2.0");
}

TEST(MajorMinorVersion, Str) {
  http::Version vers{1, 3};
  const auto strArr = vers.str();
  EXPECT_EQ(std::string_view(strArr.data(), strArr.size()), "HTTP/1.3");
}

TEST(MajorMinorVersion, InvalidVersion) {
  http::Version vers{};
  EXPECT_FALSE(vers.isValid());

  http::Version vers2{10, 0};  // major > 9
  EXPECT_FALSE(vers2.isValid());

  http::Version vers3{1, 10};  // minor > 9
  EXPECT_FALSE(vers3.isValid());
}

TEST(MajorMinorVersion, ValidVersion) {
  http::Version vers0{static_cast<uint8_t>(0), 7};
  EXPECT_TRUE(vers0.isValid());

  http::Version vers1{1, 1};
  EXPECT_TRUE(vers1.isValid());

  http::Version vers2{9, 9};
  EXPECT_TRUE(vers2.isValid());
}

}  // namespace aeronet