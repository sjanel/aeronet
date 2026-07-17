// Unit coverage for HttpRequest: the fluent (lvalue & rvalue) builder API and the getters. No sockets.
#include "aeronet/http-request.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/concatenated-headers.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/temp-file.hpp"

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
#include "aeronet/encoding.hpp"
#include "aeronet/raw-chars.hpp"
#endif

namespace aeronet {

class HttpRequestTest : public ::testing::Test {
 protected:
  HttpRequest makeRequest(http::Method method, std::string_view url) {
    return {method, url, globalHeaders.fullStringWithLastSep(), makeRequestOptions()};
  }

  HttpRequest makeRequest(http::Method method, std::string_view url, std::string_view concatenatedHeaders) {
    return {method, url, concatenatedHeaders, makeRequestOptions()};
  }

  HttpRequest makeRequest(http::Method method, std::string_view url, std::string_view concatenatedHeaders,
                          std::string_view body,
                          std::string_view contentType = http::ContentTypeApplicationOctetStream) {
    return {0U, method, url, concatenatedHeaders, makeRequestOptions(), body, contentType};
  }

  HttpRequest makeRequestWithoutTrailerHeader(http::Method method, std::string_view url) {
    return {method, url, globalHeaders.fullStringWithLastSep(), makeRequestOptions(false)};
  }

  static internal::CompressionState CreateResponseCompressionState(CompressionConfig* config) {
    internal::CompressionState ret{*config};
    ret.pCompressionConfig = config;
    return ret;
  }

  static void finalize(HttpRequest& req) { req.finalize(); }

  static HttpRequest finalize(const HttpRequest& req, internal::HttpClientCodec& clientCodec,
                              const DecompressionConfig& decompressionConfig) {
    return req.finalize(clientCodec, decompressionConfig);
  }

  static bool IsAutomaticDirectCompression(const HttpRequest& req) { return req._opts.isAutomaticDirectCompression(); }

  static bool resolveRedirect(HttpRequest& req, std::string_view location) { return req.resolveRedirect(location); }

  HttpMessage::Options makeRequestOptions(bool addTrailerHeader = true) {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
    HttpMessage::Options opts(compressionState, test::SupportedEncodings().front());
    // Vary header does not make sense for a request, we don't add it.
#else
    HttpMessage::Options opts;
#endif
    if (config.hasProxy()) {
      opts.setHasProxy();
    }
    if (!config.keepAlive) {
      opts.setClose();
    }
    if (addTrailerHeader) {
      opts.addTrailerHeader();
    }
    opts.setHttpRequest();
    opts.setPrepared();
    return opts;
  }

  ConcatenatedHeaders globalHeaders{{"user-agent: aeronet"}};
  HttpClientConfig config{};
  internal::CompressionState compressionState = CreateResponseCompressionState(&config.requestCompression.codec);
};

TEST_F(HttpRequestTest, InvalidUrl) {
  EXPECT_THROW(makeRequest(http::Method::GET, "invalid-url"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http://"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http:///path"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http://:80/path"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http://host:port/path"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http://host:-1/path"), std::invalid_argument);
  EXPECT_THROW(makeRequest(http::Method::GET, "http://host:65536/path"), std::invalid_argument);

  EXPECT_THROW(makeRequest(http::Method::GET, "salut://invalid-host:55555/path", {}, "some body"),
               std::invalid_argument);

  // invalid target
  EXPECT_THROW(makeRequest(http::Method::GET, "http://host/path with space", {}, "some body"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorWithProxy) {
  for (std::string_view scheme : {"http", "https"}) {
    config.withProxy(std::string(scheme) + "://proxy.example.com:8080");

    std::string url = std::string(scheme) + "://example.com/path";
    // the request-target is the absolute-form URL
    auto req = makeRequest(http::Method::PATCH, url);
    EXPECT_EQ(req.method(), http::Method::PATCH);
    EXPECT_EQ(req.scheme(), scheme);
    EXPECT_EQ(req.host(), "example.com");
    if (scheme == "http") {
      EXPECT_EQ(req.target(), "http://example.com:80/path");
    } else {
      EXPECT_EQ(req.target(), "/path");
    }
    EXPECT_EQ(req.port(), scheme == "http" ? 80 : 443);
    EXPECT_EQ(req.isTlsRequest(), scheme == "https");
    EXPECT_EQ(req.originKey(), scheme == "https" ? "https://example.com:443" : "http://example.com:80");

    req = makeRequest(http::Method::OPTIONS, url, {}, "payload", "text/plain");
    EXPECT_EQ(req.method(), http::Method::OPTIONS);
    EXPECT_EQ(req.scheme(), scheme);
    EXPECT_EQ(req.host(), "example.com");
    if (scheme == "http") {
      EXPECT_EQ(req.target(), "http://example.com:80/path");
    } else {
      EXPECT_EQ(req.target(), "/path");
    }
    EXPECT_EQ(req.port(), scheme == "http" ? 80 : 443);
    EXPECT_EQ(req.isTlsRequest(), scheme == "https");
    EXPECT_EQ(req.originKey(), scheme == "https" ? "https://example.com:443" : "http://example.com:80");
    EXPECT_EQ(req.headerValueOrEmpty(http::ContentType), "text/plain");
    EXPECT_EQ(req.bodyInMemory(), "payload");
  }
}

TEST_F(HttpRequestTest, ConstructorRejectsEmptyTarget) {
  EXPECT_THROW(makeRequest(http::Method::GET, ""), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorRejectsSpaceInTarget) {
  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/hello world"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorAcceptsPercentEncodedTarget) {
  auto req = makeRequest(http::Method::GET, "https://example.com/hello%20world%2Ftest");

  EXPECT_EQ(req.target(), "/hello%20world%2Ftest");
}

TEST_F(HttpRequestTest, ConstructorRejectsTrailingPercent) {
  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/test%"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorRejectsIncompletePercentEncoding) {
  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/test%2"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorRejectsInvalidPercentEncoding) {
  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/test%2G"), std::invalid_argument);

  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/test%XZ"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorRejectsDeleteControlCharacter) {
  auto req = makeRequest(http::Method::GET, "https://example.com/test");
  std::string target = "/test";
  target.push_back('\x7F');

  EXPECT_THROW(req.target(""), std::invalid_argument);
  EXPECT_THROW(req.target(target), std::invalid_argument);
  EXPECT_THROW(req.target("/test%!!"), std::invalid_argument);
}

TEST_F(HttpRequestTest, ConstructorRejectsTooLongTarget) {
  std::string target(1UL << 24, 'a');

  EXPECT_THROW(makeRequest(http::Method::GET, "https://example.com/" + target), std::invalid_argument);
}

TEST_F(HttpRequestTest, MethodAndUrlConstructor) {
  for (uint32_t methodPos = 0; methodPos < http::kNbMethods; ++methodPos) {
    http::Method method = static_cast<http::Method>(1U << methodPos);
    for (std::string_view scheme : {"http", "https"}) {
      for (std::string_view host : {"example.com", "example.com:8080"}) {
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator)
        uint16_t port = host.ends_with(":8080") ? 8080 : (scheme == "http" ? 80 : 443);
        for (std::string_view target : {"/x", "/y/z?q=1"}) {
          std::string url = std::string(scheme) + "://" + std::string(host) + std::string(target);

          auto req = makeRequest(method, url);
          EXPECT_EQ(req.method(), method);
          EXPECT_EQ(req.scheme(), scheme);
          EXPECT_EQ(req.host(), host.substr(0, host.find(':')));
          EXPECT_EQ(req.target(), target);
          EXPECT_EQ(req.port(), port);
          EXPECT_EQ(req.isTlsRequest(), scheme == "https");
          EXPECT_EQ(req.originKey(),
                    std::string(scheme) + std::string("://") + std::string(req.host()) + ":" + std::to_string(port));
          EXPECT_EQ(req.statusLineSize(), http::MethodToStr(method).size() + 1U + target.size() + 1U +
                                              http::HTTP10Sv.size() + http::CRLF.size());
          EXPECT_EQ(req.statusLineLength(), req.statusLineSize());
          EXPECT_EQ(req.file(), nullptr);
        }
      }
    }
  }
}

TEST_F(HttpRequestTest, EmptyConcatenatedHeaders) {
  auto req = makeRequest(http::Method::GET, "http://host/path", {}, "some body");
  EXPECT_EQ(req.headerValueOrEmpty("user-agent"), "");
  EXPECT_EQ(req.bodyInMemory(), "some body");

  req = makeRequest(http::Method::GET, "http://host/path", {}, "payload", "text/plain");
  EXPECT_EQ(req.headerValueOrEmpty("user-agent"), "");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentType), "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "payload");

  req = makeRequest(http::Method::GET, "http://host/path", {});
  EXPECT_EQ(req.headerValueOrEmpty("user-agent"), "");
  EXPECT_EQ(req.bodyInMemory(), "");
}

TEST_F(HttpRequestTest, LvalueFluentSetters) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.method(http::Method::PUT).header("X-A", "1").headerAddLine("X-B", "2").body("payload");
  EXPECT_EQ(req.method(), http::Method::PUT);
  EXPECT_EQ(req.host(), "h");
  EXPECT_EQ(req.bodyInMemory(), "payload");
  EXPECT_EQ(req.directCompressionMode(), DirectCompressionMode::Auto);
  EXPECT_EQ(req.target(), "/p");
  req.body(std::string_view{});
  req.contentEncoding("custom-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "custom-codec");
  req.headerAppendValue(http::ContentEncoding, "another-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "custom-codec, another-codec");
  req.headerRemoveValue(http::ContentEncoding, "custom-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "another-codec");
  req.headerRemoveLine(http::ContentEncoding);
  EXPECT_FALSE(req.hasHeader(http::ContentEncoding));
  EXPECT_EQ(req.target(), "/p");
}

TEST_F(HttpRequestTest, RvalueFluentSetters) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req = std::move(req).method(http::Method::PUT).header("X-A", "1").headerAddLine("X-B", "2").body("payload");
  EXPECT_EQ(req.method(), http::Method::PUT);
  EXPECT_EQ(req.host(), "h");
  EXPECT_EQ(req.bodyInMemory(), "payload");
  req = std::move(req).directCompressionMode(DirectCompressionMode::On).target("/q");
  EXPECT_EQ(req.target(), "/q");
  EXPECT_EQ(req.directCompressionMode(), DirectCompressionMode::On);
  req = std::move(req).body(std::string_view{}).contentEncoding("custom-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "custom-codec");
  req = std::move(req).headerAppendValue(http::ContentEncoding, "another-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "custom-codec, another-codec");
  req = std::move(req).headerRemoveValue(http::ContentEncoding, "custom-codec");
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), "another-codec");
  req = std::move(req).headerRemoveLine(http::ContentEncoding);
  EXPECT_FALSE(req.hasHeader(http::ContentEncoding));
}

TEST_F(HttpRequestTest, BodyWithContentType) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.body("data", "application/json");
  EXPECT_EQ(req.bodyInMemory(), "data");
}

TEST_F(HttpRequestTest, BodyFromBytesSpan) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  const std::byte abcBytes[]{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  req.body(std::span<const std::byte>(abcBytes));
  EXPECT_EQ(req.bodyInMemory(), "abc");

  const std::byte defBytes[]{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};

  req = std::move(req).body(std::span<const std::byte>(defBytes), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "def");
}

TEST_F(HttpRequestTest, BodyFromConstCharStar) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.body("hello", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "hello");

  req = std::move(req).body("world", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "world");

  // nullptr removes the body (no-op if the body is already empty).
  req = std::move(req).body(nullptr, "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "");
}

TEST_F(HttpRequestTest, BodyFromStdString) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.body(std::string("hello"), "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "hello");

  req = makeRequest(http::Method::POST, "http://h/p").body(std::string("world"), "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "world");
}

TEST_F(HttpRequestTest, BodyFromVectorChar) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  std::vector<char> vec{'a', 'b', 'c'};
  req.body(std::move(vec), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abc");

  std::vector<char> vec2{'x', 'y', 'z'};
  req = makeRequest(http::Method::POST, "http://h/p").body(std::move(vec2), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "xyz");
}

TEST_F(HttpRequestTest, BodyFromVectorByte) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  std::vector<std::byte> vec{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  req.body(std::move(vec), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abc");

  std::vector<std::byte> vec2{std::byte{'x'}, std::byte{'y'}, std::byte{'z'}};
  req = makeRequest(http::Method::POST, "http://h/p").body(std::move(vec2), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "xyz");
}

TEST_F(HttpRequestTest, BodyFromUniquePtrChar) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  auto body = std::make_unique<char[]>(4);
  std::memcpy(body.get(), "abcd", 4);
  req.body(std::move(body), 4, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abcd");

  auto body2 = std::make_unique<char[]>(3);
  std::memcpy(body2.get(), "xyz", 3);
  req = std::move(req).body(std::move(body2), 3, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "xyz");
}

TEST_F(HttpRequestTest, BodyFromUniquePtrByte) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  auto body = std::make_unique<std::byte[]>(4);
  std::memcpy(body.get(), "abcd", 4);
  req.body(std::move(body), 4, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abcd");

  auto body2 = std::make_unique<std::byte[]>(3);
  std::memcpy(body2.get(), "xyz", 3);
  req = std::move(req).body(std::move(body2), 3, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "xyz");
}

TEST_F(HttpRequestTest, BodyStaticFromStringView) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.bodyStatic("static-data", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "static-data");

  req = std::move(req).bodyStatic("more-static-data", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "more-static-data");
}

TEST_F(HttpRequestTest, BodyStaticFromSpanBytes) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  static const std::byte abcBytes[]{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  req.bodyStatic(std::span<const std::byte>(abcBytes), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abc");

  static const std::byte defBytes[]{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
  req = std::move(req).bodyStatic(std::span<const std::byte>(defBytes), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "def");
}

TEST_F(HttpRequestTest, BodyAppendFromStringView) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.bodyAppend(std::string_view("data1"), "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "data1");

  req = std::move(req).bodyAppend(std::string_view("data2"), "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "data1data2");
}

TEST_F(HttpRequestTest, BodyAppendFromSpanBytes) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  static const std::byte abcBytes[]{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  req.bodyAppend(std::span<const std::byte>(abcBytes), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abc");

  static const std::byte defBytes[]{std::byte{'d'}, std::byte{'e'}, std::byte{'f'}};
  req = std::move(req).bodyAppend(std::span<const std::byte>(defBytes), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "abcdef");
}

TEST_F(HttpRequestTest, BodyAppendFromConstCharStar) {
  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.bodyAppend("data1", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "data1");

  req = std::move(req).bodyAppend("data2", "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "data1data2");

  // nullptr is treated as an empty chunk to append (no-op).
  req = std::move(req).bodyAppend(nullptr, "text/plain");
  EXPECT_EQ(req.bodyInMemory(), "data1data2");
}

TEST_F(HttpRequestTest, FileLValue) {
  static constexpr std::string_view kPayload = "small";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.file(std::move(file), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "");

  const auto* pFile = req.file();
  ASSERT_NE(pFile, nullptr);
  EXPECT_EQ(pFile->size(), kPayload.size());
}

TEST_F(HttpRequestTest, FileRValue) {
  static constexpr std::string_view kPayload = "small";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  auto req = makeRequest(http::Method::POST, "http://h/p").file(std::move(file), "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "");

  const auto* pFile = req.file();
  ASSERT_NE(pFile, nullptr);
  EXPECT_EQ(pFile->size(), kPayload.size());
}

TEST_F(HttpRequestTest, FileWithOffsetLValue) {
  static constexpr std::string_view kPayload = "small";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  auto req = makeRequest(http::Method::POST, "http://h/p");
  req.file(std::move(file), 2, 2, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "");

  const auto* pFile = req.file();
  ASSERT_NE(pFile, nullptr);
  std::byte buf[2];
  EXPECT_EQ(pFile->readAt(buf, 2), 2U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf), 2), "al");
}

TEST_F(HttpRequestTest, FileWithOffsetRValue) {
  static constexpr std::string_view kPayload = "small";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);

  auto req = makeRequest(http::Method::POST, "http://h/p").file(std::move(file), 2, 2, "application/octet-stream");
  EXPECT_EQ(req.bodyInMemory(), "");

  const auto* pFile = req.file();
  ASSERT_NE(pFile, nullptr);
  std::byte buf[2];
  EXPECT_EQ(pFile->readAt(buf, 2), 2U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buf), 2), "al");
}

TEST_F(HttpRequestTest, TrailerAddLine) {
  auto req = makeRequest(http::Method::POST, "http://h/p").body("payload");
  req.trailerAddLine("X-Trailer-1", "v1").trailerAddLine("X-Trailer-2", "v2");
  EXPECT_EQ(req.trailerValueOrEmpty("X-Trailer-1"), "v1");
  EXPECT_EQ(req.trailerValueOrEmpty("X-Trailer-2"), "v2");

  req = makeRequest(http::Method::POST, "http://h/p").body("payload").trailerAddLine("X-Trailer-3", "v3");
  EXPECT_EQ(req.trailerValueOrEmpty("X-Trailer-3"), "v3");
}

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)

TEST_F(HttpRequestTest, AutomaticDirectCompressionHonorsThresholdAndMode) {
  const Encoding encoding = test::SupportedEncodings().front();
  config.requestCompression.codec.minBytes = 2048;

  auto smallReq = makeRequest(http::Method::POST, "http://h/small").body(std::string(1024, 'S'));
  EXPECT_FALSE(IsAutomaticDirectCompression(smallReq));
  EXPECT_EQ(smallReq.bodyInMemory(), std::string(1024, 'S'));
  EXPECT_FALSE(smallReq.hasHeader(http::ContentEncoding));

  const std::string largePayload(4096, 'L');
  auto disabled = makeRequest(http::Method::POST, "http://h/disabled");
  disabled.directCompressionMode(DirectCompressionMode::Off).body(largePayload);
  EXPECT_FALSE(IsAutomaticDirectCompression(disabled));
  EXPECT_EQ(disabled.bodyInMemory(), largePayload);
  EXPECT_FALSE(disabled.hasHeader(http::ContentEncoding));

  const std::string forcedPayload(256, 'F');
  auto forced = makeRequest(http::Method::POST, "http://h/forced");
  forced.directCompressionMode(DirectCompressionMode::On).body(forcedPayload);
  EXPECT_TRUE(IsAutomaticDirectCompression(forced));
  EXPECT_EQ(forced.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(encoding));

  finalize(forced);

  const RawChars decoded = test::Decompress(encoding, forced.bodyInMemory());
  EXPECT_EQ(std::string_view(decoded.data(), decoded.size()), forcedPayload);
  EXPECT_EQ(forced.headerValueOrEmpty(http::ContentLength), std::to_string(forced.bodyInMemory().size()));
}

TEST_F(HttpRequestTest, InPlaceFinalizeCompletesAutomaticCompression) {
  const Encoding encoding = test::SupportedEncodings().front();
  config.requestCompression.codec.minBytes = 1;
  const std::string firstChunk(4096, 'A');
  const std::string secondChunk(2048, 'B');
  const std::string expected = firstChunk + secondChunk;
  auto req = makeRequest(http::Method::POST, "http://h/compressed");
  req.body(firstChunk).bodyAppend(secondChunk);
  ASSERT_TRUE(IsAutomaticDirectCompression(req));

  finalize(req);

  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(encoding));
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentLength), std::to_string(req.bodyInMemory().size()));
  EXPECT_LT(req.bodyInMemory().size(), expected.size());
  const RawChars decoded = test::Decompress(encoding, req.bodyInMemory());
  EXPECT_EQ(std::string_view(decoded.data(), decoded.size()), expected);
}

TEST_F(HttpRequestTest, ConstFinalizePreservesCompressionStateForReuse) {
  const Encoding encoding = test::SupportedEncodings().front();
  config.requestCompression.codec.minBytes = 1;
  const std::string firstChunk(4096, 'A');
  const std::string secondChunk(2048, 'B');
  auto req = makeRequest(http::Method::POST, "http://h/reusable").body(firstChunk);
  ASSERT_TRUE(IsAutomaticDirectCompression(req));
  const std::string originalCompressedPrefix(req.bodyInMemory());
  internal::HttpClientCodec clientCodec(config.requestCompression.codec);

  const auto firstFinalized = finalize(std::as_const(req), clientCodec, config.decompression);

  EXPECT_EQ(req.bodyInMemory(), originalCompressedPrefix);
  EXPECT_EQ(firstFinalized.headerValueOrEmpty(http::ContentLength),
            std::to_string(firstFinalized.bodyInMemory().size()));
  const RawChars firstDecoded = test::Decompress(encoding, firstFinalized.bodyInMemory());
  EXPECT_EQ(std::string_view(firstDecoded.data(), firstDecoded.size()), firstChunk);

  req.bodyAppend(secondChunk);
  const auto secondFinalized = finalize(std::as_const(req), clientCodec, config.decompression);

  const RawChars secondDecoded = test::Decompress(encoding, secondFinalized.bodyInMemory());
  EXPECT_EQ(std::string_view(secondDecoded.data(), secondDecoded.size()), firstChunk + secondChunk);
  EXPECT_EQ(secondFinalized.headerValueOrEmpty(http::ContentLength),
            std::to_string(secondFinalized.bodyInMemory().size()));
}

TEST_F(HttpRequestTest, AutomaticDirectCompressionWithTrailersIsFinalizedOnce) {
  const Encoding encoding = test::SupportedEncodings().front();
  config.requestCompression.codec.minBytes = 1;
  const std::string largePayload(1024UL * 1024, 'x');
  auto req = makeRequest(http::Method::POST, "http://h/p").body(largePayload);
  ASSERT_TRUE(IsAutomaticDirectCompression(req));

  req.trailerAddLine("X-Trailer-1", "v1").trailerAddLine("X-Trailer-2", "v2");
  const std::string compressedBody(req.bodyInMemory());
  EXPECT_LT(compressedBody.size(), largePayload.size());
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(encoding));
  EXPECT_EQ(req.headerValueOrEmpty(http::ContentLength), std::to_string(compressedBody.size()));
  const RawChars decoded = test::Decompress(encoding, compressedBody);
  EXPECT_EQ(std::string_view(decoded.data(), decoded.size()), largePayload);

  internal::HttpClientCodec clientCodec(config.requestCompression.codec);
  const auto finalizedCopy = finalize(std::as_const(req), clientCodec, config.decompression);
  EXPECT_EQ(finalizedCopy.bodyInMemory(), compressedBody);
  EXPECT_EQ(req.bodyInMemory(), compressedBody);

  finalize(req);

  EXPECT_EQ(req.bodyInMemory(), compressedBody);
  EXPECT_EQ(req.trailerValueOrEmpty("X-Trailer-1"), "v1");
  EXPECT_EQ(req.trailerValueOrEmpty("X-Trailer-2"), "v2");
}

#endif

TEST_F(HttpRequestTest, ChunkedRequestSerializationIncludesAdvertisedTrailers) {
  auto req = makeRequest(http::Method::POST, "http://h/p").body("payload");
  req.trailerAddLine("X-First", "one").trailerAddLine("X-Second", "two");
  RawChars out;

  req.writeChunkedRequestForHttp11(out);

  const std::string_view wire(out.data(), out.size());
  EXPECT_TRUE(wire.starts_with("POST /p HTTP/1.1\r\n"));
  EXPECT_TRUE(wire.contains(std::string(http::TransferEncoding) + std::string(http::HeaderSep) +
                            std::string(http::chunked) + std::string(http::CRLF)));
  EXPECT_FALSE(wire.contains(std::string(http::ContentLength) + std::string(http::HeaderSep)));
  EXPECT_TRUE(wire.contains(std::string(http::Trailer) + std::string(http::HeaderSep) + "X-First, X-Second\r\n"));
  EXPECT_TRUE(wire.ends_with("7\r\npayload\r\n0\r\nX-First: one\r\nX-Second: two\r\n\r\n"));
}

TEST_F(HttpRequestTest, ChunkedRequestSerializationCanOmitTrailerHeader) {
  auto req = makeRequestWithoutTrailerHeader(http::Method::POST, "http://h/p").body("payload");
  req.trailerAddLine("X-Only", "value");
  RawChars out;

  req.writeChunkedRequestForHttp11(out);

  const std::string_view wire(out.data(), out.size());
  EXPECT_TRUE(wire.contains(std::string(http::TransferEncoding) + std::string(http::HeaderSep) +
                            std::string(http::chunked) + std::string(http::CRLF)));
  EXPECT_FALSE(wire.contains(std::string(http::Trailer) + std::string(http::HeaderSep)));
  EXPECT_TRUE(wire.ends_with("7\r\npayload\r\n0\r\nX-Only: value\r\n\r\n"));
}

TEST_F(HttpRequestTest, VeryLongUrlIsNotTruncated) {
  // The URL lives in HttpMessage's reason slot, whose public setter truncates reason phrases longer
  // than 1024 bytes. A request target (e.g. a large query string) must be stored intact.
  std::string_view host = "http://example.com";
  std::string longUrl(host);
  longUrl += "/v1/orderbook?markets=";

  while (longUrl.size() <= 8000) {
    longUrl.append("KRW-BTC,");
  }
  std::string_view longUrlSv = longUrl;
  std::string_view expectedTarget = longUrlSv.substr(host.size());  // "/v1/orderbook?markets=..."

  auto req = makeRequest(http::Method::GET, longUrl);
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.target(), expectedTarget);

  auto req2 = makeRequest(http::Method::GET, longUrl);
  EXPECT_EQ(req2.host(), "example.com");
  EXPECT_EQ(req2.target(), expectedTarget);
}

TEST_F(HttpRequestTest, RvalueOverloadsCompose) {
  // Exercise every rvalue-qualified overload in a single chained temporary.
  auto req = makeRequest(http::Method::PATCH, "http://h/r");
  req = std::move(req).method(http::Method::PATCH).header("X-H", "v").headerAddLine("X-Add", "w").body("rbody");

  EXPECT_EQ(req.method(), http::Method::PATCH);
  EXPECT_EQ(req.host(), "h");
  EXPECT_EQ(req.target(), "/r");
  EXPECT_EQ(req.bodyInMemory(), "rbody");

  req = std::move(req).method(http::Method::POST).body("json", "application/json");
  EXPECT_EQ(req.bodyInMemory(), "json");
}

TEST_F(HttpRequestTest, PortValue) {
  auto req = makeRequest(http::Method::GET, "http://example.com:8080/x");
  EXPECT_EQ(req.port(), 8080);

  req = makeRequest(http::Method::GET, "https://example.com/x");
  EXPECT_EQ(req.port(), 443);

  req = makeRequest(http::Method::GET, "http://example.com:45678/x");
  EXPECT_EQ(req.port(), 45678);

  req = makeRequest(http::Method::GET, "http://example.com:3/x");
  EXPECT_EQ(req.port(), 3);
}

TEST_F(HttpRequestTest, OriginKey) {
  auto req = makeRequest(http::Method::GET, "http://example.com:8080/x");

  EXPECT_EQ(req.host(), "example.com");

  EXPECT_EQ(req.headerValueOrEmpty(http::Host), "example.com:8080");
  EXPECT_EQ(req.originKey(), "http://example.com:8080");
  EXPECT_EQ(req.headerValueOrEmpty(http::Host), "example.com:8080");
}

TEST_F(HttpRequestTest, ResolveRedirectAbsoluteUrlNonTls) {
  for (bool withProxy : {false, true}) {
    if (withProxy) {
      config.withProxy("http://proxy.example.com:8080");
    }

    auto req = makeRequest(http::Method::GET, "http://example.com/x");
    auto res = HttpRequestTest::resolveRedirect(req, "http://other.com/y");

    EXPECT_TRUE(res);

    EXPECT_EQ(req.scheme(), "http");
    EXPECT_EQ(req.host(), "other.com");
    EXPECT_EQ(req.port(), 80);
    EXPECT_EQ(req.target(), withProxy ? "http://other.com:80/y" : "/y");
  }
}

TEST_F(HttpRequestTest, ResolveRedirectAbsoluteUrlTls) {
  for (bool withProxy : {false, true}) {
    if (withProxy) {
      config.withProxy("http://proxy.example.com:8080");
    }

    auto req = makeRequest(http::Method::GET, "https://example.com/x");
    auto res = HttpRequestTest::resolveRedirect(req, "https://other.com/y");

    EXPECT_TRUE(res);

    EXPECT_EQ(req.scheme(), "https");
    EXPECT_EQ(req.host(), "other.com");
    EXPECT_EQ(req.port(), 443);
    EXPECT_EQ(req.target(), "/y");
  }
}

TEST_F(HttpRequestTest, ResolveRedirectNetworkRelativeNonTls) {
  auto req = makeRequest(http::Method::GET, "http://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "//other.com/y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "http");
  EXPECT_EQ(req.host(), "other.com");
  EXPECT_EQ(req.port(), 80);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectNetworkRelativeInvalid) {
  auto req = makeRequest(http::Method::GET, "http://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "//");

  EXPECT_FALSE(res);
}

TEST_F(HttpRequestTest, ResolveRedirectNetworkRelativeTls) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "//other.com/y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "other.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectAbsolutePathNonTls) {
  auto req = makeRequest(http::Method::GET, "http://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "/y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "http");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 80);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectAbsolutePathTls) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "/y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectFragmentShouldBeStripped) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "/y#fragment");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativePathNonTls) {
  auto req = makeRequest(http::Method::GET, "http://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "http");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 80);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativePathTls) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "y");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/y");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativePathReplacesExistingQuery) {
  auto req = makeRequest(http::Method::GET, "https://example.com/dir/old?q=1");
  auto res = HttpRequestTest::resolveRedirect(req, "next");

  EXPECT_TRUE(res);
  EXPECT_EQ(req.target(), "/dir/next");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativePathFromAsteriskTarget) {
  auto req = makeRequest(http::Method::OPTIONS, "https://example.com/").target("*");
  auto res = HttpRequestTest::resolveRedirect(req, "next");

  EXPECT_TRUE(res);
  EXPECT_EQ(req.target(), "*next");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativeWithQuestionMarkEmptyPath) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "?q=1");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/x?q=1");
}

TEST_F(HttpRequestTest, ResolveRedirectRelativeWithQuestionMarkNonEmptyPath) {
  auto req = makeRequest(http::Method::GET, "https://example.com/x");
  auto res = HttpRequestTest::resolveRedirect(req, "y?q=1");

  EXPECT_TRUE(res);

  EXPECT_EQ(req.scheme(), "https");
  EXPECT_EQ(req.host(), "example.com");
  EXPECT_EQ(req.port(), 443);
  EXPECT_EQ(req.target(), "/y?q=1");
}

}  // namespace aeronet
