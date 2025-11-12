// Tests for RFC7230 tchar classification helper.
#include "aeronet/tchars.hpp"

#include <gtest/gtest.h>

#include <ios>
#include <string>
#include <string_view>

namespace aeronet {

namespace {
// Helper to assert that every character in str satisfies is_tchar.
void ExpectAllTchars(std::string_view str) {
  for (char ch : str) {
    ASSERT_TRUE(is_tchar(ch)) << "Expected '" << ch << "' (0x" << std::hex
                              << static_cast<int>(static_cast<unsigned char>(ch)) << ") to be a tchar";
  }
}
}  // namespace

TEST(TCharsTest, AllowedPunctuationMatchesSpec) {
  // Explicit punctuation set from RFC7230 (section defining token / tchar)
  //   ! # $ % & ' * + - . ^ _ ` | ~
  ExpectAllTchars("!#$%&'*+-.^_`|~");
}

TEST(TCharsTest, DigitsAllowed) {
  std::string digits;
  for (char ch = '0'; ch <= '9'; ++ch) {
    digits.push_back(ch);
  }
  ExpectAllTchars(digits);
}

TEST(TCharsTest, UpperAlphaAllowed) {
  std::string uppers;
  for (char ch = 'A'; ch <= 'Z'; ++ch) {
    uppers.push_back(ch);
  }
  ExpectAllTchars(uppers);
}

TEST(TCharsTest, LowerAlphaAllowed) {
  std::string lowers;
  for (char ch = 'a'; ch <= 'z'; ++ch) {
    lowers.push_back(ch);
  }
  ExpectAllTchars(lowers);
}

TEST(TCharsTest, DisallowedASCIIExamples) {
  // Space, tab, control chars, and separators like '(', ')', ',', ';', ':', '/', '?', '=' should be false.
  const std::string disallowed = std::string() + ' ' + '\t' + '\n' + '\r' + '(' + ')' + '[' + ']' + '{' + '}' + ',' +
                                 ';' + ':' + '/' + '?' + '=' + '@' + '"' + '<' + '>' + '\\';
  for (char ch : disallowed) {
    EXPECT_FALSE(is_tchar(ch)) << "Unexpectedly classified disallowed char '" << ch << "' (0x" << std::hex
                               << static_cast<int>(static_cast<unsigned char>(ch)) << ") as tchar";
  }
}

TEST(TCharsTest, ExtendedASCIIAlwaysFalse) {
  for (int i = 128; i < 256; ++i) {
    EXPECT_FALSE(is_tchar(static_cast<char>(i))) << "0x" << std::hex << i << " must not be a tchar";
  }
}

TEST(TCharsTest, BoundaryCharacters) {
  // Lowest printable allowed '!' and highest allowed '~'
  EXPECT_TRUE(is_tchar('!'));
  EXPECT_TRUE(is_tchar('~'));
  // Just below first allowed (space) and DEL (0x7F) are not allowed.
  EXPECT_FALSE(is_tchar(' '));
  EXPECT_FALSE(is_tchar(static_cast<char>(0x7F)));
}

TEST(TCharsTest, IdempotentAcrossMultipleCalls) {
  // Spot-check a mix across both bitmap halves to ensure no hidden state.
  const char samples[] = {'A', 'z', '0', '9', '!', '~', '_', '^', '+', '*'};
  for (int i = 0; i < 100; ++i) {
    for (char ch : samples) {
      EXPECT_TRUE(is_tchar(ch));
    }
  }
}

}  // namespace aeronet