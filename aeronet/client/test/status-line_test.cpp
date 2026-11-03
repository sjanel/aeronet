#include "../src/status-line.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"

namespace aeronet::http {

namespace {

// 'line' is a status line of these parts, and so is every prefix of it holding the status code.
void ExpectValid(std::string_view line, Version version, StatusCode code, std::string_view reason) {
  SCOPED_TRACE(line);
  const StatusLine parsed = ParseStatusLine(line);
  ASSERT_TRUE(parsed.valid());
  EXPECT_EQ(parsed.version, version);
  EXPECT_EQ(parsed.code, code);
  EXPECT_EQ(parsed.reason, reason);
  for (std::size_t prefixSize = kStatusLineMinLen; prefixSize < line.size(); ++prefixSize) {
    const StatusLine prefix = ParseStatusLine(line.substr(0, prefixSize));
    ASSERT_TRUE(prefix.valid()) << prefixSize;
    EXPECT_EQ(prefix.code, code) << prefixSize;
  }
}

// 'line' is not a status line, whatever follows it.
void ExpectInvalid(std::string_view line) {
  SCOPED_TRACE(line);
  for (const std::string& input : {std::string(line), std::string(line).append(" OK")}) {
    const StatusLine parsed = ParseStatusLine(input);
    EXPECT_FALSE(parsed.valid()) << input;
    EXPECT_EQ(parsed.code, 0);
    EXPECT_FALSE(parsed.version.isValid());
    EXPECT_TRUE(parsed.reason.empty());
  }
}

}  // namespace

static_assert(kStatusLineMinLen == std::string_view("HTTP/1.1 200").size());
static_assert(ParseStatusLine("HTTP/1.1 200 OK").code == StatusCodeOK);
static_assert(ParseStatusLine("HTTP/1.1 200 OK").reason == "OK");
static_assert(!ParseStatusLine("HTTP/2.0 200 OK").valid());

TEST(StatusLine, Valid) {
  ExpectValid("HTTP/1.1 200 OK", HTTP_1_1, 200, "OK");
  ExpectValid("HTTP/1.0 404 Not Found", HTTP_1_0, 404, "Not Found");
  ExpectValid("HTTP/1.1 100 Continue", HTTP_1_1, 100, "Continue");
  // RFC 9110 §6.2: a higher minor version is still HTTP/1.
  ExpectValid("HTTP/1.9 599 Last", Version{1, 9}, 599, "Last");
}

// status-line = HTTP-version SP status-code SP [ reason-phrase ] (RFC 9112 §4): the reason phrase is optional, and so
// is the SP before it, which some servers omit.
TEST(StatusLine, WithoutReasonPhrase) {
  ExpectValid("HTTP/1.1 200", HTTP_1_1, 200, "");
  ExpectValid("HTTP/1.1 204 ", HTTP_1_1, 204, "");
  ExpectValid("HTTP/1.1 103 \t ", HTTP_1_1, 103, "");
}

// reason-phrase = 1*( HTAB / SP / VCHAR / obs-text ), without the whitespace around it.
TEST(StatusLine, ReasonPhrase) {
  ExpectValid("HTTP/1.1 200  \t OK \t", HTTP_1_1, 200, "OK");
  ExpectValid("HTTP/1.1 200 a\tb  c", HTTP_1_1, 200, "a\tb  c");
  ExpectValid("HTTP/1.1 200 !~\"#", HTTP_1_1, 200, "!~\"#");
  ExpectValid("HTTP/1.1 200 Tr\xC3\xA8s bien \xFF\x80", HTTP_1_1, 200, "Tr\xC3\xA8s bien \xFF\x80");
}

TEST(StatusLine, ReasonPhraseWithControlCharacter) {
  for (std::string_view line : {
           std::string_view("HTTP/1.1 200 O\nK"),
           std::string_view("HTTP/1.1 200 O\rK"),
           std::string_view("HTTP/1.1 200 \n"),
           std::string_view("HTTP/1.1 200 O\x01K"),
           std::string_view("HTTP/1.1 200 O\x1FK"),
           std::string_view("HTTP/1.1 200 O\x7FK"),
           std::string_view("HTTP/1.1 200 O\0K", 16),
       }) {
    ExpectInvalid(line);
  }
}

// RFC 9112 §2.3: HTTP-version = "HTTP/" DIGIT "." DIGIT, case-sensitive, of major version 1 here.
TEST(StatusLine, Version) {
  for (std::string_view line : {
           "HTTP/2.0 200",
           "HTTP/0.9 200",
           "HTTP/3.1 200",
           "XTTP/1.1 200",
           "http/1.1 200",
           "HTTP-1.1 200",
           "HTTP/1x1 200",
           "HTTP/1.x 200",
           "HTTP/1.10 200",
           "HTTP/1. 200",
           " HTTP/1.1 200",
           "HTTP/11 200 ",
       }) {
    ExpectInvalid(line);
  }
}

// status-code = 3DIGIT (RFC 9112 §4), from 100 to 599 (RFC 9110 §15).
TEST(StatusLine, StatusCode) {
  ExpectValid("HTTP/1.1 100", HTTP_1_1, 100, "");
  ExpectValid("HTTP/1.1 599", HTTP_1_1, 599, "");
  ExpectValid("HTTP/1.1 509", HTTP_1_1, 509, "");
  ExpectValid("HTTP/1.1 190", HTTP_1_1, 190, "");
  for (std::string_view line : {
           "HTTP/1.1 099",
           "HTTP/1.1 000",
           "HTTP/1.1 600",
           "HTTP/1.1 999",
           // The characters around the digits.
           "HTTP/1.1 /00",
           "HTTP/1.1 :00",
           "HTTP/1.1 1/0",
           "HTTP/1.1 1:0",
           "HTTP/1.1 10/",
           "HTTP/1.1 10:",
           "HTTP/1.1 20O",
           "HTTP/1.1 2x0",
           "HTTP/1.1 -20",
           "HTTP/1.1 +20",
           "HTTP/1.1 2 0",
           "HTTP/1.1 \xB2\xB0\xB0",
           // Not 3 digits.
           "HTTP/1.1 2000",
           "HTTP/1.1 0200",
           "HTTP/1.1 200OK",
           "HTTP/1.1 200p",
       }) {
    ExpectInvalid(line);
  }
}

// A single SP separates the version from the status code, and the status code from the reason phrase.
TEST(StatusLine, Separators) {
  for (std::string_view line : {
           "HTTP/1.1  200",
           "HTTP/1.1\t200",
           "HTTP/1.1_200",
           "HTTP/1.1 200\tOK",
           "HTTP/1.1 200\t",
       }) {
    ExpectInvalid(line);
  }
}

TEST(StatusLine, TooShort) {
  for (std::string_view line : {"", "H", "HTTP/1.1", "HTTP/1.1 ", "HTTP/1.1 20", "NOT-HTTP", "garbage"}) {
    ExpectInvalid(line);
  }
}

}  // namespace aeronet::http
