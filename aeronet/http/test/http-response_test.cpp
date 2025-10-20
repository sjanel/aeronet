#include "aeronet/http-response.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "exception.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"

namespace aeronet {

class HttpResponseTest : public ::testing::Test {
 protected:
  static constexpr SysTimePoint tp{};
  static constexpr bool keepAlive = false;
  static constexpr bool isHeadMethod = false;
  static constexpr std::size_t minCapturedBodySize = 4096;

  static HttpResponseData finalize(HttpResponse &&resp) {
    std::vector<http::Header> globalHeaders;
    return resp.finalizeAndStealData(http::HTTP_1_1, tp, keepAlive, globalHeaders, isHeadMethod, minCapturedBodySize);
  }

  static std::string concatenated(HttpResponse &&resp) {
    HttpResponseData httpResponseData = finalize(std::move(resp));
    std::string out(httpResponseData.firstBuffer());
    out.append(httpResponseData.secondBuffer());
    return out;
  }
};

TEST_F(HttpResponseTest, StatusOnly) {
  HttpResponse resp(200);
  EXPECT_EQ(200, resp.statusCode());
  resp.statusCode(404);
  EXPECT_EQ(404, resp.statusCode());

  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 404\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodySimple) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("Content-Type", "text/plain").addCustomHeader("X-A", "B").body("Hello");
  auto full = concatenated(std::move(resp));
  ASSERT_GE(full.size(), 16U);
  auto prefix = full.substr(0, 15);
  EXPECT_EQ(prefix.substr(0, 8), "HTTP/1.1") << "Raw prefix: '" << std::string(prefix) << "'";
  EXPECT_EQ(prefix.substr(8, 1), " ");
  EXPECT_EQ(prefix.substr(9, 3), "200");
  EXPECT_TRUE(full.contains("Content-Type: text/plain"));
  EXPECT_TRUE(full.contains("X-A: B"));
  auto posBody = full.find("Hello");
  ASSERT_NE(posBody, std::string_view::npos);
  auto separator = full.substr(0, posBody);
  EXPECT_TRUE(separator.contains(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithoutHeaders) {
  HttpResponse resp(200, "OK");
  EXPECT_EQ(resp.reason(), "OK");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithoutHeaders) {
  HttpResponse resp(404, "Not Found");
  EXPECT_EQ(resp.reason(), http::NotFound);
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithHeaders) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("X-Header", "Value");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(
      full,
      "HTTP/1.1 404 Not Found\r\nX-Header: Value\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithHeaders) {
  HttpResponse resp(404, "Not Found");
  resp.addCustomHeader("X-Header-1", "Value1");
  resp.addCustomHeader("X-Header-2", "Value2");
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nConnection: close\r\nDate: Thu, 01 Jan "
            "1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyAddReasonWithHeaders) {
  HttpResponse resp(200, "");
  resp.addCustomHeader("X-Header", "Value");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(
      full,
      "HTTP/1.1 404 Not Found\r\nX-Header: Value\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyRemoveReasonWithHeaders) {
  HttpResponse resp(404, "Not Found");
  resp.addCustomHeader("X-Header-1", "Value1");
  resp.addCustomHeader("X-Header-2", "Value2");
  resp.statusCode(200).reason("");
  EXPECT_EQ(resp.reason(), "");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 200\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithBody) {
  HttpResponse resp(200, "OK");
  resp.body("Hello");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 "
            "GMT\r\n\r\nHello");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithBody) {
  HttpResponse resp(404, "Not Found");
  resp.body("Hello");
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\nHello");
}

TEST_F(HttpResponseTest, AllowsDuplicates) {
  HttpResponse resp(204, "No Content");
  resp.addCustomHeader("X-Dup", "1").addCustomHeader("X-Dup", "2").body("");
  auto full = concatenated(std::move(resp));
  auto first = full.find("X-Dup: 1\r\n");
  auto second = full.find("X-Dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, ProperTermination) {
  HttpResponse resp(200, "OK");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
}

TEST_F(HttpResponseTest, SingleTerminatingCRLF) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("X-Header", "v1");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
  EXPECT_TRUE(full.contains("X-Header: v1"));
}

TEST_F(HttpResponseTest, ReplaceDifferentSizes) {
  HttpResponse resp1(200, "OK");
  resp1.addCustomHeader("X-A", "1").body("Hello");
  HttpResponse resp2(200, "OK");
  resp2.addCustomHeader("X-A", "1").body("Hello");
  HttpResponse resp3(200, "OK");
  resp3.addCustomHeader("X-A", "1").body("Hello");
  auto firstFull = concatenated(std::move(resp1));
  auto firstLen = firstFull.size();
  resp2.body("WorldWide");
  auto secondFull = concatenated(std::move(resp2));
  EXPECT_GT(secondFull.size(), firstLen);
  EXPECT_TRUE(secondFull.contains("WorldWide"));
  resp3.body("Yo");
  auto thirdFull = concatenated(std::move(resp3));
  EXPECT_TRUE(thirdFull.contains("Yo"));
}

// --- New test: body() called with a std::string_view referencing internal buffer memory
// This exercises the safety logic in mutateBody that detects when the source view
// points inside the existing buffer and may become invalid after a reallocation.
// Prior to the fix, this scenario could read from stale memory if ensureAvailableCapacity
// triggered a reallocation.
TEST_F(HttpResponseTest, BodyAssignFromInternalReasonTriggersReallocSafe) {
  // Choose a non-empty reason so we have internal bytes to reference.
  HttpResponse resp(200, "INTERNAL-REASON");
  std::string_view src = resp.reason();  // points into resp's internal buffer
  EXPECT_EQ(src, "INTERNAL-REASON");
  // Body currently empty -> diff = src.size() => ensureAvailableCapacity likely reallocates
  resp.body(src);       // must be safe even if reallocation occurs
  src = resp.reason();  // reset reason after realloc
  EXPECT_EQ(src, "INTERNAL-REASON");
  EXPECT_EQ(resp.body(), src);
  auto full = concatenated(std::move(resp));
  resp = HttpResponse(200, "INTERNAL-REASON");
  src = resp.reason();
  // Validate Content-Length header matches and body placed at tail.
  std::string clNeedle = std::string("Content-Length: ") + std::to_string(src.size()) + "\r\n";
  EXPECT_TRUE(full.contains(clNeedle)) << full;
  EXPECT_TRUE(full.ends_with(src)) << full;
}

// --- New tests for header(K,V) replacement logic ---

TEST_F(HttpResponseTest, HeaderNewViaSetter) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-First", "One");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-First: One\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceLargerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "AA");
  // Replace with larger value
  resp.customHeader("X-Replace", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Replace: ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSmallerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "LONG-LONG-VALUE");
  // Replace with smaller
  resp.customHeader("X-Replace", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Replace: S\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSameLengthValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "LEN10VALUE");  // length 10
  resp.customHeader("X-Replace", "0123456789");  // also length 10
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nX-Replace: 0123456789\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

// Ensure replacement logic does not mistake key pattern inside a value as a header start.
TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternLarger) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Key", "before X-Key: should-not-trigger");
  // Replace header; algorithm must not treat the embedded "X-Key: " in the value as another header start
  resp.customHeader("X-Key", "REPLACED-VALUE");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nX-Key: REPLACED-VALUE\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternSmaller) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Key", "AAAA X-Key: B BBBBBB");
  resp.customHeader("X-Key", "SMALL");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Key: SMALL\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

// --- New tests: header replacement while a body is present ---

TEST_F(HttpResponseTest, HeaderReplaceWithBodyLargerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "AA");
  resp.body("Hello");                        // body length 5
  resp.customHeader("X-Val", "ABCDEFGHIJ");  // grow header value
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: ABCDEFGHIJ\r\nContent-Length: 5\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\nHello");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySmallerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "SOME-LONG-VALUE");
  resp.body("WorldWide");           // length 9
  resp.customHeader("X-Val", "S");  // shrink header value
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: S\r\nContent-Length: 9\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 "
            "GMT\r\n\r\nWorldWide");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySameLengthValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                         // length 4
  resp.customHeader("X-Val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Length: 4\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\nData");
}

TEST_F(HttpResponseTest, HeaderReplaceCaseInsensitive) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                         // length 4
  resp.customHeader("x-val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Length: 4\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\nData");
}

// Interleaved reason/header mutations stress test:
// 1. Start with empty reason
// 2. Append two headers
// 3. Add longer reason
// 4. Replace first header with larger value
// 5. Shrink reason to empty
// 6. Replace header with smaller value
// 7. Finalize and assert exact layout
TEST_F(HttpResponseTest, InterleavedReasonAndHeaderMutations) {
  HttpResponse resp(200, "");
  resp.addCustomHeader("X-A", "1");
  resp.addCustomHeader("X-B", "2");
  resp.reason("LONGER-REASON");
  resp.customHeader("X-a", "LARGER-VALUE-123");
  resp.reason("");
  resp.customHeader("x-A", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200\r\nX-A: S\r\nX-B: 2\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

// ---------------- Additional Stress / Fuzz Tests ----------------

TEST_F(HttpResponseTest, RepeatedGrowShrinkCycles) {
  HttpResponse resp(200, "");
  resp.addCustomHeader("X-Static", "STATIC");
  resp.customHeader("X-Cycle", "A");
  resp.reason("R1");
  resp.customHeader("X-Cycle", "ABCDEFGHIJ");
  resp.body("one");
  resp.reason("");
  resp.customHeader("X-Cycle", "B");
  resp.body("two-two");
  resp.reason("LONGER-REASON");
  resp.customHeader("X-Cycle", "ABCDEFGHIJKLMNOP");
  resp.body("short");
  resp.reason("");
  resp.customHeader("X-Cycle", "C");
  resp.body("0123456789ABCDEFGHIJ");
  resp.customHeader("X-Cycle", "LONGVALUE-1234567890");
  resp.reason("MID");
  resp.customHeader("X-Cycle", "X");
  resp.body("XYZ");
  resp.reason("");
  resp.customHeader("X-Cycle", "Z");
  resp.body("END");
  auto full = concatenated(std::move(resp));
  std::string expected =
      "HTTP/1.1 200\r\nX-Static: STATIC\r\nX-Cycle: Z\r\nContent-Length: 3\r\nConnection: close\r\nDate: "
      "Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\nEND";
  EXPECT_EQ(full, expected);
}

// --- Trailer-related tests (response-side) ---

TEST_F(HttpResponseTest, AddTrailerWithoutBodyThrows) {
  HttpResponse resp(200);
  // No body set at all -> adding trailer should throw
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AddTrailerAfterEmptyBodyThrows) {
  HttpResponse resp(200);
  resp.body("");
  // Explicitly-empty body should still be considered 'no body' for trailers
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, SetBodyAfterTrailerThrows) {
  HttpResponse resp(200);
  resp.body("initial");
  resp.addTrailer("X-Test", "val");
  // Once a trailer was inserted, setting body later must throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
}

TEST_F(HttpResponseTest, LargeHeaderCountStress) {
  constexpr int kCount = 600;
  HttpResponse resp(200, "OK");
  for (int i = 0; i < kCount; ++i) {
    resp.addCustomHeader("X-" + std::to_string(i), std::to_string(i));
  }
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  // Count custom headers (exclude Date/Connection)
  auto pos = full.find("\r\n") + 2;  // after status line CRLF
  int userHeaders = 0;
  while (pos < full.size()) {
    auto lineEnd = full.find("\r\n", pos);
    ASSERT_NE(lineEnd, std::string_view::npos);
    if (lineEnd == pos) {
      pos += 2;
      break;
    }
    auto line = full.substr(pos, lineEnd - pos);
    if (!line.starts_with("Date: ") && !line.starts_with("Connection: ")) {
      ++userHeaders;
    }
    pos = lineEnd + 2;
  }
  EXPECT_EQ(userHeaders, kCount);
  EXPECT_TRUE(full.contains("Connection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n"));
}

namespace {  // local helpers for fuzz test
struct ParsedResponse {
  int status{};
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::vector<std::pair<std::string, std::string>> trailers;
};

ParsedResponse parseResponse(std::string_view full) {
  ParsedResponse pr;
  if (!full.starts_with("HTTP/1.1 ")) {
    throw exception("Bad version in '{}'", full);
  }
  // Extract the status line first (up to CRLF)
  auto firstCRLF = full.find("\r\n");
  if (firstCRLF == std::string_view::npos) {
    throw exception("Missing CRLF after status line in '{}'", full);
  }
  std::string_view statusLine = full.substr(0, firstCRLF);  // e.g. HTTP/1.1 200 OK
  // Accept status line with or without reason; minimal validated by prefix & three digits.
  // Parse status code at positions 9..11
  // Extract the three digits after the single space following version
  // Pattern: HTTP/1.1<space><d1><d2><d3>[optional space reason]
  pr.status = std::stoi(std::string(statusLine.substr(9, 3)));
  // Optional reason phrase begins at first space after the status code
  // Expected patterns:
  //   "HTTP/1.1 200"            (no reason)
  //   "HTTP/1.1 200 Reason..."  (reason present)
  if (statusLine.size() > 12) {
    // Find first space after status code digits
    if (statusLine.size() > 12 && statusLine[12] == ' ') {
      if (statusLine.size() > 13) {
        pr.reason.assign(statusLine.substr(13));
      }
    }
  }
  // Find end of headers (CRLF CRLF) to robustly locate header-body boundary
  std::size_t headerEnd = full.find(http::DoubleCRLF, firstCRLF + 2);
  if (headerEnd == std::string_view::npos) {
    throw exception("Missing terminating header block in '{}'", full);
  }
  std::size_t cursor = firstCRLF + 2;  // move past CRLF into headers section
  while (cursor < headerEnd) {
    auto eol = full.find(http::CRLF, cursor);
    if (eol == std::string_view::npos || eol > headerEnd) {
      throw exception("Invalid header line in '{}'", full);
    }
    auto line = full.substr(cursor, eol - cursor);
    auto sep = line.find(http::HeaderSep);
    if (sep == std::string_view::npos) {
      throw exception("No separator in header line '{}'", line);
    }
    pr.headers.emplace_back(std::string(line.substr(0, sep)), std::string(line.substr(sep + 2)));
    cursor = eol + 2;
  }
  cursor = headerEnd + http::DoubleCRLF.size();  // move past CRLFCRLF into body
  // If Content-Length header present, body length is known; otherwise body is the remainder
  std::size_t contentLen = 0;
  bool hasContentLen = false;
  for (auto &hdr : pr.headers) {
    if (hdr.first == http::ContentLength) {
      contentLen = StringToIntegral<std::size_t>(hdr.second);
      hasContentLen = true;
      break;
    }
  }

  if (hasContentLen) {
    if (cursor + contentLen > full.size()) {
      throw exception("Truncated body: expected {} bytes, have {}", contentLen, full.size() - cursor);
    }
    pr.body.assign(full.substr(cursor, contentLen));
    cursor += contentLen;
    // After body, there may be optional trailer headers terminated by a blank line (CRLF CRLF)
    // If there's remaining data, parse trailers until an empty line is encountered.
    if (cursor < full.size()) {
      // If the next characters are CRLF, consume and treat as no trailers
      if (full.substr(cursor, http::CRLF.size()) == http::CRLF) {
        cursor += http::CRLF.size();  // consume terminating CRLF (no trailers)
      } else {
        while (true) {
          auto eol = full.find(http::CRLF, cursor);
          if (eol == std::string_view::npos) {
            throw exception("No terminating trailer line in '{}'", full);
          }
          if (eol == cursor) {  // blank line terminator
            cursor += http::CRLF.size();
            break;
          }
          auto line = full.substr(cursor, eol - cursor);
          auto sep = line.find(http::HeaderSep);
          if (sep == std::string_view::npos) {
            throw exception("No separator in trailer line '{}'", line);
          }
          pr.trailers.emplace_back(std::string(line.substr(0, sep)), std::string(line.substr(sep + http::CRLF.size())));
          cursor = eol + http::CRLF.size();
        }
      }
    }
  } else {
    // No Content-Length header: treat rest as body
    pr.body.assign(full.substr(cursor));
    cursor = full.size();
  }
  return pr;
}
}  // namespace

TEST_F(HttpResponseTest, FuzzStructuralValidation) {
  static constexpr int kNbHttpResponses = 60;
  static constexpr int kNbOperationsPerHttpResponse = 100;

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> opDist(0, 5);
  std::uniform_int_distribution<int> smallLen(0, 12);
  std::uniform_int_distribution<int> midLen(0, 24);
  auto makeValue = [&](int length) {
    std::string value;
    value.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      value.push_back(static_cast<char>('A' + (i % 26)));
    }
    return value;
  };
  auto makeReason = [&](int length) {
    // Reasons must not start or end with space for this simplified parser
    if (length <= 0) {
      return std::string();
    }
    std::string reasonStr = makeValue(length);
    return reasonStr;  // generated chars are A..Z only (no spaces)
  };
  for (int caseIndex = 0; caseIndex < kNbHttpResponses; ++caseIndex) {
    HttpResponse resp;
    std::string lastReason;
    std::string lastBody;
    std::string lastHeaderKey;
    std::string lastHeaderValue;
    std::string lastTrailerKey;
    std::string lastTrailerValue;
    for (int step = 0; step < kNbOperationsPerHttpResponse; ++step) {
      switch (opDist(rng)) {
        case 0:
          lastHeaderKey = "X-" + std::to_string(step);
          lastHeaderValue = makeValue(smallLen(rng));
          resp.addCustomHeader(lastHeaderKey, lastHeaderValue);
          break;
        case 1:
          lastHeaderKey = "U-" + std::to_string(step % 5);
          lastHeaderValue = makeValue(midLen(rng));
          resp.customHeader(lastHeaderKey, lastHeaderValue);
          break;
        case 2:
          lastReason = makeReason(smallLen(rng));
          resp.reason(lastReason);
          break;
        case 3:
          if (lastTrailerKey.empty()) {
            lastBody = makeValue(smallLen(rng));
            resp.body(lastBody);
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.body({}), std::logic_error);
          }
          break;
        case 4: {
          static constexpr http::StatusCode opts[] = {200, 204, 404};
          resp.statusCode(opts[static_cast<std::size_t>(step) % std::size(opts)]);
          break;
        }
        case 5:
          if (lastBody.empty()) {
            EXPECT_THROW(resp.addTrailer("X-Trailer", "value"), std::logic_error);
          } else {
            lastTrailerKey = "X-" + std::to_string(step);
            lastTrailerValue = makeValue(smallLen(rng));
            resp.addTrailer(lastTrailerKey, lastTrailerValue);
          }
          break;
        default:
          throw exception("Invalid random value, update the test");
      }
    }

    // Pre-finalize state checks (reason/body accessible before finalize)
    EXPECT_EQ(resp.reason(), std::string_view(lastReason));
    EXPECT_EQ(resp.body(), std::string_view(lastBody));

    auto full = concatenated(std::move(resp));
    ParsedResponse pr = parseResponse(full);

    int dateCount = 0;
    int connCount = 0;
    int clCount = 0;
    std::size_t clVal = 0;
    for (auto &headerPair : pr.headers) {
      if (headerPair.first == http::Date) {
        ++dateCount;
      } else if (headerPair.first == http::Connection) {
        ++connCount;
      } else if (headerPair.first == http::ContentLength) {
        ++clCount;
        clVal = StringToIntegral<std::size_t>(headerPair.second);
      }
    }
    EXPECT_EQ(dateCount, 1);
    EXPECT_EQ(connCount, 1);
    if (!pr.body.empty()) {
      EXPECT_EQ(clCount, 1);
      if (clVal != pr.body.size()) {
        // Diagnostic: content-length mismatch
        ADD_FAILURE() << "Content-Length header=" << clVal << " but parsed body size=" << pr.body.size()
                      << "\nFull response:\n"
                      << full;
        return;  // stop early to inspect this failing case
      }
      EXPECT_EQ(clVal, pr.body.size());
    } else {
      EXPECT_EQ(clCount, 0);
    }
    ASSERT_GE(pr.headers.size(), 2U);
    EXPECT_EQ(pr.headers[pr.headers.size() - 2].first, http::Connection);
    EXPECT_EQ(pr.headers[pr.headers.size() - 1].first, http::Date);

    if (!lastHeaderKey.empty()) {
      std::string needle = lastHeaderKey;
      needle.append(http::HeaderSep).append(lastHeaderValue);
      EXPECT_TRUE(full.contains(needle)) << "Missing last header '" << needle << "' in: " << full;
    }
    if (!lastTrailerKey.empty()) {
      std::string needle = lastTrailerKey;
      needle.append(http::HeaderSep).append(lastTrailerValue);
      EXPECT_TRUE(full.contains(needle)) << "Missing last trailer '" << needle << "' in: " << full;
    }
  }
}

}  // namespace aeronet