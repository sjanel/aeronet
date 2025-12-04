#include "aeronet/static-string-view-helpers.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace aeronet {
struct test1 {
  static_assert(JoinStringView_v<>.empty());
};

struct test2 {
  static constexpr std::string_view kStr1 = "hello";

  static_assert(JoinStringView_v<kStr1> == "hello");
};

struct test3 {
  static constexpr std::string_view kStr1 = "this";
  static constexpr std::string_view kStr2 = " is a ";
  static constexpr std::string_view kStr3 = "composed ";
  static constexpr std::string_view kStr4 = "string";

  static_assert(JoinStringView_v<kStr1, kStr2, kStr3, kStr4> == "this is a composed string");
};

struct test4 {
  static constexpr std::string_view kStr1 = "The letter coming after ";
  static constexpr std::string_view kStr2 = " is ";

  static_assert(JoinStringView_v<kStr1, CharToStringView_v<'g'>, kStr2, CharToStringView_v<'h'>> ==
                "The letter coming after g is h");
};

struct test5 {
  static constexpr std::string_view kStr1 = "I have ";
  static constexpr std::string_view kStr2 = " oranges and ";
  static constexpr std::string_view kStr3 = " bananas ";
  static constexpr std::string_view kStr4 = "in my bag";

  static_assert(JoinStringView_v<kStr1, IntToStringView_v<70>, kStr2, IntToStringView_v<1894>, kStr3, kStr4> ==
                "I have 70 oranges and 1894 bananas in my bag");
};

struct test6 {
  static constexpr std::string_view kSep = "|";
  static constexpr std::string_view kStr1 = "apples";
  static constexpr std::string_view kStr2 = "bananas";
  static constexpr std::string_view kStr3 = "oranges";
  static constexpr std::string_view kStr4 = "blueberries";
  static constexpr std::string_view kStr5 = "strawberries";

  static_assert(JoinStringViewWithSep_v<kSep, kStr1, kStr2, kStr3, kStr4, kStr5> ==
                "apples|bananas|oranges|blueberries|strawberries");

  static constexpr std::string_view kStrArr[] = {"apples", "bananas", "oranges", "blueberries", "strawberries"};

  static_assert(make_joined_string_view<kSep, kStrArr>::value == "apples|bananas|oranges|blueberries|strawberries");
};

// IntToStringView
static_assert(IntToStringView_v<0> == "0");
static_assert(IntToStringView_v<37> == "37");
static_assert(IntToStringView_v<-1273006> == "-1273006");

// Runtime helpers to force materialization for coverage tools.
inline constexpr std::string_view kRT_A = "hello";
inline constexpr std::string_view kRT_B = " world";

TEST(StaticStringViewHelpersRuntime, Materialize) {
  using namespace aeronet;

  using J = JoinStringView<kRT_A, kRT_B>;

  // Force runtime reads of the joined c_str storage
  volatile const char* ptr = J::c_str;
  EXPECT_EQ(ptr[0], 'h');
  EXPECT_EQ(ptr[5], ' ');
  EXPECT_EQ(ptr[10], 'd');

  // Force runtime reads of IntToStringView storage
  volatile const char* p2 = IntToStringView<37>::value.data();
  EXPECT_EQ(p2[0], '3');
}

}  // namespace aeronet
