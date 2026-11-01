#include "../src/trace-echo.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

constexpr std::size_t kMaxHeaderBytes = 8192;

struct CopyResult {
  bool complete;
  std::string echo;
};

CopyResult Copy(std::string_view input, std::size_t maxHeaderBytes = kMaxHeaderBytes) {
  RawChars32 echo;
  echo.append(std::string_view("stale echo of a previous request"));
  const bool complete = CopyTraceEcho(input, maxHeaderBytes, echo);
  return {complete, std::string(std::string_view(echo))};
}

}  // namespace

TEST(TraceEchoTest, CopiesTheHeadAsReceived) {
  // Neither decoded, nor lower-cased, nor merged: the parsing of the head does all this in place.
  static constexpr std::string_view kHead =
      "TRACE /a%20b?x=%41&y=%2F HTTP/1.1\r\n"
      "Host: Example.COM\r\n"
      "X-Mixed-Case: Value With  Spaces \r\n"
      "X-Dup: a\r\n"
      "Content-Encoding: GZIP\r\n"
      "X-Dup: b\r\n"
      "\r\n";
  const auto [complete, echo] = Copy(kHead);
  EXPECT_TRUE(complete);
  EXPECT_EQ(echo, kHead);
}

TEST(TraceEchoTest, AbsoluteFormTarget) {
  static constexpr std::string_view kHead = "TRACE http://user@Example.com:8080?q=%7E HTTP/1.1\r\nHost: other\r\n\r\n";
  EXPECT_EQ(Copy(kHead).echo, kHead);
}

TEST(TraceEchoTest, WithoutHeaderFields) {
  static constexpr std::string_view kHead = "TRACE / HTTP/1.0\r\n\r\n";
  EXPECT_EQ(Copy(kHead).echo, kHead);
}

TEST(TraceEchoTest, LeavesOutTheFieldsLikelyToContainSensitiveData) {
  const auto [complete, echo] = Copy(
      "TRACE /p HTTP/1.1\r\n"
      "Cookie: session=secret\r\n"
      "Host: h\r\n"
      "authorization: Basic Zm9vOmJhcg==\r\n"
      "AUTHORIZATION: Bearer token\r\n"
      "X-Keep: 1\r\n"
      "Proxy-Authorization: Basic eDp5\r\n"
      "COOKIE: other=secret\r\n"
      "coOKie: third\r\n"
      "\r\n");
  EXPECT_TRUE(complete);
  EXPECT_EQ(echo,
            "TRACE /p HTTP/1.1\r\n"
            "Host: h\r\n"
            "X-Keep: 1\r\n"
            "\r\n");
}

TEST(TraceEchoTest, KeepsTheFieldsWhoseNameOnlyLooksSensitive) {
  static constexpr std::string_view kHead =
      "TRACE / HTTP/1.1\r\n"
      "Host: h\r\n"
      "Cookie2: $Version=1\r\n"
      "X-Cookie: a\r\n"
      "Cookies: b\r\n"
      "Set-Cookie: c=d\r\n"
      "Authorization-Info: nextnonce=x\r\n"
      "Proxy-Authenticate: Basic\r\n"
      "X-Forwarded-For: Cookie: x\r\n"
      "\r\n";
  EXPECT_EQ(Copy(kHead).echo, kHead);
}

TEST(TraceEchoTest, LeavesOutTheLastFieldBeforeTheEmptyLine) {
  EXPECT_EQ(Copy("TRACE / HTTP/1.1\r\nHost: h\r\nCookie: a=b\r\n\r\n").echo, "TRACE / HTTP/1.1\r\nHost: h\r\n\r\n");
  EXPECT_EQ(Copy("TRACE / HTTP/1.1\r\nCookie:\r\n\r\n").echo, "TRACE / HTTP/1.1\r\n\r\n");
}

TEST(TraceEchoTest, MalformedLinesAreCopiedAsIs) {
  // Rejected by the parsing of the head: never echoed, but the copy must stay in its bounds.
  static constexpr std::string_view kHead =
      "TRACE / HTTP/1.1\r\n"
      "NoColon\r\n"
      "X-Bare-CR: a\rb\r\n"
      "Cookie : spaced\r\n"
      " Cookie: folded\r\n"
      ":\r\n"
      "\r\n";
  EXPECT_EQ(Copy(kHead).echo, kHead);
  EXPECT_EQ(Copy("TRACE \r\n\r\n").echo, "TRACE \r\n\r\n");
}

TEST(TraceEchoTest, StopsAtTheEndOfTheHead) {
  static constexpr std::string_view kHead = "TRACE /first HTTP/1.1\r\nHost: h\r\nContent-Length: 6\r\n\r\n";
  const auto [complete, echo] =
      Copy(std::format("{}\r\n\r\nX\r\nTRACE /second HTTP/1.1\r\nHost: h\r\nX-Second: 1\r\n\r\n", kHead));
  EXPECT_TRUE(complete);
  EXPECT_EQ(echo, kHead);
}

TEST(TraceEchoTest, OtherMethodsAreNotCopied) {
  // The method is case-sensitive (RFC 9110 §9.1).
  for (std::string_view input : {
           "GET / HTTP/1.1\r\nHost: h\r\n\r\n",
           "trace / HTTP/1.1\r\nHost: h\r\n\r\n",
           "TRACEX / HTTP/1.1\r\nHost: h\r\n\r\n",
           "OPTIONS * HTTP/1.1\r\nHost: h\r\n",
           " TRACE / HTTP/1.1\r\n\r\n",
           "TRACE\t/ HTTP/1.1\r\n\r\n",
       }) {
    SCOPED_TRACE(input);
    const auto [complete, echo] = Copy(input);
    EXPECT_TRUE(complete);
    EXPECT_TRUE(echo.empty());
  }
}

TEST(TraceEchoTest, WaitsForTheWholeHead) {
  static constexpr std::string_view kHead = "TRACE /a HTTP/1.1\r\nHost: h\r\nCookie: c\r\nX-Last: z\r\n\r\n";
  for (std::size_t size = std::string_view("TRACE ").size(); size < kHead.size(); ++size) {
    SCOPED_TRACE(size);
    const auto [complete, echo] = Copy(kHead.substr(0, size));
    EXPECT_FALSE(complete);
    EXPECT_TRUE(echo.empty());
  }
  EXPECT_EQ(Copy(kHead).echo, "TRACE /a HTTP/1.1\r\nHost: h\r\nX-Last: z\r\n\r\n");
}

TEST(TraceEchoTest, HeadLongerThanTheLimit) {
  static constexpr std::string_view kHead = "TRACE /limit HTTP/1.1\r\nHost: h\r\n\r\n";
  // The largest head accepted takes maxHeaderBytes, its empty line included.
  EXPECT_EQ(Copy(kHead, kHead.size()).echo, kHead);

  // Longer: no echo, but its parsing must not wait, as it rejects it.
  for (std::size_t maxHeaderBytes : {kHead.size() - 1, kHead.size() - 2, std::size_t{8}}) {
    SCOPED_TRACE(maxHeaderBytes);
    const auto [complete, echo] = Copy(kHead, maxHeaderBytes);
    EXPECT_TRUE(complete);
    EXPECT_TRUE(echo.empty());
  }

  // Incomplete: waits until it reaches the limit.
  const std::string_view incomplete = kHead.substr(0, kHead.size() - 1);
  EXPECT_FALSE(Copy(incomplete, incomplete.size() + 1).complete);
  EXPECT_TRUE(Copy(incomplete, incomplete.size()).complete);
  EXPECT_TRUE(Copy(incomplete, incomplete.size() - 1).echo.empty());
}

TEST(TraceEchoTest, HeadWithABareLfDoesNotWait) {
  // Its parsing rejects it: waiting for the rest of a head that can no longer be valid would last until its timeout.
  for (std::string_view input : {
           "TRACE /a HTTP/1.1\nHost: h\n\n",
           "TRACE /a HTTP/1.1\nHost: h\n",
           "TRACE /a HTTP/1.1\n",
           "TRACE /a HTTP/1.1\r\nHost: h\n\n",
           "TRACE /a HTTP/1.1\r\nHost: h\r\nX: v\n",
           "TRACE /a HTTP/1.1\r\nHost: h\r\n\n",
           "TRACE /a HTTP/1.1\r\nHo\nst: h\r\n",
       }) {
    SCOPED_TRACE(input);
    const auto [complete, echo] = Copy(input);
    EXPECT_TRUE(complete);
    EXPECT_TRUE(echo.empty());
  }

  // A CR and its LF received apart are a CRLF: the head is waited for.
  for (std::string_view input :
       {"TRACE /a HTTP/1.1\r", "TRACE /a HTTP/1.1\r\nHost: h\r", "TRACE /a HTTP/1.1\r\nHost: h\r\n\r"}) {
    SCOPED_TRACE(input);
    EXPECT_FALSE(Copy(input).complete);
  }
}

TEST(TraceEchoTest, LargeHead) {
  static constexpr std::size_t kLargeMaxHeaderBytes = 1UL << 20U;
  std::string head = "TRACE /large HTTP/1.1\r\nHost: h\r\n";
  std::string expected = head;
  for (int fieldPos = 0; head.size() < kLargeMaxHeaderBytes - 512; ++fieldPos) {
    const std::string value(static_cast<std::size_t>(fieldPos % 97), static_cast<char>('a' + (fieldPos % 26)));
    if (fieldPos % 3 == 0) {
      head += std::format("{}: {}\r\n", fieldPos % 2 == 0 ? "Cookie" : "Authorization", value);
    } else {
      const std::string line = std::format("X-Field-{}: {}\r\n", fieldPos, value);
      head += line;
      expected += line;
    }
  }
  head += "\r\n";
  expected += "\r\n";

  const auto [complete, echo] = Copy(head + "GET /next HTTP/1.1\r\n\r\n", kLargeMaxHeaderBytes);
  EXPECT_TRUE(complete);
  EXPECT_EQ(echo, expected);

  EXPECT_FALSE(Copy(std::string_view(head).substr(0, head.size() - 1), kLargeMaxHeaderBytes).complete);
}

}  // namespace aeronet