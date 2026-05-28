#include "aeronet/route-constraint.hpp"

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <string_view>
#include <type_traits>

#include "aeronet/object-array-pool.hpp"

namespace aeronet {
namespace {

vector<uint32_t> usedPerAtom;
const auto Matches = [](const RouteConstraint& rc, std::string_view value) { return rc.matches(value, usedPerAtom); };

// ============================================================================
// Basic unconstrained behavior
// ============================================================================

TEST(RouteConstraintTest, DefaultConstructedIsUnconstrained) {
  RouteConstraint rc;
  EXPECT_TRUE(rc.empty());
  EXPECT_TRUE(Matches(rc, "anything"));
  EXPECT_TRUE(Matches(rc, ""));
  EXPECT_EQ(rc.pattern(), "");
}

TEST(RouteConstraintTest, EmptyPatternReturnsUnconstrained) {
  RouteConstraint rc;
  EXPECT_TRUE(rc.empty());
  EXPECT_TRUE(Matches(rc, "anything"));
}

TEST(RouteConstraintTest, IsMoveOnly) {
  static_assert(!std::is_copy_constructible_v<RouteConstraint>);
  static_assert(!std::is_copy_assignable_v<RouteConstraint>);
  static_assert(std::is_move_constructible_v<RouteConstraint>);
  static_assert(std::is_move_assignable_v<RouteConstraint>);
}

TEST(RouteConstraintTest, StorageBackedConstructorCopiesPatternIntoPool) {
  ObjectArrayPool<char> charStorage;
  std::string pattern = "[0-9]+";

  RouteConstraint rc(pattern, charStorage);
  pattern[0] = 'x';

  EXPECT_EQ(rc.pattern(), "[0-9]+");
  EXPECT_TRUE(Matches(rc, "123"));
}

// ============================================================================
// Character classes
// ============================================================================

TEST(RouteConstraintTest, DigitClassPlus) {
  auto rc = RouteConstraint("[0-9]+");
  EXPECT_FALSE(rc.empty());
  EXPECT_EQ(rc.pattern(), "[0-9]+");

  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "0"));
  EXPECT_TRUE(Matches(rc, "9999999"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, "12a3"));
  EXPECT_FALSE(Matches(rc, "12 3"));
}

TEST(RouteConstraintTest, AlphaClassPlus) {
  auto rc = RouteConstraint("[a-zA-Z]+");

  EXPECT_TRUE(Matches(rc, "hello"));
  EXPECT_TRUE(Matches(rc, "World"));
  EXPECT_TRUE(Matches(rc, "Z"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "hello123"));
  EXPECT_FALSE(Matches(rc, "hello world"));
}

TEST(RouteConstraintTest, AlphanumericWithSpecialChars) {
  auto rc = RouteConstraint("[a-zA-Z0-9/._-]+");

  EXPECT_TRUE(Matches(rc, "hello/world"));
  EXPECT_TRUE(Matches(rc, "file.txt"));
  EXPECT_TRUE(Matches(rc, "path/to/file_name-v2.0"));
  EXPECT_TRUE(Matches(rc, "a"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "path with spaces"));
  EXPECT_FALSE(Matches(rc, "path@special"));
}

TEST(RouteConstraintTest, NegatedCharClass) {
  auto rc = RouteConstraint("[^/]+");

  EXPECT_TRUE(Matches(rc, "hello"));
  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "a-b_c"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "hello/world"));
  EXPECT_FALSE(Matches(rc, "/"));
}

// ============================================================================
// Quantifiers
// ============================================================================

TEST(RouteConstraintTest, StarQuantifier) {
  auto rc = RouteConstraint("[0-9]*");

  EXPECT_TRUE(Matches(rc, ""));
  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "0"));
}

TEST(RouteConstraintTest, QuestionQuantifier) {
  auto rc = RouteConstraint("[0-9]?");

  EXPECT_TRUE(Matches(rc, ""));
  EXPECT_TRUE(Matches(rc, "5"));
  EXPECT_FALSE(Matches(rc, "55"));
}

TEST(RouteConstraintTest, ExactRepetition) {
  auto rc = RouteConstraint("[0-9]{3}");

  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "000"));
  EXPECT_FALSE(Matches(rc, "12"));
  EXPECT_FALSE(Matches(rc, "1234"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, RangeRepetition) {
  auto rc = RouteConstraint("[0-9]{2,4}");

  EXPECT_FALSE(Matches(rc, "1"));
  EXPECT_TRUE(Matches(rc, "12"));
  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "1234"));
  EXPECT_FALSE(Matches(rc, "12345"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, OpenEndedRepetition) {
  auto rc = RouteConstraint("[a-z]{2,}");

  EXPECT_FALSE(Matches(rc, "a"));
  EXPECT_TRUE(Matches(rc, "ab"));
  EXPECT_TRUE(Matches(rc, "abcdefghijklmnopqrstuvwxyz"));
  EXPECT_FALSE(Matches(rc, ""));
}

// ============================================================================
// Dot (any character)
// ============================================================================

TEST(RouteConstraintTest, DotPlus) {
  auto rc = RouteConstraint(".+");

  EXPECT_TRUE(Matches(rc, "anything"));
  EXPECT_TRUE(Matches(rc, "x"));
  EXPECT_TRUE(Matches(rc, "123!@#"));
  EXPECT_TRUE(Matches(rc, "café"));  // UTF-8 accented character
  EXPECT_FALSE(Matches(rc, "\n"));
  EXPECT_FALSE(Matches(rc, "\r"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, DotExactRepetition) {
  auto rc = RouteConstraint(".{3}");

  EXPECT_TRUE(Matches(rc, "abc"));
  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_FALSE(Matches(rc, "ab"));
  EXPECT_FALSE(Matches(rc, "abcd"));
}

TEST(RouteConstraintTest, ZeroExactRepetitionMatchesOnlyEmpty) {
  auto rc = RouteConstraint("[0-9]{0}");

  EXPECT_TRUE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "0"));
  EXPECT_FALSE(Matches(rc, "123"));
}

// ============================================================================
// Shorthand character classes
// ============================================================================

TEST(RouteConstraintTest, BackslashD) {
  auto rc = RouteConstraint("\\d+");

  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "0"));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, BackslashW) {
  auto rc = RouteConstraint("\\w+");

  EXPECT_TRUE(Matches(rc, "hello_world"));
  EXPECT_TRUE(Matches(rc, "Test123"));
  EXPECT_TRUE(Matches(rc, "_"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "hello world"));
  EXPECT_FALSE(Matches(rc, "hi!"));
}

TEST(RouteConstraintTest, BackslashS) {
  auto rc = RouteConstraint("\\s+");

  EXPECT_TRUE(Matches(rc, " "));
  EXPECT_TRUE(Matches(rc, "\t\n"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "hello"));
}

// ============================================================================
// Mixed patterns (multiple atoms)
// ============================================================================

TEST(RouteConstraintTest, LettersThenDigits) {
  auto rc = RouteConstraint("[a-z]+[0-9]+");

  EXPECT_TRUE(Matches(rc, "abc123"));
  EXPECT_TRUE(Matches(rc, "x1"));
  EXPECT_FALSE(Matches(rc, "123abc"));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, "123"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, LiteralPrefix) {
  auto rc = RouteConstraint("v[0-9]+");

  EXPECT_TRUE(Matches(rc, "v1"));
  EXPECT_TRUE(Matches(rc, "v123"));
  EXPECT_FALSE(Matches(rc, "v"));
  EXPECT_FALSE(Matches(rc, "1"));
  EXPECT_FALSE(Matches(rc, "V1"));
}

TEST(RouteConstraintTest, UuidLikePattern) {
  auto rc = RouteConstraint("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");

  EXPECT_TRUE(Matches(rc, "550e8400-e29b-41d4-a716-446655440000"));
  EXPECT_FALSE(Matches(rc, "550e8400-e29b-41d4-a716-44665544000"));    // too short
  EXPECT_FALSE(Matches(rc, "550e8400-e29b-41d4-a716-4466554400000"));  // too long
  EXPECT_FALSE(Matches(rc, "ZZZZZZZZ-e29b-41d4-a716-446655440000"));   // non-hex
}

// ============================================================================
// std::regex fallback (complex patterns)
// ============================================================================

TEST(RouteConstraintTest, AlternationFallsBackToRegex) {
  auto rc = RouteConstraint("(json|xml|yaml)");

  EXPECT_FALSE(rc.empty());
  EXPECT_TRUE(Matches(rc, "json"));
  EXPECT_TRUE(Matches(rc, "xml"));
  EXPECT_TRUE(Matches(rc, "yaml"));
  EXPECT_FALSE(Matches(rc, "csv"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, PipeAlternationFallsBackToRegex) {
  auto rc = RouteConstraint("foo|bar|baz");

  EXPECT_TRUE(Matches(rc, "foo"));
  EXPECT_TRUE(Matches(rc, "bar"));
  EXPECT_TRUE(Matches(rc, "baz"));
  EXPECT_FALSE(Matches(rc, "qux"));
}

TEST(RouteConstraintTest, LiteralBracesNotQuantifierFallback) {
  // Braces that don't match quantifier shape should fall back to std::regex and match successfully.
  auto rc = RouteConstraint("[0-9]+{abc}");
  EXPECT_FALSE(rc.empty());
  EXPECT_TRUE(Matches(rc, "123{abc}"));
  EXPECT_FALSE(Matches(rc, "123"));
}

// ============================================================================
// Error cases
// ============================================================================

TEST(RouteConstraintTest, UnterminatedCharClassThrows) {
  EXPECT_THROW(RouteConstraint("[0-9"), std::regex_error);
  EXPECT_THROW(RouteConstraint("["), std::regex_error);
}

TEST(RouteConstraintTest, InvalidRegexThrows) {
  // This pattern uses alternation (falls to regex) but has unbalanced parens
  EXPECT_THROW(RouteConstraint("(abc"), std::regex_error);
}

TEST(RouteConstraintTest, MalformedQuantifierThrows) { EXPECT_THROW(RouteConstraint("[0-9]{abc}"), std::regex_error); }

// ============================================================================
// Copy and move semantics
// ============================================================================

TEST(RouteConstraintTest, MoveConstruction) {
  auto rc = RouteConstraint("[0-9]+");
  RouteConstraint moved(std::move(rc));

  EXPECT_FALSE(moved.empty());
  EXPECT_TRUE(Matches(moved, "123"));
}

TEST(RouteConstraintTest, MoveAssignment) {
  auto rc = RouteConstraint("[0-9]+");
  RouteConstraint moved;
  moved = std::move(rc);

  EXPECT_FALSE(moved.empty());
  EXPECT_TRUE(Matches(moved, "123"));
}

// ============================================================================
// Edge cases for the fast matcher
// ============================================================================

TEST(RouteConstraintTest, SingleCharLiteral) {
  auto rc = RouteConstraint("x");

  EXPECT_TRUE(Matches(rc, "x"));
  EXPECT_FALSE(Matches(rc, "y"));
  EXPECT_FALSE(Matches(rc, "xx"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, EscapedDot) {
  auto rc = RouteConstraint("\\.");

  EXPECT_TRUE(Matches(rc, "."));
  EXPECT_FALSE(Matches(rc, "a"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, EscapedBackslash) {
  auto rc = RouteConstraint("\\\\");

  EXPECT_TRUE(Matches(rc, "\\"));
  EXPECT_FALSE(Matches(rc, "a"));
}

TEST(RouteConstraintTest, GreedyBacktracking) {
  // Pattern: [a-z]+b matches "aab" because the greedy [a-z]+ must backtrack
  auto rc = RouteConstraint("[a-z]+b");

  EXPECT_TRUE(Matches(rc, "ab"));
  EXPECT_TRUE(Matches(rc, "aab"));
  EXPECT_TRUE(Matches(rc, "xyzb"));
  EXPECT_FALSE(Matches(rc, "xyz"));
  EXPECT_FALSE(Matches(rc, "b"));  // [a-z]+ needs at least 1 char before 'b'
}

// ============================================================================
// Shorthands inside character classes (coverage)
// ============================================================================

TEST(RouteConstraintTest, ShorthandDigitInsideCharClass) {
  auto rc = RouteConstraint("[\\d]+");

  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_TRUE(Matches(rc, "0"));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, ShorthandWordInsideCharClass) {
  auto rc = RouteConstraint("[\\w]+");

  EXPECT_TRUE(Matches(rc, "hello_123"));
  EXPECT_TRUE(Matches(rc, "A"));
  EXPECT_FALSE(Matches(rc, ""));
  EXPECT_FALSE(Matches(rc, "!@#"));
}

TEST(RouteConstraintTest, ShorthandSpaceInsideCharClass) {
  auto rc = RouteConstraint("[\\s]+");

  EXPECT_TRUE(Matches(rc, " \t\n"));
  EXPECT_TRUE(Matches(rc, " "));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, ""));
}

// ============================================================================
// Negated shorthands (coverage for \D, \W, \S)
// ============================================================================

TEST(RouteConstraintTest, BackslashCapitalD) {
  auto rc = RouteConstraint("\\D+");

  EXPECT_TRUE(Matches(rc, "abc"));
  EXPECT_TRUE(Matches(rc, "!@#"));
  EXPECT_FALSE(Matches(rc, "123"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, BackslashCapitalW) {
  auto rc = RouteConstraint("\\W+");

  EXPECT_TRUE(Matches(rc, "!@#"));
  EXPECT_TRUE(Matches(rc, " "));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, "123"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, BackslashCapitalS) {
  auto rc = RouteConstraint("\\S+");

  EXPECT_TRUE(Matches(rc, "abc"));
  EXPECT_TRUE(Matches(rc, "123!@#"));
  EXPECT_FALSE(Matches(rc, " "));
  EXPECT_FALSE(Matches(rc, "\t"));
  EXPECT_FALSE(Matches(rc, ""));
}

// ============================================================================
// Anchors fall back to regex (coverage for ^ and $)
// ============================================================================

TEST(RouteConstraintTest, AnchorCaretFallsBackToRegex) {
  auto rc = RouteConstraint("^[0-9]+$");

  EXPECT_TRUE(Matches(rc, "123"));
  EXPECT_FALSE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, AnchorDollarFallsBackToRegex) {
  auto rc = RouteConstraint("[a-z]+$");

  EXPECT_TRUE(Matches(rc, "hello"));
  EXPECT_FALSE(Matches(rc, "HELLO"));
}

// ============================================================================
// Escaped range end inside character class (coverage for [a-\n])
// ============================================================================

TEST(RouteConstraintTest, EscapedRangeEndInCharClass) {
  // Range from '!' to '\/' (escaped /)
  auto rc = RouteConstraint("[!-\\/]+");

  EXPECT_TRUE(Matches(rc, "!\"#$%&'()*+,-./"));
  EXPECT_TRUE(Matches(rc, "!"));
  EXPECT_FALSE(Matches(rc, "0"));
  EXPECT_FALSE(Matches(rc, ""));
}

TEST(RouteConstraintTest, EscapedLiteralInsideCharClass) {
  // [\\.] — escaped dot inside char class (literal dot only)
  auto rc = RouteConstraint("[\\\\.]+");

  EXPECT_TRUE(Matches(rc, "..."));
  EXPECT_TRUE(Matches(rc, "."));
  EXPECT_FALSE(Matches(rc, "a"));
  EXPECT_FALSE(Matches(rc, ""));
}

// ============================================================================
// Trailing backslash (coverage)
// ============================================================================

TEST(RouteConstraintTest, TrailingBackslashFallsBackToRegex) {
  // A trailing backslash makes tryCompileFast return nullptr.
  // std::regex will also reject it, so it should throw.
  EXPECT_THROW(RouteConstraint("abc\\"), std::regex_error);
}

// ============================================================================
// Closing paren alone falls back to regex (coverage)
// ============================================================================

TEST(RouteConstraintTest, CloseParenFallsBackToRegex) {
  // ')' without matching '(' — std::regex should reject it
  EXPECT_THROW(RouteConstraint(")abc"), std::regex_error);
}

// ============================================================================
// Backslash at end of character class (coverage: escape inside [] at end of pattern)
// ============================================================================

TEST(RouteConstraintTest, BackslashAtEndOfCharClass) {
  // '[\\' — escape at end of char class without anything following, unterminated
  EXPECT_THROW(RouteConstraint("[\\"), std::regex_error);
}

// ============================================================================
// Escaped range end where pattern is too short (coverage: pos+3 >= size)
// ============================================================================

TEST(RouteConstraintTest, EscapedRangeEndTruncated) {
  // '[a-\\' — range with escaped end but no char after backslash, unterminated
  EXPECT_THROW(RouteConstraint("[a-\\"), std::regex_error);
}

// ============================================================================
// Malformed quantifier: {n,m without closing brace (coverage)
// ============================================================================

TEST(RouteConstraintTest, MalformedQuantifierNoClosingBrace) {
  EXPECT_THROW(RouteConstraint("[a-z]{2,3"), std::regex_error);
}

TEST(RouteConstraintTest, MalformedQuantifierBadChar) {
  // {2,3x} — non-digit, non-} after maxVal
  EXPECT_THROW(RouteConstraint("[a-z]{2,3x}"), std::regex_error);
  // {2,!} — char below '0' after comma, maxVal loop never entered
  EXPECT_THROW(RouteConstraint("[a-z]{2,!}"), std::regex_error);
}

TEST(RouteConstraintTest, MalformedQuantifierTruncatedAfterBrace) {
  // '{' at end of pattern — pos goes past size immediately
  EXPECT_THROW(RouteConstraint("[a-z]{"), std::regex_error);
}

TEST(RouteConstraintTest, MalformedQuantifierTruncatedAfterComma) {
  // '{2,' at end of pattern — comma found but nothing after
  EXPECT_THROW(RouteConstraint("[a-z]{2,"), std::regex_error);
}

// ============================================================================
// Backtracking inner loop exits due to end-of-string (coverage: line 103 False branch)
// ============================================================================

TEST(RouteConstraintTest, BacktrackInnerLoopHitsEndOfString) {
  // Pattern [a-z]+[a-z]+ with value "ab": first atom greedily takes "ab",
  // backtracks to "a", second atom matches "b" and hits end of string.
  auto rc = RouteConstraint("[a-z]+[a-z]+");

  EXPECT_TRUE(Matches(rc, "ab"));
  EXPECT_TRUE(Matches(rc, "abc"));
  EXPECT_FALSE(Matches(rc, "a"));  // can't split 1 char into two +
  EXPECT_FALSE(Matches(rc, ""));
}

}  // namespace
}  // namespace aeronet
