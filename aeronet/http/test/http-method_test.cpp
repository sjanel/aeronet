#include "aeronet/http-method.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "../src/http-method-parse.hpp"
#include "aeronet/toupperlower.hpp"

namespace {

using aeronet::http::IsIdempotent;
using aeronet::http::IsMethodIdxSet;
using aeronet::http::Method;
using aeronet::http::MethodBmp;
using aeronet::http::MethodFromIdx;
using aeronet::http::MethodToIdx;
using aeronet::http::MethodToStr;
using aeronet::http::ParseMethodStr;

struct MethodCase {
  Method method;
  std::string_view token;
};

constexpr MethodCase kMethodCases[] = {
    {Method::GET, "GET"},         {Method::HEAD, "HEAD"},     {Method::POST, "POST"},
    {Method::PUT, "PUT"},         {Method::DELETE, "DELETE"}, {Method::CONNECT, "CONNECT"},
    {Method::OPTIONS, "OPTIONS"}, {Method::TRACE, "TRACE"},   {Method::PATCH, "PATCH"},
};

std::string ToLower(std::string_view token) {
  std::string lower;
  lower.reserve(token.size());
  for (char ch : token) {
    lower.push_back(aeronet::tolower(ch));
  }
  return lower;
}

std::string AlternateCase(std::string_view token) {
  std::string mixed;
  mixed.reserve(token.size());
  for (std::size_t i = 0; i < token.size(); ++i) {
    const char ch = token[i];
    mixed.push_back(i % 2 == 0 ? aeronet::tolower(ch) : aeronet::toupper(ch));
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

TEST(HttpMethod, OperatorBitOr) {
  MethodBmp combined = Method::GET | Method::POST | Method::TRACE;
  EXPECT_TRUE(IsMethodSet(combined, Method::GET));
  EXPECT_TRUE(IsMethodSet(combined, Method::POST));
  EXPECT_TRUE(IsMethodSet(combined, Method::TRACE));
  EXPECT_FALSE(IsMethodSet(combined, Method::PUT));
  EXPECT_FALSE(IsMethodSet(combined, Method::DELETE));
  EXPECT_FALSE(IsMethodSet(combined, Method::CONNECT));
  EXPECT_FALSE(IsMethodSet(combined, Method::OPTIONS));
  EXPECT_FALSE(IsMethodSet(combined, Method::HEAD));
  EXPECT_FALSE(IsMethodSet(combined, Method::PATCH));

  auto result = Method::CONNECT | combined;
  EXPECT_TRUE(IsMethodSet(result, Method::CONNECT));
  EXPECT_TRUE(IsMethodSet(result, Method::GET));
  EXPECT_TRUE(IsMethodSet(result, Method::POST));
  EXPECT_TRUE(IsMethodSet(result, Method::TRACE));
  EXPECT_FALSE(IsMethodSet(result, Method::PUT));
  EXPECT_FALSE(IsMethodSet(result, Method::DELETE));
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
    const auto canonical = ParseMethodStr(methodCase.token);
    EXPECT_EQ(canonical, methodCase.method);

    const auto lower = ParseMethodStr(ToLower(methodCase.token));
    EXPECT_EQ(lower, methodCase.method);

    const auto mixed = ParseMethodStr(AlternateCase(methodCase.token));
    EXPECT_EQ(mixed, methodCase.method);
  }
}

TEST(HttpMethodParse, RejectsInvalidTokens) {
  const std::string_view invalid[]{"", "GE", "POSTS", "OPTIONS ", "tracee", "123"};
  for (auto token : invalid) {
    EXPECT_EQ(ParseMethodStr(token), aeronet::http::kMethodInvalid) << token;
  }
}

TEST(HttpMethodParse, RejectsNearMissTokensWithSameLength) {
  static constexpr std::string_view nearMiss[]{
      "GXT",      // size 3, same prefix as GET but mismatched letters
      "P0T",      // size 3, same first letter as PUT
      "HEAe",     // size 4, starts with H but not HEAD
      "P0ST",     // size 4, corrupted POST
      "TEST",     // size 4, starts with T but not TEST
      "TRACX",    // size 5, close to TRACE
      "PATCX",    // size 5, starts with P but not PATCH
      "SALUT",    // size 6, invalid method of correct length
      "CONNECX",  // size 7, starts with C but not CONNECT
      "OPTIONX",  // size 7, starts with O but not OPTIONS
      "APTIONS",  // size 7 does not start with C or O
  };

  for (auto token : nearMiss) {
    EXPECT_EQ(ParseMethodStr(token), aeronet::http::kMethodInvalid) << token;
  }

  EXPECT_EQ(ParseMethodStr("UNKNOWN"), aeronet::http::kMethodInvalid);  // length 7 default branch
}

TEST(HttpMethod, IdempotencyMatchesRfc) {
  EXPECT_TRUE(IsIdempotent(Method::GET));
  EXPECT_TRUE(IsIdempotent(Method::HEAD));
  EXPECT_TRUE(IsIdempotent(Method::PUT));
  EXPECT_TRUE(IsIdempotent(Method::DELETE));
  EXPECT_TRUE(IsIdempotent(Method::OPTIONS));
  EXPECT_TRUE(IsIdempotent(Method::TRACE));
  EXPECT_FALSE(IsIdempotent(Method::POST));
  EXPECT_FALSE(IsIdempotent(Method::PATCH));
  EXPECT_FALSE(IsIdempotent(Method::CONNECT));
}