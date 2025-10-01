#include "aeronet/http-response.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "exception.hpp"

namespace aeronet {

class HttpResponseTest : public ::testing::Test {
 protected:
  static constexpr std::string_view date{"date"};
  static constexpr bool keepAlive = false;
  static constexpr bool isHeadMethod = false;

  static std::string_view finalize(HttpResponse &resp) {
    return resp.finalizeAndGetFullTextResponse(http::HTTP_1_1, date, keepAlive, isHeadMethod);
  }
};

TEST_F(HttpResponseTest, StatusOnly) {
  HttpResponse resp(200);
  EXPECT_EQ(200, resp.statusCode());
  resp.statusCode(404);
  EXPECT_EQ(404, resp.statusCode());

  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 404\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodySimple) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("Content-Type", "text/plain").addCustomHeader("X-A", "B").body("Hello");
  auto full = finalize(resp);
  ASSERT_GE(full.size(), 16U);
  std::string_view prefix = full.substr(0, 15);
  EXPECT_EQ(prefix.substr(0, 8), "HTTP/1.1") << "Raw prefix: '" << std::string(prefix) << "'";
  EXPECT_EQ(prefix.substr(8, 1), " ");
  EXPECT_EQ(prefix.substr(9, 3), "200");
  EXPECT_NE(full.find("Content-Type: text/plain"), std::string_view::npos);
  EXPECT_NE(full.find("X-A: B"), std::string_view::npos);
  auto posBody = full.find("Hello");
  ASSERT_NE(posBody, std::string_view::npos);
  auto separator = full.substr(0, posBody);
  EXPECT_NE(separator.find("\r\n\r\n"), std::string_view::npos);
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithoutHeaders) {
  HttpResponse resp(200, "OK");
  EXPECT_EQ(resp.reason(), "OK");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithoutHeaders) {
  HttpResponse resp(404, "Not Found");
  EXPECT_EQ(resp.reason(), http::NotFound);
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithHeaders) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("X-Header", "Value");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nX-Header: Value\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithHeaders) {
  HttpResponse resp(404, "Not Found");
  resp.addCustomHeader("X-Header-1", "Value1");
  resp.addCustomHeader("X-Header-2", "Value2");
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = finalize(resp);

  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyAddReasonWithHeaders) {
  HttpResponse resp(200, "");
  resp.addCustomHeader("X-Header", "Value");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nX-Header: Value\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyRemoveReasonWithHeaders) {
  HttpResponse resp(404, "Not Found");
  resp.addCustomHeader("X-Header-1", "Value1");
  resp.addCustomHeader("X-Header-2", "Value2");
  resp.statusCode(200).reason("");
  EXPECT_EQ(resp.reason(), "");
  auto full = finalize(resp);

  EXPECT_EQ(full,
            "HTTP/1.1 200\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithBody) {
  HttpResponse resp(200, "OK");
  resp.body("Hello");
  resp.statusCode(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nContent-Length: 5\r\nDate: date\r\nConnection: close\r\n\r\nHello");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithBody) {
  HttpResponse resp(404, "Not Found");
  resp.body("Hello");
  resp.statusCode(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = finalize(resp);

  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nDate: date\r\nConnection: close\r\n\r\nHello");
}

TEST_F(HttpResponseTest, AllowsDuplicates) {
  HttpResponse resp(204, "No Content");
  resp.addCustomHeader("X-Dup", "1").addCustomHeader("X-Dup", "2").body("");
  auto full = finalize(resp);
  auto first = full.find("X-Dup: 1\r\n");
  auto second = full.find("X-Dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, ProperTermination) {
  HttpResponse resp(200, "OK");
  auto full = finalize(resp);
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), "\r\n\r\n");
}

TEST_F(HttpResponseTest, SingleTerminatingCRLF) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("X-Header", "v1");
  auto full = finalize(resp);
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), "\r\n\r\n");
  EXPECT_NE(full.find("X-Header: v1"), std::string_view::npos);
}

TEST_F(HttpResponseTest, ReplaceDifferentSizes) {
  HttpResponse resp(200, "OK");
  resp.addCustomHeader("X-A", "1").body("Hello");
  auto firstFull = finalize(resp);
  auto firstLen = firstFull.size();
  resp.body("WorldWide");
  auto secondFull = finalize(resp);
  EXPECT_GT(secondFull.size(), firstLen);
  EXPECT_NE(secondFull.find("WorldWide"), std::string_view::npos);
  resp.body("Yo");
  auto thirdFull = finalize(resp);
  EXPECT_NE(thirdFull.find("Yo"), std::string_view::npos);
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
  auto full = finalize(resp);
  src = resp.reason();
  // Validate Content-Length header matches and body placed at tail.
  std::string clNeedle = std::string("Content-Length: ") + std::to_string(src.size()) + "\r\n";
  EXPECT_NE(full.find(clNeedle), std::string_view::npos) << full;
  EXPECT_TRUE(full.ends_with(std::string(src))) << full;
}

// --- New tests for header(K,V) replacement logic ---

TEST_F(HttpResponseTest, HeaderNewViaSetter) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-First", "One");
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nX-First: One\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceLargerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "AA");
  // Replace with larger value
  resp.customHeader("X-Replace", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  auto full = finalize(resp);
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Replace: ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSmallerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "LONG-LONG-VALUE");
  // Replace with smaller
  resp.customHeader("X-Replace", "S");
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nX-Replace: S\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSameLengthValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Replace", "LEN10VALUE");  // length 10
  resp.customHeader("X-Replace", "0123456789");  // also length 10
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nX-Replace: 0123456789\r\nDate: date\r\nConnection: close\r\n\r\n");
}

// Ensure replacement logic does not mistake key pattern inside a value as a header start.
TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternLarger) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Key", "before X-Key: should-not-trigger");
  // Replace header; algorithm must not treat the embedded "X-Key: " in the value as another header start
  resp.customHeader("X-Key", "REPLACED-VALUE");
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nX-Key: REPLACED-VALUE\r\nDate: date\r\nConnection: close\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternSmaller) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Key", "AAAA X-Key: B BBBBBB");
  resp.customHeader("X-Key", "SMALL");
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nX-Key: SMALL\r\nDate: date\r\nConnection: close\r\n\r\n");
}

// --- New tests: header replacement while a body is present ---

TEST_F(HttpResponseTest, HeaderReplaceWithBodyLargerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "AA");
  resp.body("Hello");                        // body length 5
  resp.customHeader("X-Val", "ABCDEFGHIJ");  // grow header value
  auto full = finalize(resp);
  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nX-Val: ABCDEFGHIJ\r\nContent-Length: 5\r\nDate: date\r\nConnection: close\r\n\r\nHello");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySmallerValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "SOME-LONG-VALUE");
  resp.body("WorldWide");           // length 9
  resp.customHeader("X-Val", "S");  // shrink header value
  auto full = finalize(resp);
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: S\r\nContent-Length: 9\r\nDate: date\r\nConnection: close\r\n\r\nWorldWide");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySameLengthValue) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                         // length 4
  resp.customHeader("X-Val", "0123456789");  // same length replacement
  auto full = finalize(resp);
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Length: 4\r\nDate: date\r\nConnection: close\r\n\r\nData");
}

TEST_F(HttpResponseTest, HeaderReplaceCaseInsensitive) {
  HttpResponse resp(200, "OK");
  resp.customHeader("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                         // length 4
  resp.customHeader("x-val", "0123456789");  // same length replacement
  auto full = finalize(resp);
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Length: 4\r\nDate: date\r\nConnection: close\r\n\r\nData");
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
  auto full = finalize(resp);
  EXPECT_EQ(full, "HTTP/1.1 200\r\nX-A: S\r\nX-B: 2\r\nDate: date\r\nConnection: close\r\n\r\n");
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
  auto full = finalize(resp);
  std::string expected =
      "HTTP/1.1 200\r\nX-Static: STATIC\r\nX-Cycle: Z\r\nContent-Length: 3\r\nDate: date\r\nConnection: "
      "close\r\n\r\nEND";
  EXPECT_EQ(full, expected);
}

TEST_F(HttpResponseTest, LargeHeaderCountStress) {
  constexpr int kCount = 600;
  HttpResponse resp(200, "OK");
  for (int i = 0; i < kCount; ++i) {
    resp.addCustomHeader("X-" + std::to_string(i), std::to_string(i));
  }
  auto full = finalize(resp);
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
    std::string_view line = full.substr(pos, lineEnd - pos);
    if (!line.starts_with("Date: ") && !line.starts_with("Connection: ")) {
      ++userHeaders;
    }
    pos = lineEnd + 2;
  }
  EXPECT_EQ(userHeaders, kCount);
  EXPECT_NE(full.find("Date: date\r\nConnection: close\r\n\r\n"), std::string_view::npos);
}

namespace {  // local helpers for fuzz test
struct ParsedResponse {
  int status{};
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
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
  std::size_t cursor = firstCRLF + 2;  // move past CRLF into headers section
  while (true) {
    auto eol = full.find("\r\n", cursor);
    if (eol == std::string_view::npos) {
      throw exception("No terminating header line in '{}'", full);
    }
    if (eol == cursor) {  // blank line
      cursor += 2;
      break;
    }
    auto line = full.substr(cursor, eol - cursor);
    auto sep = line.find(http::HeaderSep);
    if (sep == std::string_view::npos) {
      throw exception("No separator in header line '{}'", line);
    }
    pr.headers.emplace_back(std::string(line.substr(0, sep)), std::string(line.substr(sep + 2)));
    cursor = eol + 2;
  }
  pr.body.assign(full.substr(cursor));
  return pr;
}
}  // namespace

TEST_F(HttpResponseTest, FuzzStructuralValidation) {
  static constexpr int kCases = 60;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> opDist(0, 4);
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
  for (int caseIndex = 0; caseIndex < kCases; ++caseIndex) {
    HttpResponse resp(200, "");
    std::string lastReason;
    std::string lastBody;
    std::string lastHeaderKey;
    std::string lastHeaderValue;
    for (int step = 0; step < 40; ++step) {
      switch (opDist(rng)) {
        case 0: {
          lastHeaderKey = "X-" + std::to_string(step);
          lastHeaderValue = makeValue(smallLen(rng));
          resp.addCustomHeader(lastHeaderKey, lastHeaderValue);
          break;
        }
        case 1: {
          lastHeaderKey = "U-" + std::to_string(step % 5);
          lastHeaderValue = makeValue(midLen(rng));
          resp.customHeader(lastHeaderKey, lastHeaderValue);
          break;
        }
        case 2: {
          lastReason = makeReason(smallLen(rng));
          resp.reason(lastReason);
          break;
        }
        case 3: {
          lastBody = makeValue(smallLen(rng));
          resp.body(lastBody);
          break;
        }
        case 4: {
          static constexpr http::StatusCode opts[3] = {200, 204, 404};
          resp.statusCode(opts[step % 3]);
          break;
        }
        default:
          throw exception("Invalid random value, update the test");
      }
    }
    // Pre-finalize state checks (reason/body accessible before finalize)
    EXPECT_EQ(resp.reason(), std::string_view(lastReason));
    EXPECT_EQ(resp.body(), std::string_view(lastBody));

    auto full = finalize(resp);  // adds reserved headers
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
        clVal = std::stoul(headerPair.second);
      }
    }
    EXPECT_EQ(dateCount, 1);
    EXPECT_EQ(connCount, 1);
    if (!pr.body.empty()) {
      EXPECT_EQ(clCount, 1);
      EXPECT_EQ(clVal, pr.body.size());
    } else {
      EXPECT_EQ(clCount, 0);
    }
    ASSERT_GE(pr.headers.size(), 2U);
    EXPECT_EQ(pr.headers[pr.headers.size() - 2].first, http::Date);
    EXPECT_EQ(pr.headers.back().first, http::Connection);

    if (!lastHeaderKey.empty()) {
      std::string needle = lastHeaderKey;
      needle.append(": ").append(lastHeaderValue);
      EXPECT_NE(full.find(needle), std::string_view::npos) << "Missing last header '" << needle << "' in: " << full;
    }
  }
}

}  // namespace aeronet