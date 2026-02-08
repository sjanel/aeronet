#include "aeronet/http-response.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file-helpers.hpp"
#include "aeronet/file-sys-test-support.hpp"
#include "aeronet/file.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class HttpResponseTest : public ::testing::Test {
 protected:
  static constexpr SysTimePoint kTp{};
  static constexpr bool kKeepAlive = false;
  static constexpr bool kIsHeadMethod = false;
  static constexpr bool kAddTrailerHeader = false;
  static constexpr std::size_t kMinCapturedBodySize = 4096;
  static const RawChars kExpectedDateRaw;

  static HttpResponse MakePrepared(bool isPrepared, bool head, const ConcatenatedHeaders& globalHeaders = {}) {
    HttpResponse resp;
    if (isPrepared) {
      resp._opts.setPrepared();
      for (std::string_view headerNameAndValue : globalHeaders) {
        const auto sepPos = headerNameAndValue.find(http::HeaderSep);
        if (sepPos == std::string_view::npos) {
          throw std::invalid_argument("Invalid header in global headers");
        }
        resp.headerAddLine(headerNameAndValue.substr(0, sepPos),
                           headerNameAndValue.substr(sepPos + http::HeaderSep.size()));
      }
      resp._opts.headMethod(head);
    }
    return resp;
  }

  static void AddTrailerHeader(HttpResponse& resp, bool addTrailerHeader) {
    if (resp._opts.isPrepared()) {
      resp._opts.addTrailerHeader(addTrailerHeader);
    }
  }

  static HttpResponseData finalizePrepared(HttpResponse&& resp, bool head = kIsHeadMethod,
                                           bool keepAliveFlag = kKeepAlive) {
    return finalizePrepared(std::move(resp), {}, head, kAddTrailerHeader, keepAliveFlag, kMinCapturedBodySize);
  }

  static HttpResponseData finalizePrepared(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders, bool head,
                                           bool addTrailerHeader, bool keepAliveFlag, std::size_t minCapturedBodySize) {
    HttpResponse::Options opts;

    opts.close(!keepAliveFlag);
    opts.addTrailerHeader(addTrailerHeader);
    opts.headMethod(head);

    return resp.finalizeForHttp1(kTp, http::HTTP_1_1, opts, &globalHeaders, minCapturedBodySize);
  }

  static HttpResponseData finalize(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders, bool head,
                                   bool keepAliveFlag, bool addTrailerHeader, std::size_t minCapturedBodySize) {
    std::size_t expectedFileLen = 0;
    if (resp.hasBodyFile()) {
      expectedFileLen = resp.file()->size();
    }
    auto prepared =
        finalizePrepared(std::move(resp), globalHeaders, head, addTrailerHeader, keepAliveFlag, minCapturedBodySize);
    if (prepared.getIfFilePayload() != nullptr) {
      EXPECT_EQ(prepared.fileLength(), expectedFileLen);
    }
    return prepared;
  }

  static std::string concatenated(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders = {},
                                  bool head = kIsHeadMethod, bool keepAliveFlag = kKeepAlive,
                                  std::size_t minCapturedBodySize = kMinCapturedBodySize,
                                  bool addTrailerHeader = kAddTrailerHeader) {
    HttpResponseData httpResponseData =
        finalize(std::move(resp), globalHeaders, head, keepAliveFlag, addTrailerHeader, minCapturedBodySize);
    auto firstBuf = httpResponseData.firstBuffer();
    auto secondBuf = httpResponseData.secondBuffer();
    std::string out;
    out.reserve(firstBuf.size() + secondBuf.size());
    out.append(firstBuf);
    EXPECT_TRUE(secondBuf.data() != nullptr || secondBuf.empty());
    out.append(secondBuf);
    return out;
  }

  static void MakeAllHeaderNamesLowerCase(HttpResponse& resp) { resp.makeAllHeaderNamesLowerCase(); }
};

const RawChars HttpResponseTest::kExpectedDateRaw = MakeHttp1HeaderLine(http::Date, "Thu, 01 Jan 1970 00:00:00 GMT");

TEST_F(HttpResponseTest, StatusFromRvalue) {
  auto resp = HttpResponse(http::StatusCodeOK).status(404);
  EXPECT_EQ(resp.status(), 404);
}

TEST_F(HttpResponseTest, BodyFromSpanBytesLValue) {
  static constexpr std::byte bodyBytes[]{std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
                                         std::byte{'o'}};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::span<const std::byte>(bodyBytes));
  EXPECT_EQ(resp.bodyInMemory(), "Hello");
}

TEST_F(HttpResponseTest, StatusOnly) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_EQ(200, resp.status());
  EXPECT_EQ(resp.statusStr(), "200");
  EXPECT_FALSE(resp.hasReason());
  EXPECT_FALSE(resp.hasBodyCaptured());
  EXPECT_FALSE(resp.hasBodyFile());
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_FALSE(resp.hasBody());
  EXPECT_FALSE(resp.hasTrailer("X-Nonexistent"));
  resp.status(404);
  EXPECT_EQ(404, resp.status());
  EXPECT_EQ(resp.statusStr(), "404");

  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, TooLongReasonShouldBeTruncated) {
  HttpResponse resp(http::StatusCodeOK);
  std::string longReason(70000, 'A');
  resp.reason(longReason);
  EXPECT_LT(resp.reason().size(), longReason.size());

  resp = HttpResponse(http::StatusCodeOK).reason(longReason);
  EXPECT_LT(resp.reason().size(), longReason.size());
}

TEST_F(HttpResponseTest, ConstructorWithBody) {
  HttpResponse resp("Hello, World!");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.reason(), "");
  EXPECT_EQ(resp.bodyInMemory(), "Hello, World!");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");

  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, ConstructorWithConcatenatedHeadersBadFormat) {
  static constexpr std::string_view kBadConcatenatedHeaders[] = {
      "HeaderWithoutSep\r\n",
      "HeaderWithNoValue: \r\nAnotherHeaderWithoutSep\r\n",
      "HeaderWithNoCRLF: Value",
      "NotUsingHeaderSep:Value",
      "Invalid Header Name!: Value\r\n",
      "Valid-Header: Invalid\x01Value\r\n",
  };

  for (std::string_view badHeaders : kBadConcatenatedHeaders) {
    EXPECT_THROW(HttpResponse(0, 200, badHeaders), std::invalid_argument);
  }
}

TEST_F(HttpResponseTest, HttpPartsSizes) {
  HttpResponse resp("Hello, World!");

  EXPECT_EQ(resp.statusLineSize(), std::string_view("HTTP/1.1 200\r\n").size());
  EXPECT_EQ(resp.statusLineSize(), resp.statusLineLength());

  EXPECT_EQ(resp.headersSize(), std::string_view("Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                                 "Content-Type: text/plain\r\n"
                                                 "Content-Length: 13\r\n")
                                    .size());
  EXPECT_EQ(resp.headersSize(), resp.headersLength());

  EXPECT_EQ(resp.headSize(), std::string_view("HTTP/1.1 200\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                              "Content-Type: text/plain\r\n"
                                              "Content-Length: 13\r\n\r\n")
                                 .size());
  EXPECT_EQ(resp.headSize(), resp.headLength());

  EXPECT_EQ(resp.sizeInMemory(), std::string_view("HTTP/1.1 200\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                                  "Content-Type: text/plain\r\n"
                                                  "Content-Length: 13\r\n\r\n"
                                                  "Hello, World!")
                                     .size());

  resp.reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  EXPECT_EQ(resp.statusLineSize(), std::string_view("HTTP/1.1 200 Not Found\r\n").size());
  EXPECT_EQ(resp.statusLineSize(), resp.statusLineLength());
}

TEST_F(HttpResponseTest, ConstructorWithBodyContentTypeOnly) {
  HttpResponse resp("Hello, World!", "text/my-text");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.reason(), "");
  EXPECT_EQ(resp.bodyInMemory(), "Hello, World!");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/my-text");

  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/my-text")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, ConstructorWithConcatenatedHeaders) {
  static constexpr std::string_view kConcatenatedHeaders[] = {
      "",
      "X-Custom-Header: CustomValue\r\n",
      "X-1: Salut\r\nX-2: Bonjour\r\nX-3: Hola\r\n",
  };

  static constexpr std::string_view kBodies[] = {
      "",
      "Hello!",
      "This is a longer body to test the HttpResponse constructor with concatenated headers.",
  };

  static constexpr std::size_t kAdditionalCapacities[] = {
      0U,
      16U,
      64U,
  };

  for (std::string_view body : kBodies) {
    for (std::size_t additionalCapacity : kAdditionalCapacities) {
      for (std::string_view concatenatedHeaders : kConcatenatedHeaders) {
        HttpResponse resp(additionalCapacity, 200, concatenatedHeaders, body, "text/custom");
        EXPECT_EQ(resp.status(), 200);
        EXPECT_EQ(resp.reason(), "");
        EXPECT_EQ(resp.bodyInMemory(), body);
        if (concatenatedHeaders.contains("X-Custom-Header: ")) {
          EXPECT_EQ(resp.headerValueOrEmpty("X-Custom-Header"), "CustomValue");
        } else {
          EXPECT_FALSE(resp.hasHeader("X-Custom-Header"));
        }
        if (concatenatedHeaders.contains("X-1: ")) {
          EXPECT_EQ(resp.headerValueOrEmpty("X-1"), "Salut");
          EXPECT_EQ(resp.headerValueOrEmpty("X-2"), "Bonjour");
          EXPECT_EQ(resp.headerValueOrEmpty("X-3"), "Hola");
        } else {
          EXPECT_FALSE(resp.hasHeader("X-1"));
        }
        if (!body.empty()) {
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/custom");
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), std::to_string(body.size()));
        } else {
          EXPECT_FALSE(resp.hasHeader(http::ContentType));
          EXPECT_FALSE(resp.hasHeader(http::ContentLength));
        }

        const auto full = concatenated(std::move(resp));
        EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine("X-Custom-Header", "CustomValue")),
                  concatenatedHeaders.contains("X-Custom-Header: "));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine("X-Another-Header", "AnotherValue")),
                  concatenatedHeaders.contains("X-Another-Header: "));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/custom")), !body.empty());
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(body.size()))), !body.empty());
        EXPECT_TRUE(full.ends_with("\r\n\r\n" + std::string(body)));
      }
    }
  }
}

TEST_F(HttpResponseTest, BadStatusCode) {
  EXPECT_THROW(HttpResponse(42), std::invalid_argument);
  EXPECT_THROW(HttpResponse(1000), std::invalid_argument);
}

TEST_F(HttpResponseTest, HeadersRange) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.headerAddLine("Header-1", "Value1");
  resp.headerAddLine("Header-2", "Value2");
  auto headers = resp.headers();

  static_assert(std::ranges::input_range<decltype(headers)>);

  static_assert(
      std::indirect_unary_predicate<decltype([](auto&&) { return true; }), std::ranges::iterator_t<decltype(headers)>>);

  EXPECT_TRUE(std::ranges::any_of(
      headers, [](const auto& header) { return header.name == "Header-1" && header.value == "Value1"; }));
}

TEST_F(HttpResponseTest, HeaderAndBodySize) {
  std::string buf(256U, 'A');
  std::string buf2(512U, 'B');

  EXPECT_EQ(HttpResponse::HeaderSize(buf.size(), buf2.size()),
            http::CRLF.size() + buf.size() + http::HeaderSep.size() + buf2.size());

  EXPECT_EQ(HttpResponse::BodySize(buf.size(), buf2.size()),
            buf.size() + HttpResponse::HeaderSize(http::ContentType.size(), buf2.size()) +
                HttpResponse::HeaderSize(http::ContentLength.size(), nchars(buf.size())));
}

namespace {

uint32_t counter;

constexpr auto kAppendZeroOrOneA = [](char* buf) {
  if (counter++ % 2 == 0) {
    return 0U;
  }
  *buf = 'A';
  return 1U;
};

constexpr auto kAppendZeroOrOneABytes = [](std::byte* buf) {
  if (counter++ % 2 == 0) {
    return 0U;
  }
  *buf = std::byte{'A'};
  return 1U;
};

}  // namespace
// =============================================================================
// STATUS TESTS
// =============================================================================

// =============================================================================
// REASON TESTS
// =============================================================================

TEST_F(HttpResponseTest, InterleavedReasonAndHeaderMutations) {
  HttpResponse resp(http::StatusCodeOK, "");
  resp.headerAddLine("X-A", "1");
  resp.headerAddLine("X-B", "2");
  resp.reason("LONGER-REASON");
  resp.header("X-a", "LARGER-VALUE-123");
  resp.reason("");
  resp.header("x-A", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains("X-A: S\r\n"));
  EXPECT_TRUE(full.contains("X-B: 2\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyAddReasonWithHeaders) {
  auto resp = HttpResponse(200).headerAddLine("X-Header", 127);
  resp.status(404);
  resp.reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains("X-Header: 127\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithBody) {
  HttpResponse resp(200);
  resp.reason("OK");
  resp.body("Hello", "MySpecialContentType");
  resp.status(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "MySpecialContentType")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "5")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello"));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithHeaders) {
  HttpResponse resp(200);
  resp.reason("OK");
  resp.headerAddLine("X-Header", 127);
  resp.status(404);
  resp.reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains("X-Header: 127\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithoutHeaders) {
  HttpResponse resp(200);
  resp.reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  EXPECT_TRUE(resp.hasReason());
  resp.status(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  EXPECT_TRUE(resp.hasReason());
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithBody) {
  HttpResponse resp(http::StatusCodeNotFound);
  resp.reason("Not Found");
  resp.body("Hello");
  resp.status(http::StatusCodeOK).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "5")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello"));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithHeaders) {
  auto resp = HttpResponse(404).reason("Not Found").headerAddLine("X-Header-1", "Value1");
  resp.headerAddLine("X-Header-2", "Value2");
  resp.status(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Header-1: Value1\r\n"));
  EXPECT_TRUE(full.contains("X-Header-2: Value2\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithoutHeaders) {
  HttpResponse resp(404, http::NotFound);
  EXPECT_EQ(resp.bodyInMemory(), http::NotFound);
  resp = HttpResponse{}.status(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyRemoveReasonWithHeaders) {
  HttpResponse resp(404);
  resp.reason("Not Found");
  resp.headerAddLine("X-Header-1", "Value1");
  resp.headerAddLine("X-Header-2", "Value2");
  resp.status(200).reason("");
  EXPECT_EQ(resp.reason(), "");
  EXPECT_FALSE(resp.hasReason());
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains("X-Header-1: Value1\r\n"));
  EXPECT_TRUE(full.contains("X-Header-2: Value2\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodySimple) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  EXPECT_EQ(resp.reasonLength(), 2U);
  EXPECT_EQ(resp.reason(), "OK");
  EXPECT_TRUE(resp.hasReason());
  EXPECT_EQ(resp.reasonSize(), resp.reasonLength());
  resp.headerAddLine(http::ContentType, "text/plain").headerAddLine("X-A", "B").body("Hello");
  auto full = concatenated(std::move(resp));
  ASSERT_GE(full.size(), 16U);
  auto prefix = full.substr(0, 15);
  EXPECT_EQ(prefix.substr(0, 8), "HTTP/1.1") << "Raw prefix: '" << std::string(prefix) << "'";
  EXPECT_EQ(prefix.substr(8, 1), " ");
  EXPECT_EQ(prefix.substr(9, 3), "200");
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains("X-A: B\r\n"));
  auto posBody = full.find("Hello");
  ASSERT_NE(posBody, std::string_view::npos);
  auto separator = full.substr(0, posBody);
  EXPECT_TRUE(separator.contains(http::DoubleCRLF));
}

// =============================================================================
// HEADERS TESTS
// =============================================================================

TEST_F(HttpResponseTest, EmptyHeaderSearchShouldReturnNullopt) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value");
  auto val = resp.headerValue("");
  EXPECT_FALSE(val.has_value());
}

TEST_F(HttpResponseTest, InsertingInvalidHeaderNameShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.headerAddLine("Invalid Header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("Another:Invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("", "value"), std::invalid_argument);

  EXPECT_THROW(resp.headerAppendValue("Invalid Header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("Another:Invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("", "value"), std::invalid_argument);

  resp.body("some body");
  EXPECT_THROW(resp.trailerAddLine("Invalid Trailer", "value"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("Another:Invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("", "value"), std::invalid_argument);

  EXPECT_THROW(resp.header("Invalid Header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.header("Another:Invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.header("", "value"), std::invalid_argument);
}

TEST_F(HttpResponseTest, InsertingInvalidHeaderValueShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.headerAddLine("X-Test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("X-Test", "value\x7F"), std::invalid_argument);

  EXPECT_THROW(resp.headerAppendValue("X-Test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("X-Test", "value\x7F"), std::invalid_argument);

  resp.body("some body");
  EXPECT_THROW(resp.trailerAddLine("X-Trailer", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("X-Trailer", "value\x7F"), std::invalid_argument);

  EXPECT_THROW(resp.header("X-Test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.header("X-Test", "value\x7F"), std::invalid_argument);
}

TEST_F(HttpResponseTest, AllowsDuplicates) {
  HttpResponse resp;
  resp.headerAddLine("X-Dup", "1").headerAddLine("X-Dup", "2");
  auto full = concatenated(std::move(resp));
  auto first = full.find("X-Dup: 1\r\n");
  auto second = full.find("X-Dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, AppendHeaderValueAppendsToExistingHeader) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").header("X-Custom", "value1");
  resp.headerAppendValue("X-Custom", "value2");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Custom: value1, value2\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueCreatesHeaderWhenMissing) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").headerAppendValue("X-Missing", "v1");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Missing: v1\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptySeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-List", "first");
  resp.headerAppendValue("X-List", "second", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: firstsecond\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-List", "first");
  resp.headerAppendValue("X-List", "", ", ");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first, \r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValueAndSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-List", "first");
  resp.headerAppendValue("X-List", "", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueHonorsCustomSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-List", "first");
  resp.headerAppendValue("X-List", "second", "; ");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first; second\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueSupportsNumericOverload) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Numeric", "1");
  resp.headerAppendValue("X-Numeric", 42, "|");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Numeric: 1|42\r\n")) << full;
}

TEST_F(HttpResponseTest, HeaderRemoveLineNotFound) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-First", "value1");
  resp.headerAddLine("X-Second", "value2");

  resp.headerRemoveLine("X-NotExists");

  EXPECT_TRUE(resp.hasHeader("X-First"));
  EXPECT_TRUE(resp.hasHeader("X-Second"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-First"), "value1");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Second"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveLineEmptyName) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value");

  resp.headerRemoveLine("");

  EXPECT_TRUE(resp.hasHeader("X-Test"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value");
}

TEST_F(HttpResponseTest, HeaderRemoveLineSimple) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Remove", "value");
  resp.headerAddLine("X-Keep", "keep-value");

  EXPECT_TRUE(resp.hasHeader("X-Remove"));
  resp.headerRemoveLine("X-Remove");

  EXPECT_FALSE(resp.hasHeader("X-Remove"));
  EXPECT_TRUE(resp.hasHeader("X-Keep"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Keep"), "keep-value");
}

TEST_F(HttpResponseTest, HeaderRemoveLinePartialMatch) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Remove", "value");
  resp.headerAddLine("X-Keep", "keep-value");

  ASSERT_EQ(resp.headerValueOrEmpty("X-Remove"), "value");

  resp.headerRemoveLine("-Remove");  // should do nothing
  EXPECT_EQ(resp.headerValueOrEmpty("X-Remove"), "value");

  resp.headerRemoveLine("-Keep");  // should do nothing
  EXPECT_EQ(resp.headerValueOrEmpty("X-Keep"), "keep-value");
}

TEST_F(HttpResponseTest, HeaderRemoveLineCaseInsensitive) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-CasE-TeSt", "value");
  resp.headerAddLine("X-Other", "other-value");

  EXPECT_TRUE(resp.hasHeader("X-CasE-TeSt"));
  resp.headerRemoveLine("x-case-test");

  EXPECT_FALSE(resp.hasHeader("X-CasE-TeSt"));
  EXPECT_FALSE(resp.hasHeader("x-case-test"));
  EXPECT_TRUE(resp.hasHeader("X-Other"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineRemovesLast) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Duplicate", "first");
  resp.headerAddLine("X-Duplicate", "second");
  resp.headerAddLine("X-Duplicate", "third");

  resp.headerRemoveLine("X-Duplicate");

  // Should remove the last occurrence (from reverse search)
  EXPECT_EQ(resp.headerValueOrEmpty("X-Duplicate"), "first");

  resp.headerRemoveLine("X-Duplicate");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Duplicate"), "first");

  resp.headerRemoveLine("X-Duplicate");
  EXPECT_FALSE(resp.hasHeader("X-Duplicate"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineMultipleTimes) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Multi", "value1");
  resp.headerAddLine("X-Multi", "value2");
  resp.headerAddLine("X-Multi", "value3");

  // Removes value3 (last), headerValue still returns value1 (first)
  resp.headerRemoveLine("X-Multi");
  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "value1");

  // Removes value2 (now last), headerValue still returns value1 (first and only)
  resp.headerRemoveLine("X-Multi");
  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "value1");

  // Removes value1 (only remaining)
  resp.headerRemoveLine("X-Multi");
  EXPECT_FALSE(resp.hasHeader("X-Multi"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineWithBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Before-Body", "value");
  resp.body("Test body content");

  EXPECT_TRUE(resp.hasHeader("X-Before-Body"));
  resp.headerRemoveLine("X-Before-Body");

  EXPECT_FALSE(resp.hasHeader("X-Before-Body"));
  EXPECT_EQ(resp.bodyInMemory(), "Test body content");
  EXPECT_EQ(resp.bodyLength(), 17);
}

TEST_F(HttpResponseTest, HeaderRemoveLineRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .headerAddLine("X-Remove", "value")
                  .headerAddLine("X-Keep", "keep")
                  .headerRemoveLine("X-Remove");

  EXPECT_FALSE(resp.hasHeader("X-Remove"));
  EXPECT_TRUE(resp.hasHeader("X-Keep"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotFound) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value1, value2");

  resp.headerRemoveValue("X-NotExists", "value1");
  EXPECT_TRUE(resp.hasHeader("X-Test"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotInHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value1, value2");

  resp.headerRemoveValue("X-Test", "value3");
  EXPECT_TRUE(resp.hasHeader("X-Test"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueFullLineRemoval) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Single", "only-value");
  resp.headerAddLine("X-Keep", "keep-me");

  EXPECT_TRUE(resp.hasHeader("X-Single"));
  resp.headerRemoveValue("X-Single", "only-value");

  EXPECT_FALSE(resp.hasHeader("X-Single"));
  EXPECT_TRUE(resp.hasHeader("X-Keep"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Keep"), "keep-me");
}

TEST_F(HttpResponseTest, HeaderRemoveValueAtStart) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Multi", "first, second, third");

  resp.headerRemoveValue("X-Multi", "first");

  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "second, third");
}

TEST_F(HttpResponseTest, HeaderRemoveValueAtEnd) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Multi", "first, second, third");

  resp.headerRemoveValue("X-Multi", "third");

  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "first, second");
}

TEST_F(HttpResponseTest, HeaderRemoveValueInMiddle) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Multi", "first, second, third");

  resp.headerRemoveValue("X-Multi", "second");

  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "first, third");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Custom", "alpha;beta;gamma");

  resp.headerRemoveValue("X-Custom", "beta", ";");

  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "alpha;gamma");
}

TEST_F(HttpResponseTest, HeaderRemoveValueLongSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Custom", "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("X-Custom", "alpha", "<sep>");  // should do nothing

  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("X-Custom", "gamma", "<sep>");  // should do nothing
  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("X-Custom", "beta", "<sep>");
  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "sep>alpha<sep>gamma<sep");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparatorAtStart) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Custom", "alpha|beta|gamma");

  resp.headerRemoveValue("X-Custom", "alpha", "|");

  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "beta|gamma");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparatorAtEnd) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Custom", "alpha|beta|gamma");

  resp.headerRemoveValue("X-Custom", "gamma", "|");

  EXPECT_TRUE(resp.hasHeader("X-Custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Custom"), "alpha|beta");
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotProperlyDelimited) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "somevalue, valueanother, value");

  // Try to remove "value" which is a substring but not properly delimited
  resp.headerRemoveValue("X-Test", "value");

  // Should not remove anything because "value" is not properly delimited
  EXPECT_TRUE(resp.hasHeader("X-Test"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "somevalue, valueanother");
}

TEST_F(HttpResponseTest, HeaderRemoveValuePartialMatch) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value1, value2, value3");

  // Try to remove "value" which is a substring of all values
  resp.headerRemoveValue("X-Test", "value");

  // Should not remove anything because "value" is not properly delimited
  EXPECT_TRUE(resp.hasHeader("X-Test"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value1, value2, value3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueMultipleOccurrences) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Multi", "apple, banana, apple, cherry");

  // Removes first occurrence within the value (left-to-right search)
  resp.headerRemoveValue("X-Multi", "apple");

  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "banana, apple, cherry");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithSpaces) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Spaced", "value1, value2, value3");

  resp.headerRemoveValue("X-Spaced", "value2");

  EXPECT_TRUE(resp.hasHeader("X-Spaced"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Spaced"), "value1, value3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueFromEmptyHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "");

  resp.headerRemoveValue("X-Test", "");

  EXPECT_FALSE(resp.hasHeader("X-Test"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueInMiddle) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value1, , value2");

  resp.headerRemoveValue("X-Test", "");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueAtFirst) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", ", value2");

  resp.headerRemoveValue("X-Test", "", ", ");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueAtLast) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "value2-");

  resp.headerRemoveValue("X-Test", "", "-");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCaseInsensitiveHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-CaSe", "val1, val2, val3");

  resp.headerRemoveValue("x-case", "val2");

  EXPECT_TRUE(resp.hasHeader("X-CaSe"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-CaSe"), "val1, val3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueFromDuplicateHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Dup", "alpha, beta");
  resp.headerAddLine("X-Dup", "gamma, delta");

  // Works on the last header (reverse search), but headerValue returns first
  resp.headerRemoveValue("X-Dup", "gamma");

  // Both headers still exist, second is now just "delta"
  EXPECT_TRUE(resp.hasHeader("X-Dup"));
  // headerValue returns the first header
  EXPECT_EQ(resp.headerValueOrEmpty("X-Dup"), "alpha, beta");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Values", "a, b, c");
  resp.body("Body content here");

  resp.headerRemoveValue("X-Values", "b");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Values"), "a, c");
  EXPECT_EQ(resp.bodyInMemory(), "Body content here");
}

TEST_F(HttpResponseTest, HeaderRemoveValueRValue) {
  auto resp =
      HttpResponse(http::StatusCodeOK).headerAddLine("X-Multi", "v1, v2, v3").headerRemoveValue("X-Multi", "v2");

  EXPECT_TRUE(resp.hasHeader("X-Multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Multi"), "v1, v3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueLeavesSingleValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Two", "first, second");

  resp.headerRemoveValue("X-Two", "first", ", ");

  EXPECT_TRUE(resp.hasHeader("X-Two"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Two"), "second");

  // Removing the only remaining value removes the whole header line
  resp.headerRemoveValue("X-Two", "second", ", ");
  EXPECT_FALSE(resp.hasHeader("X-Two"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueComplexSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Complex", "item1::item2::item3");

  resp.headerRemoveValue("X-Complex", "item2", "::");

  EXPECT_TRUE(resp.hasHeader("X-Complex"));
  EXPECT_EQ(resp.headerValueOrEmpty("X-Complex"), "item1::item3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithEmptySepartorShouldThrow) {
  EXPECT_THROW(HttpResponse{}.headerRemoveValue("X-Test", "value1", ""), std::invalid_argument);
}

TEST_F(HttpResponseTest, ContentEncodingHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.contentEncoding("gzip");
  resp.body("CompressedData");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentEncoding, "gzip")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "14")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nCompressedData"));
}

TEST_F(HttpResponseTest, ContentEncodingHeaderRValue) {
  auto resp = HttpResponse(http::StatusCodeOK).reason("OK").contentEncoding("deflate").body("DeflatedData");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentEncoding, "deflate")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "12")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nDeflatedData"));
}

TEST_F(HttpResponseTest, EmptyContentTypeIsDisallowed) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.body("some body", ""), std::invalid_argument);
  EXPECT_NO_THROW(resp.body("", ""));  // empty body with empty content type is allowed
}

TEST_F(HttpResponseTest, InvalidContentTypeIsDisallowed) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.body("some body", "text/\x7Fplain"), std::invalid_argument);
  EXPECT_THROW(resp.body("some body", "text/\r\nplain"), std::invalid_argument);

  EXPECT_THROW(resp.bodyAppend("some body", "text/\r\nplain"), std::invalid_argument);
  resp.body(std::string("captured body"));
  EXPECT_THROW(resp.bodyAppend("some body", "text/\x7Fplain"), std::invalid_argument);
}

TEST_F(HttpResponseTest, FileOffsetExceedsSizeThrows) {
  static constexpr std::string_view kPayload = "small";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.file(std::move(file), static_cast<std::size_t>(kPayload.size() + 1), 0), std::invalid_argument);
}

TEST_F(HttpResponseTest, FileOffsetPlusLengthExceedsSizeThrows) {
  static constexpr std::string_view kPayload = "12345";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  HttpResponse resp(http::StatusCodeOK);
  // offset 3, length 5 -> 8 > size(5)
  EXPECT_THROW(resp.file(std::move(file), 3, 5), std::invalid_argument);
}

TEST_F(HttpResponseTest, GlobalHeadersShouldNotOverrideUserHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Global", "UserValue");
  ConcatenatedHeaders globalHeaders;
  globalHeaders.append("X-Global: GlobalValue");
  globalHeaders.append("X-Another: AnotherValue");
  resp.reason("Some Reason");
  auto full = concatenated(std::move(resp), globalHeaders);
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 Some Reason\r\n"));
  EXPECT_TRUE(full.contains("X-Global: UserValue\r\n"));
  EXPECT_TRUE(full.contains("X-Another: AnotherValue\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderAppendValuesAreTrimmed) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAppendValue("X-Trimmed-Header", "   Value1   ", ", ");
  resp.headerAppendValue("X-Trimmed-Header", "\tValue2\t", ", ");
  resp.headerAppendValue("X-Trimmed-Header", " \t  Value3 \t ", ", ");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header"), "Value1, Value2, Value3");
}

TEST_F(HttpResponseTest, HeaderGetterAfterSet) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  // Mix of headers to exercise several lookup cases:
  // - customHeader replaces case-insensitively
  // - addCustomHeader allows duplicates (first occurrence should be returned by headerValue)
  // - empty value is a present-but-empty header
  resp.header("X-Simple", "hello");
  resp.headerAddLine("X-Dup", "1");
  resp.headerAddLine("X-Dup", "2");
  // Replace X-Simple with different casing (should replace existing header)
  resp.header("x-simple", "HELLO2");
  // Present but empty value
  resp.header("X-Empty", "");

  // headerValue should see the replaced value (case-insensitive replace)
  auto opt = resp.headerValue("X-Simple");
  EXPECT_EQ(opt.value_or(""), "HELLO2");

  // duplicate headers: headerValue returns the first occurrence
  auto dup = resp.headerValue("X-Dup");
  EXPECT_EQ(dup.value_or(""), "1");

  // empty-but-present header: headerValue returns an empty string_view but present
  auto emptyOpt = resp.headerValue("X-Empty");
  EXPECT_EQ(emptyOpt.value_or("something"), std::string_view{});

  // missing header should return nullopt via headerValue and empty view via headerValueOrEmpty
  auto missing = resp.headerValue("No-Such-Header");
  EXPECT_FALSE(missing.has_value());
  EXPECT_EQ(resp.headerValueOrEmpty("No-Such-Header"), std::string_view{});
}

TEST_F(HttpResponseTest, HeaderNewViaSetter) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-First", "One");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-First: One\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceCaseInsensitive) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("x-val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Val: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nData"));
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternLarger) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Key", "before X-Key: should-not-trigger");
  // Replace header; algorithm must not treat the embedded "X-Key: " in the value as another header start
  resp.header("X-Key", "REPLACED-VALUE");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Key: REPLACED-VALUE\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternSmaller) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Key", "AAAA X-Key: B BBBBBB");
  resp.header("X-Key", "SMALL");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Key: SMALL\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceLargerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Replace", "AA");
  // Replace with larger value
  resp.header("X-Replace", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Replace: ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceSameLengthValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Replace", "LEN10VALUE");  // length 10
  resp.header("X-Replace", "0123456789");  // also length 10
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Replace: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceSmallerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Replace", "LONG-LONG-VALUE");
  // Replace with smaller
  resp.header("X-Replace", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Replace: S\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderValueContentTypeAreTrimmed) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("Some body", "   text/custom   ");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/custom");

  resp.bodyAppend(" More body", "\ttext/another\t");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/another");

  resp.bodyInlineSet(16U, kAppendZeroOrOneA, " \t  text/inline \t ");
  resp.bodyInlineSet(16U, kAppendZeroOrOneA, " \t  text/inline \t ");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/inline");

  resp.bodyInlineAppend(16, kAppendZeroOrOneABytes, " \t  text/append \t ");
  resp.bodyInlineAppend(16, kAppendZeroOrOneABytes, " \t  text/append \t ");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/append");

  resp.bodyStatic("Static body", "   text/static   ");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/static");
}

TEST_F(HttpResponseTest, HeaderValueFindsLastHeader) {
  HttpResponse resp(http::StatusCodeOK);
  // Add multiple headers and ensure headerValue finds the last one when searching
  resp.headerAddLine("X-A", "one");
  resp.headerAddLine("X-B", "two");
  resp.headerAddLine("X-C", "three");

  EXPECT_EQ(resp.headerValue("X-C").value_or(""), "three");
  EXPECT_EQ(resp.headerValue("X-D"), std::nullopt);
  EXPECT_TRUE(resp.hasHeader("X-C"));
  EXPECT_FALSE(resp.hasHeader("X-D"));
}

TEST_F(HttpResponseTest, HeaderValuesAreTrimmedHeaderAddLine) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Trimmed-Header-1", "   Value1   ");
  resp.headerAddLine("X-Trimmed-Header-2", "\tValue2\t");
  resp.headerAddLine("X-Trimmed-Header-3", " \t  Value3 \t ");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-1"), "Value1");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-2"), "Value2");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-3"), "Value3");
}

TEST_F(HttpResponseTest, HeaderValuesAreTrimmedHeaderSet) {
  HttpResponse resp(http::StatusCodeOK);
  resp.header("X-Trimmed-Header-1", "   Value1   ");
  resp.header("X-Trimmed-Header-2", "\tValue2\t");
  resp.header("X-Trimmed-Header-3", " \t  Value3 \t ");
  resp.header("X-Trimmed-Header-3", " \t  Value33 \t ");

  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-1"), "Value1");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-2"), "Value2");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Trimmed-Header-3"), "Value33");
}

TEST_F(HttpResponseTest, LargeHeaderCountStress) {
  constexpr int kCount = 600;
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  for (int i = 0; i < kCount; ++i) {
    resp.headerAddLine("X-" + std::to_string(i), std::to_string(i));
  }
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  // Count custom headers (exclude Date/Connection)
  auto pos = full.find(http::CRLF) + 2;  // after status line CRLF
  int userHeaders = 0;
  std::string datePrefix(http::Date);
  datePrefix += ": ";

  std::string connectionPrefix(http::Connection);
  connectionPrefix += ": ";
  while (pos < full.size()) {
    auto lineEnd = full.find(http::CRLF, pos);
    ASSERT_NE(lineEnd, std::string_view::npos);
    if (lineEnd == pos) {
      pos += 2;
      break;
    }
    auto line = full.substr(pos, lineEnd - pos);
    if (!line.starts_with(datePrefix) && !line.starts_with(connectionPrefix)) {
      ++userHeaders;
    }
    pos = lineEnd + 2;
  }
  EXPECT_EQ(userHeaders, kCount);
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, LocationHeader) {
  HttpResponse resp(http::StatusCodeFound);
  resp.reason("Found");
  resp.location("http://example.com/new-location");
  resp.body("Redirecting...");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 302 Found\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Location, "http://example.com/new-location")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "14")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nRedirecting..."));
}

TEST_F(HttpResponseTest, LocationHeaderRValue) {
  auto resp = HttpResponse(http::StatusCodeFound)
                  .reason("Found")
                  .location("https://another.example.com/redirect-here")
                  .body("Please wait...");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 302 Found\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Location, "https://another.example.com/redirect-here")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "14")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nPlease wait..."));
}

TEST_F(HttpResponseTest, NoAddedHeadersInFinalize) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Custom", "Value");
  resp.body("BodyContent");
  resp.trailerAddLine("X-Trailer", "TrailerValue1");
  resp.trailerAddLine("X-Trailer-2", "TrailerValue-2");

  EXPECT_EQ(resp.trailersLength(), HttpResponse::HeaderSize(9U, 13U) + HttpResponse::HeaderSize(11U, 14U));

  static constexpr bool kHead = false;
  static constexpr bool kAddTrailerHeader = true;

  auto prepared = finalizePrepared(std::move(resp), {}, kHead, kAddTrailerHeader, true, kMinCapturedBodySize);
  std::string all(prepared.firstBuffer());
  all.append(prepared.secondBuffer());

  EXPECT_TRUE(all.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("X-Custom", "Value")));
  // When trailers are present, body is chunked encoded per RFC 7230 §4.1.2
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine(http::Trailer, "X-Trailer, X-Trailer-2")));
  EXPECT_FALSE(all.contains(http::ContentLength));
  // Body should be in chunked format: "b\r\nBodyContent\r\n0\r\n" (b = 11 in hex)
  EXPECT_TRUE(all.contains("b\r\nBodyContent\r\n0\r\n"));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("X-Trailer", "TrailerValue1")));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("X-Trailer-2", "TrailerValue-2")));
  EXPECT_FALSE(all.contains(MakeHttp1HeaderLine(http::Connection, http::close)));
  EXPECT_FALSE(all.contains(MakeHttp1HeaderLine(http::Connection, http::keepalive)));
  EXPECT_TRUE(all.contains(kExpectedDateRaw));
}

TEST_F(HttpResponseTest, ReplaceDifferentSizes) {
  HttpResponse resp1(http::StatusCodeOK, "OK");
  resp1.headerAddLine("X-A", "1").body("Hello");
  HttpResponse resp2(http::StatusCodeOK, "OK");
  resp2.headerAddLine("X-A", "1").body("Hello");
  HttpResponse resp3(http::StatusCodeOK, "OK");
  resp3.headerAddLine("X-A", "1").body("Hello");
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

TEST_F(HttpResponseTest, SendInvalidFile) {
  test::FileSyscallHookGuard guard;

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "some-data");

  test::gFstatSizes.setActions(tmp.filePath().string(), {-1});
  File file(tmp.filePath().string());

  auto resp = HttpResponse(http::StatusCodeOK, "OK");
  EXPECT_THROW(resp.file(std::move(file)), std::invalid_argument);
}

// =============================================================================
// BODY TESTS
// =============================================================================

TEST_F(HttpResponseTest, AllowsDuplicatesAfterResettingBody) {
  HttpResponse resp(204, "No Content");
  resp.headerAddLine("X-Dup", "1").headerAddLine("X-Dup", "2").body("");
  auto full = concatenated(std::move(resp));
  auto first = full.find("X-Dup: 1\r\n");
  auto second = full.find("X-Dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, AppendBodyAfterCapturedPayloadShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"some body"}, "text/captured");
  EXPECT_THROW(resp.bodyInlineAppend(16U, kAppendZeroOrOneA), std::logic_error);
  EXPECT_THROW(resp.bodyInlineAppend(1U, kAppendZeroOrOneABytes), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyInvalidContentTypeShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.bodyInlineAppend(16U, kAppendZeroOrOneA, "\x7F"), std::invalid_argument);
  EXPECT_THROW(resp.bodyInlineAppend(1U, kAppendZeroOrOneABytes, "\x7F"), std::invalid_argument);
}

TEST_F(HttpResponseTest, AppendBodyAfterFileCapturedIsLogicError) {
  HttpResponse resp(http::StatusCodeOK);
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "data");
  File file(tmp.filePath().string());

  resp.file(std::move(file));

  EXPECT_THROW(resp.bodyAppend("additional body"), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyBytesSpan) {
  for (bool captured : {true, false}) {
    static constexpr std::byte vec[]{std::byte{'X'}, std::byte{'Y'}};
    HttpResponse resp(http::StatusCodeOK);
    if (captured) {
      resp.body(std::string{"XY"}, "text/initial");
    } else {
      resp.bodyAppend(std::span<const std::byte>(vec), "text/initial");
    }

    resp.bodyAppend(std::span<const std::byte>{}, "text/another");
    resp.bodyAppend(std::span<const std::byte>{vec}, "text/another2");
    EXPECT_EQ(resp.bodyInMemory(), "XYXY");
    EXPECT_EQ(resp.headerValue(http::ContentType), "text/another2");
    EXPECT_EQ(resp.headerValue(http::ContentLength), "4");

    while (resp.bodyInMemoryLength() != 10) {
      resp.bodyAppend(std::span<const std::byte>(vec));
    }
    std::string expected = "XYXYXYXYXY";
    EXPECT_EQ(resp.bodyInMemory(), expected);
    EXPECT_EQ(resp.headerValue(http::ContentLength), "10");

    auto bodyLen = resp.bodyInMemoryLength();
    EXPECT_EQ(bodyLen, 10U);
    while (resp.bodyInMemoryLength() < 1024UL) {
      resp.bodyAppend(std::span<const std::byte>(vec));
      bodyLen += std::size(vec);
      expected += "XY";
      EXPECT_EQ(resp.bodyInMemory(), expected);
      EXPECT_EQ(resp.bodyInMemoryLength(), bodyLen);
      EXPECT_EQ(resp.headerValue(http::ContentLength), std::string_view(IntegralToCharVector(bodyLen)));
    }
  }
}

TEST_F(HttpResponseTest, AppendBodyCStrRvalue) {
  auto resp =
      HttpResponse(http::StatusCodeOK).bodyAppend("Hello, C-String!").bodyAppend(static_cast<const char*>(nullptr));
  EXPECT_EQ(resp.bodyInMemory(), "Hello, C-String!");
}

TEST_F(HttpResponseTest, AppendBodyCapturedStringView) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"captured body"}, "text/captured");

  resp.bodyAppend(std::string_view(" appended body"), "");
  EXPECT_EQ(resp.bodyInMemory(), "captured body appended body");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/captured");  // unchanged since contentType was empty

  resp.bodyAppend(std::string_view(" more"), "text/appended");
  EXPECT_EQ(resp.bodyInMemory(), "captured body appended body more");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/appended");
}

TEST_F(HttpResponseTest, AppendBodyFromEmpty) {
  HttpResponse resp(http::StatusCodeOK);

  resp.bodyInlineAppend(16U, kAppendZeroOrOneA, "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.hasBody());
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_FALSE(resp.hasHeader(http::ContentLength));

  resp.bodyInlineAppend(16U, kAppendZeroOrOneA, "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "A");
  EXPECT_TRUE(resp.hasBody());
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "1");

  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes, "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "A");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "1");

  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes, "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "AA");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "2");

  resp.bodyInlineAppend(16U, kAppendZeroOrOneA);
  counter = 0;
  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes);
  counter = 0;
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "AA");
}

TEST_F(HttpResponseTest, AppendBodyFromNonEmpty) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial body ");
  resp.bodyInlineAppend(16U, kAppendZeroOrOneA, "text/custom");
  resp.bodyInlineAppend(16U, kAppendZeroOrOneA, "text/custom");
  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes, "text/custom");
  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes, "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "initial body AA");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
}

TEST_F(HttpResponseTest, AppendBodyInlineCstr) {
  // c-string nullptr should be treated as empty (no change)
  HttpResponse resp(http::StatusCodeOK);
  resp.body("orig");
  resp.bodyAppend(static_cast<const char*>(nullptr));
  EXPECT_EQ(resp.bodyInMemory(), "orig");

  // c-string non-null
  resp = HttpResponse(http::StatusCodeOK);
  resp.bodyAppend("abc", "text/x-test");
  EXPECT_EQ(resp.bodyInMemory(), "abc");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/x-test");
}

TEST_F(HttpResponseTest, AppendBodyInlineStringView) {
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyAppend(std::string_view("hello"));
  EXPECT_EQ(resp.bodyInMemory(), "hello");
  // Check content-length and content-type headers
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "5");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");

  resp.bodyAppend(std::string_view(" world"), "text/greeting");
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
  // Check content-length and content-type headers
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "11");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/greeting");
}

TEST_F(HttpResponseTest, AppendBodyMultipleFlavorsAndRvalueChaining) {
  // start with a body
  HttpResponse resp(http::StatusCodeOK);
  resp.body("start ");

  // append with string_view
  resp.bodyAppend(std::string_view("middle "));

  // append with span
  static constexpr std::byte tail[]{std::byte{'t'}, std::byte{'e'}, std::byte{'r'}};
  resp.bodyAppend(std::span<const std::byte>(tail));

  EXPECT_EQ(resp.bodyInMemory(), "start middle ter");

  // rvalue chaining
  auto chained =
      HttpResponse(http::StatusCodeOK).bodyAppend(std::string_view("one")).bodyAppend(std::string_view("two"));
  EXPECT_EQ(chained.bodyInMemory(), "onetwo");
}

TEST_F(HttpResponseTest, AppendBodyRValue) {
  auto resp = HttpResponse{}
                  .bodyInlineAppend(16U, kAppendZeroOrOneA)
                  .bodyInlineAppend(16U, kAppendZeroOrOneA)
                  .bodyInlineAppend(16U, kAppendZeroOrOneABytes)
                  .bodyInlineAppend(16U, kAppendZeroOrOneABytes);
  EXPECT_EQ(resp.bodyInMemory(), "AA");
}

TEST_F(HttpResponseTest, AppendBodyRvalueSpanBytesContentType) {
  static constexpr std::byte vec[]{std::byte{'A'}, std::byte{'B'}, std::byte{'C'}};
  auto resp = HttpResponse(http::StatusCodeOK)
                  .bodyAppend(std::span<const std::byte>(vec))
                  .bodyAppend(std::span<const std::byte>(vec), "text/type")
                  .bodyAppend(std::span<const std::byte>{}, "some/type")
                  .bodyAppend(std::span<const std::byte>{});
  EXPECT_EQ(resp.bodyInMemory(), "ABCABC");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/type");
}

TEST_F(HttpResponseTest, AppendBodyRvalueSpanBytesDefaultContentType) {
  static constexpr std::byte vec[]{std::byte{'A'}, std::byte{'B'}, std::byte{'C'}};
  auto resp = HttpResponse(http::StatusCodeOK).bodyAppend(std::span<const std::byte>(vec));
  EXPECT_EQ(resp.bodyInMemory(), "ABC");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/octet-stream");
}

TEST_F(HttpResponseTest, AppendHeaderValueKeepsBodyIntact) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Trace", "alpha");
  resp.body("payload");
  resp.headerAppendValue("X-Trace", "beta");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Trace: alpha, beta\r\n")) << full;
  EXPECT_TRUE(full.ends_with("payload")) << full;
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "7"))) << full;
}

TEST_F(HttpResponseTest, AppendToCapturedBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("Body"), "text/captured");
  resp.bodyAppend(" plus appended part");
  EXPECT_EQ(resp.bodyInMemory(), "Body plus appended part");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/captured");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "23");
}

TEST_F(HttpResponseTest, AppendToInlineBodyFromEmptyShouldNotAddContentTypeIfNoDataWritten) {
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyInlineAppend(16U, kAppendZeroOrOneA);
  counter = 0;
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType));
  resp.bodyInlineAppend(16U, kAppendZeroOrOneABytes);
  counter = 0;
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType));
}

TEST_F(HttpResponseTest, BodyAppendToCapturedBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"captured"}, "text/captured");
  EXPECT_EQ(resp.bodyInMemory(), "captured");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "8");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/captured");

  resp.bodyAppend(" appended body", "");
  EXPECT_EQ(resp.bodyInMemory(), "captured appended body");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "22");           // updated content-length
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/captured");  // unchanged since contentType was empty

  resp.bodyAppend(" more", "text/more");
  EXPECT_EQ(resp.bodyInMemory(), "captured appended body more");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "27");       // updated content-length
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/more");  // changed since contentType was provided
}

TEST_F(HttpResponseTest, BodyFromConstCharStar) {
  const char* bodyCStr = "Hello, C-String!";
  HttpResponse resp(http::StatusCodeOK);
  resp.body(bodyCStr);
  EXPECT_EQ(resp.bodyInMemory(), "Hello, C-String!");

  const char* nullPtr = nullptr;

  auto resp2 = HttpResponse(http::StatusCodeOK).body(nullPtr);
  EXPECT_EQ(resp2.bodyInMemory(), "");
}

TEST_F(HttpResponseTest, BodyFromSpanBytesRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .body(std::span<const std::byte>(std::vector<std::byte>{
                      std::byte{'W'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'}, std::byte{'d'}}));
  EXPECT_EQ(resp.bodyInMemory(), "World");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrByte) {
  const char text[] = "UniquePtrByte";
  auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text));
  for (std::size_t i = 0; i < sizeof(text); ++i) {
    bodyPtr[i] = static_cast<std::byte>(text[i]);
  }
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.bodyInMemory(), "UniquePtrByte");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrByteRValue) {
  const char text[] = "UniquePtrByteRValue";
  auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text));
  for (std::size_t i = 0; i < sizeof(text); ++i) {
    bodyPtr[i] = static_cast<std::byte>(text[i]);
  }
  auto resp = HttpResponse(http::StatusCodeOK).body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.bodyInMemory(), "UniquePtrByteRValue");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrChar) {
  const char text[] = "UniquePtrChar";
  auto bodyPtr = std::make_unique<char[]>(sizeof(text));
  std::ranges::copy(text, bodyPtr.get());
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.bodyInMemory(), "UniquePtrChar");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrCharRValue) {
  const char text[] = "UniquePtrCharRValue";
  auto bodyPtr = std::make_unique<char[]>(sizeof(text));
  std::ranges::copy(text, bodyPtr.get());
  auto resp = HttpResponse(http::StatusCodeOK).body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.bodyInMemory(), "UniquePtrCharRValue");
}

TEST_F(HttpResponseTest, BodyFromVectorBytes) {
  std::vector<std::byte> bodyBytes = {std::byte{'B'}, std::byte{'y'}, std::byte{'t'}, std::byte{'e'}, std::byte{'s'}};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyBytes));
  EXPECT_EQ(resp.bodyInMemory(), "Bytes");

  resp = MakePrepared(true, true);
  resp.body(std::vector<std::byte>{std::byte{'B'}, std::byte{'y'}, std::byte{'t'}, std::byte{'e'}, std::byte{'s'}});
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_EQ(resp.bodyInMemoryLength(), 5UL);
}

TEST_F(HttpResponseTest, BodyFromVectorBytesRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .body(std::vector<std::byte>{std::byte{'R'}, std::byte{'V'}, std::byte{'a'}, std::byte{'l'},
                                               std::byte{'u'}, std::byte{'e'}});
  EXPECT_EQ(resp.bodyInMemory(), "RValue");
}

TEST_F(HttpResponseTest, BodyFromVectorChar) {
  std::vector<char> bodyChars = {'C', '+', '+'};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyChars));
  EXPECT_EQ(resp.bodyInMemory(), "C++");
}

TEST_F(HttpResponseTest, BodyFromVectorCharRValue) {
  auto resp = HttpResponse(http::StatusCodeOK).body(std::vector<char>{'R', 'V', 'a', 'l', 'u', 'e'});
  EXPECT_EQ(resp.bodyInMemory(), "RValue");
}

TEST_F(HttpResponseTest, BodyStaticBytes) {
  static constexpr std::byte bodyBytes[]{std::byte{'S'}, std::byte{'t'}, std::byte{'a'},
                                         std::byte{'t'}, std::byte{'i'}, std::byte{'c'}};
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyStatic(std::span<const std::byte>(bodyBytes), "application/octet-stream");
  EXPECT_EQ(resp.bodyInMemory(), "Static");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/octet-stream");
  EXPECT_EQ(resp.headerValue(http::ContentLength), std::to_string(sizeof(bodyBytes)));

  resp = HttpResponse{}.bodyStatic(std::span<const std::byte>(bodyBytes), "application/data");
  EXPECT_EQ(resp.bodyInMemory(), "Static");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/data");
  EXPECT_EQ(resp.headerValue(http::ContentLength), std::to_string(sizeof(bodyBytes)));

  resp = HttpResponse{}.bodyStatic(std::span<const std::byte>{}, "text/empty");
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_FALSE(resp.hasHeader(http::ContentLength));
}

TEST_F(HttpResponseTest, BodyStaticSv) {
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyStatic("This is a static body", "text/static");
  EXPECT_EQ(resp.bodyInMemory(), "This is a static body");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/static");
  EXPECT_EQ(resp.headerValue(http::ContentLength), std::to_string(std::string_view("This is a static body").size()));

  resp = HttpResponse{}.bodyStatic("Another static body, it's great because it does not allocate memory", "text/empty");
  EXPECT_EQ(resp.bodyInMemory(), "Another static body, it's great because it does not allocate memory");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/empty");
  EXPECT_EQ(
      resp.headerValue(http::ContentLength),
      std::to_string(std::string_view("Another static body, it's great because it does not allocate memory").size()));

  resp = HttpResponse{}.bodyStatic("", "text/empty");
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_FALSE(resp.hasHeader(http::ContentLength));

  // Even for HEAD method, static body should not be visible
  resp = MakePrepared(true, true).bodyStatic("Head method static body", "text/head");
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/head");

  resp.bodyStatic("", "text/empty");
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.hasHeader(http::ContentType));
  EXPECT_FALSE(resp.hasHeader(http::ContentLength));
}

TEST_F(HttpResponseTest, FileWithClosedFileThrows) {
  File file;  // default-constructed, closed
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.file(std::move(file)), std::invalid_argument);
}

TEST_F(HttpResponseTest, HeadBodyWithoutGlobalHeaders) {
  HttpResponse resp("Hello, World!");
  auto full = concatenated(std::move(resp), {}, true, true);
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
  EXPECT_FALSE(full.contains("Hello, World!"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodyLargerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Val", "AA");
  resp.body("Hello");                  // body length 5
  resp.header("X-Val", "ABCDEFGHIJ");  // grow header value
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Val: ABCDEFGHIJ\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "5")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySameLengthValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("X-Val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Val: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nData"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySmallerValue) {
  auto resp = HttpResponse(http::StatusCodeOK).reason("OK").header("X-Val", "SOME-LONG-VALUE");
  resp.body("WorldWide");     // length 9
  resp.header("X-Val", "S");  // shrink header value
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("X-Val: S\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "9")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nWorldWide"));
}

TEST_F(HttpResponseTest, ProperTermination) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
}

TEST_F(HttpResponseTest, SendFileEmptyShouldReturnNullptr) {
  HttpResponse resp;
  EXPECT_EQ(resp.file(), nullptr);
}

TEST_F(HttpResponseTest, SendFileHeadMovesFileAndSuppressesLength) {
  constexpr std::string_view kPayload = "head sendfile payload move";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const std::size_t sz = file.size();

  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.file(std::move(file));

  EXPECT_TRUE(resp.hasBodyFile());
  EXPECT_TRUE(resp.hasBody());

  auto prepared = finalizePrepared(std::move(resp), true /*head*/);
  // The file should be moved out, but head suppresses payload length to 0
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), 0U);
  EXPECT_EQ(prepared.file().size(), sz);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(sz))));
  EXPECT_FALSE(headers.contains(http::TransferEncoding));
}

TEST_F(HttpResponseTest, SendFileHeadSuppressesPayload) {
  static constexpr std::string_view kPayload = "head sendfile payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const std::size_t sz = file.size();

  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.file(std::move(file));

  auto prepared = finalizePrepared(std::move(resp), true /*head*/);
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), 0U);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(sz))));
  EXPECT_FALSE(headers.contains(http::TransferEncoding));
}

TEST_F(HttpResponseTest, SendFilePayload) {
  constexpr std::string_view kPayload = "static file payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();

  auto resp = HttpResponse(http::StatusCodeOK, "OK").file(std::move(file));

  EXPECT_EQ(resp.bodyInlinedLength(), 0);
  EXPECT_EQ(resp.bodyInMemoryLength(), 0);
  EXPECT_EQ(resp.bodyLength(), sz);
  EXPECT_TRUE(resp.hasBodyFile());
  EXPECT_TRUE(resp.hasBody());
  EXPECT_FALSE(resp.hasBodyCaptured());
  EXPECT_FALSE(resp.hasBodyInMemory());

  EXPECT_THROW(resp.trailerAddLine("X-trailer", "value");, std::logic_error);

  auto prepared = finalizePrepared(std::move(resp));
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), sz);
  EXPECT_EQ(prepared.file().size(), sz);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(sz))));
  EXPECT_FALSE(headers.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

TEST_F(HttpResponseTest, SendFilePayloadOffsetLength) {
  constexpr std::string_view kPayload = "static file payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();

  auto resp = HttpResponse(http::StatusCodeOK, "OK").file(std::move(file), 2, sz - 4);

  auto prepared = finalizePrepared(std::move(resp));
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), sz - 4);
  EXPECT_EQ(prepared.file().size(), sz);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(sz - 4))));
  EXPECT_FALSE(headers.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

TEST_F(HttpResponseTest, SendFilePayloadOffsetLengthRvalue) {
  constexpr std::string_view kPayload = "static file payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();

  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.file(std::move(file), 3, sz - 6);

  auto prepared = finalizePrepared(std::move(resp));
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), sz - 6);
  EXPECT_EQ(prepared.file().size(), sz);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(sz - 6))));
  EXPECT_FALSE(headers.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

TEST_F(HttpResponseTest, SendFileZeroLengthPayload) {
  // Create an empty temp file
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "");
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();
  EXPECT_EQ(sz, 0U);

  auto resp = HttpResponse(http::StatusCodeOK, "OK");
  resp.file(std::move(file));

  auto prepared = finalizePrepared(std::move(resp));
  ASSERT_NE(prepared.getIfFilePayload(), nullptr);
  EXPECT_EQ(prepared.fileLength(), 0U);

  std::string headers(prepared.firstBuffer());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentLength, "0")));
  EXPECT_FALSE(headers.contains(http::TransferEncoding));
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyFromUniquePtrShouldResetBodyAndRemoveContentType) {
  for (bool head : {false, true}) {
    auto resp = MakePrepared(head, head);
    resp.reason("Longer Reason");
    static constexpr const char text[] = "UniquePtrBody";
    auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text) - 1);
    for (size_t i = 0; i < sizeof(text) - 1; ++i) {
      bodyPtr[i] = static_cast<std::byte>(text[i]);
    }
    resp.body(std::move(bodyPtr), sizeof(text) - 1);
    if (head) {
      EXPECT_EQ(resp.bodyInMemory(), "");
    } else {
      EXPECT_EQ(resp.bodyInMemory(), "UniquePtrBody");
    }

    EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
    resp.body(std::make_unique<char[]>(0), 0);  // set empty body
    EXPECT_EQ(resp.bodyInMemory(), "");
    EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
    auto full = concatenated(std::move(resp), {}, head);
    EXPECT_TRUE(full.starts_with("HTTP/1.1 200 Longer Reason\r\n"));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
    EXPECT_TRUE(full.contains(kExpectedDateRaw));
    EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
  }
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeString) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.body("Non-empty body");
  EXPECT_EQ(resp.bodyInMemory(), "Non-empty body");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::string());  // set empty body
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.hasBodyFile());
  EXPECT_FALSE(resp.hasBody());
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeUniquePtrBytes) {
  for (bool head : {false, true}) {
    auto resp = MakePrepared(head, head);
    resp.reason("OK");
    resp.body("Non-empty body");
    if (head) {
      EXPECT_EQ(resp.bodyInMemory(), "");
    } else {
      EXPECT_EQ(resp.bodyInMemory(), "Non-empty body");
    }
    EXPECT_TRUE(resp.hasHeader(http::ContentType));
    resp.body(std::unique_ptr<std::byte[]>(), 0);  // set empty body
    EXPECT_EQ(resp.bodyInMemory(), "");
    EXPECT_FALSE(resp.hasHeader(http::ContentType));
    auto full = concatenated(std::move(resp), {}, head);
    EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
    EXPECT_TRUE(full.contains(kExpectedDateRaw));
    EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
  }
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeVectorBytes) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.body("Non-empty body");
  EXPECT_EQ(resp.bodyInMemory(), "Non-empty body");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::vector<std::byte>{});  // set empty body
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, SetInlineBodyFromWriterCharPtrDefaultContentType) {
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyInlineSet(8U, kAppendZeroOrOneA);
  EXPECT_FALSE(resp.headerValue(http::ContentType));
  EXPECT_EQ(resp.bodyInMemory(), "");
  resp.bodyInlineSet(8U, kAppendZeroOrOneA, "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "A");
  resp.bodyInlineSet(8U, kAppendZeroOrOneABytes);
  EXPECT_FALSE(resp.headerValue(http::ContentType));
  EXPECT_EQ(resp.bodyInMemory(), "");

  resp.bodyInlineSet(8U, kAppendZeroOrOneABytes, "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.bodyInMemory(), "A");

  // should be ok after captured payload
  resp.body(std::string{"captured body"}, "text/captured");
  resp.bodyInlineSet(8U, kAppendZeroOrOneA);
  resp.bodyInlineSet(8U, kAppendZeroOrOneA);
  EXPECT_EQ(resp.bodyInMemory(), "A");
  resp.bodyInlineSet(8U, kAppendZeroOrOneABytes);
  resp.bodyInlineSet(8U, kAppendZeroOrOneABytes);
  EXPECT_EQ(resp.bodyInMemory(), "A");
}

TEST_F(HttpResponseTest, SetInlineBodyFromWriterEmptyWriteNoContentType) {
  HttpResponse resp(http::StatusCodeOK);
  // writer that writes nothing - should not add any content type header
  resp.bodyInlineSet(8U, kAppendZeroOrOneA);
  counter = 0;
  resp.bodyInlineSet(8U, kAppendZeroOrOneABytes);
  counter = 0;
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType));
}

TEST_F(HttpResponseTest, BodyInlineSetInvalidContentTypeThrows) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneA, "\x7F"), std::invalid_argument);
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneABytes, "\x7F"), std::invalid_argument);
}

TEST_F(HttpResponseTest, SeveralBodyAppend) {
  HttpResponse resp(http::StatusCodeOK);
  resp.bodyAppend("Some body data that takes roughly 50 characters.\n", "text/plain");
  EXPECT_EQ(resp.bodyInMemory(), "Some body data that takes roughly 50 characters.\n");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/plain");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "49");
  resp.bodyAppend(" Additional data to be appended");
  EXPECT_EQ(resp.bodyInMemory(), "Some body data that takes roughly 50 characters.\n Additional data to be appended");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/plain");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "80");
  resp.bodyAppend(
      " And some more to reach more than 100 characters in total. Lorem ipsum dolor sit amet, "
      "consectetur adipiscing elit.",
      "text/custom");
  EXPECT_EQ(resp.bodyInMemory(),
            "Some body data that takes roughly 50 characters.\n Additional data to be appended And some more to reach "
            "more than 100 characters in total. Lorem ipsum dolor sit amet, consectetur adipiscing elit.");
  EXPECT_EQ(resp.bodyLength(), 195UL);
  EXPECT_EQ(resp.bodySize(), 195UL);
  EXPECT_EQ(resp.bodyInMemoryLength(), 195UL);
  EXPECT_EQ(resp.bodyInMemorySize(), 195UL);
  EXPECT_EQ(resp.bodyInlinedLength(), 195UL);
  EXPECT_EQ(resp.bodyInlinedSize(), 195UL);
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
  EXPECT_EQ(resp.headerValue(http::ContentLength), "195");
}

TEST_F(HttpResponseTest, SimpleBodyWithoutGlobalHeaders) {
  static constexpr std::size_t kMinCapturedBodySize[] = {1ULL, 13ULL, 4096ULL};
  for (const auto minCapturedBodySize : kMinCapturedBodySize) {
    HttpResponse resp;
    resp.body(std::string("Hello, World!"));
    EXPECT_FALSE(resp.hasBodyFile());
    EXPECT_TRUE(resp.hasBody());
    EXPECT_TRUE(resp.hasBodyCaptured());
    auto full = concatenated(std::move(resp), {}, false, true, minCapturedBodySize);
    EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
    EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
  }
}

TEST_F(HttpResponseTest, SingleTerminatingCRLF) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.headerAddLine("X-Header", "v1");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
  EXPECT_TRUE(full.contains("X-Header: v1"));
}

// =============================================================================
// TRAILERS TESTS
// =============================================================================

TEST_F(HttpResponseTest, AddTrailerAfterEmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");
  // Explicitly-empty body should still be considered 'no body' for trailers
  EXPECT_THROW(resp.trailerAddLine("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AddTrailerWithoutBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  // No body set at all -> adding trailer should throw
  EXPECT_THROW(resp.trailerAddLine("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyAfterTrailersShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("X-Trailer", "value");
  EXPECT_THROW(resp.bodyInlineAppend(16U, kAppendZeroOrOneA), std::logic_error);
  EXPECT_THROW(resp.bodyInlineAppend(1U, kAppendZeroOrOneABytes), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyFromStringAfterTrailersIsLogicError) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("X-Trailer", "value");
  EXPECT_THROW(resp.bodyAppend("additional body"), std::logic_error);
}

TEST_F(HttpResponseTest, CannotSendFileAfterTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.body("some body");
  resp.trailerAddLine("X-trailer", "value");
  constexpr std::string_view kPayload = "static file payload";

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());

  EXPECT_THROW(resp.file(std::move(file)), std::logic_error);
}

TEST_F(HttpResponseTest, CapturedBodyWithTrailersAppendsFinalCRLF) {
  // Create a captured body larger than minCapturedBodySize (4096) so it remains external
  std::string bigBody(5000, 'x');
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bigBody));
  resp.trailerAddLine("X-Custom-Trail", "trail-value");

  // Finalize and inspect the serialized response which concatenates head + external payload
  auto prepared = finalizePrepared(std::move(resp));
  std::string tail(prepared.secondBuffer());

  // The external payload (tail) should contain the body followed by the trailer line and a terminating CRLF
  EXPECT_TRUE(tail.contains("X-Custom-Trail: trail-value\r\n"));
  EXPECT_TRUE(tail.size() >= 2 && tail.substr(tail.size() - 2) == "\r\n");
}

TEST_F(HttpResponseTest, LoopOnTrailers) {
  HttpResponse resp("some body");
  auto trailers = resp.trailers();

  EXPECT_EQ(trailers.begin(), trailers.end());
  resp.trailerAddLine("Header-1", "Value1");

  trailers = resp.trailers();
  EXPECT_EQ(std::distance(trailers.begin(), trailers.end()), 1);
  EXPECT_EQ((*trailers.begin()).name, "Header-1");
  EXPECT_EQ((*trailers.begin()).value, "Value1");

  resp.trailerAddLine("Header-2", "Value2").trailerAddLine("Header-3", "Value3");

  trailers = resp.trailers();
  EXPECT_EQ(std::distance(trailers.begin(), trailers.end()), 3);
  auto it = trailers.begin();
  EXPECT_EQ((*it).name, "Header-1");
  EXPECT_EQ((*it).value, "Value1");
  ++it;
  EXPECT_EQ((*it).name, "Header-2");
  EXPECT_EQ((*it).value, "Value2");
  ++it;
  EXPECT_EQ((*it).name, "Header-3");
  EXPECT_EQ((*it).value, "Value3");
  EXPECT_EQ(++it, trailers.end());
}

TEST_F(HttpResponseTest, SetBodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.trailerAddLine("X-Test", "val");
  // Once a trailer was inserted, setting body later must throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
}

TEST_F(HttpResponseTest, SetInlineBodyFromWriterShouldThrowAfterTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("X-Trailer", "value");
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneA), std::logic_error);
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneABytes), std::logic_error);
}

// =============================================================================
// OTHER TESTS
// =============================================================================

TEST_F(HttpResponseTest, RepeatedGrowShrinkCycles) {
  HttpResponse resp(http::StatusCodeOK, "");
  resp.headerAddLine("X-Static", "STATIC");
  resp.header("X-Cycle", "A");
  resp.reason("R1");
  resp.header("X-Cycle", "ABCDEFGHIJ");
  resp.body("one");
  resp.reason("");
  resp.header("X-Cycle", "B");
  resp.body("two-two");
  resp.reason("LONGER-REASON");
  resp.header("X-Cycle", "ABCDEFGHIJKLMNOP");
  resp.body("short");
  resp.reason("");
  resp.header("X-Cycle", "C");
  resp.body("0123456789ABCDEFGHIJ");
  resp.header("X-Cycle", "LONGVALUE-1234567890");
  resp.reason("MID");
  resp.header("X-Cycle", "X");
  resp.body("XYZ");
  resp.reason("");
  resp.header("X-Cycle", "Z");
  resp.body("END");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains("X-Static: STATIC\r\n"));
  EXPECT_TRUE(full.contains("X-Cycle: Z\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "3")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nEND"));
}

// =============================================================================
// FUZZ / VALIDATION TESTS - Testing many things together
// =============================================================================

namespace {  // local helpers for fuzz test
struct ParsedResponse {
  int status{};
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::vector<std::pair<std::string, std::string>> trailers;
};

ParsedResponse parseResponse(std::string_view full, bool hasFile) {
  ParsedResponse pr;
  if (!full.starts_with("HTTP/1.1 ")) {
    throw std::runtime_error("Bad version in response");
  }
  // Extract the status line first (up to CRLF)
  auto firstCRLF = full.find(http::CRLF);
  if (firstCRLF == std::string_view::npos) {
    throw std::runtime_error("Missing CRLF after status line in response");
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
    throw std::runtime_error("Missing terminating header block");
  }
  std::size_t cursor = firstCRLF + http::CRLF.size();  // move past CRLF into headers section
  while (cursor < headerEnd) {
    auto eol = full.find(http::CRLF, cursor);
    if (eol == std::string_view::npos || eol > headerEnd) {
      throw std::runtime_error("Invalid header line in response");
    }
    auto line = full.substr(cursor, eol - cursor);
    auto sep = line.find(http::HeaderSep);
    if (sep == std::string_view::npos) {
      throw std::runtime_error("No separator in header line in response");
    }
    pr.headers.emplace_back(line.substr(0, sep), line.substr(sep + 2));
    cursor = eol + 2;
  }
  cursor = headerEnd + http::DoubleCRLF.size();  // move past CRLFCRLF into body
  // If Content-Length header present, body length is known; otherwise body is the remainder
  std::size_t contentLen = 0;
  bool hasContentLen = false;
  for (auto& hdr : pr.headers) {
    if (hdr.first == http::ContentLength) {
      contentLen = StringToIntegral<std::size_t>(hdr.second);
      hasContentLen = true;
      break;
    }
  }

  if (hasContentLen && !hasFile) {
    if (cursor + contentLen > full.size()) {
      throw std::runtime_error("Truncated body");
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
            throw std::runtime_error("No terminating trailer line in response");
          }
          if (eol == cursor) {  // blank line terminator
            cursor += http::CRLF.size();
            break;
          }
          auto line = full.substr(cursor, eol - cursor);
          auto sep = line.find(http::HeaderSep);
          if (sep == std::string_view::npos) {
            throw std::runtime_error("No separator in trailer line in response");
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

const std::pair<std::string, std::string>* FindHeaderCaseInsensitive(const ParsedResponse& pr, std::string_view name) {
  for (const auto& headerPair : pr.headers) {
    if (CaseInsensitiveEqual(headerPair.first, name)) {
      return &headerPair;
    }
  }
  return nullptr;
}

auto ExpectedGlobalHeaderValues(const HttpResponse& resp, const ConcatenatedHeaders& globalHeaders) {
  flat_hash_map<std::string, std::string, CaseInsensitiveHashFunc, CaseInsensitiveEqualFunc> expected;
  for (std::string_view gh : globalHeaders) {
    std::string_view name = gh.substr(0, gh.find(": "));
    std::string_view value = gh.substr(gh.find(": ") + 2);
    auto opt = resp.headerValue(name);
    if (opt) {
      expected.emplace(std::string(name), *opt);
    } else {
      expected.emplace(std::string(name), std::string(value));
    }
  }
  return expected;
}

}  // namespace

TEST_F(HttpResponseTest, RandomGlobalHeadersApplyOnce) {
  constexpr int kCases = 64;
  std::mt19937 rng(20251115);
  std::uniform_int_distribution<int> globalCountDist(0, 64);
  std::uniform_int_distribution<int> valueLenDist(1, 24);
  std::bernoulli_distribution userOverrideDist(0.35);

  auto makeValue = [&](int len) {
    std::string value;
    value.reserve(static_cast<std::size_t>(len));
    for (int i = 0; i < len; ++i) {
      value.push_back(static_cast<char>('A' + (rng() % 26)));
    }
    return value;
  };

  for (int iter = 0; iter < kCases; ++iter) {
    HttpResponse resp(http::StatusCodeOK);
    resp.body("payload-" + std::to_string(iter));
    ConcatenatedHeaders globalHeaders;
    const int headerCount = globalCountDist(rng);
    for (int headerIdx = 0; headerIdx < headerCount; ++headerIdx) {
      std::string name = "X-Global-" + std::to_string(iter) + "-" + std::to_string(headerIdx);
      std::string value = makeValue(valueLenDist(rng));
      std::string header = name;
      header += http::HeaderSep;
      header += value;
      globalHeaders.append(header);
      if (userOverrideDist(rng)) {
        resp.header(name, "user-" + value);
      }
    }

    auto expected = ExpectedGlobalHeaderValues(resp, globalHeaders);
    auto serialized = concatenated(std::move(resp), globalHeaders);
    ParsedResponse parsed = parseResponse(serialized, false);

    for (std::string_view gh : globalHeaders) {
      // gh is a string_view of the form "Name: Value". Extract the name for comparisons.
      const auto sep = gh.find(http::HeaderSep);
      ASSERT_NE(sep, std::string_view::npos);
      std::string_view name = gh.substr(0, sep);
      const auto* actual = FindHeaderCaseInsensitive(parsed, name);
      ASSERT_NE(actual, nullptr) << "Missing global header: " << name << " in response\n" << serialized;
      auto expIt = expected.find(name);
      ASSERT_NE(expIt, expected.end());
      EXPECT_EQ(actual->second, expIt->second) << "Header mismatch for " << name << " in response\n" << serialized;

      const auto occurrences = std::count_if(parsed.headers.begin(), parsed.headers.end(),
                                             [&](const auto& hdr) { return CaseInsensitiveEqual(hdr.first, name); });
      EXPECT_EQ(occurrences, 1) << "Duplicate copies of global header '" << name << "'";
    }
  }
}

TEST_F(HttpResponseTest, ALotOfGlobalHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Seed", "local-value");
  resp.body("payload");

  constexpr int kGlobalHeaders = HttpServerConfig::kMaxGlobalHeaders;
  // Build concatenated global headers but keep an indexed vector for targeted overrides below.
  std::vector<http::Header> headerVec;
  headerVec.reserve(kGlobalHeaders);
  aeronet::ConcatenatedHeaders globalHeaders;
  for (int headerIdx = 0; headerIdx < kGlobalHeaders; ++headerIdx) {
    std::string name = "X-Bulk-" + std::to_string(headerIdx);
    std::string value = "Value-" + std::to_string(headerIdx);
    headerVec.emplace_back(name, value);
    std::string header;
    header.reserve(name.size() + 2 + value.size());
    header.append(name);
    header.append(": ");
    header.append(value);
    globalHeaders.append(header);
  }
  // Force overlap with a couple of entries (exercise dynamic bitmap skip path)
  resp.header(headerVec[42].name(), "UserOverride-42");
  resp.header(headerVec[199].name(), "UserOverride-199");

  auto expected = ExpectedGlobalHeaderValues(resp, globalHeaders);
  auto serialized = concatenated(std::move(resp), globalHeaders);
  ParsedResponse parsed = parseResponse(serialized, false);

  ASSERT_GE(parsed.headers.size(), static_cast<std::size_t>(kGlobalHeaders));
  for (std::string_view gh : globalHeaders) {
    const auto sep = gh.find(http::HeaderSep);
    ASSERT_NE(sep, std::string_view::npos);
    std::string_view name = gh.substr(0, sep);
    const auto* actual = FindHeaderCaseInsensitive(parsed, name);
    ASSERT_NE(actual, nullptr) << "Missing global header " << name;
    auto expIt = expected.find(name);
    ASSERT_NE(expIt, expected.end());
    EXPECT_EQ(actual->second, expIt->second);
    const auto occurrences = std::count_if(parsed.headers.begin(), parsed.headers.end(),
                                           [&](const auto& hdr) { return CaseInsensitiveEqual(hdr.first, name); });
    EXPECT_EQ(occurrences, 1) << "Header " << name << " appeared " << occurrences << " times";
  }
}

TEST_F(HttpResponseTest, FuzzStructuralValidation) {
  static constexpr int kNbHttpResponses = 500;
  static constexpr int kNbOperationsPerHttpResponse = 50;

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "some data");

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> opDist(0, 9);
  std::uniform_int_distribution<int> smallLen(0, 12);
  std::uniform_int_distribution<int> midLen(0, 24);
  std::uniform_int_distribution<int> globalHeaderCountDist(0, 32);
  std::uniform_int_distribution<int> globalValueLenDist(1, 20);
  std::uniform_int_distribution<int> reuseGlobalNameDist(0, 3);
  auto makeValue = [&](int length) {
    std::string value;
    value.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      value.push_back(static_cast<char>('A' + (i % 26)));
    }
    return value;
  };
  vector<http::Header> fuzzHeaderVec;
  vector<int> operations;
  for (int caseIndex = 0; caseIndex < kNbHttpResponses; ++caseIndex) {
    ConcatenatedHeaders fuzzGlobalHeaders;

    const int fuzzGlobalCount = globalHeaderCountDist(rng);
    fuzzHeaderVec.clear();
    operations.clear();
    for (int globalIdx = 0; globalIdx < fuzzGlobalCount; ++globalIdx) {
      std::string name = "X-Fuzz-Global-" + std::to_string(caseIndex) + "-" + std::to_string(globalIdx);
      std::string value = makeValue(globalValueLenDist(rng));
      fuzzHeaderVec.emplace_back(name, value);
      std::string hdr;
      hdr.reserve(name.size() + 2 + value.size());
      hdr.append(name);
      hdr.append(": ");
      hdr.append(value);
      fuzzGlobalHeaders.append(hdr);
    }
    std::string lastReason;
    std::string lastBody;
    std::string lastHeaderKey;
    std::string lastHeaderValue;
    std::string lastTrailerKey;
    std::string lastTrailerValue;
    HttpResponse resp;
    http::StatusCode lastStatus = resp.status();
    for (int step = 0; step < kNbOperationsPerHttpResponse; ++step) {
      const int op = opDist(rng);
      operations.push_back(op);

      // periodic checks
      EXPECT_EQ(resp.status(), lastStatus);
      EXPECT_EQ(resp.reason(), lastReason);
      if (lastHeaderKey.empty()) {
        EXPECT_FALSE(resp.headerValue(lastHeaderKey).has_value());
      }
      EXPECT_EQ(resp.trailerValueOrEmpty(lastTrailerKey), lastTrailerValue);
      if (resp.hasBodyFile()) {
        const auto& file = *resp.file();
        const auto sz = file.size();
        EXPECT_EQ(sz, static_cast<std::uint64_t>(lastBody.size()));
        EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/octet-stream");
        EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), std::to_string(lastBody.size()));
      } else {
        EXPECT_EQ(resp.bodyLength(), lastBody.size());
        EXPECT_EQ(resp.bodyInMemoryLength(), lastBody.size());
        if (resp.hasBodyInMemory()) {
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), std::to_string(lastBody.size()));
        } else {
          EXPECT_FALSE(resp.headerValue(http::ContentType));
          EXPECT_FALSE(resp.headerValue(http::ContentLength));
        }
      }

      switch (op) {
        case 0:
          lastHeaderKey = "X-" + std::to_string(step);
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<uint32_t>(rng() % fuzzHeaderVec.size())].name();
          }
          lastHeaderValue = makeValue(smallLen(rng));
          resp.headerAddLine(lastHeaderKey, lastHeaderValue);
          break;
        case 1:
          lastHeaderKey = "U-" + std::string(static_cast<std::size_t>(step % 5), 'S');
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<uint32_t>(rng() % fuzzHeaderVec.size())].name();
          }
          lastHeaderValue = makeValue(midLen(rng));
          resp.header(lastHeaderKey, lastHeaderValue);
          break;
        case 2:
          lastReason = makeValue(smallLen(rng));
          resp.reason(lastReason);
          break;
        case 3:
          if (lastTrailerKey.empty()) {
            if (lastBody.empty()) {
              lastBody = makeValue(smallLen(rng));
              resp.body(std::string_view(lastBody));
              EXPECT_EQ(resp.hasBodyInMemory(), !lastBody.empty());
              EXPECT_FALSE(resp.hasBodyFile());
              EXPECT_EQ(resp.hasBody(), !lastBody.empty());
            } else {
              resp.body({});  // empty body
              EXPECT_FALSE(resp.hasBodyInMemory());
              EXPECT_FALSE(resp.hasBodyFile());
              EXPECT_FALSE(resp.hasBody());
              lastBody.clear();
            }
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.body({}), std::logic_error);
          }
          break;
        case 4: {
          static constexpr http::StatusCode opts[] = {200, 204, 404, 418};
          lastStatus = opts[static_cast<std::size_t>(step) % std::size(opts)];
          resp.status(lastStatus);
          break;
        }
        case 5:
          if (lastBody.empty()) {
            EXPECT_THROW(resp.trailerAddLine("X-Trailer", "value"), std::logic_error);
          } else if (!resp.hasBodyFile()) {
            lastTrailerKey = "X-" + std::to_string(step);
            lastTrailerValue = makeValue(smallLen(rng));
            resp.trailerAddLine(lastTrailerKey, lastTrailerValue);
          } else {
            // Once a file body was set, trailers cannot be added
            EXPECT_THROW(resp.trailerAddLine("X-Trailer", "value"), std::logic_error);
          }
          break;
        case 6: {  // File
          File file(tmp.filePath().string());
          if (lastTrailerKey.empty()) {
            lastBody = LoadAllContent(file);
            resp.file(std::move(file));
            EXPECT_TRUE(resp.hasBodyFile());
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.file(std::move(file)), std::logic_error);
          }
          break;
        }
        case 7:  // body inline append
          if (!resp.hasBodyFile() && lastTrailerKey.empty()) {
            const auto len = 2UL * static_cast<std::size_t>(midLen(rng));
            for (std::size_t i = 0; i < len; ++i) {
              resp.bodyInlineAppend(len, kAppendZeroOrOneA);
            }
            lastBody.append(std::string(len / 2, 'A'));
          } else {
            // If file body or once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.bodyInlineAppend(1UL, kAppendZeroOrOneA), std::logic_error);
          }
          break;
        case 8: {  // body inline set
          if (lastTrailerKey.empty()) {
            resp.bodyInlineSet(1UL + static_cast<std::size_t>(midLen(rng)), kAppendZeroOrOneA);
            resp.bodyInlineSet(1UL + static_cast<std::size_t>(midLen(rng)), kAppendZeroOrOneA);
            lastBody = "A";
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.bodyInlineSet(1UL, kAppendZeroOrOneA), std::logic_error);
          }
          break;
        }
        case 9: {  // body append string
          if (!resp.hasBodyFile() && lastTrailerKey.empty()) {
            std::string toAppend = makeValue(static_cast<int>(midLen(rng)));
            resp.bodyAppend(toAppend);
            lastBody.append(toAppend);
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.bodyAppend("data"), std::logic_error);
          }
          break;
        }
        default:
          throw std::runtime_error("Invalid random value, update the test");
      }
    }

    const bool hasFile = resp.hasBodyFile();

    // Pre-finalize state checks (reason/body accessible before finalize)
    EXPECT_EQ(resp.status(), lastStatus);
    EXPECT_EQ(resp.reason(), lastReason);
    if (hasFile) {
      EXPECT_FALSE(resp.hasBodyInMemory());
      EXPECT_TRUE(resp.hasBodyFile());
      EXPECT_EQ(LoadAllContent(*resp.file()), lastBody);
    } else {
      EXPECT_EQ(resp.bodyLength(), lastBody.size());
      EXPECT_EQ(resp.bodyInMemoryLength(), lastBody.size());
      EXPECT_FALSE(resp.hasBodyFile());
    }

    auto expectedGlobals = ExpectedGlobalHeaderValues(resp, fuzzGlobalHeaders);

    auto full = concatenated(std::move(resp), fuzzGlobalHeaders);
    ParsedResponse pr = parseResponse(full, hasFile);

    int dateCount = 0;
    int connCount = 0;
    int clCount = 0;
    std::size_t clVal = 0;
    for (auto& headerPair : pr.headers) {
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
    if (!hasFile) {
      // When trailers are present, chunked transfer encoding is used instead of Content-Length
      // per RFC 7230 §4.1.2
      const bool hasTrailers = !lastTrailerKey.empty();
      if (hasTrailers) {
        // With trailers, we use Transfer-Encoding: chunked, not Content-Length
        EXPECT_EQ(clCount, 0);
        EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)))
            << "Missing Transfer-Encoding: chunked when trailers present";
      } else if (!pr.body.empty()) {
        EXPECT_EQ(clCount, 1);
        if (clVal != pr.body.size()) {
          // Diagnostic: content-length mismatch
          std::string operationsStr;
          for (int op : operations) {
            if (!operationsStr.empty()) {
              operationsStr.append(",");
            }
            operationsStr.append(std::to_string(op));
          }
          ADD_FAILURE() << "Fuzz case " << caseIndex << " operations=[" << operationsStr
                        << "] Content-Length header=" << clVal << " but parsed body size=" << pr.body.size()
                        << "\nFull response:\n"
                        << full;
          return;  // stop early to inspect this failing case
        }
        EXPECT_EQ(clVal, pr.body.size());
      } else {
        EXPECT_EQ(clCount, 0);
      }
    }

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

    for (const auto& gh : fuzzGlobalHeaders) {
      const auto sep = gh.find(": ");
      ASSERT_NE(sep, std::string_view::npos);
      std::string_view name = gh.substr(0, sep);
      const auto* actual = FindHeaderCaseInsensitive(pr, name);
      ASSERT_NE(actual, nullptr) << "Missing fuzz global header " << name;
      auto expIt = expectedGlobals.find(name);
      ASSERT_NE(expIt, expectedGlobals.end());
      EXPECT_EQ(actual->second, expIt->second);
    }
  }
}

// Basic trailer test - verify trailers are appended after body
TEST(HttpResponseTrailers, BasicTrailer) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test body");
  EXPECT_FALSE(resp.hasTrailer("X-Checksum"));
  resp.trailerAddLine("X-Checksum", "abc123");

  EXPECT_EQ(resp.trailerValueOrEmpty("X-Another"), "");
  EXPECT_EQ(resp.trailerValueOrEmpty("X-checksum"), "abc123");
  EXPECT_EQ(resp.trailerValue("x-CHECKSUM").value_or(""), "abc123");
  EXPECT_FALSE(resp.trailerValue("x-CHEKSUM"));
  EXPECT_TRUE(resp.hasTrailer("X-Checksum"));

  // We can't easily test the serialized output without finalizing,
  // but we can verify no exception is thrown
  EXPECT_NO_THROW(resp.trailerAddLine("X-Signature", "sha256:..."));
}

// Test error when adding trailer before body
TEST(HttpResponseTrailers, ErrorBeforeBody) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.trailerAddLine("X-Checksum", "abc123"), std::logic_error);
}

// Test error when adding trailer after an explicitly empty body
TEST(HttpResponseTrailers, EmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");  // empty body set explicitly
  EXPECT_THROW(resp.trailerAddLine("X-Checksum", "abc123"), std::logic_error);
}

// Test trailer with captured body (std::string)
TEST(HttpResponseTrailers, CapturedBodyString) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured body content"));
  EXPECT_NO_THROW(resp.trailerAddLine("X-Custom", "value"));
}

// Test trailer with captured body (vector<char>)
TEST(HttpResponseTrailers, CapturedBodyVector) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::vector<char>{'h', 'e', 'l', 'l', 'o'});
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("hello"));
  EXPECT_NO_THROW(resp.trailerAddLine("X-Data", "123"));

  resp = HttpResponse{"some body that should be erased"};
  resp.body(std::vector<char>{});  // empty body
  EXPECT_EQ(resp.bodyInMemory(), std::string_view());
  EXPECT_THROW(resp.trailerAddLine("X-Data", "123"), std::logic_error);
}

// Test multiple trailers
TEST(HttpResponseTrailers, MultipleTrailers) {
  HttpResponse resp("body");
  resp.trailerAddLine("X-Checksum", "abc");
  resp.trailerAddLine("X-Timestamp", "2025-10-20T12:00:00Z");
  resp.trailerAddLine("X-Custom", "val");
  EXPECT_EQ(resp.trailersFlatView(), "X-Checksum: abc\r\nX-Timestamp: 2025-10-20T12:00:00Z\r\nX-Custom: val\r\n");
}

// Test empty trailer value
TEST(HttpResponseTrailers, EmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test");
  EXPECT_NO_THROW(resp.trailerAddLine("X-Empty", ""));
}

// Test rvalue ref version
TEST(HttpResponseTrailers, RvalueRef) {
  EXPECT_NO_THROW(HttpResponse(http::StatusCodeOK).body("test").trailerAddLine("X-Check", "val"));
}

// Test that setting the body after inserting a trailer throws
TEST(HttpResponseTrailers, BodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.trailerAddLine("X-After", "v");
  // setting inline body after trailer insertion should throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
  // setting captured string body after trailer insertion should also throw
  EXPECT_THROW(resp.body(std::string_view("later2")), std::logic_error);
}

// -----------------------------------------------------------------------------
// Tests for trailers() retrieval (response-side)
// -----------------------------------------------------------------------------

TEST_F(HttpResponseTest, TrailersNoBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Test", "val");
  // No trailers added -> empty view
  EXPECT_TRUE(resp.trailersFlatView().empty());
  // body remains accessible and unchanged
  EXPECT_EQ(resp.bodyInMemory(), std::string_view());
}

TEST_F(HttpResponseTest, TrailersInline_NoTrailers) {
  HttpResponse resp("inline-body");
  // No trailers added -> empty view
  auto tv = resp.trailersFlatView();
  EXPECT_TRUE(tv.empty());
  // body remains accessible and unchanged
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("inline-body"));
}

TEST_F(HttpResponseTest, TrailersInline_WithTrailers) {
  HttpResponse resp("inline-body");
  resp.trailerAddLine("X-First", "one");
  resp.trailerAddLine("X-Second", "two");
  auto tv = resp.trailersFlatView();
  EXPECT_FALSE(tv.empty());
  // trailers are stored as header lines terminated by CRLF
  EXPECT_TRUE(tv.contains("X-First: one\r\n"));
  EXPECT_TRUE(tv.contains("X-Second: two\r\n"));
  EXPECT_TRUE(tv.ends_with(http::CRLF));
  // body() should not include trailers
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("inline-body"));
}

TEST_F(HttpResponseTest, TrailersCaptured_NoTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured-body-content"));
  auto tv = resp.trailersFlatView();
  EXPECT_TRUE(tv.empty());
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("captured-body-content"));
}

TEST_F(HttpResponseTest, TrailersCaptured_WithTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured-body"));
  resp.trailerAddLine("X-Custom", "val");
  auto tv = resp.trailersFlatView();
  EXPECT_FALSE(tv.empty());
  EXPECT_TRUE(tv.contains("X-Custom: val\r\n"));
  EXPECT_TRUE(tv.ends_with(http::CRLF));
  // body() must remain the original captured body (trailers excluded)
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("captured-body"));
}

TEST(HttpResponseAppendHeaderValue, AppendsToEmptyHeader) {
  HttpResponse resp;
  resp.headerAppendValue("X-Test", "alpha");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "alpha");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithDefaultSeparator) {
  HttpResponse resp;
  resp.header("X-Test", "one");
  resp.headerAppendValue("X-Test", "two");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "one, two");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithCustomSeparator) {
  HttpResponse resp;
  resp.header("X-Test", "first");
  resp.headerAppendValue("X-Test", "second", "; ");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "first; second");
}

TEST(HttpResponseAppendHeaderValue, NumericOverloadAndSubsequentAppend) {
  HttpResponse resp;
  resp.headerAppendValue("X-Num", 123);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "123");

  resp.headerAppendValue("X-Num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "123, 456");

  resp = HttpResponse{}.headerAppendValue("X-Num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "456");
}

TEST(HttpResponseAppendHeaderValue, CaseInsensitiveKeyMatch) {
  HttpResponse resp;
  resp.header("x-TeSt", "lower");
  resp.headerAppendValue("X-TEST", "upper");
  EXPECT_EQ(resp.headerValueOrEmpty("X-test"), "lower, upper");
}

TEST(HttpResponseAppendHeaderValue, VaryMergesAcceptEncoding) {
  HttpResponse resp;
  resp.header(http::Vary, http::Origin);
  resp.headerAppendValue(http::Vary, http::AcceptEncoding);
  std::string expectedVary(http::Origin);
  expectedVary += ", ";
  expectedVary += http::AcceptEncoding;
  EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), expectedVary);
}

TEST_F(HttpResponseTest, MakeAllHeadersLowerCaseForHttp2_NoHeadersIsNoop) {
  HttpResponse resp(http::StatusCodeOK);
  // No headers added -> no-op
  MakeAllHeaderNamesLowerCase(resp);
  EXPECT_TRUE(resp.headersFlatView().empty());
}

TEST_F(HttpResponseTest, MakeAllHeadersLowerCaseForHttp2_LowercasesNamesOnly) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-CaSe-Header", "VaLuE");
  resp.headerAddLine("Another-Header", "Val:With:Colon");

  MakeAllHeaderNamesLowerCase(resp);

  std::string headers(resp.headersFlatView());
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("x-case-header", "VaLuE")));
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("another-header", "Val:With:Colon")));
  // Ensure values were not lowercased
  EXPECT_TRUE(headers.contains("VaLuE"));
  EXPECT_TRUE(headers.contains("Val:With:Colon"));
}

TEST_F(HttpResponseTest, MakeAllHeadersLowerCaseForHttp2_Idempotent) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-Repeat", "V");
  resp.headerAddLine("Mixed-Case", "Value");

  MakeAllHeaderNamesLowerCase(resp);
  std::string first(resp.headersFlatView());

  MakeAllHeaderNamesLowerCase(resp);
  std::string second(resp.headersFlatView());

  EXPECT_EQ(first, second);
}

TEST_F(HttpResponseTest, MakeAllHeadersLowerCaseForHttp2_MultipleHeadersAndValuesPreserved) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("X-One", "ONE");
  resp.headerAddLine("X-Two", "Two:COLON:IN:VALUE");
  resp.headerAddLine("Already-Lower", "MixedValue:ABC");

  MakeAllHeaderNamesLowerCase(resp);
  std::string headers(resp.headersFlatView());

  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("x-one", "ONE")));
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("x-two", "Two:COLON:IN:VALUE")));
  EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("already-lower", "MixedValue:ABC")));
}

// =============================================================================
// Tests for automatic chunked encoding conversion when trailers are present
// Per RFC 7230 §4.1.2, trailers require chunked transfer encoding
// =============================================================================

TEST_F(HttpResponseTest, FinalizationCombinations) {
  static constexpr std::size_t kMinCapturedBodySz[] = {1UL, 4096UL};
  static constexpr std::string_view kConcatenatedGlobalHeaders[] = {"", "server: aeronet\r\n",
                                                                    "x-custom: value\r\nx-another: another-value\r\n"};

  // This test covers ALL combinations of:
  // - prepared vs non-prepared response
  // - knownAddTrailerAtConstruction true/false
  // - HEAD vs non-HEAD request
  // - keep-alive vs close connection
  // - addTrailerHeader true/false
  // - minCapturedBodySz small/large (inline vs captured body)
  // - various global headers (none, one, multiple)
  for (std::string_view globalHeaders : kConcatenatedGlobalHeaders) {
    ConcatenatedHeaders gh;
    for (std::string_view tmp = globalHeaders; !tmp.empty();) {
      const auto crlfPos = tmp.find(http::CRLF);
      ASSERT_NE(crlfPos, std::string_view::npos);
      gh.append(tmp.substr(0, crlfPos));
      tmp.remove_prefix(crlfPos + http::CRLF.size());
    }
    for (bool isPrepared : {true, false}) {
      for (bool head : {true, false}) {
        for (bool keepAlive : {true, false}) {
          for (bool addTrailerHeader : {true, false}) {
            for (std::size_t minCapturedBodySz : kMinCapturedBodySz) {
              // Captured body
              std::string_view bodySv = "CapturedData12345";  // 17 bytes
              HttpResponse resp = MakePrepared(isPrepared, head, gh);
              AddTrailerHeader(resp, addTrailerHeader);
              resp.body(std::string(bodySv));  // 17 bytes = 0x11
              resp.trailerAddLine("X-Sig", "sig-value");
              resp.trailerAddLine("X-Sig-2", "sig-value-2");

              std::string result =
                  concatenated(std::move(resp), gh, head, keepAlive, minCapturedBodySz, addTrailerHeader);

              std::string exp;
              exp.reserve(result.size());
              exp += "HTTP/1.1 200\r\n";

              exp += MakeHttp1HeaderLine(http::Date, "Thu, 01 Jan 1970 00:00:00 GMT");
              if (isPrepared) {
                exp += globalHeaders;
              }
              if (addTrailerHeader && head && isPrepared) {
                exp += MakeHttp1HeaderLine(http::Trailer, "X-Sig, X-Sig-2");
              }
              exp += MakeHttp1HeaderLine(http::ContentType, "text/plain");
              if (addTrailerHeader && (!head || !isPrepared)) {
                exp += MakeHttp1HeaderLine(http::Trailer, "X-Sig, X-Sig-2");
              }
              if (head) {
                exp += MakeHttp1HeaderLine(http::ContentLength, std::to_string(bodySv.size()));
              } else {
                exp += MakeHttp1HeaderLine(http::TransferEncoding, http::chunked);
              }

              if (!isPrepared) {
                exp += globalHeaders;
              }
              if (!keepAlive) {
                exp += MakeHttp1HeaderLine(http::Connection, http::close);
              }
              exp += "\r\n";

              if (!head) {
                // 17 in hex = 0x11 = "11"
                exp += "11\r\n";
                exp += bodySv;
                exp += "\r\n";
                exp += "0\r\n";
                exp += "X-Sig: sig-value\r\n";
                exp += "X-Sig-2: sig-value-2\r\n";
                exp += "\r\n";
              }

              ASSERT_EQ(result, exp) << "Failed with keepAlive=" << keepAlive
                                     << " addTrailerHeader=" << addTrailerHeader
                                     << " inlineBody=" << (minCapturedBodySz == 1) << " globalHeaders=["
                                     << globalHeaders << "]" << " head=" << head;

              // Inline body
              resp = MakePrepared(isPrepared, head, gh);
              AddTrailerHeader(resp, addTrailerHeader);
              resp.body(bodySv).trailerAddLine("X-Sig", "sig-value").trailerAddLine("X-Sig-2", "sig-value-2");
              result = concatenated(std::move(resp), gh, head, keepAlive, minCapturedBodySz, addTrailerHeader);
              ASSERT_EQ(result, exp) << "Failed with keepAlive=" << keepAlive
                                     << " addTrailerHeader=" << addTrailerHeader
                                     << " inlineBody=" << (minCapturedBodySz == 1) << " globalHeaders=["
                                     << globalHeaders << "]" << " head=" << head;
            }
          }
        }
      }
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedMultipleTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test");
  resp.trailerAddLine("Trailer-One", "value1");
  resp.trailerAddLine("Trailer-Two", "value2");
  resp.trailerAddLine("Trailer-Three", "value3");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("4\r\ntest\r\n0\r\n"));
  EXPECT_TRUE(result.contains("Trailer-One: value1\r\n"));
  EXPECT_TRUE(result.contains("Trailer-Two: value2\r\n"));
  EXPECT_TRUE(result.contains("Trailer-Three: value3\r\n"));
}

TEST_F(HttpResponseTest, HeadLargeBodyDoesNotGrowInlineCapacity) {
  HttpResponse resp = MakePrepared(true, true);

  const auto initialCapacity = resp.capacityInlined();
  const std::string bigBody(1U << 20, 'x');

  resp.body(bigBody, http::ContentTypeTextPlain);

  EXPECT_EQ(resp.bodyLength(), bigBody.size());
  EXPECT_LT(resp.capacityInlined(), bigBody.size());
  EXPECT_LE(resp.capacityInlined(), initialCapacity + 512U);
}

TEST_F(HttpResponseTest, HeadLargeCapturedBodyDoesNotGrowInlineCapacity) {
  HttpResponse resp = MakePrepared(true, true);

  const auto initialCapacity = resp.capacityInlined();
  std::vector<char> bigBody(1U << 20, 'y');

  resp.body(std::move(bigBody), http::ContentTypeTextPlain);

  EXPECT_EQ(resp.bodyLength(), 1U << 20);
  EXPECT_LT(resp.capacityInlined(), 1U << 20);
  EXPECT_LE(resp.capacityInlined(), initialCapacity + 512U);
}

TEST_F(HttpResponseTest, HeadBodyInlineAppend) {
  for (bool useBytes : {false, true}) {
    for (bool prepared : {true, false}) {
      HttpResponse resp = MakePrepared(prepared, true);

      if (useBytes) {
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneABytes);
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneABytes);
      } else {
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneA);
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneA);
      }
      if (prepared) {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
      } else {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view("A"));
      }

      if (useBytes) {
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneABytes, "text/custom");
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneABytes, "text/custom");
      } else {
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneA, "text/custom");
        resp.bodyInlineAppend(2UL, kAppendZeroOrOneA, "text/custom");
      }
      if (prepared) {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
      } else {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view("AA"));
      }
      resp.trailerAddLine("X-Test", "value");

      std::string result = concatenated(std::move(resp), {}, true);

      EXPECT_EQ(result,
                "HTTP/1.1 200\r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/custom\r\ncontent-length: "
                "2\r\nconnection: close\r\n\r\n")
          << "failed for prepared=" << prepared;
    }
  }
}

TEST_F(HttpResponseTest, HeadBodyAppend) {
  for (bool prepared : {true, false}) {
    HttpResponse resp = MakePrepared(prepared, true);

    resp.bodyAppend("");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "");
    resp.bodyAppend("A");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "1");
    if (prepared) {
      EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
    } else {
      EXPECT_EQ(resp.bodyInMemory(), std::string_view("A"));
    }

    resp.bodyAppend("");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "1");
    resp.bodyAppend("A");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "2");
    if (prepared) {
      EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
    } else {
      EXPECT_EQ(resp.bodyInMemory(), std::string_view("AA"));
    }
    // We cannot infer anything for the capacity, because we don't know in advance how many bytes the user will actually
    // append

    resp.trailerAddLine("X-Test", "value");

    std::string result = concatenated(std::move(resp), {}, true);

    EXPECT_EQ(result,
              "HTTP/1.1 200\r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/plain\r\ncontent-length: "
              "2\r\nconnection: close\r\n\r\n")
        << "failed for prepared=" << prepared;
  }
}

TEST_F(HttpResponseTest, HeadBodyInlineSet) {
  for (bool useBytes : {false, true}) {
    for (bool prepared : {true, false}) {
      HttpResponse resp = MakePrepared(prepared, true);

      if (useBytes) {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneA);
      } else {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneABytes);
      }

      EXPECT_FALSE(resp.hasHeader(http::ContentType));
      EXPECT_FALSE(resp.hasHeader(http::ContentLength));
      EXPECT_EQ(resp.bodyInMemoryLength(), 0);
      if (useBytes) {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneA);
      } else {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneABytes);
      }
      EXPECT_TRUE(resp.hasHeader(http::ContentType));
      EXPECT_TRUE(resp.hasHeader(http::ContentLength));
      EXPECT_EQ(resp.bodyInMemoryLength(), 1);
      if (prepared) {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
      } else {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view("A"));
      }

      if (useBytes) {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneA);
      } else {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneABytes);
      }
      EXPECT_FALSE(resp.hasHeader(http::ContentType));
      EXPECT_FALSE(resp.hasHeader(http::ContentLength));
      EXPECT_EQ(resp.bodyInMemoryLength(), 0);
      if (useBytes) {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneA, "text/custom");
      } else {
        resp.bodyInlineSet(2UL, kAppendZeroOrOneABytes, "text/custom");
      }
      EXPECT_TRUE(resp.hasHeader(http::ContentType));
      EXPECT_TRUE(resp.hasHeader(http::ContentLength));
      EXPECT_EQ(resp.bodyInMemoryLength(), 1);
      if (prepared) {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view(""));
      } else {
        EXPECT_EQ(resp.bodyInMemory(), std::string_view("A"));
      }
      std::string result = concatenated(std::move(resp), {}, true);

      EXPECT_EQ(result,
                "HTTP/1.1 200\r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/custom\r\ncontent-length: "
                "1\r\nconnection: close\r\n\r\n")
          << "failed for prepared=" << prepared;
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedPreservesOtherHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.header("X-Custom", "custom-value");
  resp.header(http::ContentType, "application/json");
  resp.body(R"({"key":"value"})");
  resp.trailerAddLine("X-Hash", "sha256:...");

  const std::string result = concatenated(std::move(resp));

  // Other headers should be preserved
  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine("X-Custom", "custom-value")));
  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::ContentType, "application/json")));
  // But Content-Length should be replaced with Transfer-Encoding
  EXPECT_FALSE(result.contains(http::ContentLength));
  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
}

TEST_F(HttpResponseTest, TrailersAutoChunkedLargeBody) {
  // Test with body large enough to require multiple hex digits
  const std::string largeBody(0x1234, 'X');  // 4660 bytes
  HttpResponse resp(http::StatusCodeOK);
  resp.body(largeBody);
  resp.trailerAddLine("X-Size", "large");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_FALSE(result.contains(http::ContentLength));
  // Should have correct hex length "1234" followed by CRLF and the body data
  EXPECT_TRUE(result.contains("1234\r\n" + largeBody + "\r\n0\r\n")) << "Body should be chunked with hex length";
}

TEST_F(HttpResponseTest, TrailersAutoChunkedEmptyTrailerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("data");
  resp.trailerAddLine("X-Empty", "");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("X-Empty: \r\n"));
}

TEST_F(HttpResponseTest, NoTrailersNoChunkedConversion) {
  static constexpr bool kConnection[] = {true, false};

  for (bool keepAlive : kConnection) {
    // Verify that responses without trailers still use Content-Length
    const std::string result = concatenated(HttpResponse("no-trailers-body"), {}, false, keepAlive);

    EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::ContentLength, "16")));
    if (!keepAlive) {
      EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::Connection, http::close)));
    }
    EXPECT_FALSE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
    // Body should not be chunked
    EXPECT_FALSE(result.contains("10\r\nno-trailers-body\r\n"));
    EXPECT_TRUE(result.ends_with("\r\n\r\nno-trailers-body"));
  }
}

TEST_F(HttpResponseTest, NoTrailersNoChunkedConversionCapturedBody) {
  static constexpr bool kConnection[] = {true, false};

  static constexpr std::size_t kMinCapturedBodySz[] = {1UL};

  for (std::size_t minCapturedBodySz : kMinCapturedBodySz) {
    for (bool keepAlive : kConnection) {
      // Verify that responses without trailers still use Content-Length
      const std::string result =
          concatenated(HttpResponse{}.body(std::string("no-trailers-body")), {}, false, keepAlive, minCapturedBodySz);

      EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::ContentLength, "16")));
      if (!keepAlive) {
        EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::Connection, http::close)));
      }
      EXPECT_FALSE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
      // Body should not be chunked
      EXPECT_FALSE(result.contains("10\r\nno-trailers-body\r\n"));
      EXPECT_TRUE(result.ends_with("\r\n\r\nno-trailers-body"));
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedVectorBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::vector<char>{'A', 'B', 'C', 'D'});
  resp.trailerAddLine("X-Check", "done");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("4\r\nABCD\r\n0\r\n"));
  EXPECT_TRUE(result.contains("X-Check: done\r\n"));
}

TEST_F(HttpResponseTest, TrailersAutoChunkedUniquePtrBody) {
  for (bool head : {false, true}) {
    const char data[] = "Hello";
    auto bodyPtr = std::make_unique<char[]>(sizeof(data));
    std::ranges::copy(data, bodyPtr.get());

    auto resp = MakePrepared(head, head);
    resp.body(std::move(bodyPtr), sizeof(data) - 1);
    resp.trailerAddLine("X-Final", "yes");

    const std::string result = concatenated(std::move(resp), {}, head);

    EXPECT_EQ(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)), !head);
    if (head) {
      EXPECT_TRUE(result.contains(
          MakeHttp1HeaderLine(http::ContentLength, std::string_view(IntegralToCharVector(sizeof(data) - 1)))));
      EXPECT_FALSE(result.contains("5\r\nHello\r\n0\r\n"));
      EXPECT_FALSE(result.contains("X-Final: yes\r\n"));
      EXPECT_TRUE(result.ends_with("\r\n\r\n"));
    } else {
      EXPECT_FALSE(result.contains(http::ContentLength));
      EXPECT_TRUE(result.contains("5\r\nHello\r\n0\r\n"));
      EXPECT_TRUE(result.contains("X-Final: yes\r\n"));
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedBytesSpanBody) {
  // Use bytes span which is handled correctly
  const char data[] = "Hello";
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), 5));
  resp.trailerAddLine("X-Final", "yes");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("5\r\nHello\r\n0\r\n"));
  EXPECT_TRUE(result.contains("X-Final: yes\r\n"));
}

TEST_F(HttpResponseTest, TrailersAutoChunkedBodySizeEdgeCases) {
  // Test with body sizes that are powers of 16 to verify hex encoding
  for (int sz : {1, 15, 16, 255, 256, 4095, 4096}) {
    const std::string body(static_cast<std::size_t>(sz), 'X');
    HttpResponse resp(http::StatusCodeOK);
    resp.body(body);
    resp.trailerAddLine("X-Size", std::to_string(sz));

    const std::string result = concatenated(std::move(resp));

    EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)))
        << "Failed for size " << sz;
    EXPECT_FALSE(result.contains(http::ContentLength)) << "Failed for size " << sz;
    // Verify the last-chunk marker is present
    EXPECT_TRUE(result.contains("\r\n0\r\n")) << "Missing last-chunk for size " << sz;
  }
}

}  // namespace aeronet
