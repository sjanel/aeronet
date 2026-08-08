#include "aeronet/lower-ascii-key.hpp"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace aeronet {
// ---------------------------------------------------------------------------
// Default construction
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, DefaultConstructedIsEmpty) {
  constexpr LowerAsciiKey key;
  EXPECT_TRUE(key.empty());
  EXPECT_EQ(key.size(), 0U);
  EXPECT_EQ(key.get(), std::string_view());
}

TEST(LowerAsciiKeyTest, DefaultConstructedIsUsableInConstantExpression) {
  // The default constructor is `constexpr`; make sure that's actually true and not just
  // syntactically declared as such.
  static_assert(LowerAsciiKey{}.empty(), "default LowerAsciiKey must be empty at compile time");
  // NOLINTNEXTLINE(readability-container-size-empty)
  static_assert(LowerAsciiKey{}.size() == 0, "default LowerAsciiKey must have size 0 at compile time");
}

// ---------------------------------------------------------------------------
// Construction from a string literal (consteval, path 1 in the class docs)
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, ConstructFromLowercaseLiteral) {
  constexpr LowerAsciiKey key = "content-type";
  EXPECT_EQ(key.get(), "content-type");
  EXPECT_EQ(key.size(), 12U);
  EXPECT_FALSE(key.empty());
}

TEST(LowerAsciiKeyTest, ConstructFromLiteralIsFullyConstantEvaluated) {
  // Validates that both construction and the upper-case check happen at compile time.
  static_assert(LowerAsciiKey{"accept-encoding"}.size() == 15, "size mismatch");
  static_assert(!LowerAsciiKey{"accept-encoding"}.empty(), "must not be empty");
  static_assert(LowerAsciiKey{"accept-encoding"}.get() == "accept-encoding", "content mismatch");
}

TEST(LowerAsciiKeyTest, ConstructFromEmptyLiteral) {
  constexpr LowerAsciiKey key = "";
  EXPECT_TRUE(key.empty());
  EXPECT_EQ(key.size(), 0U);
}

TEST(LowerAsciiKeyTest, ConstructFromSingleCharacterLiteral) {
  constexpr LowerAsciiKey key = "a";
  EXPECT_EQ(key.size(), 1U);
  EXPECT_EQ(key.get(), "a");
}

TEST(LowerAsciiKeyTest, ConstructFromLiteralWithDigitsAndSymbols) {
  // Only ASCII 'A'-'Z' is rejected; digits and punctuation are always fine, lower-case or not.
  constexpr LowerAsciiKey key = "x-request-id-42:/._~";
  EXPECT_EQ(key.get(), "x-request-id-42:/._~");
}

TEST(LowerAsciiKeyTest, LiteralConstructorIsImplicit) {
  // The whole point of the consteval literal constructor is that it is usable without an
  // explicit cast, e.g. directly as a function argument.
  auto identity = [](LowerAsciiKey key) { return key; };
  constexpr LowerAsciiKey key = identity("cache-control");
  EXPECT_EQ(key.get(), "cache-control");
}

// NOTE: the following is intentionally left uncompiled -- by design it must NOT compile, so it
// cannot be expressed as a runtime gtest case. Kept here only as documentation of the enforced
// compile-time contract (construction path 1):
//
//   constexpr LowerAsciiKey bad = "Content-Type";  // <-- hard compile error: 'C' and 'T' are upper-case
//
// Verifying this would require a dedicated "must not compile" build target, which is out of
// scope for a gtest suite that must itself compile and run.

// ---------------------------------------------------------------------------
// Construction from a dynamic std::string_view (runtime/assert-checked, path 2)
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, ConstructFromDynamicLowercaseStringView) {
  std::string str = "x-forwarded-for";
  LowerAsciiKey key{std::string_view(str)};
  EXPECT_EQ(key.get(), "x-forwarded-for");
  EXPECT_EQ(key.size(), str.size());
}

TEST(LowerAsciiKeyTest, ConstructFromDynamicEmptyStringView) {
  LowerAsciiKey key{std::string_view{}};
  EXPECT_TRUE(key.empty());
}

TEST(LowerAsciiKeyTest, ConstructFromDynamicStringViewWithDigitsAndSymbols) {
  std::string str = "trace-id-007";
  LowerAsciiKey key{std::string_view(str)};
  EXPECT_EQ(key.get(), "trace-id-007");
}

TEST(LowerAsciiKeyTest, ConstructorFromStringViewIsExplicitNotImplicit) {
  // Compile-time property: the std::string_view constructor must not allow implicit conversion,
  // which is exactly what makes every runtime-checked-only call site visually distinct.
  static_assert(!std::is_convertible_v<std::string_view, LowerAsciiKey>,
                "constructor taking std::string_view must be explicit");
  static_assert(std::is_constructible_v<LowerAsciiKey, std::string_view>,
                "LowerAsciiKey must still be explicitly constructible from std::string_view");
}

TEST(LowerAsciiKeyTest, BoundaryLowercaseLettersDoNotAssert) {
  // 'a' (0x61) and 'z' (0x7A) bound the lower-case range and must never be flagged.
  std::string_view sv = "az";
  LowerAsciiKey key{sv};
  EXPECT_EQ(key.get(), sv);
}

TEST(LowerAsciiKeyTest, CharactersJustOutsideUppercaseRangeDoNotAssert) {
  // '@' (0x40) sits immediately below 'A' (0x41), and '[' (0x5B) immediately above 'Z' (0x5A);
  // neither should be misclassified as upper-case ASCII.
  std::string_view sv = "@[";
  LowerAsciiKey key{sv};
  EXPECT_EQ(key.get(), sv);
}

#ifndef NDEBUG

TEST(LowerAsciiKeyDeathTest, ConstructFromDynamicUppercaseStringViewAssertsInDebug) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937_64 gen{47};
  std::uniform_int_distribution<uint32_t> lowerCase('a', 'z');
  std::uniform_int_distribution<uint32_t> upperCase('A', 'Z');

  std::string str(10, 'x');
  for (char& ii : str) {
    ii = static_cast<char>(lowerCase(gen));
  }

  LowerAsciiKey key(str);  // OK

  // Now inject an upper-case letter and verify that the assert() fires.
  str.back() = static_cast<char>(upperCase(gen));

  EXPECT_DEATH({ [[maybe_unused]] LowerAsciiKey key(str); }, "lower-case already");
}

#endif  // NDEBUG

// ---------------------------------------------------------------------------
// Accessors: get() / empty() / data() / size()
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, DataPointsIntoOriginalBuffer) {
  std::string str = "if-none-match";
  LowerAsciiKey key{std::string_view(str)};
  // No copy is made: the view must alias the original buffer.
  EXPECT_EQ(key.data(), str.data());
}

TEST(LowerAsciiKeyTest, GetReturnsEquivalentStringView) {
  constexpr LowerAsciiKey key = "etag";
  std::string_view sv = key.get();
  EXPECT_EQ(sv, "etag");
}

TEST(LowerAsciiKeyTest, SizeMatchesContentLength) {
  constexpr LowerAsciiKey key = "user-agent";
  EXPECT_EQ(key.size(), std::string_view("user-agent").size());
}

TEST(LowerAsciiKeyTest, EmptyReflectsZeroSize) {
  constexpr LowerAsciiKey nonEmpty = "a";
  constexpr LowerAsciiKey isEmpty = "";
  EXPECT_FALSE(nonEmpty.empty());
  EXPECT_TRUE(isEmpty.empty());
}

// ---------------------------------------------------------------------------
// Equality operator
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, EqualityBetweenIdenticalContent) {
  constexpr LowerAsciiKey lhs = "content-length";
  constexpr LowerAsciiKey rhs = "content-length";
  EXPECT_TRUE(lhs == rhs);
}

TEST(LowerAsciiKeyTest, InequalityBetweenDifferentContent) {
  constexpr LowerAsciiKey lhs = "content-length";
  constexpr LowerAsciiKey rhs = "content-type";
  EXPECT_FALSE(lhs == rhs);
}

TEST(LowerAsciiKeyTest, EqualityComparesContentNotAddress) {
  // Two distinct buffers holding the same characters must still compare equal: operator== is
  // defaulted on std::string_view, which compares content, not identity.
  std::string s1 = "keep-alive";
  std::string s2 = "keep-alive";
  ASSERT_NE(static_cast<const void*>(s1.data()), static_cast<const void*>(s2.data()));

  LowerAsciiKey k1{std::string_view(s1)};
  LowerAsciiKey k2{std::string_view(s2)};
  EXPECT_TRUE(k1 == k2);
}

TEST(LowerAsciiKeyTest, EqualityBetweenTwoEmptyKeys) {
  constexpr LowerAsciiKey lhs;
  constexpr LowerAsciiKey rhs = "";
  EXPECT_TRUE(lhs == rhs);
}

TEST(LowerAsciiKeyTest, EqualityDistinguishesDifferentSingleCharacters) {
  constexpr LowerAsciiKey lhs = "a";
  constexpr LowerAsciiKey rhs = "b";
  EXPECT_FALSE(lhs == rhs);
}

TEST(LowerAsciiKeyTest, LiteralAndDynamicKeysWithSameContentCompareEqual) {
  constexpr LowerAsciiKey fromLiteral = "transfer-encoding";
  std::string dynamicStr = "transfer-encoding";
  LowerAsciiKey fromDynamic{std::string_view(dynamicStr)};
  EXPECT_TRUE(fromLiteral == fromDynamic);
}

// ---------------------------------------------------------------------------
// Implicit conversion to std::string_view
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, ImplicitConversionToStringView) {
  constexpr LowerAsciiKey key = "connection";
  std::string_view sv = key;  // relies on the implicit `operator std::string_view()`
  EXPECT_EQ(sv, "connection");
}

TEST(LowerAsciiKeyTest, ConversionOperatorIsImplicit) {
  static_assert(std::is_convertible_v<LowerAsciiKey, std::string_view>,
                "operator std::string_view() must be implicit (non-explicit)");
}

TEST(LowerAsciiKeyTest, UsableWherePlainStringViewParameterExpected) {
  constexpr LowerAsciiKey key = "vary";
  auto takesStringView = [](std::string_view sv) { return std::string(sv); };
  EXPECT_EQ(takesStringView(key), "vary");
}

TEST(LowerAsciiKeyTest, UsableAsHeterogeneousUnorderedMapKey) {
  std::unordered_map<std::string_view, int> headers;
  headers.emplace("host", 1);
  headers.emplace("accept", 2);

  constexpr LowerAsciiKey lookupKey = "host";
  auto it = headers.find(lookupKey);  // implicit conversion to std::string_view
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(it->second, 1);
}

TEST(LowerAsciiKeyTest, UsableAsOrderedMapKey) {
  std::map<std::string_view, int> headers{{"accept", 1}, {"host", 2}};
  constexpr LowerAsciiKey lookupKey = "accept";
  auto it = headers.find(lookupKey);  // implicit conversion to std::string_view
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(it->second, 1);
}

// ---------------------------------------------------------------------------
// Whole-API constexpr sanity check
// ---------------------------------------------------------------------------

TEST(LowerAsciiKeyTest, AllAccessorsAndOperatorsAreConstexprFriendly) {
  static_assert(LowerAsciiKey{"range"}.size() == 5);
  static_assert(!LowerAsciiKey{"range"}.empty());
  static_assert(LowerAsciiKey{"range"}.get() == "range");
  static_assert(LowerAsciiKey{"range"} == LowerAsciiKey{"range"});
  static_assert(!(LowerAsciiKey{"range"} == LowerAsciiKey{"host"}));
  static_assert(static_cast<std::string_view>(LowerAsciiKey{"range"}) == "range");
}

}  // namespace aeronet