#include "simple-charconv.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace aeronet {

TEST(SimpleCharConv, Write2) {
  char buf[2];
  auto ptr = write2(buf, 7);
  ASSERT_EQ(ptr - buf, 2);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "07");

  ptr = write2(buf, 89);
  ASSERT_EQ(ptr - buf, 2);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "89");
}

TEST(SimpleCharConv, Write3) {
  char buf[3];
  auto ptr = write3(buf, 7);
  ASSERT_EQ(ptr - buf, 3);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "007");

  ptr = write3(buf, 89);
  ASSERT_EQ(ptr - buf, 3);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "089");

  ptr = write3(buf, 187);
  ASSERT_EQ(ptr - buf, 3);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "187");
}

TEST(SimpleCharConv, Write4) {
  char buf[4];
  auto ptr = write4(buf, 7);
  ASSERT_EQ(ptr - buf, 4);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "0007");

  ptr = write4(buf, 89);
  ASSERT_EQ(ptr - buf, 4);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "0089");

  ptr = write4(buf, 187);
  ASSERT_EQ(ptr - buf, 4);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "0187");

  ptr = write4(buf, 9876);
  ASSERT_EQ(ptr - buf, 4);
  EXPECT_EQ(std::string_view(buf, sizeof(buf)), "9876");
}

TEST(SimpleCharConv, WriteFixedWidthChar) {
  char buf[16];
  char* ptr = buf;
  ptr = write2(ptr, 7);     // 07
  ptr = write3(ptr, 123);   // 123
  ptr = write4(ptr, 9876);  // 9876
  ASSERT_EQ(ptr - buf, 2 + 3 + 4);
  std::string out(buf, ptr);
  EXPECT_EQ(out,
            "07"
            "123"
            "9876");
}

// Removed std::byte buffer variant: write helpers now intentionally operate on char* only.

TEST(SimpleCharConv, ReadFixedWidth) {
  const char* digits = "071239876";    // 2 + 3 + 4 = 9 chars
  EXPECT_EQ(read2(digits), 7);         // 07
  EXPECT_EQ(read3(digits + 2), 123);   // 123
  EXPECT_EQ(read4(digits + 5), 9876);  // 9876
}

TEST(SimpleCharConv, ReadLargerWidths) {
  const char* d6 = "123456";     // 6 digits
  const char* d9 = "987654321";  // 9 digits
  EXPECT_EQ(read6(d6), 123456);
  EXPECT_EQ(read9(d9), 987654321);
}

TEST(SimpleCharConv, Copy3) {
  char buf[4];
  char* ptr = buf;
  ptr = copy3(ptr, std::string_view{"XYZ"});
  *ptr = '\0';
  EXPECT_STREQ(buf, "XYZ");
}

}  // namespace aeronet
