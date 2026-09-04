#include "aeronet/http-response.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "aeronet/city-hash.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file-helpers.hpp"
#include "aeronet/file-sys-test-support.hpp"
#include "aeronet/file.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

class HttpResponseTest : public ::testing::Test {
 protected:
  static constexpr const char* const kDateHeader = "Thu, 01 Jan 1970 00:00:00 GMT";
  static constexpr bool kKeepAlive = false;
  static constexpr bool kIsHeadMethod = false;
  static constexpr bool kAddTrailerHeader = false;
  static constexpr std::size_t kMinCapturedBodySize = 4096;

  const RawChars kExpectedDateRaw = MakeHttp1HeaderLine(http::Date, "Thu, 01 Jan 1970 00:00:00 GMT");

  CompressionConfig cfg;
  internal::CompressionState compressionState{cfg};

  struct PreparedOptions {
    bool head = false;
    bool addVaryAcceptEncoding = false;
    bool addTrailerHeader = false;
    bool close = false;
    Encoding expectedEncoding = Encoding::none;
    // if we remove {}, we get a compiler warning about missing initializer...
    // NOLINTNEXTLINE(readability-redundant-member-init)
    ConcatenatedHeaders gh{};
  };

  HttpResponse makePrepared(const PreparedOptions& opts) {
    HttpResponse resp;
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    resp._opts = HttpResponse::Options(compressionState, opts.expectedEncoding);
#endif
    resp._opts.setPrepared();
    for (std::string_view headerNameAndValue : opts.gh) {
      const auto sepPos = headerNameAndValue.find(http::HeaderSep);
      if (sepPos == std::string_view::npos) {
        throw std::invalid_argument("Invalid header in global headers");
      }
      resp.headerAddLine(LowerAsciiKey{headerNameAndValue.substr(0, sepPos)},
                         headerNameAndValue.substr(sepPos + http::HeaderSep.size()));
    }
    if (opts.head) {
      resp._opts.setHeadMethod();
    }
    if (opts.addVaryAcceptEncoding) {
      resp._opts.addVaryAcceptEncoding();
    }
    if (opts.addTrailerHeader) {
      resp._opts.addTrailerHeader();
    }
    if (opts.close) {
      resp._opts.setClose();
    }
    return resp;
  }

  static void FinalizeCompressedBody([[maybe_unused]] HttpResponse& resp) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    if (IsAutomaticDirectCompression(resp)) {
      resp.finalizeInlineBody();
    }
#endif
  }

  static bool IsAutomaticDirectCompression([[maybe_unused]] const HttpResponse& resp) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    return resp._opts.isAutomaticDirectCompression();
#else
    return false;
#endif
  }

  static HttpMessageData finalizePrepared(HttpResponse&& resp, bool head = kIsHeadMethod,
                                          bool keepAliveFlag = kKeepAlive) {
    return finalizePrepared(std::move(resp), {}, head, kAddTrailerHeader, keepAliveFlag, kMinCapturedBodySize);
  }

  static HttpMessageData finalizePrepared(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders, bool head,
                                          bool addTrailerHeader, bool keepAliveFlag, std::size_t minCapturedBodySize) {
    HttpResponse::Options opts;

    if (!keepAliveFlag) {
      opts.setClose();
    }
    if (addTrailerHeader) {
      opts.addTrailerHeader();
    }
    if (head) {
      opts.setHeadMethod();
    }

    return resp.finalizeForHttp1(kDateHeader, http::HTTP_1_1, opts, &globalHeaders, minCapturedBodySize);
  }

  static HttpMessageData finalize(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders, bool head,
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
    HttpMessageData httpResponseData =
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

  // Number of (non-overlapping) occurrences of `needle` in `haystack`.
  static std::size_t CountSubstr(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + needle.size())) {
      ++count;
    }
    return count;
  }

#ifdef AERONET_ENABLE_HTTP_CLIENT
  static auto cloneFinalized(const HttpResponse& resp) { return resp.cloneFinalized(); }
#endif

  static void FinalizeHeadersAndBody(HttpResponse& resp) { resp.finalizeHeadersAndBody(); }
};

TEST_F(HttpResponseTest, StatusFromRvalue) {
  auto resp = HttpResponse(http::StatusCodeOK).status(404);
  EXPECT_EQ(resp.status(), 404);
}

TEST_F(HttpResponseTest, BodyFromSpanBytesLValue) {
  static constexpr std::byte bodyBytes[]{
      std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'},
  };
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
  EXPECT_FALSE(resp.hasTrailer("x-nonexistent"));
  resp.status(404);
  EXPECT_EQ(404, resp.status());
  EXPECT_EQ(resp.statusStr(), "404");

  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 \r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, TooLongReasonShouldBeTruncated) {
  HttpResponse resp(http::StatusCodeOK);
  std::string longReason((1UL << 24U) - 20U, 'A');

  std::size_t truncatedReasonSize = 0;

  for (std::string_view reason = longReason; !reason.empty(); reason.remove_suffix(1U)) {
    resp.reason(reason);
    if (truncatedReasonSize == 0) {
      truncatedReasonSize = resp.reason().size();
      EXPECT_EQ(resp.reasonLength(), truncatedReasonSize);
      EXPECT_EQ(resp.reason(), reason.substr(0, truncatedReasonSize));
      EXPECT_LT(resp.reason().size(), reason.size());
    } else {
      if (reason.size() <= truncatedReasonSize) {
        EXPECT_EQ(resp.reasonLength(), reason.size());
        EXPECT_EQ(resp.reason(), reason);
        if (reason.size() < truncatedReasonSize - 1) {
          break;
        }
      } else {
        EXPECT_EQ(resp.reason().size(), truncatedReasonSize);
        EXPECT_EQ(resp.reasonLength(), truncatedReasonSize);
      }
    }
  }
}

TEST_F(HttpResponseTest, ConstructorWithBody) {
  HttpResponse resp("Hello, World!");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.reason(), "");
  EXPECT_EQ(resp.bodyInMemory(), "Hello, World!");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");

  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, ConstructorFromBytesSpan) {
  static constexpr std::byte bodyBytes[]{
      std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'},
  };
  HttpResponse resp(bodyBytes);
  EXPECT_EQ(resp.bodyInMemory(), "Hello");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/octet-stream");
}

TEST_F(HttpResponseTest, ConstructorWithConcatenatedHeadersBadFormat) {
  static constexpr std::string_view kBadConcatenatedHeaders[]{
      "HeaderWithoutSep\r\n",
      "headerwithnovalue: \r\nAnotherHeaderWithoutSep\r\n",
      "headerwithnocrlf: Value",
      "NotUsingHeaderSep:Value",
      "Invalid Header Name!: Value\r\n",
      "NotNormalized: Value\r\n",
      "valid-header: Invalid\x01Value\r\n",
      "valid-header: Invalid\rValue\r\n",
  };

  for (std::string_view badHeaders : kBadConcatenatedHeaders) {
    EXPECT_THROW(HttpResponse(0, 200, badHeaders), std::invalid_argument);
  }
}

TEST_F(HttpResponseTest, HttpPartsSizes) {
  HttpResponse resp("Hello, World!");

  EXPECT_EQ(resp.statusLineSize(), std::string_view("HTTP/1.1 200 \r\n").size());
  EXPECT_EQ(resp.statusLineSize(), resp.statusLineLength());

  EXPECT_EQ(resp.headersSize(), std::string_view("date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                                 "content-type: text/plain\r\n"
                                                 "content-length: 13\r\n")
                                    .size());
  EXPECT_EQ(resp.headersSize(), resp.headersLength());

  EXPECT_EQ(resp.headSize(), std::string_view("HTTP/1.1 200 \r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                              "content-type: text/plain\r\n"
                                              "content-length: 13\r\n\r\n")
                                 .size());
  EXPECT_EQ(resp.headSize(), resp.headLength());

  EXPECT_EQ(resp.sizeInMemory(), std::string_view("HTTP/1.1 200 \r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                                                  "content-type: text/plain\r\n"
                                                  "content-length: 13\r\n\r\n"
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
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/my-text")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, ConstructorWithConcatenatedHeaders) {
  static constexpr std::string_view kConcatenatedHeaders[]{
      "",
      "x-custom-header: CustomValue\r\n",
      "x-1: Salut\r\nx-2: Bonjour\r\nx-3: Hola\r\n",
  };

  static constexpr std::string_view kBodies[]{
      "",
      "Hello!",
      "This is a longer body to test the HttpResponse constructor with concatenated headers.",
  };

  static constexpr std::size_t kAdditionalCapacities[]{
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
        if (concatenatedHeaders.contains("x-custom-header: ")) {
          EXPECT_EQ(resp.headerValueOrEmpty("x-custom-header"), "CustomValue");
        } else {
          EXPECT_FALSE(resp.hasHeader("x-custom-header"));
        }
        if (concatenatedHeaders.contains("x-1: ")) {
          EXPECT_EQ(resp.headerValueOrEmpty("x-1"), "Salut");
          EXPECT_EQ(resp.headerValueOrEmpty("x-2"), "Bonjour");
          EXPECT_EQ(resp.headerValueOrEmpty("x-3"), "Hola");
        } else {
          EXPECT_FALSE(resp.hasHeader("x-1"));
        }
        if (!body.empty()) {
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/custom");
          EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), std::to_string(body.size()));
        } else {
          EXPECT_FALSE(resp.hasHeader(http::ContentType));
          EXPECT_FALSE(resp.hasHeader(http::ContentLength));
        }

        const auto full = concatenated(std::move(resp));
        EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine("x-custom-header", "CustomValue")),
                  concatenatedHeaders.contains("x-custom-header: "));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine("x-another-header", "AnotherValue")),
                  concatenatedHeaders.contains("x-another-header: "));
        EXPECT_EQ(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/custom")), !body.empty());
        // Empty-body responses now synthesize `content-length: 0` at finalization so keep-alive clients can
        // frame the (zero-length) body immediately (status 200 permits it) -> a Content-Length line is always
        // present in the finalized bytes ("0" for an empty body, the real size otherwise).
        EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(body.size()))));
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
  resp.headerAddLine("header-1", "Value1");
  resp.headerAddLine("header-2", "Value2");
  auto headers = resp.headers();

  static_assert(std::ranges::input_range<decltype(headers)>);

  static_assert(
      std::indirect_unary_predicate<decltype([](auto&&) { return true; }), std::ranges::iterator_t<decltype(headers)>>);

  EXPECT_TRUE(std::ranges::any_of(
      headers, [](const auto& header) { return header.name == "header-1" && header.value == "Value1"; }));
}

TEST_F(HttpResponseTest, HeaderAndBodySize) {
  std::string buf(256U, 'A');
  std::string buf2(512U, 'B');

  EXPECT_EQ(http::HeaderSize(buf.size(), buf2.size()),
            http::CRLF.size() + buf.size() + http::HeaderSep.size() + buf2.size());

  EXPECT_EQ(HttpResponse::BodySize(buf.size(), buf2.size()),
            buf.size() + http::HeaderSize(http::ContentType.size(), buf2.size()) +
                http::HeaderSize(http::ContentLength.size(), nchars(buf.size())));
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
  resp.headerAddLine("x-a", "1");
  resp.headerAddLine("x-b", "2");
  resp.reason("LONGER-REASON");
  resp.header("x-a", "LARGER-VALUE-123");
  resp.reason("");
  resp.header("x-a", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains("x-a: S\r\n"));
  EXPECT_TRUE(full.contains("x-b: 2\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, StatusReasonAndBodyAddReasonWithHeaders) {
  auto resp = HttpResponse(200).headerAddLine("x-header", 127);
  resp.status(404);
  resp.reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains("x-header: 127\r\n"));
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
  resp.headerAddLine("x-header", 127);
  resp.status(404);
  resp.reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 404 Not Found\r\n"));
  EXPECT_TRUE(full.contains("x-header: 127\r\n"));
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
  auto resp = HttpResponse(404).reason("Not Found").headerAddLine("x-header-1", "Value1");
  resp.headerAddLine("x-header-2", "Value2");
  resp.status(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-header-1: Value1\r\n"));
  EXPECT_TRUE(full.contains("x-header-2: Value2\r\n"));
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
  resp.headerAddLine("x-header-1", "Value1");
  resp.headerAddLine("x-header-2", "Value2");
  resp.status(200).reason("");
  EXPECT_EQ(resp.reason(), "");
  EXPECT_FALSE(resp.hasReason());
  auto full = concatenated(std::move(resp));

  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains("x-header-1: Value1\r\n"));
  EXPECT_TRUE(full.contains("x-header-2: Value2\r\n"));
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
  resp.headerAddLine("x-a", "B").body("Hello");
  auto full = concatenated(std::move(resp));
  ASSERT_GE(full.size(), 16U);
  auto prefix = full.substr(0, 15);
  EXPECT_EQ(prefix.substr(0, 8), "HTTP/1.1") << "Raw prefix: '" << std::string(prefix) << "'";
  EXPECT_EQ(prefix.substr(8, 1), " ");
  EXPECT_EQ(prefix.substr(9, 3), "200");
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains("x-a: B\r\n"));
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
  resp.headerAddLine("x-test", "value");
  auto val = resp.headerValue("");
  EXPECT_FALSE(val.has_value());
}

TEST_F(HttpResponseTest, InsertingInvalidHeaderNameShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.headerAddLine("invalid header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("another:invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("", "value"), std::invalid_argument);

  EXPECT_THROW(resp.headerAppendValue("invalid header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("another:invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("", "value"), std::invalid_argument);

  resp.body("some body");
  EXPECT_THROW(resp.trailerAddLine("invalid trailer", "value"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("another:invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("", "value"), std::invalid_argument);

  EXPECT_THROW(resp.header("invalid header", "value"), std::invalid_argument);
  EXPECT_THROW(resp.header("another:invalid", "value"), std::invalid_argument);
  EXPECT_THROW(resp.header("", "value"), std::invalid_argument);
}

TEST_F(HttpResponseTest, InsertingInvalidHeaderValueShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.headerAddLine("x-test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine("x-test", "value\x7F"), std::invalid_argument);

  EXPECT_THROW(resp.headerAppendValue("x-test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.headerAppendValue("x-test", "value\x7F"), std::invalid_argument);

  resp.body("some body");
  EXPECT_THROW(resp.trailerAddLine("x-trailer", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.trailerAddLine("x-trailer", "value\x7F"), std::invalid_argument);

  EXPECT_THROW(resp.header("x-test", "value\r\n"), std::invalid_argument);
  EXPECT_THROW(resp.header("x-test", "value\x7F"), std::invalid_argument);
}

TEST_F(HttpResponseTest, ContentTypeAndContentLengthShouldBeAddedWhenSettingBody) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.header(http::ContentType, "text/plain"), std::invalid_argument);
  EXPECT_THROW(resp.header(http::ContentLength, "123"), std::invalid_argument);

  EXPECT_THROW(resp.headerAddLine(http::ContentType, "text/plain"), std::invalid_argument);
  EXPECT_THROW(resp.headerAddLine(http::ContentLength, "123"), std::invalid_argument);

  resp.body("Hello");  // should automatically add Content-Type and Content-Length
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentLength), "5");

  // modifying content-type and content-length should still be forbidden
  EXPECT_THROW(resp.header(http::ContentType, "text/html"), std::invalid_argument);
  EXPECT_THROW(resp.header(http::ContentLength, "10"), std::invalid_argument);

  // test valid corner cases
  resp.headerAddLine("content-typ", "text/html");
  resp.headerAddLine("content-lengt", "10");

  resp.headerAddLine("content-typr", "text/html");
  resp.headerAddLine("content-lengty", "10");

  resp.headerAddLine("content-typ", "text/html");
  resp.headerAddLine("content-lengt", "10");

  resp.headerAddLine("content-typr", "text/html");
  resp.headerAddLine("content-lengty", "10");
}

TEST_F(HttpResponseTest, ContentEncodingCanBeModifiedOnlyBeforeTheBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("content-encoding", "identity");
  EXPECT_TRUE(resp.hasHeader(http::ContentEncoding));
  resp.headerRemoveLine(http::ContentEncoding);  // should be possible, no body set
  EXPECT_FALSE(resp.hasHeader(http::ContentEncoding));
  resp.headerAddLine("content-encoding", "identity");
  resp.body("Hello");
  EXPECT_THROW(resp.header("content-encoding", "deflate"), std::logic_error);
  EXPECT_THROW(resp.headerRemoveLine(http::ContentEncoding), std::logic_error);
  EXPECT_EQ(resp.headerValueOrEmpty("content-encoding"), "identity");
}

TEST_F(HttpResponseTest, AllowsDuplicates) {
  HttpResponse resp;
  resp.headerAddLine("x-dup", "1").headerAddLine("x-dup", "2");
  auto full = concatenated(std::move(resp));
  auto first = full.find("x-dup: 1\r\n");
  auto second = full.find("x-dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, AppendHeaderValueAppendsToExistingHeader) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").header("x-custom", "value1");
  resp.headerAppendValue("x-custom", "value2");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-custom: value1, value2\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueCreatesHeaderWhenMissing) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").headerAppendValue("x-missing", "v1");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-missing: v1\r\n")) << full;
}

TEST_F(HttpResponseTest, SetSameReason) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n")) << full;

  resp = HttpResponse(http::StatusCodeOK);
  resp.reason("");
  EXPECT_EQ(resp.reason(), "");
  resp.reason("");
  EXPECT_EQ(resp.reason(), "");

  full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptySeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-list", "first");
  resp.headerAppendValue("x-list", "second", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-list: firstsecond\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-list", "first");
  resp.headerAppendValue("x-list", "", ", ");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-list: first, \r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValueAndSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-list", "first");
  resp.headerAppendValue("x-list", "", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-list: first\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueHonorsCustomSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-list", "first");
  resp.headerAppendValue("x-list", "second", "; ");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-list: first; second\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueSupportsNumericOverload) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-numeric", "1");
  resp.headerAppendValue("x-numeric", 42, "|");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-numeric: 1|42\r\n")) << full;
}

TEST_F(HttpResponseTest, HeaderRemoveLineNotFound) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-first", "value1");
  resp.headerAddLine("x-second", "value2");

  resp.headerRemoveLine("x-notexists");

  EXPECT_TRUE(resp.hasHeader("x-first"));
  EXPECT_TRUE(resp.hasHeader("x-second"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-first"), "value1");
  EXPECT_EQ(resp.headerValueOrEmpty("x-second"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveLineEmptyName) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value");

  resp.headerRemoveLine("");

  EXPECT_TRUE(resp.hasHeader("x-test"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value");
}

TEST_F(HttpResponseTest, HeaderRemoveLineSimple) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-remove", "value");
  resp.headerAddLine("x-keep", "keep-value");

  EXPECT_TRUE(resp.hasHeader("x-remove"));
  resp.headerRemoveLine("x-remove");

  EXPECT_FALSE(resp.hasHeader("x-remove"));
  EXPECT_TRUE(resp.hasHeader("x-keep"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-keep"), "keep-value");
}

TEST_F(HttpResponseTest, HeaderRemoveLinePartialMatch) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-remove", "value");
  resp.headerAddLine("x-keep", "keep-value");

  ASSERT_EQ(resp.headerValueOrEmpty("x-remove"), "value");

  resp.headerRemoveLine("-remove");  // should do nothing
  EXPECT_EQ(resp.headerValueOrEmpty("x-remove"), "value");

  resp.headerRemoveLine("-keep");  // should do nothing
  EXPECT_EQ(resp.headerValueOrEmpty("x-keep"), "keep-value");
}

TEST_F(HttpResponseTest, HeaderRemoveLineLowercaseKey) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-case-test", "value");
  resp.headerAddLine("x-other", "other-value");

  EXPECT_TRUE(resp.hasHeader("x-case-test"));
  resp.headerRemoveLine("x-case-test");

  EXPECT_FALSE(resp.hasHeader("x-case-test"));
  EXPECT_FALSE(resp.hasHeader("x-case-test"));
  EXPECT_TRUE(resp.hasHeader("x-other"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineRemovesLast) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-duplicate", "first");
  resp.headerAddLine("x-duplicate", "second");
  resp.headerAddLine("x-duplicate", "third");

  resp.headerRemoveLine("x-duplicate");

  // Should remove the last occurrence (from reverse search)
  EXPECT_EQ(resp.headerValueOrEmpty("x-duplicate"), "first");

  resp.headerRemoveLine("x-duplicate");
  EXPECT_EQ(resp.headerValueOrEmpty("x-duplicate"), "first");

  resp.headerRemoveLine("x-duplicate");
  EXPECT_FALSE(resp.hasHeader("x-duplicate"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineMultipleTimes) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-multi", "value1");
  resp.headerAddLine("x-multi", "value2");
  resp.headerAddLine("x-multi", "value3");

  // Removes value3 (last), headerValue still returns value1 (first)
  resp.headerRemoveLine("x-multi");
  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "value1");

  // Removes value2 (now last), headerValue still returns value1 (first and only)
  resp.headerRemoveLine("x-multi");
  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "value1");

  // Removes value1 (only remaining)
  resp.headerRemoveLine("x-multi");
  EXPECT_FALSE(resp.hasHeader("x-multi"));
}

TEST_F(HttpResponseTest, HeaderRemoveLineWithBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-header", "value");
  resp.body("Test body content");
  EXPECT_EQ(resp.bodyLength(), 17U);

  EXPECT_TRUE(resp.hasHeader("x-header"));
  resp.headerRemoveLine("x-header");

  EXPECT_FALSE(resp.hasHeader("x-header"));
  EXPECT_EQ(resp.bodyInMemory(), "Test body content");
  EXPECT_EQ(resp.bodyLength(), 17U);
}

TEST_F(HttpResponseTest, HeaderRemoveLineRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .headerAddLine("x-remove", "value")
                  .headerAddLine("x-keep", "keep")
                  .headerRemoveLine("x-remove");

  EXPECT_FALSE(resp.hasHeader("x-remove"));
  EXPECT_TRUE(resp.hasHeader("x-keep"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotFound) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value1, value2");

  resp.headerRemoveValue("x-notexists", "value1");
  EXPECT_TRUE(resp.hasHeader("x-test"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotInHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value1, value2");

  resp.headerRemoveValue("x-test", "value3");
  EXPECT_TRUE(resp.hasHeader("x-test"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueFullLineRemoval) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-single", "only-value");
  resp.headerAddLine("x-keep", "keep-me");

  EXPECT_TRUE(resp.hasHeader("x-single"));
  resp.headerRemoveValue("x-single", "only-value");

  EXPECT_FALSE(resp.hasHeader("x-single"));
  EXPECT_TRUE(resp.hasHeader("x-keep"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-keep"), "keep-me");
}

TEST_F(HttpResponseTest, HeaderRemoveValueAtStart) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-multi", "first, second, third");

  resp.headerRemoveValue("x-multi", "first");

  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "second, third");
}

TEST_F(HttpResponseTest, HeaderRemoveValueAtEnd) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-multi", "first, second, third");

  resp.headerRemoveValue("x-multi", "third");

  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "first, second");
}

TEST_F(HttpResponseTest, HeaderRemoveValueInMiddle) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-multi", "first, second, third");

  resp.headerRemoveValue("x-multi", "second");

  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "first, third");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-custom", "alpha;beta;gamma");

  resp.headerRemoveValue("x-custom", "beta", ";");

  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "alpha;gamma");
}

TEST_F(HttpResponseTest, HeaderRemoveValueLongSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-custom", "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("x-custom", "alpha", "<sep>");  // should do nothing

  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("x-custom", "gamma", "<sep>");  // should do nothing
  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "sep>alpha<sep>beta<sep>gamma<sep");

  resp.headerRemoveValue("x-custom", "beta", "<sep>");
  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "sep>alpha<sep>gamma<sep");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparatorAtStart) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-custom", "alpha|beta|gamma");

  resp.headerRemoveValue("x-custom", "alpha", "|");

  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "beta|gamma");
}

TEST_F(HttpResponseTest, HeaderRemoveValueCustomSeparatorAtEnd) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-custom", "alpha|beta|gamma");

  resp.headerRemoveValue("x-custom", "gamma", "|");

  EXPECT_TRUE(resp.hasHeader("x-custom"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "alpha|beta");
}

TEST_F(HttpResponseTest, HeaderRemoveValueNotProperlyDelimited) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "somevalue, valueanother, value");

  // Try to remove "value" which is a substring but not properly delimited
  resp.headerRemoveValue("x-test", "value");

  // Should not remove anything because "value" is not properly delimited
  EXPECT_TRUE(resp.hasHeader("x-test"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "somevalue, valueanother");
}

TEST_F(HttpResponseTest, HeaderRemoveValuePartialMatch) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value1, value2, value3");

  // Try to remove "value" which is a substring of all values
  resp.headerRemoveValue("x-test", "value");

  // Should not remove anything because "value" is not properly delimited
  EXPECT_TRUE(resp.hasHeader("x-test"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value1, value2, value3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueMultipleOccurrences) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-multi", "apple, banana, apple, cherry");

  // Removes first occurrence within the value (left-to-right search)
  resp.headerRemoveValue("x-multi", "apple");

  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "banana, apple, cherry");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithSpaces) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-spaced", "value1, value2, value3");

  resp.headerRemoveValue("x-spaced", "value2");

  EXPECT_TRUE(resp.hasHeader("x-spaced"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-spaced"), "value1, value3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueFromEmptyHeader) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "");

  resp.headerRemoveValue("x-test", "");

  EXPECT_FALSE(resp.hasHeader("x-test"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueInMiddle) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value1, , value2");

  resp.headerRemoveValue("x-test", "");

  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value1, value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueAtFirst) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", ", value2");

  resp.headerRemoveValue("x-test", "", ", ");

  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueEmptyValueAtLast) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-test", "value2-");

  resp.headerRemoveValue("x-test", "", "-");

  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "value2");
}

TEST_F(HttpResponseTest, HeaderRemoveValueLowercaseKey) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-case", "val1, val2, val3");

  resp.headerRemoveValue("x-case", "val2");

  EXPECT_TRUE(resp.hasHeader("x-case"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-case"), "val1, val3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueFromDuplicateHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-dup", "alpha, beta");
  resp.headerAddLine("x-dup", "gamma, delta");

  // Works on the last header (reverse search), but headerValue returns first
  resp.headerRemoveValue("x-dup", "gamma");

  // Both headers still exist, second is now just "delta"
  EXPECT_TRUE(resp.hasHeader("x-dup"));
  // headerValue returns the first header
  EXPECT_EQ(resp.headerValueOrEmpty("x-dup"), "alpha, beta");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-values", "a, b, c");
  resp.body("Body content here");

  resp.headerRemoveValue("x-values", "b");

  EXPECT_EQ(resp.headerValueOrEmpty("x-values"), "a, c");
  EXPECT_EQ(resp.bodyInMemory(), "Body content here");
}

TEST_F(HttpResponseTest, HeaderRemoveValueRValue) {
  auto resp =
      HttpResponse(http::StatusCodeOK).headerAddLine("x-multi", "v1, v2, v3").headerRemoveValue("x-multi", "v2");

  EXPECT_TRUE(resp.hasHeader("x-multi"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-multi"), "v1, v3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueLeavesSingleValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-two", "first, second");

  resp.headerRemoveValue("x-two", "first", ", ");

  EXPECT_TRUE(resp.hasHeader("x-two"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-two"), "second");

  // Removing the only remaining value removes the whole header line
  resp.headerRemoveValue("x-two", "second", ", ");
  EXPECT_FALSE(resp.hasHeader("x-two"));
}

TEST_F(HttpResponseTest, HeaderRemoveValueComplexSeparator) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-complex", "item1::item2::item3");

  resp.headerRemoveValue("x-complex", "item2", "::");

  EXPECT_TRUE(resp.hasHeader("x-complex"));
  EXPECT_EQ(resp.headerValueOrEmpty("x-complex"), "item1::item3");

  resp.headerRemoveValue("x-compley", "item1", "::");  // should do nothing

  EXPECT_EQ(resp.headerValueOrEmpty("x-complex"), "item1::item3");
}

TEST_F(HttpResponseTest, HeaderRemoveValueWithEmptySepartorShouldThrow) {
  EXPECT_THROW(HttpResponse{}.headerRemoveValue("x-test", "value1", ""), std::invalid_argument);
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
  resp.header("x-global", "UserValue");
  ConcatenatedHeaders globalHeaders;
  globalHeaders.append("x-global: GlobalValue");
  globalHeaders.append("x-another: AnotherValue");
  resp.reason("Some Reason");
  auto full = concatenated(std::move(resp), globalHeaders);
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 Some Reason\r\n"));
  EXPECT_TRUE(full.contains("x-global: UserValue\r\n"));
  EXPECT_TRUE(full.contains("x-another: AnotherValue\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderAppendValuesAreTrimmed) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAppendValue("x-trimmed-header", "   Value1   ", ", ");
  resp.headerAppendValue("x-trimmed-header", "\tValue2\t", ", ");
  resp.headerAppendValue("x-trimmed-header", " \t  Value3 \t ", ", ");

  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header"), "Value1, Value2, Value3");
}

TEST_F(HttpResponseTest, HeaderGetterAfterSet) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  // Mix of headers to exercise several lookup cases:
  // - header replaces an existing normalized lower-case name
  // - addCustomHeader allows duplicates (first occurrence should be returned by headerValue)
  // - empty value is a present-but-empty header
  resp.header("x-simple", "hello");
  resp.headerAddLine("x-dup", "1");
  resp.headerAddLine("x-dup", "2");
  // Replace x-simple (should replace the existing header)
  resp.header("x-simple", "HELLO2");
  // Present but empty value
  resp.header("x-empty", "");

  // headerValue should see the replaced value
  auto opt = resp.headerValue("x-simple");
  EXPECT_EQ(opt.value_or(""), "HELLO2");

  // duplicate headers: headerValue returns the first occurrence
  auto dup = resp.headerValue("x-dup");
  EXPECT_EQ(dup.value_or(""), "1");

  // empty-but-present header: headerValue returns an empty string_view but present
  auto emptyOpt = resp.headerValue("x-empty");
  EXPECT_EQ(emptyOpt.value_or("something"), std::string_view{});

  // missing header should return nullopt via headerValue and empty view via headerValueOrEmpty
  auto missing = resp.headerValue("no-such-header");
  EXPECT_FALSE(missing.has_value());
  EXPECT_EQ(resp.headerValueOrEmpty("no-such-header"), std::string_view{});
}

TEST_F(HttpResponseTest, HeaderNewViaSetter) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-first", "One");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-first: One\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceLowercaseKey) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("x-val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-val: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nData"));
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternLarger) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-key", "before X-Key: should-not-trigger");
  // Replace header; algorithm must not treat the embedded "x-key: " in the value as another header start
  resp.header("x-key", "REPLACED-VALUE");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-key: REPLACED-VALUE\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternSmaller) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-key", "AAAA X-Key: B BBBBBB");
  resp.header("x-key", "SMALL");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-key: SMALL\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceLargerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-replace", "AA");
  // Replace with larger value
  resp.header("x-replace", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-replace: ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceSameLengthValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-replace", "LEN10VALUE");  // length 10
  resp.header("x-replace", "0123456789");  // also length 10
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-replace: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
}

TEST_F(HttpResponseTest, HeaderReplaceSmallerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-replace", "LONG-LONG-VALUE");
  // Replace with smaller
  resp.header("x-replace", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-replace: S\r\n"));
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
  resp.headerAddLine("x-a", "one");
  resp.headerAddLine("x-b", "two");
  resp.headerAddLine("x-c", "three");

  EXPECT_EQ(resp.headerValue("x-c").value_or(""), "three");
  EXPECT_EQ(resp.headerValue("x-d"), std::nullopt);
  EXPECT_TRUE(resp.hasHeader("x-c"));
  EXPECT_FALSE(resp.hasHeader("x-d"));
}

TEST_F(HttpResponseTest, HeaderValuesAreTrimmedHeaderAddLine) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("x-trimmed-header-1", "   Value1   ");
  resp.headerAddLine("x-trimmed-header-2", "\tValue2\t");
  resp.headerAddLine("x-trimmed-header-3", " \t  Value3 \t ");

  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-1"), "Value1");
  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-2"), "Value2");
  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-3"), "Value3");
}

TEST_F(HttpResponseTest, HeaderValuesAreTrimmedHeaderSet) {
  HttpResponse resp(http::StatusCodeOK);
  resp.header("x-trimmed-header-1", "   Value1   ");
  resp.header("x-trimmed-header-2", "\tValue2\t");
  resp.header("x-trimmed-header-3", " \t  Value3 \t ");
  resp.header("x-trimmed-header-3", " \t  Value33 \t ");

  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-1"), "Value1");
  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-2"), "Value2");
  EXPECT_EQ(resp.headerValueOrEmpty("x-trimmed-header-3"), "Value33");
}

TEST_F(HttpResponseTest, LargeHeaderCountStress) {
  constexpr int kCount = 600;
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  for (int i = 0; i < kCount; ++i) {
    resp.headerAddLine(LowerAsciiKey{"x-" + std::to_string(i)}, std::to_string(i));
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
  // Empty-body responses now synthesize a `content-length: 0` line at finalization; exclude it (like
  // Date / Connection) from the user-header count.
  std::string contentLengthPrefix(http::ContentLength);
  contentLengthPrefix += ": ";
  while (pos < full.size()) {
    auto lineEnd = full.find(http::CRLF, pos);
    ASSERT_NE(lineEnd, std::string_view::npos);
    if (lineEnd == pos) {
      pos += 2;
      break;
    }
    auto line = full.substr(pos, lineEnd - pos);
    if (!line.starts_with(datePrefix) && !line.starts_with(connectionPrefix) &&
        !line.starts_with(contentLengthPrefix)) {
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
  resp.header("x-custom", "Value");
  resp.body("BodyContent");
  resp.trailerAddLine("x-trailer", "TrailerValue1");
  resp.trailerAddLine("x-trailer-2", "TrailerValue-2");

  EXPECT_EQ(resp.trailersLength(), http::HeaderSize(9U, 13U) + http::HeaderSize(11U, 14U));

  static constexpr bool kHead = false;

  auto prepared = finalizePrepared(std::move(resp), {}, kHead, true, true, kMinCapturedBodySize);
  std::string all(prepared.firstBuffer());
  all.append(prepared.secondBuffer());

  EXPECT_TRUE(all.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("x-custom", "Value")));
  // When trailers are present, body is chunked encoded per RFC 7230 §4.1.2
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine(http::Trailer, "x-trailer, x-trailer-2")));
  EXPECT_FALSE(all.contains(http::ContentLength));
  // Body should be in chunked format: "b\r\nBodyContent\r\n0\r\n" (b = 11 in hex)
  EXPECT_TRUE(all.contains("b\r\nBodyContent\r\n0\r\n"));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("x-trailer", "TrailerValue1")));
  EXPECT_TRUE(all.contains(MakeHttp1HeaderLine("x-trailer-2", "TrailerValue-2")));
  EXPECT_FALSE(all.contains(MakeHttp1HeaderLine(http::Connection, http::close)));
  EXPECT_FALSE(all.contains(MakeHttp1HeaderLine(http::Connection, http::keepalive)));
  EXPECT_TRUE(all.contains(kExpectedDateRaw));
}

TEST_F(HttpResponseTest, ReplaceDifferentSizes) {
  HttpResponse resp1(http::StatusCodeOK, "OK");
  resp1.headerAddLine("x-a", "1").body("Hello");
  HttpResponse resp2(http::StatusCodeOK, "OK");
  resp2.headerAddLine("x-a", "1").body("Hello");
  HttpResponse resp3(http::StatusCodeOK, "OK");
  resp3.headerAddLine("x-a", "1").body("Hello");
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

#ifdef AERONET_LINUX
TEST_F(HttpResponseTest, SendInvalidFile) {
  test::FileSyscallHookGuard guard;

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "some-data");

  test::gFstatSizes.setActions(tmp.filePath().string(), {-1});
  File file(tmp.filePath().string());

  auto resp = HttpResponse(http::StatusCodeOK, "OK");
  EXPECT_THROW(resp.file(std::move(file)), std::invalid_argument);
}
#endif

// =============================================================================
// BODY TESTS
// =============================================================================

TEST_F(HttpResponseTest, AllowsDuplicatesAfterResettingBody) {
  HttpResponse resp(204, "No Content");
  resp.headerAddLine("x-dup", "1").headerAddLine("x-dup", "2").body("");
  auto full = concatenated(std::move(resp));
  auto first = full.find("x-dup: 1\r\n");
  auto second = full.find("x-dup: 2\r\n");
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

      char bodyLenStr[std::numeric_limits<decltype(bodyLen)>::digits10 + 1];
      const char* pEnd = std::to_chars(bodyLenStr, bodyLenStr + sizeof(bodyLenStr), bodyLen).ptr;
      EXPECT_EQ(resp.headerValue(http::ContentLength), std::string_view(bodyLenStr, pEnd));
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
  resp.header("x-trace", "alpha");
  resp.body("payload");
  resp.headerAppendValue("x-trace", "beta");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("x-trace: alpha, beta\r\n")) << full;
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
                      std::byte{'W'},
                      std::byte{'o'},
                      std::byte{'r'},
                      std::byte{'l'},
                      std::byte{'d'},
                  }));
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
  std::vector<std::byte> bodyBytes = {
      std::byte{'B'}, std::byte{'y'}, std::byte{'t'}, std::byte{'e'}, std::byte{'s'},
  };
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyBytes));
  EXPECT_EQ(resp.bodyInMemory(), "Bytes");

  resp = makePrepared(PreparedOptions{.head = true});
  resp.body(std::vector<std::byte>{
      std::byte{'B'},
      std::byte{'y'},
      std::byte{'t'},
      std::byte{'e'},
      std::byte{'s'},
  });
  EXPECT_EQ(resp.bodyInMemory(), "");
  EXPECT_EQ(resp.bodyInMemoryLength(), 5UL);
}

TEST_F(HttpResponseTest, BodyFromVectorBytesRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .body(std::vector<std::byte>{
                      std::byte{'R'},
                      std::byte{'V'},
                      std::byte{'a'},
                      std::byte{'l'},
                      std::byte{'u'},
                      std::byte{'e'},
                  });
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
  static constexpr std::byte bodyBytes[]{
      std::byte{'S'}, std::byte{'t'}, std::byte{'a'}, std::byte{'t'}, std::byte{'i'}, std::byte{'c'},
  };
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
  resp = makePrepared(PreparedOptions{.head = true}).bodyStatic("Head method static body", "text/head");
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
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF));
  EXPECT_FALSE(full.contains("Hello, World!"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodyLargerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-val", "AA");
  resp.body("Hello");                  // body length 5
  resp.header("x-val", "ABCDEFGHIJ");  // grow header value
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-val: ABCDEFGHIJ\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "5")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySameLengthValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.header("x-val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("x-val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-val: 0123456789\r\n"));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Connection, "close")));
  EXPECT_TRUE(full.contains(kExpectedDateRaw));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_TRUE(full.ends_with("\r\n\r\nData"));
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySmallerValue) {
  auto resp = HttpResponse(http::StatusCodeOK).reason("OK").header("x-val", "SOME-LONG-VALUE");
  resp.body("WorldWide");     // length 9
  resp.header("x-val", "S");  // shrink header value
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(full.contains("x-val: S\r\n"));
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

  EXPECT_THROW(resp.trailerAddLine("x-trailer", "value");, std::logic_error);

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
  // An empty file already declares its own Content-Length: 0; finalization must not synthesize a second one.
  EXPECT_EQ(CountSubstr(headers, http::ContentLength), 1U);
}

// -----------------------------------------------------------------------------
// Synthesized `Content-Length: 0` for empty-body responses (keep-alive framing).
// Per RFC 7230 §3.3.3, a response without a declared body length is framed by connection close, which
// defeats keep-alive reuse. aeronet therefore emits `Content-Length: 0` for empty-body responses at
// finalization (except for body-less statuses, HEAD, file, streaming and direct-compression responses).
// -----------------------------------------------------------------------------

TEST_F(HttpResponseTest, EmptyBodySynthesizesContentLengthZero) {
  static constexpr http::StatusCode kStatuses[]{200, 201, 301, 302, 303, 307, 308, 400, 404, 418, 500, 503};
  for (http::StatusCode status : kStatuses) {
    HttpResponse resp(status);
    const auto full = concatenated(std::move(resp));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "0"))) << "status " << status << ": " << full;
    EXPECT_EQ(CountSubstr(full, http::ContentLength), 1U) << "status " << status << ": " << full;
    EXPECT_FALSE(full.contains(http::TransferEncoding)) << "status " << status << ": " << full;
    // No Content-Type is synthesized for an empty body.
    EXPECT_FALSE(full.contains(http::ContentType)) << "status " << status << ": " << full;
    EXPECT_TRUE(full.ends_with(http::DoubleCRLF)) << "status " << status << ": " << full;
  }
}

TEST_F(HttpResponseTest, EmptyBodyRedirectWithLocationGetsContentLengthZero) {
  // The canonical case from the roadmap note: a bare 302 redirect with only a Location header.
  auto resp = HttpResponse(http::StatusCodeFound).location("https://example.com/new");
  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::Location, "https://example.com/new"))) << full;
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "0"))) << full;
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF)) << full;
}

TEST_F(HttpResponseTest, EmptyBodyBodylessStatusesGetNoContentLength) {
  // 1xx / 204 / 304 are always terminated by the first empty line and either MUST NOT (1xx, 204) or cannot
  // reliably (304) carry a synthesized Content-Length (RFC 7230 §3.3.2 / §3.3.3).
  static constexpr http::StatusCode kStatuses[]{100, 101, 102, 103, 199, 204, 304};
  for (http::StatusCode status : kStatuses) {
    HttpResponse resp(status);
    const auto full = concatenated(std::move(resp));
    EXPECT_FALSE(full.contains(http::ContentLength)) << "status " << status << ": " << full;
    EXPECT_TRUE(full.ends_with(http::DoubleCRLF)) << "status " << status << ": " << full;
  }
}

TEST_F(HttpResponseTest, EmptyBodyHeadResponseGetsNoSynthesizedContentLength) {
  // HEAD responses never carry a body regardless (RFC 7230 §3.3.3); we don't fabricate a Content-Length
  // that may not equal the GET body length.
  HttpResponse resp(http::StatusCodeOK);
  const auto full = concatenated(std::move(resp), /*globalHeaders=*/{}, /*head=*/true);
  EXPECT_FALSE(full.contains(http::ContentLength)) << full;
  EXPECT_TRUE(full.ends_with(http::DoubleCRLF)) << full;
}

TEST_F(HttpResponseTest, NonEmptyBodyKeepsSingleContentLength) {
  HttpResponse resp(http::StatusCodeOK, "Hello");
  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "5"))) << full;
  EXPECT_FALSE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "0"))) << full;
  EXPECT_EQ(CountSubstr(full, http::ContentLength), 1U) << full;
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyFromUniquePtrShouldResetBodyAndRemoveContentType) {
  for (bool head : {false, true}) {
    auto resp = makePrepared(PreparedOptions{.head = head});
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
    auto resp = makePrepared(PreparedOptions{.head = head});
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
  for (const auto minCapturedBodySize : {1ULL, 13ULL, 4096ULL}) {
    HttpResponse resp;
    resp.body(std::string("Hello, World!"));
    EXPECT_FALSE(resp.hasBodyFile());
    EXPECT_TRUE(resp.hasBody());
    EXPECT_TRUE(resp.hasBodyCaptured());
    auto full = concatenated(std::move(resp), {}, false, true, minCapturedBodySize);
    EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
    EXPECT_TRUE(full.contains(MakeHttp1HeaderLine(http::ContentLength, "13")));
    EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
  }
}

TEST_F(HttpResponseTest, SingleTerminatingCRLF) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.headerAddLine("x-header", "v1");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
  EXPECT_TRUE(full.contains("x-header: v1"));
}

// =============================================================================
// TRAILERS TESTS
// =============================================================================

TEST_F(HttpResponseTest, AddTrailerAfterEmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");
  // Explicitly-empty body should still be considered 'no body' for trailers
  EXPECT_THROW(resp.trailerAddLine("x-checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AddTrailerWithoutBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  // No body set at all -> adding trailer should throw
  EXPECT_THROW(resp.trailerAddLine("x-checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyAfterTrailersShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("x-trailer", "value");
  EXPECT_THROW(resp.bodyInlineAppend(16U, kAppendZeroOrOneA), std::logic_error);
  EXPECT_THROW(resp.bodyInlineAppend(1U, kAppendZeroOrOneABytes), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyFromStringAfterTrailersIsLogicError) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("x-trailer", "value");
  EXPECT_THROW(resp.bodyAppend("additional body"), std::logic_error);
}

TEST_F(HttpResponseTest, CannotSendFileAfterTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.reason("OK");
  resp.body("some body");
  resp.trailerAddLine("x-trailer", "value");
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
  resp.trailerAddLine("x-custom-trail", "trail-value");

  // Finalize and inspect the serialized response which concatenates head + external payload
  auto prepared = finalizePrepared(std::move(resp));
  std::string tail(prepared.secondBuffer());

  // The external payload (tail) should contain the body followed by the trailer line and a terminating CRLF
  EXPECT_TRUE(tail.contains("x-custom-trail: trail-value\r\n"));
  EXPECT_TRUE(tail.size() >= 2 && tail.substr(tail.size() - 2) == "\r\n");
}

TEST_F(HttpResponseTest, LoopOnTrailers) {
  HttpResponse resp("some body");
  auto trailers = resp.trailers();

  EXPECT_EQ(trailers.begin(), trailers.end());
  resp.trailerAddLine("header-1", "Value1");

  trailers = resp.trailers();
  EXPECT_EQ(std::distance(trailers.begin(), trailers.end()), 1);
  EXPECT_EQ((*trailers.begin()).name, "header-1");
  EXPECT_EQ((*trailers.begin()).value, "Value1");

  resp.trailerAddLine("header-2", "Value2").trailerAddLine("header-3", "Value3");

  trailers = resp.trailers();
  EXPECT_EQ(std::distance(trailers.begin(), trailers.end()), 3);
  auto it = trailers.begin();
  EXPECT_EQ((*it).name, "header-1");
  EXPECT_EQ((*it).value, "Value1");
  ++it;
  EXPECT_EQ((*it).name, "header-2");
  EXPECT_EQ((*it).value, "Value2");
  ++it;
  EXPECT_EQ((*it).name, "header-3");
  EXPECT_EQ((*it).value, "Value3");
  EXPECT_EQ(++it, trailers.end());
}

TEST_F(HttpResponseTest, SetBodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.trailerAddLine("x-test", "val");
  // Once a trailer was inserted, setting body later must throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
}

TEST_F(HttpResponseTest, SetInlineBodyFromWriterShouldThrowAfterTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.trailerAddLine("x-trailer", "value");
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneA), std::logic_error);
  EXPECT_THROW(resp.bodyInlineSet(8U, kAppendZeroOrOneABytes), std::logic_error);
}

// ============================
// Direct Compression Tests
// ============================

// Test that body remains uncompressed when no compression state is set
TEST_F(HttpResponseTest, Body_NoCompressionState_NoCompression) {
  HttpResponse resp;
  const std::string body = "Hello, World!";
  const std::string_view bodyView(body);

  EXPECT_EQ(resp.directCompressionMode(), DirectCompressionMode::Off);
  resp.body(bodyView, http::ContentTypeTextPlain);
  EXPECT_EQ(resp.bodyInMemoryLength(), bodyView.size());
  EXPECT_EQ(resp.bodyInMemory(), bodyView);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
}

// Test that body remains uncompressed when no encoding was negotiated
TEST_F(HttpResponseTest, Body_NoEncodingNegotiated_NoCompression) {
  // Set up compression state but with no acceptable encoding (Encoding::none)
  HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = Encoding::none});

  const std::string body = "Hello, World!";
  const std::string_view bodyView(body);

  // Even with compression state, if no encoding was negotiated, body should remain uncompressed
  resp.directCompressionMode(DirectCompressionMode::On);

  EXPECT_EQ(resp.directCompressionMode(), DirectCompressionMode::On);
  resp.body(bodyView, http::ContentTypeTextPlain);
  EXPECT_EQ(resp.bodyInMemoryLength(), bodyView.size());
  EXPECT_EQ(resp.bodyInMemory(), bodyView);
  EXPECT_FALSE(resp.hasHeader(http::ContentEncoding));
}

// Test successful compression with compressed body
TEST_F(HttpResponseTest, Body_Accepted_CompressesBody) {
  static constexpr std::string_view kVaryContents[]{
      "", http::AcceptEncoding, "User-Agent", "Accept-Encoding, User-Agent", "*",
  };

  const std::string body(1024, 'A');  // Compressible content
  for (std::string_view varyContent : kVaryContents) {
    for (bool addVary : {false, true}) {
      for (Encoding enc : test::SupportedEncodings()) {
        HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = addVary, .expectedEncoding = enc})
                                .directCompressionMode(DirectCompressionMode::Auto);

        if (!varyContent.empty()) {
          resp.headerAddLine(http::Vary, varyContent);
        }

        const std::string_view bodyView(body);

        EXPECT_EQ(resp.directCompressionMode(), DirectCompressionMode::Auto);
        resp.body(bodyView, http::ContentTypeTextPlain);
        FinalizeCompressedBody(resp);

        // Body should be compressed (smaller than original)
        EXPECT_LT(resp.bodyInMemoryLength(), body.size());
        EXPECT_NE(resp.bodyInMemory(), body);
        // Content-Encoding should be set
        EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
        // Content-Type should be set
        EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeTextPlain);
        // Vary should not be set by default
        if (addVary) {
          if (varyContent.empty()) {
            EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::AcceptEncoding);
          } else if (varyContent == "*") {
            EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), "*");
          } else {
            int count = 0;
            for (http::HeaderValueReverseTokensIterator<','> it(resp.headerValueOrEmpty(http::Vary)); it.hasNext();) {
              if (CaseInsensitiveEqual(it.next(), http::AcceptEncoding)) {
                ++count;
              }
            }
            EXPECT_EQ(count, 1);
          }
        } else {
          EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), varyContent);
        }
      }
    }
  }
}

// Test compressed body with bytes span
TEST_F(HttpResponseTest, Body_BytesSpan_CompressesBody) {
  const std::vector<std::byte> body(1024, std::byte{'B'});
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::span<const std::byte>(body));
    FinalizeCompressedBody(resp);

    // Body should be compressed
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    const std::string_view bodyView(reinterpret_cast<const char*>(body.data()), body.size());
    EXPECT_NE(resp.bodyInMemory(), bodyView);
  }
}

TEST_F(HttpResponseTest, Body_CString_CompressesBody) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    std::string body(1024, 'a');

    resp.body(body.c_str());
    FinalizeCompressedBody(resp);

    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

TEST_F(HttpResponseTest, BodyAppend_Accepted_CompressesBody) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    const std::string body(1024, 'C');

    resp.bodyAppend(body, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    EXPECT_TRUE(resp.headerValueOrEmpty(http::Vary).empty());
    EXPECT_NE(resp.bodyInMemory(), body);
  }
}

// Test Vary: Accept-Encoding for bodyAppend when enabled
TEST_F(HttpResponseTest, BodyAppend_Accepted_VaryEnabled_AddsHeader) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = true, .expectedEncoding = enc});

    const std::string body(1024, 'C');

    resp.bodyAppend(body, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::AcceptEncoding);
  }
}

// Test bodyAppend stays uncompressed when no compression state
TEST_F(HttpResponseTest, BodyAppend_NoCompressionState_NoCompression) {
  HttpResponse resp;
  const std::string body = "Hello!";

  resp.bodyAppend(body, http::ContentTypeTextPlain);
  FinalizeCompressedBody(resp);

  EXPECT_EQ(resp.bodyInMemoryLength(), body.size());
  EXPECT_EQ(resp.bodyInMemory(), body);
  EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
}

// Test bodyAppend without encoding continues compression stream
TEST_F(HttpResponseTest, BodyAppend_ContinuesStreamingCompression) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    std::string data(16384, 'A');

    resp.bodyAppend(data, http::ContentTypeTextPlain);
    const auto beforeLen = resp.bodyInMemoryLength();
    EXPECT_NE(resp.bodyInMemory(), data);

    std::memset(data.data(), 'B', data.size());

    resp.bodyAppend(data);

    FinalizeCompressedBody(resp);

    const auto afterLen = resp.bodyInMemoryLength();

    EXPECT_LT(afterLen - beforeLen, data.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// ============================
// Direct Compression - Corner Cases
// ============================

// Test that setting body() below minBytes threshold does NOT trigger direct compression (Auto mode)
TEST_F(HttpResponseTest, Body_BelowMinBytes_NoDirectCompression) {
  cfg.minBytes = 2048;
  const std::string body(1024, 'A');  // Below 2048 threshold
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(body, http::ContentTypeTextPlain);

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.bodyInMemory(), body);
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  }
}

// Test that DirectCompressionMode::On bypasses minBytes threshold
TEST_F(HttpResponseTest, Body_OnMode_BypassesMinBytes) {
  cfg.minBytes = 2048;
  const std::string body(256, 'B');  // Well below 2048 threshold
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    resp.directCompressionMode(DirectCompressionMode::On);

    resp.body(body, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test DirectCompressionMode::Off disables even with large body
TEST_F(HttpResponseTest, Body_OffMode_NoCompression) {
  const std::string body(4096, 'C');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    resp.directCompressionMode(DirectCompressionMode::Off);

    resp.body(body, http::ContentTypeTextPlain);

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.bodyInMemory(), body);
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  }
}

// Test: reset body multiple times, each time re-initiating streaming compression
TEST_F(HttpResponseTest, Body_ResetMultipleTimes_ReinitiatesCompression) {
  const std::string body1(2048, 'X');
  const std::string body2(4096, 'Y');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    // First body set
    resp.body(std::string_view{body1}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

    // Reset with a different body
    resp.body(std::string_view{body2}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    FinalizeCompressedBody(resp);

    // Verify we can decompress and get body2 (not body1)
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), body2);
  }
}

// Test: set body, then reset to empty body, verify Content-Encoding removed
TEST_F(HttpResponseTest, Body_SetThenClear_RemovesEncodingHeaders) {
  const std::string body(2048, 'A');
  for (bool addVary : {false, true}) {
    for (Encoding enc : test::SupportedEncodings()) {
      HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = addVary, .expectedEncoding = enc});

      resp.body(body, http::ContentTypeTextPlain);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
      if (addVary) {
        EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::AcceptEncoding);
      } else {
        EXPECT_FALSE(resp.hasHeader(http::Vary));
      }

      // Clear the body
      resp.body("", http::ContentTypeTextPlain);
      EXPECT_FALSE(IsAutomaticDirectCompression(resp));
      EXPECT_FALSE(resp.hasBody());
      EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
      EXPECT_FALSE(resp.hasHeader(http::Vary));
    }
  }
}

// Test: set body with compression, then reset to small body (below minBytes), verify headers removed
TEST_F(HttpResponseTest, Body_CompressedThenSmall_RemovesEncodingHeaders) {
  const std::string largeBody(2048, 'L');
  const std::string smallBody = "tiny";
  for (bool addVary : {false, true}) {
    for (Encoding enc : test::SupportedEncodings()) {
      HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = addVary, .expectedEncoding = enc});

      resp.body(std::string_view{largeBody}, http::ContentTypeTextPlain);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
      if (addVary) {
        EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::AcceptEncoding);
      } else {
        EXPECT_FALSE(resp.hasHeader(http::Vary));
      }

      // Reset with a small body below threshold
      resp.body(smallBody, http::ContentTypeTextPlain);
      EXPECT_FALSE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.bodyInMemory(), smallBody);
      EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
      EXPECT_FALSE(resp.hasHeader(http::Vary));
    }
  }
}

// Test: bodyAppend after body() continues streaming compression
TEST_F(HttpResponseTest, BodyAppend_AfterBody_ContinuesStreaming) {
  const std::string body1(2048, 'A');
  const std::string body2(1024, 'B');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string_view{body1}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    resp.bodyAppend(body2);
    FinalizeCompressedBody(resp);

    EXPECT_LT(resp.bodyInMemoryLength(), body1.size() + body2.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

    // Verify decompressed content is body1 + body2
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    std::string expected = body1 + body2;
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);
  }
}

// Test: multiple bodyAppend calls build up streaming compression
TEST_F(HttpResponseTest, BodyAppend_MultipleChunks_StreamingCompression) {
  std::string aaa(1024, 'A');
  std::string bbb(1024, 'B');
  std::string ccc(1024, 'C');
  std::string ddd(1024, 'D');
  std::string expected = aaa + bbb + ccc + ddd;  // Expect all chunks concatenated in decompressed output
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.bodyAppend(aaa, http::ContentTypeTextPlain);
    resp.bodyAppend(bbb);
    resp.bodyAppend(ccc);
    resp.bodyAppend(ddd);
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), 4096U);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

    auto decompressed = test::Decompress(enc, resp.bodyInMemory());

    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);
  }
}

// Test: body() after bodyAppend() resets and re-initiates compression
TEST_F(HttpResponseTest, Body_AfterBodyAppend_ResetsCompression) {
  const std::string newBody(3000, 'Z');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.bodyAppend(std::string(2048, 'X'), http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    // Now reset with body()
    resp.body(std::string_view{newBody}, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), newBody);
  }
}

// Test: bodyInlineAppend during active streaming compression fallback path
TEST_F(HttpResponseTest, BodyInlineAppend_DuringStreamingCompression) {
  const std::string initialBody(2048, 'A');
  const std::string appendData(512, 'A');
  const std::string expected = initialBody + appendData;

  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string_view{initialBody}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    // Use bodyInlineAppend - this triggers the decompress-and-append path
    for (std::size_t i = 0; i < 2UL * appendData.size(); ++i) {
      resp.bodyInlineAppend(1U, kAppendZeroOrOneA);
    }
    FinalizeCompressedBody(resp);

    // The body should still be compressed
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    EXPECT_LT(resp.bodyInMemoryLength(), initialBody.size() + appendData.size());

    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);
  }
}

// Test: bodyInlineAppend with bytes writer during streaming compression
TEST_F(HttpResponseTest, BodyInlineAppend_BytesWriter_DuringStreamingCompression) {
  const std::string initialBody(1500, 'M');
  const std::vector<std::byte> appendBytes(256, std::byte{'N'});
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string_view{initialBody}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    for (std::size_t i = 0; i < 2UL * appendBytes.size(); ++i) {
      resp.bodyInlineAppend(1U, kAppendZeroOrOneABytes);
    }

    FinalizeCompressedBody(resp);

    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(decompressed.size(), initialBody.size() + appendBytes.size());
  }
}

// Test: bodyInlineAppend writes zero bytes during streaming compression
TEST_F(HttpResponseTest, BodyInlineAppend_ZeroWritten_DuringStreamingCompression) {
  const std::string initialBody(2048, 'Q');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string_view{initialBody}, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    resp.bodyInlineAppend(1U, kAppendZeroOrOneA);
    char dummy{};
    kAppendZeroOrOneA(&dummy);  // dummy call to reset counter
    FinalizeCompressedBody(resp);

    // Body should still be compressed with original data only
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), initialBody);
  }
}

// Test: User-provided Content-Encoding header prevents direct compression
TEST_F(HttpResponseTest, Body_UserContentEncoding_PreventsDirectCompression) {
  const std::string body(4096, 'D');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.headerAddLine(http::ContentEncoding, "identity");
    resp.body(body, http::ContentTypeTextPlain);

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.bodyInMemory(), body);
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), "identity");
  }
}

// Test: encoding headers are not duplicated on body reset
TEST_F(HttpResponseTest, Body_ResetDoesNotDuplicateContentEncoding) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = true, .expectedEncoding = enc});

    // Set body 3 times
    for (int ii = 0; ii < 3; ++ii) {
      const std::string body(2048, static_cast<char>('A' + ii));
      resp.body(body, http::ContentTypeTextPlain);
    }
    FinalizeCompressedBody(resp);

    // Count occurrences of Content-Encoding in headers
    auto flatHeaders = resp.headersFlatView();
    int ceCount = 0;
    int varyCount = 0;
    std::string_view needle = http::ContentEncoding;
    auto pos = flatHeaders.find(needle);
    while (pos != std::string_view::npos) {
      ++ceCount;
      pos = flatHeaders.find(needle, pos + 1);
    }
    needle = http::Vary;
    pos = flatHeaders.find(needle);
    while (pos != std::string_view::npos) {
      ++varyCount;
      pos = flatHeaders.find(needle, pos + 1);
    }
    EXPECT_EQ(ceCount, 1) << "Content-Encoding should appear exactly once";
    EXPECT_EQ(varyCount, 1) << "Vary should appear exactly once";
  }
}

// Test: bodyInlineSet clears direct compression state
TEST_F(HttpResponseTest, BodyInlineSet_AfterDirectCompression_ClearsState) {
  const std::string body(2048, 'A');
  const std::string newBody = "A";
  for (bool bodyInlineSetBytes : {false, true}) {
    for (Encoding enc : test::SupportedEncodings()) {
      HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

      resp.body(body, http::ContentTypeTextPlain);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

      // bodyInlineSet replaces the body - it calls removeBodyAndItsHeaders()
      if (bodyInlineSetBytes) {
        resp.bodyInlineSet(1U, kAppendZeroOrOneABytes);
        resp.bodyInlineSet(1U, kAppendZeroOrOneABytes);
      } else {
        resp.bodyInlineSet(1U, kAppendZeroOrOneA);
        resp.bodyInlineSet(1U, kAppendZeroOrOneA);
      }

      // Direct compression state should be cleared
      EXPECT_FALSE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.bodyInMemory(), newBody);
      EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
    }
  }
}

// Test: captured body (moved string) does NOT use direct compression
TEST_F(HttpResponseTest, Body_CapturedString_NoDirectCompression) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string(4096, 'C'));

    // Captured bodies should not trigger direct compression (compressed at finalization instead)
    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  }
}

// Test: file body does NOT use direct compression
TEST_F(HttpResponseTest, File_NoDirectCompression) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, std::string(2048, 'F'));

  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    File fl(tmp.filePath().string());
    ASSERT_TRUE(fl);

    resp.file(std::move(fl));

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  }
}

// Test: Vary header correctly managed alongside existing Vary header
TEST_F(HttpResponseTest, Body_ExistingVaryHeader_Appends) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = true, .expectedEncoding = enc});

    resp.headerAddLine(http::Vary, "Origin");
    const std::string body(2048, 'V');
    resp.body(body, http::ContentTypeTextPlain);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    auto varyValue = resp.headerValueOrEmpty(http::Vary);
    // Vary should contain both Origin and Accept-Encoding
    EXPECT_TRUE(varyValue.contains("Origin"));
    EXPECT_TRUE(varyValue.contains(http::AcceptEncoding));
  }
}

// Test: Content-Type allowlist filtering works in Auto mode
TEST_F(HttpResponseTest, Body_ContentTypeNotInAllowList_NoCompression) {
  cfg.contentTypeAllowList.append("text/html");
  cfg.contentTypeAllowList.append("application/json");

  const std::string body(2048, 'I');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    // image/png is not in the allowlist
    resp.body(body, "image/png");

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.bodyInMemory(), body);
    EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
  }
}

// Test: Content-Type in allowlist allows compression
TEST_F(HttpResponseTest, Body_ContentTypeInAllowList_CompressesBody) {
  cfg.contentTypeAllowList.append("text/html");
  cfg.contentTypeAllowList.append("application/json");

  const std::string body(2048, '{');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(body, "application/json");
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test: On mode bypasses content type allow list check
TEST_F(HttpResponseTest, Body_OnMode_BypassesContentTypeAllowList) {
  cfg.contentTypeAllowList.append("text/html");

  const std::string body(2048, 'P');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    resp.directCompressionMode(DirectCompressionMode::On);

    resp.body(body, "image/png");  // Not in allowlist
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test: head method does not trigger compression (no body sent)
TEST_F(HttpResponseTest, Body_HeadMethod_NoDirectCompression) {
  const std::string body(2048, 'H');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.head = true, .expectedEncoding = enc});

    resp.body(body, http::ContentTypeTextPlain);

    // HEAD method should not store body
    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
  }
}

// Test: large body with multiple bodyAppend cycles verifying round-trip
TEST_F(HttpResponseTest, BodyAppend_LargeDataRoundTrip) {
  cfg.minBytes = 256;

  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    std::string expected;
    constexpr int kChunks = 20;
    for (int ii = 0; ii < kChunks; ++ii) {
      std::string chunk(512, static_cast<char>('a' + (ii % 26)));
      if (ii == 0) {
        resp.bodyAppend(chunk, http::ContentTypeTextPlain);
      } else {
        resp.bodyAppend(chunk);
      }
      expected += chunk;
    }
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), expected.size());

    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);
  }
}

// Test: body() with content type change during body reset keeps correct content type
TEST_F(HttpResponseTest, Body_Reset_ContentTypeUpdated) {
  const std::string bodyA(2048, 'A');
  const std::string bodyB(2048, 'B');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::string_view{bodyA}, "text/html");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/html");

    resp.body(std::string_view{bodyB}, "application/json");
    FinalizeCompressedBody(resp);

    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/json");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test: bodyAppend with content type update during streaming compression
TEST_F(HttpResponseTest, BodyAppend_ContentTypeUpdate_DuringStreaming) {
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.bodyAppend(std::string(2048, 'A'), http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    // Append with new content type
    resp.bodyAppend(std::string(512, 'B'), "application/json");
    FinalizeCompressedBody(resp);

    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "application/json");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test: trailerAddLine finalizes inline body when direct compression is active
TEST_F(HttpResponseTest, TrailerAddLine_FinalizesDirectCompression) {
  const std::string body(2048, 'T');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(body, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    // Adding a trailer should finalize the compressed body
    resp.trailerAddLine("x-checksum", "abc123");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

    // Verify the body is properly compressed and decompressible
    auto bodyView = resp.bodyInMemory();
    EXPECT_LT(bodyView.size(), body.size());
    auto decompressed = test::Decompress(enc, bodyView);
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), body);
  }
}

// Test: defaultDirectCompressionMode from CompressionConfig is respected
TEST_F(HttpResponseTest, DefaultDirectCompressionMode_FromConfig) {
  for (Encoding enc : test::SupportedEncodings()) {
    cfg.defaultDirectCompressionMode = DirectCompressionMode::Off;
    HttpResponse resp1 = makePrepared(PreparedOptions{.expectedEncoding = enc});
    EXPECT_EQ(resp1.directCompressionMode(), DirectCompressionMode::Off);

    cfg.defaultDirectCompressionMode = DirectCompressionMode::On;
    HttpResponse resp2 = makePrepared(PreparedOptions{.expectedEncoding = enc});
    EXPECT_EQ(resp2.directCompressionMode(), DirectCompressionMode::On);

    cfg.defaultDirectCompressionMode = DirectCompressionMode::Auto;
    HttpResponse resp3 = makePrepared(PreparedOptions{.expectedEncoding = enc});
    EXPECT_EQ(resp3.directCompressionMode(), DirectCompressionMode::Auto);
  }
}

// Test: switching directCompressionMode at runtime before setting body
TEST_F(HttpResponseTest, SwitchMode_BeforeBody) {
  const std::string body(4096, 'S');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    EXPECT_EQ(resp.directCompressionMode(), DirectCompressionMode::Auto);

    resp.directCompressionMode(DirectCompressionMode::Off);
    resp.body(body, http::ContentTypeTextPlain);

    EXPECT_FALSE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.bodyInMemory(), body);

    // Switch back and reset body
    resp.directCompressionMode(DirectCompressionMode::Auto);
    resp.body(body, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
  }
}

// Test: rvalue chaining of directCompressionMode
TEST_F(HttpResponseTest, DirectCompressionMode_RvalueChaining) {
  const std::string body(256, 'R');
  for (Encoding enc : test::SupportedEncodings()) {
    auto resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    resp.directCompressionMode(DirectCompressionMode::On);

    std::move(resp).body(body, http::ContentTypeTextPlain);
    // Just verify it doesn't crash - rvalue chain
  }
}

// Test: finalize for HTTP/1.1 with direct compression updates Content-Length
TEST_F(HttpResponseTest, FinalizeHttp1_DirectCompression_UpdatesContentLength) {
  const std::string body(4096, 'F');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(body, http::ContentTypeTextPlain);
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));

    // Do NOT call FinalizeCompressedBody here - finalizeForHttp1 handles it internally
    auto full = concatenated(std::move(resp));

    // Verify the full response is valid and contains compressed body
    EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
    // Headers use lowercase names in internal representation
    const auto contentEncodingLine =
        std::format("{}{}{}{}", http::ContentEncoding.get(), http::HeaderSep, GetEncodingStr(enc), http::CRLF);
    EXPECT_TRUE(full.contains(contentEncodingLine));
    EXPECT_TRUE(full.contains("content-length:"));
    // The compressed body should be much smaller
    auto headerEnd = full.find(http::DoubleCRLF);
    ASSERT_NE(headerEnd, std::string::npos);
    auto bodyPart = std::string_view(full).substr(headerEnd + http::DoubleCRLF.size());
    EXPECT_LT(bodyPart.size(), body.size());
  }
}

// Test: body reset cycle: compressed -> empty -> compressed
TEST_F(HttpResponseTest, Body_CompressedEmptyCompressedCycle) {
  const std::string body1(2048, '1');
  const std::string body2(3000, '2');
  for (bool addVary : {false, true}) {
    for (Encoding enc : test::SupportedEncodings()) {
      HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = addVary, .expectedEncoding = enc});

      // Step 1: Set compressed body
      resp.body(body1, http::ContentTypeTextPlain);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

      // Step 2: Clear body
      resp.body("", http::ContentTypeTextPlain);
      EXPECT_FALSE(IsAutomaticDirectCompression(resp));
      EXPECT_FALSE(resp.hasBody());
      EXPECT_TRUE(resp.headerValueOrEmpty(http::ContentEncoding).empty());
      EXPECT_FALSE(resp.hasHeader(http::Vary));

      // Step 3: Set compressed body again
      resp.body(body2, http::ContentTypeTextPlain);
      FinalizeCompressedBody(resp);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
      if (addVary) {
        EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), http::AcceptEncoding);
      } else {
        EXPECT_FALSE(resp.hasHeader(http::Vary));
      }

      auto decompressed = test::Decompress(enc, resp.bodyInMemory());
      EXPECT_EQ(std::string_view(decompressed), body2);
    }
  }
}

// Test: many rapid body resets don't corrupt state
TEST_F(HttpResponseTest, Body_ManyRapidResets) {
  const std::string finalBody(2048, 'Z');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    for (int ii = 0; ii < 10; ++ii) {
      const std::string body(static_cast<std::size_t>(1024 + (ii * 100)), static_cast<char>('A' + (ii % 26)));
      resp.body(body, http::ContentTypeTextPlain);
      EXPECT_TRUE(IsAutomaticDirectCompression(resp));
      EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
    }

    // Final body
    resp.body(std::string_view{finalBody}, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), finalBody);
  }
}

// Test: setting custom headers between body resets
TEST_F(HttpResponseTest, Body_ResetWithCustomHeaders_HeadersPreserved) {
  const std::string bodyA(2048, 'A');
  const std::string bodyB(2048, 'B');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.headerAddLine("x-custom", "value1");
    resp.body(std::string_view{bodyA}, http::ContentTypeTextPlain);
    EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "value1");

    // Reset body
    resp.body(std::string_view{bodyB}, http::ContentTypeTextPlain);
    FinalizeCompressedBody(resp);

    // Custom header should still be present
    EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "value1");
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
  }
}

// Test: CString body with direct compression
TEST_F(HttpResponseTest, Body_CString_DirectCompression) {
  std::string body(2048, 'S');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(body.c_str());
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_LT(resp.bodyInMemoryLength(), body.size());
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), body);
  }
}

// Test: bytes span body with direct compression and round-trip
TEST_F(HttpResponseTest, Body_BytesSpan_DirectCompressionRoundTrip) {
  std::vector<std::byte> body(2048, std::byte{'D'});
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});

    resp.body(std::span<const std::byte>(body));
    FinalizeCompressedBody(resp);

    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));

    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    std::string_view expectedStr(reinterpret_cast<const char*>(body.data()), body.size());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expectedStr);
  }
}

// =============================================================================
// OTHER TESTS
// =============================================================================

TEST_F(HttpResponseTest, RepeatedGrowShrinkCycles) {
  HttpResponse resp(http::StatusCodeOK, "");
  resp.headerAddLine("x-static", "STATIC");
  resp.header("x-cycle", "A");
  resp.reason("R1");
  resp.header("x-cycle", "ABCDEFGHIJ");
  resp.body("one");
  resp.reason("");
  resp.header("x-cycle", "B");
  resp.body("two-two");
  resp.reason("LONGER-REASON");
  resp.header("x-cycle", "ABCDEFGHIJKLMNOP");
  resp.body("short");
  resp.reason("");
  resp.header("x-cycle", "C");
  resp.body("0123456789ABCDEFGHIJ");
  resp.header("x-cycle", "LONGVALUE-1234567890");
  resp.reason("MID");
  resp.header("x-cycle", "X");
  resp.body("XYZ");
  resp.reason("");
  resp.header("x-cycle", "Z");
  resp.body("END");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200 \r\n"));
  EXPECT_TRUE(full.contains("x-static: STATIC\r\n"));
  EXPECT_TRUE(full.contains("x-cycle: Z\r\n"));
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
  // Find first space after status code digits
  if (statusLine.size() > 13 && statusLine[12] == ' ') {
    pr.reason.assign(statusLine.substr(13));
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
    if (hdr.first == http::ContentLength.get()) {
      EXPECT_EQ(std::from_chars(hdr.second.data(), hdr.second.data() + hdr.second.size(), contentLen).ec, std::errc());
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
  flat_hash_map<std::string, std::string, CityHash, std::equal_to<>> expected;
  for (std::string_view gh : globalHeaders) {
    std::string_view name = gh.substr(0, gh.find(": "));
    std::string_view value = gh.substr(gh.find(": ") + 2);
    auto opt = resp.headerValue(LowerAsciiKey{name});
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
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
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
      std::string name = "x-global-" + std::to_string(iter) + "-" + std::to_string(headerIdx);
      std::string value = makeValue(valueLenDist(rng));
      std::string header = name;
      header += http::HeaderSep;
      header += value;
      globalHeaders.append(header);
      if (userOverrideDist(rng)) {
        resp.header(LowerAsciiKey{name}, "user-" + value);
      }
    }

    auto expected = ExpectedGlobalHeaderValues(resp, globalHeaders);
    auto serialized = concatenated(std::move(resp), globalHeaders);
    ParsedResponse parsed = parseResponse(serialized, false);

    for (std::string_view gh : globalHeaders) {
      // gh is a string_view of the form "name: Value". Extract the name for comparisons.
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
  resp.headerAddLine("x-seed", "local-value");
  resp.body("payload");

  constexpr int kGlobalHeaders = HttpServerConfig::kMaxGlobalHeaders;
  // Build concatenated global headers but keep an indexed vector for targeted overrides below.
  std::vector<http::Header> headerVec;
  headerVec.reserve(kGlobalHeaders);
  aeronet::ConcatenatedHeaders globalHeaders;
  for (int headerIdx = 0; headerIdx < kGlobalHeaders; ++headerIdx) {
    std::string name = "x-bulk-" + std::to_string(headerIdx);
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
  resp.header(LowerAsciiKey{headerVec[42].name()}, "UserOverride-42");
  resp.header(LowerAsciiKey{headerVec[199].name()}, "UserOverride-199");

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

  // NOLINTNEXTLINE(bugprone-random-generator-seed)
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
      std::string name = "x-fuzz-global-" + std::to_string(caseIndex) + "-" + std::to_string(globalIdx);
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
        EXPECT_FALSE(resp.headerValue(LowerAsciiKey{lastHeaderKey}).has_value());
      }
      EXPECT_EQ(resp.trailerValueOrEmpty(LowerAsciiKey{lastTrailerKey}), lastTrailerValue);
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
          lastHeaderKey = "x-" + std::to_string(step);
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<uint32_t>(rng() % fuzzHeaderVec.size())].name();
          }
          lastHeaderValue = makeValue(smallLen(rng));
          resp.headerAddLine(LowerAsciiKey{lastHeaderKey}, lastHeaderValue);
          break;
        case 1:
          lastHeaderKey = "u-" + std::string(static_cast<std::size_t>(step % 5), 's');
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<uint32_t>(rng() % fuzzHeaderVec.size())].name();
          }
          lastHeaderValue = makeValue(midLen(rng));
          resp.header(LowerAsciiKey{lastHeaderKey}, lastHeaderValue);
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
          static constexpr http::StatusCode opts[]{200, 204, 404, 418};
          lastStatus = opts[static_cast<std::size_t>(step) % std::size(opts)];
          resp.status(lastStatus);
          break;
        }
        case 5:
          if (lastBody.empty()) {
            EXPECT_THROW(resp.trailerAddLine("x-trailer", "value"), std::logic_error);
          } else if (!resp.hasBodyFile()) {
            lastTrailerKey = "x-" + std::to_string(step);
            lastTrailerValue = makeValue(smallLen(rng));
            resp.trailerAddLine(LowerAsciiKey{lastTrailerKey}, lastTrailerValue);
          } else {
            // Once a file body was set, trailers cannot be added
            EXPECT_THROW(resp.trailerAddLine("x-trailer", "value"), std::logic_error);
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
      if (headerPair.first == http::Date.get()) {
        ++dateCount;
      } else if (headerPair.first == http::Connection.get()) {
        ++connCount;
      } else if (headerPair.first == http::ContentLength.get()) {
        ++clCount;
        EXPECT_EQ(
            std::from_chars(headerPair.second.data(), headerPair.second.data() + headerPair.second.size(), clVal).ec,
            std::errc());
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
        // Empty body: an allowed status now synthesizes `Content-Length: 0` (keep-alive framing).
        // Body-less statuses (204, and by RFC also 1xx/304) must not carry it. The fuzz only exercises
        // {200, 204, 404, 418}, so 204 is the sole excluded case here.
        const bool statusAllowsCL0 = lastStatus >= 200 && lastStatus != 204 && lastStatus != 304;
        if (statusAllowsCL0) {
          EXPECT_EQ(clCount, 1);
          EXPECT_EQ(clVal, 0U);
        } else {
          EXPECT_EQ(clCount, 0);
        }
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
  EXPECT_FALSE(resp.hasTrailer("x-checksum"));
  resp.trailerAddLine("x-checksum", "abc123");

  EXPECT_EQ(resp.trailerValueOrEmpty("x-another"), "");
  EXPECT_EQ(resp.trailerValueOrEmpty("x-checksum"), "abc123");
  EXPECT_EQ(resp.trailerValue("x-checksum").value_or(""), "abc123");
  EXPECT_FALSE(resp.trailerValue("x-cheksum"));
  EXPECT_TRUE(resp.hasTrailer("x-checksum"));

  // We can't easily test the serialized output without finalizing,
  // but we can verify no exception is thrown
  EXPECT_NO_THROW(resp.trailerAddLine("x-signature", "sha256:..."));
}

// Test error when adding trailer before body
TEST(HttpResponseTrailers, ErrorBeforeBody) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.trailerAddLine("x-checksum", "abc123"), std::logic_error);
}

// Test error when adding trailer after an explicitly empty body
TEST(HttpResponseTrailers, EmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");  // empty body set explicitly
  EXPECT_THROW(resp.trailerAddLine("x-checksum", "abc123"), std::logic_error);
}

// Test trailer with captured body (std::string)
TEST(HttpResponseTrailers, CapturedBodyString) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured body content"));
  EXPECT_NO_THROW(resp.trailerAddLine("x-custom", "value"));
}

// Test trailer with captured body (vector<char>)
TEST(HttpResponseTrailers, CapturedBodyVector) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::vector<char>{'h', 'e', 'l', 'l', 'o'});
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("hello"));
  EXPECT_NO_THROW(resp.trailerAddLine("x-data", "123"));

  resp = HttpResponse{"some body that should be erased"};
  resp.body(std::vector<char>{});  // empty body
  EXPECT_EQ(resp.bodyInMemory(), std::string_view());
  EXPECT_THROW(resp.trailerAddLine("x-data", "123"), std::logic_error);
}

// Test multiple trailers
TEST(HttpResponseTrailers, MultipleTrailers) {
  HttpResponse resp("body");
  resp.trailerAddLine("x-checksum", "abc");
  resp.trailerAddLine("x-timestamp", "2025-10-20T12:00:00Z");
  resp.trailerAddLine("x-custom", "val");
  EXPECT_EQ(resp.trailersFlatView(), "x-checksum: abc\r\nx-timestamp: 2025-10-20T12:00:00Z\r\nx-custom: val\r\n");
}

// Test empty trailer value
TEST(HttpResponseTrailers, EmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test");
  EXPECT_NO_THROW(resp.trailerAddLine("x-empty", ""));
}

// Test rvalue ref version
TEST(HttpResponseTrailers, RvalueRef) {
  EXPECT_NO_THROW(HttpResponse(http::StatusCodeOK).body("test").trailerAddLine("x-check", "val"));
}

// Test that setting the body after inserting a trailer throws
TEST(HttpResponseTrailers, BodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.trailerAddLine("x-after", "v");
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
  resp.headerAddLine("x-test", "val");
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
  resp.trailerAddLine("x-first", "one");
  resp.trailerAddLine("x-second", "two");
  auto tv = resp.trailersFlatView();
  EXPECT_FALSE(tv.empty());
  // trailers are stored as header lines terminated by CRLF
  EXPECT_TRUE(tv.contains("x-first: one\r\n"));
  EXPECT_TRUE(tv.contains("x-second: two\r\n"));
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
  resp.trailerAddLine("x-custom", "val");
  auto tv = resp.trailersFlatView();
  EXPECT_FALSE(tv.empty());
  EXPECT_TRUE(tv.contains("x-custom: val\r\n"));
  EXPECT_TRUE(tv.ends_with(http::CRLF));
  // body() must remain the original captured body (trailers excluded)
  EXPECT_EQ(resp.bodyInMemory(), std::string_view("captured-body"));
}

TEST(HttpResponseAppendHeaderValue, AppendsToEmptyHeader) {
  HttpResponse resp;
  resp.headerAppendValue("x-test", "alpha");
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "alpha");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithDefaultSeparator) {
  HttpResponse resp;
  resp.header("x-test", "one");
  resp.headerAppendValue("x-test", "two");
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "one, two");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithCustomSeparator) {
  HttpResponse resp;
  resp.header("x-test", "first");
  resp.headerAppendValue("x-test", "second", "; ");
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "first; second");
}

TEST(HttpResponseAppendHeaderValue, NumericOverloadAndSubsequentAppend) {
  HttpResponse resp;
  resp.headerAppendValue("x-num", 123);
  EXPECT_EQ(resp.headerValueOrEmpty("x-num"), "123");

  resp.headerAppendValue("x-num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("x-num"), "123, 456");

  resp = HttpResponse{}.headerAppendValue("x-num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("x-num"), "456");
}

TEST(HttpResponseAppendHeaderValue, LowercaseKeyMatch) {
  HttpResponse resp;
  resp.header("x-test", "lower");
  resp.headerAppendValue("x-test", "upper");
  EXPECT_EQ(resp.headerValueOrEmpty("x-test"), "lower, upper");
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

TEST_F(HttpResponseTest, FinalizeHeadersAndBody_NoHeadersIsNoop) {
  HttpResponse resp(http::StatusCodeOK);
  // No headers added -> no-op
  FinalizeHeadersAndBody(resp);
  EXPECT_TRUE(resp.headersFlatView().empty());
}

TEST_F(HttpResponseTest, FinalizeHeadersAndBody_DirectCompressionFinalization) {
  // Set a body large enough to be compressed
  const std::string body(4096, 'C');
  for (bool addVary : {false, true}) {
    for (bool addTrailers : {false, true}) {
      for (Encoding enc : test::SupportedEncodings()) {
        HttpResponse resp = makePrepared(PreparedOptions{.addVaryAcceptEncoding = addVary, .expectedEncoding = enc});
        resp.directCompressionMode(DirectCompressionMode::On);  // Force direct compression

        resp.body(body, http::ContentTypeTextPlain);

        // Verify direct compression is active before finalization
        EXPECT_TRUE(IsAutomaticDirectCompression(resp));
        EXPECT_EQ(resp.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(enc));
        if (addVary) {
          EXPECT_TRUE(resp.headerValueOrEmpty(http::Vary).contains(http::AcceptEncoding));
        } else {
          EXPECT_FALSE(resp.headerValue(http::Vary));
        }
        if (addTrailers) {
          resp.trailerAddLine("x-trailer", "value");
        } else {
          EXPECT_EQ(resp.trailersSize(), 0U);
        }

        const auto compressedBodySize = resp.bodyInMemoryLength();
        EXPECT_LT(compressedBodySize, body.size());  // Should be compressed

        FinalizeHeadersAndBody(resp);

        // Verify header names are lowercased
        std::string headers(resp.headersFlatView());
        // Content-Encoding header should be present and lowercase
        EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("content-encoding", GetEncodingStr(enc))));
        // Should not contain uppercase version
        EXPECT_FALSE(headers.contains("Content-Encoding:"));

        // Verify content type header is also lowercased
        EXPECT_TRUE(headers.contains("content-type:"));
        EXPECT_FALSE(headers.contains("Content-Type:"));

        if (addVary) {
          EXPECT_TRUE(resp.headerValueOrEmpty(http::Vary).contains(http::AcceptEncoding));
        } else {
          EXPECT_FALSE(resp.headerValue(http::Vary));
        }

        // Decompress and verify original body is recovered
        auto decompressed = test::Decompress(enc, resp.bodyInMemory());
        EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), body);
      }
    }
  }
}

TEST_F(HttpResponseTest, FinalizeHeadersAndBody_DirectCompressionPreservesNormalizedHeaders) {
  // Lower-case custom names remain unchanged while compression is finalized.
  const std::string body(2048, 'D');
  for (Encoding enc : test::SupportedEncodings()) {
    HttpResponse resp = makePrepared(PreparedOptions{.expectedEncoding = enc});
    resp.directCompressionMode(DirectCompressionMode::On);

    resp.headerAddLine("x-custom-header", "ValuePreserved");
    resp.headerAddLine("cache-control", "max-age=3600");

    resp.body(body, http::ContentTypeApplicationJson);

    // Verify setup
    EXPECT_TRUE(IsAutomaticDirectCompression(resp));
    EXPECT_EQ(resp.trailersSize(), 0U);

    FinalizeHeadersAndBody(resp);

    std::string headers(resp.headersFlatView());

    EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("x-custom-header", "ValuePreserved")));
    EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine("cache-control", "max-age=3600")));
    EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentEncoding, GetEncodingStr(enc))));
    EXPECT_TRUE(headers.contains(MakeHttp1HeaderLine(http::ContentType, "application/json")));

    // Verify compression still works
    auto decompressed = test::Decompress(enc, resp.bodyInMemory());
    EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), body);
  }
}

TEST_F(HttpResponseTest, FinalizeHeadersAndBody_NoDirectCompressionNoTrailers_Idempotent) {
  // Test that finalizeForHttp2 can be called multiple times without issues
  // when there's no direct compression and no trailers
  HttpResponse resp = makePrepared(PreparedOptions{});
  resp.directCompressionMode(DirectCompressionMode::Off);  // No compression

  resp.headerAddLine("x-test", "Value");
  resp.body("Hello World", "text/plain");

  // Call finalize multiple times
  FinalizeHeadersAndBody(resp);
  std::string firstHeaders(resp.headersFlatView());

  FinalizeHeadersAndBody(resp);
  std::string secondHeaders(resp.headersFlatView());

  // Should be identical
  EXPECT_EQ(firstHeaders, secondHeaders);
  EXPECT_TRUE(firstHeaders.contains(MakeHttp1HeaderLine("x-test", "Value")));
}

// =============================================================================
// Tests for automatic chunked encoding conversion when trailers are present
// Per RFC 7230 §4.1.2, trailers require chunked transfer encoding
// =============================================================================

TEST_F(HttpResponseTest, FinalizationCombinations) {
  static constexpr std::size_t kMinCapturedBodySz[]{1UL, 4096UL};
  static constexpr std::string_view kConcatenatedGlobalHeaders[]{
      "",
      "server: aeronet\r\n",
      "x-custom: value\r\nx-another: another-value\r\n",
  };

  // This test covers ALL combinations of:
  // - prepared vs non-prepared response
  // - knownAddTrailerAtConstruction true/false
  // - HEAD vs non-HEAD request
  // - keep-alive vs close connection
  // - addTrailerHeader true/false
  // - minCapturedBodySz small/large (inline vs captured body)
  // - various global headers (none, one, multiple)
  for (std::string_view globalHeaders : kConcatenatedGlobalHeaders) {
    PreparedOptions opts;
    for (std::string_view tmp = globalHeaders; !tmp.empty();) {
      const auto crlfPos = tmp.find(http::CRLF);
      ASSERT_NE(crlfPos, std::string_view::npos);
      opts.gh.append(tmp.substr(0, crlfPos));
      tmp.remove_prefix(crlfPos + http::CRLF.size());
    }
    for (bool isPrepared : {true, false}) {
      for (bool head : {true, false}) {
        opts.head = head;
        for (bool keepAlive : {true, false}) {
          for (bool addTrailerHeader : {true, false}) {
            opts.addTrailerHeader = addTrailerHeader;
            for (std::size_t minCapturedBodySz : kMinCapturedBodySz) {
              // Captured body
              std::string_view bodySv = "CapturedData12345";  // 17 bytes
              HttpResponse resp = isPrepared ? makePrepared(opts) : HttpResponse{};
              resp.body(std::string(bodySv));  // 17 bytes = 0x11
              resp.trailerAddLine("x-sig", "sig-value");
              resp.trailerAddLine("x-sig-2", "sig-value-2");

              std::string result =
                  concatenated(std::move(resp), opts.gh, head, keepAlive, minCapturedBodySz, addTrailerHeader);

              std::string exp;
              exp.reserve(result.size());
              exp += "HTTP/1.1 200 \r\n";

              exp += MakeHttp1HeaderLine(http::Date, "Thu, 01 Jan 1970 00:00:00 GMT");
              if (isPrepared) {
                exp += globalHeaders;
              }
              if (addTrailerHeader && head && isPrepared) {
                exp += MakeHttp1HeaderLine(http::Trailer, "x-sig, x-sig-2");
              }
              exp += MakeHttp1HeaderLine(http::ContentType, "text/plain");
              if (addTrailerHeader && (!head || !isPrepared)) {
                exp += MakeHttp1HeaderLine(http::Trailer, "x-sig, x-sig-2");
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
                exp += "x-sig: sig-value\r\n";
                exp += "x-sig-2: sig-value-2\r\n";
                exp += "\r\n";
              }

              ASSERT_EQ(result, exp) << "Failed with keepAlive=" << keepAlive
                                     << " addTrailerHeader=" << addTrailerHeader
                                     << " inlineBody=" << (minCapturedBodySz == 1) << " globalHeaders=["
                                     << globalHeaders << "]" << " head=" << head;

              // Inline body
              resp = isPrepared ? makePrepared(opts) : HttpResponse{};
              resp.body(bodySv).trailerAddLine("x-sig", "sig-value").trailerAddLine("x-sig-2", "sig-value-2");
              result = concatenated(std::move(resp), opts.gh, head, keepAlive, minCapturedBodySz, addTrailerHeader);
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
  resp.trailerAddLine("trailer-one", "value1");
  resp.trailerAddLine("trailer-two", "value2");
  resp.trailerAddLine("trailer-three", "value3");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("4\r\ntest\r\n0\r\n"));
  EXPECT_TRUE(result.contains("trailer-one: value1\r\n"));
  EXPECT_TRUE(result.contains("trailer-two: value2\r\n"));
  EXPECT_TRUE(result.contains("trailer-three: value3\r\n"));
}

TEST_F(HttpResponseTest, HeadLargeBodyDoesNotGrowInlineCapacity) {
  HttpResponse resp = makePrepared(PreparedOptions{.head = true});

  const auto initialCapacity = resp.capacityInlined();
  const std::string bigBody(1UL << 20U, 'x');

  resp.body(bigBody, http::ContentTypeTextPlain);

  EXPECT_EQ(resp.bodyLength(), bigBody.size());
  EXPECT_LT(resp.capacityInlined(), bigBody.size());
  EXPECT_LE(resp.capacityInlined(), initialCapacity + 512U);
}

TEST_F(HttpResponseTest, HeadLargeCapturedBodyDoesNotGrowInlineCapacity) {
  HttpResponse resp = makePrepared(PreparedOptions{.head = true});

  const auto initialCapacity = resp.capacityInlined();
  std::vector<char> bigBody(1UL << 20U, 'y');

  resp.body(std::move(bigBody), http::ContentTypeTextPlain);

  EXPECT_EQ(resp.bodyLength(), 1U << 20U);
  EXPECT_LT(resp.capacityInlined(), 1U << 20U);
  EXPECT_LE(resp.capacityInlined(), initialCapacity + 512U);
}

TEST_F(HttpResponseTest, HeadBodyInlineAppend) {
  for (bool useBytes : {false, true}) {
    for (bool prepared : {true, false}) {
      HttpResponse resp = prepared ? makePrepared(PreparedOptions{.head = true}) : HttpResponse{};

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
      resp.trailerAddLine("x-test", "value");

      std::string result = concatenated(std::move(resp), {}, true);

      EXPECT_EQ(result,
                "HTTP/1.1 200 \r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/custom\r\ncontent-length: "
                "2\r\nconnection: close\r\n\r\n")
          << "failed for prepared=" << prepared;
    }
  }
}

TEST_F(HttpResponseTest, HeadBodyAppend) {
  for (bool prepared : {true, false}) {
    HttpResponse resp = prepared ? makePrepared(PreparedOptions{.head = true}) : HttpResponse{};

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
    // We cannot infer anything for the capacity, because we don't know in advance how many bytes the user will
    // actually append

    resp.trailerAddLine("x-test", "value");

    std::string result = concatenated(std::move(resp), {}, true);

    EXPECT_EQ(result,
              "HTTP/1.1 200 \r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/plain\r\ncontent-length: "
              "2\r\nconnection: close\r\n\r\n")
        << "failed for prepared=" << prepared;
  }
}

TEST_F(HttpResponseTest, HeadBodyInlineSet) {
  for (bool useBytes : {false, true}) {
    for (bool prepared : {true, false}) {
      HttpResponse resp = prepared ? makePrepared(PreparedOptions{.head = true}) : HttpResponse{};

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
                "HTTP/1.1 200 \r\ndate: Thu, 01 Jan 1970 00:00:00 GMT\r\ncontent-type: text/custom\r\ncontent-length: "
                "1\r\nconnection: close\r\n\r\n")
          << "failed for prepared=" << prepared;
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedPreservesOtherHeaders) {
  HttpResponse resp(http::StatusCodeOK);
  resp.header("x-custom", "custom-value");
  resp.body(R"({"key":"value"})", "application/json");
  resp.trailerAddLine("x-hash", "sha256:...");

  const std::string result = concatenated(std::move(resp));

  // Other headers should be preserved
  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine("x-custom", "custom-value")));
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
  resp.trailerAddLine("x-size", "large");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_FALSE(result.contains(http::ContentLength));
  // Should have correct hex length "1234" followed by CRLF and the body data
  EXPECT_TRUE(result.contains("1234\r\n" + largeBody + "\r\n0\r\n")) << "Body should be chunked with hex length";
}

TEST_F(HttpResponseTest, TrailersAutoChunkedEmptyTrailerValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("data");
  resp.trailerAddLine("x-empty", "");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("x-empty: \r\n"));
}

TEST_F(HttpResponseTest, NoTrailersNoChunkedConversion) {
  static constexpr bool kConnection[]{true, false};

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
  static constexpr bool kConnection[]{true, false};

  static constexpr std::size_t kMinCapturedBodySz[]{1UL};

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
  resp.trailerAddLine("x-check", "done");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("4\r\nABCD\r\n0\r\n"));
  EXPECT_TRUE(result.contains("x-check: done\r\n"));
}

TEST_F(HttpResponseTest, TrailersAutoChunkedUniquePtrBody) {
  for (bool head : {false, true}) {
    const char data[] = "Hello";
    auto bodyPtr = std::make_unique<char[]>(sizeof(data));
    std::ranges::copy(data, bodyPtr.get());

    auto resp = makePrepared(PreparedOptions{.head = head});
    resp.body(std::move(bodyPtr), sizeof(data) - 1);
    resp.trailerAddLine("x-final", "yes");

    const std::string result = concatenated(std::move(resp), {}, head);

    EXPECT_EQ(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)), !head);
    if (head) {
      char dataSzBuf[std::numeric_limits<std::size_t>::digits10 + 1];
      const char* pEnd = std::to_chars(dataSzBuf, dataSzBuf + sizeof(dataSzBuf), sizeof(data) - 1).ptr;
      EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::ContentLength, std::string_view(dataSzBuf, pEnd))));
      EXPECT_FALSE(result.contains("5\r\nHello\r\n0\r\n"));
      EXPECT_FALSE(result.contains("x-final: yes\r\n"));
      EXPECT_TRUE(result.ends_with("\r\n\r\n"));
    } else {
      EXPECT_FALSE(result.contains(http::ContentLength));
      EXPECT_TRUE(result.contains("5\r\nHello\r\n0\r\n"));
      EXPECT_TRUE(result.contains("x-final: yes\r\n"));
    }
  }
}

TEST_F(HttpResponseTest, TrailersAutoChunkedBytesSpanBody) {
  // Use bytes span which is handled correctly
  const char data[] = "Hello";
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), 5));
  resp.trailerAddLine("x-final", "yes");

  const std::string result = concatenated(std::move(resp));

  EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)));
  EXPECT_TRUE(result.contains("5\r\nHello\r\n0\r\n"));
  EXPECT_TRUE(result.contains("x-final: yes\r\n"));
}

TEST_F(HttpResponseTest, TrailersAutoChunkedBodySizeEdgeCases) {
  // Test with body sizes that are powers of 16 to verify hex encoding
  for (int sz : {1, 15, 16, 255, 256, 4095, 4096}) {
    const std::string body(static_cast<std::size_t>(sz), 'X');
    HttpResponse resp(http::StatusCodeOK);
    resp.body(body);
    resp.trailerAddLine("x-size", std::to_string(sz));

    const std::string result = concatenated(std::move(resp));

    EXPECT_TRUE(result.contains(MakeHttp1HeaderLine(http::TransferEncoding, http::chunked)))
        << "Failed for size " << sz;
    EXPECT_FALSE(result.contains(http::ContentLength)) << "Failed for size " << sz;
    // Verify the last-chunk marker is present
    EXPECT_TRUE(result.contains("\r\n0\r\n")) << "Missing last-chunk for size " << sz;
  }
}

#ifdef AERONET_ENABLE_HTTP_CLIENT

TEST_F(HttpResponseTest, CloneEmptyMessage) {
  HttpResponse resp(http::StatusCodeNotFound);
  HttpResponse copy = cloneFinalized(resp);
  EXPECT_EQ(copy.status(), http::StatusCodeNotFound);
  EXPECT_EQ(copy.bodyInMemory(), resp.bodyInMemory());
  EXPECT_EQ(copy.headersFlatView(), resp.headersFlatView());
  // Two independent clones serialize to identical bytes.
  EXPECT_EQ(concatenated(cloneFinalized(resp)), concatenated(cloneFinalized(copy)));
}

TEST_F(HttpResponseTest, CloneWithHeadersAndInlineBody) {
  HttpResponse resp(http::StatusCodeOK, "hello world", "text/plain");
  resp.reason("OK");
  resp.header("x-custom", "abc");
  resp.headerAddLine("x-multi", "1");
  resp.headerAddLine("x-multi", "2");

  HttpResponse copy = cloneFinalized(resp);
  EXPECT_EQ(copy.status(), resp.status());
  EXPECT_EQ(copy.reason(), resp.reason());
  EXPECT_EQ(copy.bodyInMemory(), "hello world");
  EXPECT_EQ(copy.headersFlatView(), resp.headersFlatView());
  EXPECT_EQ(copy.headerValueOrEmpty("x-custom"), "abc");

  // The clone is a deep, independent copy: mutating it never touches the original.
  copy.body("changed");
  copy.header("x-custom", "z");
  EXPECT_EQ(resp.bodyInMemory(), "hello world");
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "abc");

  // Serialized bytes of two clones of the untouched original match exactly.
  EXPECT_EQ(concatenated(cloneFinalized(resp)), concatenated(cloneFinalized(resp)));
}

TEST_F(HttpResponseTest, CloneWithCapturedBodyOwnsIndependentCopy) {
  const std::string payload(4096, 'x');
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string(payload));
  ASSERT_TRUE(resp.hasBodyCaptured());

  HttpResponse copy = cloneFinalized(resp);
  EXPECT_TRUE(copy.hasBodyCaptured());
  EXPECT_EQ(copy.bodyInMemory(), resp.bodyInMemory());
  EXPECT_NE(copy.bodyInMemory().data(), resp.bodyInMemory().data());

  resp.body("changed");
  EXPECT_EQ(copy.bodyInMemory(), payload);
}

#endif

}  // namespace aeronet
