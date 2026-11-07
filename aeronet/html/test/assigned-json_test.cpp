#include "../src/assigned-json.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace aeronet::internal {

TEST(FindAssignedJson, ObjectAssignedToProperty) {
  EXPECT_EQ(FindAssignedJson(R"(window.__INITIAL_PROPS__={"a":1,"b":[2,3]};)", "window.__INITIAL_PROPS__"),
            R"({"a":1,"b":[2,3]})");
}

TEST(FindAssignedJson, ArrayWithWhitespaceAroundTheEqualSign) {
  EXPECT_EQ(FindAssignedJson("var data \t=\n [1, {\"x\": []}]\n", "data"), R"([1, {"x": []}])");
  EXPECT_EQ(FindAssignedJson("data\r\f=\v[]", "data"), "[]");
}

TEST(FindAssignedJson, VariableAtTheStartOfTheScript) { EXPECT_EQ(FindAssignedJson("x={}", "x"), "{}"); }

TEST(FindAssignedJson, PropertyNameMatchesTheFullMemberExpression) {
  // '.' is not an identifier character: the last part of a member expression can be given alone.
  EXPECT_EQ(FindAssignedJson(R"(window.__DATA__ = {"k":"v"})", "__DATA__"), R"({"k":"v"})");
}

TEST(FindAssignedJson, PrecededByAnIdentifierCharacterIsAnotherVariable) {
  EXPECT_EQ(FindAssignedJson("mydata = {}", "data"), "");
  EXPECT_EQ(FindAssignedJson("my_data = {}", "data"), "");
  EXPECT_EQ(FindAssignedJson("$data = {}", "data"), "");
  EXPECT_EQ(FindAssignedJson("x1data = {}", "data"), "");
  EXPECT_EQ(FindAssignedJson("Xdata = {}", "data"), "");
  EXPECT_EQ(FindAssignedJson("\xC3\xA9"
                             "data = {}",
                             "data"),
            "");
  EXPECT_EQ(FindAssignedJson("mydata = {}; data = [1]", "data"), "[1]");
}

TEST(FindAssignedJson, FollowedByAnIdentifierCharacterIsAnotherVariable) {
  EXPECT_EQ(FindAssignedJson("dataset = {}; data.x = {}; data = [2]", "data"), "[2]");
}

TEST(FindAssignedJson, ComparisonIsNotAnAssignment) {
  EXPECT_EQ(FindAssignedJson("if (x == {}) {} x === []; x = {\"ok\":true}", "x"), R"({"ok":true})");
}

TEST(FindAssignedJson, ValueThatIsNotAnObjectOrArray) {
  EXPECT_EQ(FindAssignedJson("x = 5; x = \"s\"; x = {\"n\":1}", "x"), R"({"n":1})");
  EXPECT_EQ(FindAssignedJson("x = null", "x"), "");
}

TEST(FindAssignedJson, BracketsAndQuotesInStringsAreSkipped) {
  static constexpr std::string_view kScript = R"(x = {"s":"}]\"{[","t":'\'}',"u":"\\"};)";
  EXPECT_EQ(FindAssignedJson(kScript, "x"), R"({"s":"}]\"{[","t":'\'}',"u":"\\"})");
}

TEST(FindAssignedJson, NestedStructures) {
  EXPECT_EQ(FindAssignedJson(R"(x = {"a":[{"b":{"c":[[]]}}]} + 1)", "x"), R"({"a":[{"b":{"c":[[]]}}]})");
}

TEST(FindAssignedJson, UnclosedValue) {
  EXPECT_EQ(FindAssignedJson(R"(x = {"a":[1)", "x"), "");
  EXPECT_EQ(FindAssignedJson(R"(x = {"a":"1)", "x"), "");
  EXPECT_EQ(FindAssignedJson(R"(x = {"a":"1\)", "x"), "");
}

TEST(FindAssignedJson, TruncatedAssignment) {
  EXPECT_EQ(FindAssignedJson("x", "x"), "");
  EXPECT_EQ(FindAssignedJson("x  ", "x"), "");
  EXPECT_EQ(FindAssignedJson("x =", "x"), "");
  EXPECT_EQ(FindAssignedJson("x = ", "x"), "");
}

TEST(FindAssignedJson, Absent) {
  EXPECT_EQ(FindAssignedJson("var y = {};", "x"), "");
  EXPECT_EQ(FindAssignedJson("", "x"), "");
}

TEST(FindAssignedJson, EmptyVariable) { EXPECT_EQ(FindAssignedJson("x = {}", ""), ""); }

TEST(FindAssignedJson, LargeValue) {
  std::string script = "window.__STATE__ = {\"items\":[";
  for (int idx = 0; idx < 100000; ++idx) {
    script += R"({"id":)" + std::to_string(idx) + R"(,"name":"item {[]}"},)";
  }
  script += R"({"id":-1}]};)";
  const std::string_view json = FindAssignedJson(script, "window.__STATE__");
  ASSERT_FALSE(json.empty());
  EXPECT_TRUE(json.starts_with(R"({"items":[{"id":0,)"));
  EXPECT_TRUE(json.ends_with(R"({"id":-1}]})"));
  EXPECT_EQ(json.size(), script.size() - std::string_view("window.__STATE__ = ").size() - 1U);
}

}  // namespace aeronet::internal
