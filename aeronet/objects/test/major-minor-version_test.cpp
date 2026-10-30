
#include "aeronet/major-minor-version.hpp"

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

  // Same length as a valid version: the prefix itself is checked, case-sensitively.
  for (std::string_view prefixMismatch : {"XTTP/1.1", "HTTX/1.1", "http/1.1", "HTTP-1.1", "HTTP 1.1", "XTTP/2.0"}) {
    EXPECT_EQ(http::Version{prefixMismatch}, http::Version{}) << prefixMismatch;
  }
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

  // Characters just around the digits range.
  for (std::string_view notDigit : {"HTTP//.1", "HTTP/:.1", "HTTP/1./", "HTTP/1.:", "HTTP/1.\xFF"}) {
    EXPECT_EQ(http::Version{notDigit}, http::Version{}) << notDigit;
  }
}

namespace {
constexpr char kTlsPrefix[] = "TLS";
using TlsVersion = MajorMinorVersion<kTlsPrefix>;
}  // namespace

TEST(MajorMinorVersion, ParseOtherPrefix) {
  static_assert(TlsVersion{"TLS1.3"} == TlsVersion{1, 3});
  static_assert(!TlsVersion{"TLX1.3"}.isValid());
  static_assert(!TlsVersion{"tls1.3"}.isValid());
  static_assert(TlsVersion::OfMajor<1>("TLS1.2", 6) == TlsVersion{1, 2});
  static_assert(!TlsVersion::OfMajor<1>("HTTP/1.2", 8).isValid());
}

TEST(MajorMinorVersion, OfMajor) {
  for (std::uint8_t minorVer = 0; minorVer <= 9; ++minorVer) {
    const char str[]{'H', 'T', 'T', 'P', '/', '1', '.', static_cast<char>('0' + minorVer)};
    EXPECT_EQ(http::Version::OfMajor<1>(str, sizeof(str)), (http::Version{1, minorVer}));
    EXPECT_EQ(http::Version::OfMajor<1>(str, sizeof(str)), (http::Version{str, sizeof(str)}));
  }
  static_assert(http::Version::OfMajor<2>("HTTP/2.0", 8) == http::HTTP_2_0);
  static_assert(http::Version::OfMajor<9>("HTTP/9.9", 8) == http::Version{9, 9});

  // A valid version of another major version is not a version of the expected one.
  for (std::string_view otherMajor : {"HTTP/0.9", "HTTP/2.0", "HTTP/9.1"}) {
    EXPECT_FALSE(http::Version::OfMajor<1>(otherMajor.data(), otherMajor.size()).isValid()) << otherMajor;
    EXPECT_TRUE(http::Version{otherMajor}.isValid()) << otherMajor;
  }
  EXPECT_FALSE(http::Version::OfMajor<2>("HTTP/1.1", 8).isValid());

  for (std::string_view malformed : {
           "",
           "HTTP/1.",
           "HTTP/1.10",
           "HTTP/1.1 ",
           "XTTP/1.1",
           "http/1.1",
           "HTTP/1,1",
           "HTTP/1x1",
           "HTTP/1.x",
           "HTTP/1./",
           "HTTP/1.:",
           "HTTP/1.\xFF",
       }) {
    EXPECT_FALSE(http::Version::OfMajor<1>(malformed.data(), malformed.size()).isValid()) << malformed;
  }
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
  EXPECT_EQ(static_cast<std::size_t>(endPtr - buf), sv.size());
  EXPECT_EQ(sv, "HTTP/1.1");

  http::Version vers2{2, 0};
  endPtr = vers2.writeFull(buf);
  EXPECT_EQ(static_cast<std::size_t>(endPtr - buf), sv.size());
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