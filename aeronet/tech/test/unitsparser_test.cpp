#include "unitsparser.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "exception.hpp"

namespace aeronet {

TEST(UnitsParser, ParseNumberOfBytes1kMultipliers) {
  EXPECT_EQ(ParseNumberOfBytes("748"), 748L);
  EXPECT_EQ(ParseNumberOfBytes("788999k"), 788999000L);
  EXPECT_EQ(ParseNumberOfBytes("34M"), 34000000L);
  EXPECT_EQ(ParseNumberOfBytes("1G"), 1000000000L);
  EXPECT_EQ(ParseNumberOfBytes("5T"), 5000000000000L);
}

TEST(UnitsParser, ParseNumberOfBytes1024Multipliers) {
  EXPECT_EQ(ParseNumberOfBytes("12"), 12L);
  EXPECT_EQ(ParseNumberOfBytes("3Ki"), 3072L);
  EXPECT_EQ(ParseNumberOfBytes("5Mi"), 5242880L);
  EXPECT_EQ(ParseNumberOfBytes("57Gi"), 61203283968L);
  EXPECT_EQ(ParseNumberOfBytes("2Ti"), 2199023255552L);
}

TEST(UnitsParser, ParseNumberOfBytesSeveralUnits) {
  EXPECT_EQ(ParseNumberOfBytes("58"), 58L);
  EXPECT_EQ(ParseNumberOfBytes("256Ki58"), 262202L);
  EXPECT_EQ(ParseNumberOfBytes("1Mi256Ki58"), 1310778L);
  EXPECT_EQ(ParseNumberOfBytes("988Gi1Mi256Ki58"), 1060858232890L);
  EXPECT_EQ(ParseNumberOfBytes("4Ti988Gi1Mi256Ki58"), 5458904743994L);
}

TEST(UnitsParser, ParseNumberOfBytesInvalidInput) {
  EXPECT_THROW(ParseNumberOfBytes("12.5M"), exception);
  EXPECT_THROW(ParseNumberOfBytes("400m"), exception);
  EXPECT_THROW(ParseNumberOfBytes("-30"), exception);
}

TEST(UnitsParser, BytesToStrBufferTooSmall) {
  char buf[6];
  EXPECT_THROW(BytesToBuffer(123456789, buf), exception);
}

TEST(UnitsParser, BytesToBufferNominalCase) {
  char buf[20];
  auto resultBuf = BytesToBuffer(1060858233000, buf);
  std::string_view resultStr = std::string_view(resultBuf.data(), resultBuf.size());
  EXPECT_EQ(resultStr, "988Gi1Mi256Ki168");
}

TEST(UnitsParser, BytesToStr) {
  EXPECT_EQ(std::string_view(BytesToStr(-262144)), "-256Ki");
  EXPECT_EQ(std::string_view(BytesToStr(3145728)), "3Mi");
  EXPECT_EQ(std::string_view(BytesToStr(1060858233000)), "988Gi1Mi256Ki168");
}

TEST(UnitsParser, BytesToStrLen) {
  EXPECT_EQ(BytesToStrLen(-262144), 6);
  EXPECT_EQ(BytesToStrLen(3145728), 3);
  EXPECT_EQ(BytesToStrLen(1060858233000), 16);
}

}  // namespace aeronet
