#include "aeronet/http-method.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include "../src/http-method-parse.hpp"

namespace {

using aeronet::http::IsMethodIdxSet;
using aeronet::http::Method;
using aeronet::http::MethodBmp;
using aeronet::http::MethodFromIdx;
using aeronet::http::MethodStrToOptEnum;
using aeronet::http::MethodToIdx;
using aeronet::http::MethodToStr;

struct MethodCase {
  Method method;
  std::string_view token;
};

constexpr std::array<MethodCase, aeronet::http::kNbMethods> kMethodCases = {{{Method::GET, "GET"},
                                                                             {Method::HEAD, "HEAD"},
                                                                             {Method::POST, "POST"},
                                                                             {Method::PUT, "PUT"},
                                                                             {Method::DELETE, "DELETE"},
                                                                             {Method::CONNECT, "CONNECT"},
                                                                             {Method::OPTIONS, "OPTIONS"},
                                                                             {Method::TRACE, "TRACE"},
                                                                             {Method::PATCH, "PATCH"}}};

std::string ToLower(std::string_view token) {
  std::string lower;
  lower.reserve(token.size());
  for (char ch : token) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    lower.push_back(static_cast<char>(std::tolower(uch)));
  }
  return lower;
}

std::string AlternateCase(std::string_view token) {
  std::string mixed;
  mixed.reserve(token.size());
  for (std::size_t i = 0; i < token.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(token[i]);
    mixed.push_back(i % 2 == 0 ? static_cast<char>(std::tolower(ch)) : static_cast<char>(std::toupper(ch)));
  }
  return mixed;
}

}  // namespace

TEST(HttpMethod, MethodIdxRoundTrip) {
  for (const auto& methodCase : kMethodCases) {
    const auto idx = MethodToIdx(methodCase.method);
    EXPECT_EQ(MethodFromIdx(idx), methodCase.method);
    EXPECT_EQ(MethodToStr(methodCase.method), methodCase.token);
  }
}

TEST(HttpMethod, MethodBitmapOperatorsAndQueries) {
  MethodBmp mask = 0;
  for (const auto& methodCase : kMethodCases) {
    mask = mask | methodCase.method;
  }

  for (const auto& methodCase : kMethodCases) {
    EXPECT_TRUE(IsMethodSet(mask, methodCase.method));
    EXPECT_TRUE(IsMethodIdxSet(mask, MethodToIdx(methodCase.method)));
  }

  const auto traceMask = static_cast<MethodBmp>(Method::TRACE);
  const auto trimmed = static_cast<MethodBmp>(mask & static_cast<MethodBmp>(~traceMask));
  EXPECT_FALSE(IsMethodSet(trimmed, Method::TRACE));
}

TEST(HttpMethod, AllMethodsStringLengthMatchesSum) {
  std::size_t sum = 0;
  for (const auto& methodCase : kMethodCases) {
    sum += methodCase.token.size();
  }
  EXPECT_EQ(sum, aeronet::http::kAllMethodsStrLen);
}

TEST(HttpMethodParse, ParsesTokensCaseInsensitive) {
  for (const auto& methodCase : kMethodCases) {
    const auto canonical = MethodStrToOptEnum(methodCase.token);
    ASSERT_TRUE(canonical.has_value());
    EXPECT_EQ(*canonical, methodCase.method);

    const auto lower = MethodStrToOptEnum(ToLower(methodCase.token));
    ASSERT_TRUE(lower.has_value());
    EXPECT_EQ(*lower, methodCase.method);

    const auto mixed = MethodStrToOptEnum(AlternateCase(methodCase.token));
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(*mixed, methodCase.method);
  }
}

TEST(HttpMethodParse, RejectsInvalidTokens) {
  const std::array<std::string_view, 6> invalid = {"", "GE", "POSTS", "OPTIONS ", "tracee", "123"};
  for (auto token : invalid) {
    EXPECT_FALSE(MethodStrToOptEnum(token).has_value()) << token;
  }
}
