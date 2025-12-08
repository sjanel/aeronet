#include "aeronet/http-response.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/file.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

class HttpResponseTest : public ::testing::Test {
 protected:
  static constexpr SysTimePoint tp{};
  static constexpr bool keepAlive = false;
  static constexpr bool isHeadMethod = false;
  static constexpr std::size_t minCapturedBodySize = 4096;

  static HttpResponse::PreparedResponse finalizePrepared(HttpResponse&& resp, bool head = isHeadMethod,
                                                         bool keepAliveFlag = keepAlive) {
    return finalizePrepared(std::move(resp), {}, head, keepAliveFlag);
  }

  static HttpResponse::PreparedResponse finalizePrepared(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders,
                                                         bool head = isHeadMethod, bool keepAliveFlag = keepAlive) {
    return resp.finalizeAndStealData(http::HTTP_1_1, tp, !keepAliveFlag, globalHeaders, head, minCapturedBodySize);
  }

  static HttpResponseData finalize(HttpResponse&& resp) {
    auto prepared = finalizePrepared(std::move(resp));
    EXPECT_EQ(prepared.fileLength, 0U);
    return std::move(prepared.data);
  }

  static HttpResponseData finalize(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders,
                                   bool head = isHeadMethod, bool keepAliveFlag = keepAlive) {
    auto prepared = finalizePrepared(std::move(resp), globalHeaders, head, keepAliveFlag);
    EXPECT_EQ(prepared.fileLength, 0U);
    return std::move(prepared.data);
  }

  static const File* file(const HttpResponse::PreparedResponse& prepared) {
    return prepared.file ? &prepared.file : nullptr;
  }

  static std::string concatenated(HttpResponse&& resp) {
    HttpResponseData httpResponseData = finalize(std::move(resp));
    std::string out(httpResponseData.firstBuffer());
    out.append(httpResponseData.secondBuffer());
    return out;
  }

  static std::string concatenated(HttpResponse&& resp, const ConcatenatedHeaders& globalHeaders,
                                  bool head = isHeadMethod, bool keepAliveFlag = keepAlive) {
    HttpResponseData httpResponseData = finalize(std::move(resp), globalHeaders, head, keepAliveFlag);
    std::string out(httpResponseData.firstBuffer());
    out.append(httpResponseData.secondBuffer());
    return out;
  }
};

TEST_F(HttpResponseTest, StatusFromRvalue) {
  auto resp = HttpResponse(http::StatusCodeOK).status(404);
  EXPECT_EQ(resp.status(), 404);
}

TEST_F(HttpResponseTest, BodyFromSpanBytesLValue) {
  std::vector<std::byte> bodyBytes = {std::byte{'H'}, std::byte{'e'}, std::byte{'l'}, std::byte{'l'}, std::byte{'o'}};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::span<const std::byte>(bodyBytes));
  EXPECT_EQ(resp.body(), "Hello");
}

namespace {
constexpr auto kAppendNothing = []([[maybe_unused]] std::byte* buf) { return 0U; };

constexpr auto kAppendSpace = [](std::byte* buf) {
  *buf = std::byte{' '};
  return 1U;
};

constexpr auto kAppendSomeData = [](std::byte* buf) {
  static constexpr std::string_view kData = "some data";
  std::memcpy(buf, kData.data(), kData.size());
  return kData.size();
};

constexpr auto kAppendSomeOtherData = [](std::byte* buf) {
  static constexpr std::string_view kData = "some other data";
  std::memcpy(buf, kData.data(), kData.size());
  return kData.size();
};
}  // namespace

TEST_F(HttpResponseTest, AppendBodyFromEmpty) {
  HttpResponse resp(http::StatusCodeOK);
  resp.appendBody(16U, kAppendSomeData);
  EXPECT_EQ(resp.body(), "some data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/octet-stream");
}

TEST_F(HttpResponseTest, AppendBodyEmptyShouldDoNothing) {
  HttpResponse resp(http::StatusCodeOK);
  resp.appendBody(0U, kAppendNothing);
  resp.appendBody(2U, kAppendNothing);
  EXPECT_EQ(resp.body(), "");
  EXPECT_EQ(resp.headerValue(http::ContentType), std::nullopt);
}

TEST_F(HttpResponseTest, AppendBodyAfterTrailersShouldThrow) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.addTrailer("X-Trailer", "value");
  EXPECT_THROW(resp.appendBody(16U, kAppendSomeData), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyFromCapturedPayloadShouldEraseCapturedPayload) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"initial body "});
  resp.appendBody(16U, kAppendSomeData);
  EXPECT_EQ(resp.body(), "some data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/octet-stream");
}

TEST_F(HttpResponseTest, AppendBodyFromCapturedPayloadShouldEraseCapturedPayloadAndResetContentType) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"initial body "});
  resp.appendBody(16U, kAppendSomeData, "text/custom");
  resp.appendBody(0U, kAppendNothing);
  EXPECT_EQ(resp.body(), "some data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
}

TEST_F(HttpResponseTest, AppendBodyFromNonEmpty) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial body ");
  resp.appendBody(16U, kAppendSomeData, "text/custom");
  EXPECT_EQ(resp.body(), "initial body some data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/custom");
}

TEST_F(HttpResponseTest, SeveralAppendBody) {
  HttpResponse resp(http::StatusCodeOK);
  resp.appendBody(16U, kAppendSomeData, "some content type");
  resp.appendBody(1U, kAppendSpace, "some content type 2");
  resp.appendBody(16U, kAppendSomeOtherData, "text/my-content-type");
  EXPECT_EQ(resp.body(), "some data some other data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/my-content-type");
}

TEST_F(HttpResponseTest, AppendBodyRValue) {
  auto resp = HttpResponse{}.appendBody(16U, kAppendSomeData);
  EXPECT_EQ(resp.body(), "some data");
}

TEST_F(HttpResponseTest, AppendBodyRValueChaining) {
  auto resp = HttpResponse{}
                  .body(std::string{"initial body that should be erased"})
                  .appendBody(16U, kAppendSomeData)
                  .appendBody(1U, kAppendSpace)
                  .appendBody(16U, kAppendSomeOtherData, "text/my-content-type");
  EXPECT_EQ(resp.body(), "some data some other data");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/my-content-type");
}

TEST_F(HttpResponseTest, AppendBodyFromStringAfterTrailersIsLogicError) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("some body");
  resp.addTrailer("X-Trailer", "value");
  EXPECT_THROW(resp.appendBody("additional body"), std::logic_error);
}

TEST_F(HttpResponseTest, AppendBodyFromStringViewAndCStrAndSpan) {
  // string_view
  HttpResponse r1(http::StatusCodeOK);
  r1.appendBody(std::string_view("hello"));
  EXPECT_EQ(r1.body(), "hello");

  // c-string nullptr should be treated as empty (no change)
  HttpResponse r2(http::StatusCodeOK);
  r2.body("orig");
  r2.appendBody(static_cast<const char*>(nullptr));
  EXPECT_EQ(r2.body(), "orig");

  // c-string non-null
  HttpResponse r3(http::StatusCodeOK);
  r3.appendBody("abc", "text/x-test");
  EXPECT_EQ(r3.body(), "abc");
  EXPECT_EQ(r3.headerValue(http::ContentType), "text/x-test");

  // span of bytes
  const std::vector<std::byte> vec = {std::byte{'X'}, std::byte{'Y'}};
  HttpResponse r4(http::StatusCodeOK);
  r4.appendBody(std::span<const std::byte>(vec));
  r4.appendBody(std::span<const std::byte>{}, "text/another");
  r4.appendBody(std::span<const std::byte>{vec}, "text/another2");
  EXPECT_EQ(r4.body(), "XYXY");
  EXPECT_EQ(r4.headerValue(http::ContentType), "text/another2");
}

TEST_F(HttpResponseTest, AppendBodyRvalueSpanBytesDefaultContentType) {
  const std::vector<std::byte> vec = {std::byte{'A'}, std::byte{'B'}, std::byte{'C'}};
  auto resp = HttpResponse(http::StatusCodeOK).appendBody(std::span<const std::byte>(vec));
  EXPECT_EQ(resp.body(), "ABC");
  EXPECT_EQ(resp.headerValue(http::ContentType), "application/octet-stream");
}

TEST_F(HttpResponseTest, AppendBodyRvalueSpanBytesContentType) {
  const std::vector<std::byte> vec = {std::byte{'A'}, std::byte{'B'}, std::byte{'C'}};
  auto resp = HttpResponse(http::StatusCodeOK)
                  .appendBody(std::span<const std::byte>(vec))
                  .appendBody(std::span<const std::byte>(vec), "text/type")
                  .appendBody(std::span<const std::byte>{}, "some/type")
                  .appendBody(std::span<const std::byte>{});
  EXPECT_EQ(resp.body(), "ABCABC");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/type");
}

TEST_F(HttpResponseTest, AppendBodyShouldOverrideCapturedPayloadContentType) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string{"captured body"}, "text/captured");
  resp.appendBody(std::string_view("appended body"));
  EXPECT_EQ(resp.body(), "appended body");
  EXPECT_EQ(resp.headerValue(http::ContentType), http::ContentTypeTextPlain);
}

TEST_F(HttpResponseTest, AppendBodyMultipleFlavorsAndRvalueChaining) {
  // start with a body
  HttpResponse resp(http::StatusCodeOK);
  resp.body("start ");

  // append with string_view
  resp.appendBody(std::string_view("middle "));

  // append with span
  const std::vector<std::byte> tail = {std::byte{'t'}, std::byte{'e'}, std::byte{'r'}};
  resp.appendBody(std::span<const std::byte>(tail));

  EXPECT_EQ(resp.body(), "start middle ter");

  // rvalue chaining
  auto chained =
      HttpResponse(http::StatusCodeOK).appendBody(std::string_view("one")).appendBody(std::string_view("two"));
  EXPECT_EQ(chained.body(), "onetwo");
}

TEST_F(HttpResponseTest, AppendBodyWriterZeroWritesNoHeader) {
  HttpResponse resp(http::StatusCodeOK);
  // writer that writes nothing
  resp.appendBody(8U, kAppendNothing);
  EXPECT_EQ(resp.body(), "");
  EXPECT_EQ(resp.headerValue(http::ContentType), std::nullopt);
}

TEST_F(HttpResponseTest, AppendBodyCharPtrLvalueWriterWritesAndSetsTextPlain) {
  // lvalue std::function<char*> writer
  std::function<std::size_t(char*)> writer = [](char* buf) {
    static constexpr std::string_view kData = "hello";
    std::memcpy(buf, kData.data(), kData.size());
    return kData.size();
  };

  HttpResponse resp(http::StatusCodeOK);
  // Start with a captured payload to ensure append erases it and resets content-type
  resp.body(std::string{"captured"}, "text/captured");
  resp.appendBody(16U, writer);
  resp.appendBody(16U, []([[maybe_unused]] char* buf) { return 0; });
  EXPECT_EQ(resp.body(), "hello");
  EXPECT_EQ(resp.headerValue(http::ContentType), http::ContentTypeTextPlain);
}

TEST_F(HttpResponseTest, AppendBodyCharPtrRvalueWriterCustomContentType) {
  // rvalue lambda writer with explicit content type
  auto resp = HttpResponse(http::StatusCodeOK)
                  .appendBody(
                      8U,
                      [](char* buf) {
                        const char ch = 'X';
                        *buf = ch;
                        return 1U;
                      },
                      "text/x-custom");
  EXPECT_EQ(resp.body(), "X");
  EXPECT_EQ(resp.headerValue(http::ContentType), "text/x-custom");
}

TEST_F(HttpResponseTest, AppendBodyCStrRvalue) {
  auto resp =
      HttpResponse(http::StatusCodeOK).appendBody("Hello, C-String!").appendBody(static_cast<const char*>(nullptr));
  EXPECT_EQ(resp.body(), "Hello, C-String!");
}

TEST_F(HttpResponseTest, BodyFromSpanBytesRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .body(std::span<const std::byte>(std::vector<std::byte>{
                      std::byte{'W'}, std::byte{'o'}, std::byte{'r'}, std::byte{'l'}, std::byte{'d'}}));
  EXPECT_EQ(resp.body(), "World");
}

TEST_F(HttpResponseTest, BodyFromConstCharStar) {
  const char* bodyCStr = "Hello, C-String!";
  HttpResponse resp(http::StatusCodeOK);
  resp.body(bodyCStr);
  EXPECT_EQ(resp.body(), "Hello, C-String!");

  const char* nullPtr = nullptr;

  auto resp2 = HttpResponse(http::StatusCodeOK).body(nullPtr);
  EXPECT_EQ(resp2.body(), "");
}

TEST_F(HttpResponseTest, BodyFromVectorBytes) {
  std::vector<std::byte> bodyBytes = {std::byte{'B'}, std::byte{'y'}, std::byte{'t'}, std::byte{'e'}, std::byte{'s'}};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyBytes));
  EXPECT_EQ(resp.body(), "Bytes");
}

TEST_F(HttpResponseTest, BodyFromVectorBytesRValue) {
  auto resp = HttpResponse(http::StatusCodeOK)
                  .body(std::vector<std::byte>{std::byte{'R'}, std::byte{'V'}, std::byte{'a'}, std::byte{'l'},
                                               std::byte{'u'}, std::byte{'e'}});
  EXPECT_EQ(resp.body(), "RValue");
}

TEST_F(HttpResponseTest, BodyFromVectorChar) {
  std::vector<char> bodyChars = {'C', '+', '+'};
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyChars));
  EXPECT_EQ(resp.body(), "C++");
}

TEST_F(HttpResponseTest, BodyFromVectorCharRValue) {
  auto resp = HttpResponse(http::StatusCodeOK).body(std::vector<char>{'R', 'V', 'a', 'l', 'u', 'e'});
  EXPECT_EQ(resp.body(), "RValue");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrChar) {
  const char text[] = "UniquePtrChar";
  auto bodyPtr = std::make_unique<char[]>(sizeof(text));
  std::ranges::copy(text, bodyPtr.get());
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.body(), "UniquePtrChar");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrCharRValue) {
  const char text[] = "UniquePtrCharRValue";
  auto bodyPtr = std::make_unique<char[]>(sizeof(text));
  std::ranges::copy(text, bodyPtr.get());
  auto resp = HttpResponse(http::StatusCodeOK).body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.body(), "UniquePtrCharRValue");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrByte) {
  const char text[] = "UniquePtrByte";
  auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text));
  for (std::size_t i = 0; i < sizeof(text); ++i) {
    bodyPtr[i] = static_cast<std::byte>(text[i]);
  }
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.body(), "UniquePtrByte");
}

TEST_F(HttpResponseTest, BodyFromUniquePtrByteRValue) {
  const char text[] = "UniquePtrByteRValue";
  auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text));
  for (std::size_t i = 0; i < sizeof(text); ++i) {
    bodyPtr[i] = static_cast<std::byte>(text[i]);
  }
  auto resp = HttpResponse(http::StatusCodeOK).body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.body(), "UniquePtrByteRValue");
}

TEST_F(HttpResponseTest, StatusOnly) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_EQ(200, resp.status());
  resp.status(404);
  EXPECT_EQ(404, resp.status());

  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 404\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, TooLongReasonShouldBeTruncated) {
  HttpResponse resp(http::StatusCodeOK);
  std::string longReason(70000, 'A');
  resp.reason(longReason);
  EXPECT_LT(resp.reason().size(), longReason.size());
}

TEST_F(HttpResponseTest, ConstructorWithBody) {
  HttpResponse resp("Hello, World!");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.reason(), "");
  EXPECT_EQ(resp.body(), "Hello, World!");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/plain");

  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains("Content-Type: text/plain\r\n"));
  EXPECT_TRUE(full.contains("Content-Length: 13\r\n"));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, ConstructorWithBodyContentTypeOnly) {
  HttpResponse resp("Hello, World!", "text/my-text");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.reason(), "");
  EXPECT_EQ(resp.body(), "Hello, World!");
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/my-text");

  const auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.starts_with("HTTP/1.1 200\r\n"));
  EXPECT_TRUE(full.contains("Content-Type: text/my-text\r\n"));
  EXPECT_TRUE(full.contains("Content-Length: 13\r\n"));
  EXPECT_TRUE(full.ends_with("\r\n\r\nHello, World!"));
}

TEST_F(HttpResponseTest, BadStatusCode) { EXPECT_DEBUG_DEATH(HttpResponse(1000), ""); }

TEST_F(HttpResponseTest, StatusReasonAndBodySimple) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.addHeader("Content-Type", "text/plain").addHeader("X-A", "B").body("Hello");
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
  resp.status(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithoutHeaders) {
  HttpResponse resp(404, "Not Found");
  EXPECT_EQ(resp.reason(), http::NotFound);
  resp.status(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full, "HTTP/1.1 200 OK\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithHeaders) {
  HttpResponse resp(200, "OK");
  resp.addHeader("X-Header", 127);
  resp.status(404, "Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(
      full,
      "HTTP/1.1 404 Not Found\r\nX-Header: 127\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithHeaders) {
  auto resp = HttpResponse(404, "Not Found").addHeader("X-Header-1", "Value1");
  resp.addHeader("X-Header-2", "Value2");
  resp.status(200).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nConnection: close\r\nDate: Thu, 01 Jan "
            "1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyAddReasonWithHeaders) {
  auto resp = HttpResponse(200).addHeader("X-Header", 127);
  resp.status(404, "Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(
      full,
      "HTTP/1.1 404 Not Found\r\nX-Header: 127\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyRemoveReasonWithHeaders) {
  HttpResponse resp(404, "Not Found");
  resp.addHeader("X-Header-1", "Value1");
  resp.addHeader("X-Header-2", "Value2");
  resp.status(200).reason("");
  EXPECT_EQ(resp.reason(), "");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 200\r\nX-Header-1: Value1\r\nX-Header-2: Value2\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenHigherWithBody) {
  HttpResponse resp(200, "OK");
  resp.body("Hello", "MySpecialContentType");
  resp.status(404).reason("Not Found");
  EXPECT_EQ(resp.reason(), "Not Found");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 404 Not Found\r\nContent-Type: MySpecialContentType\r\nConnection: close\r\nDate: "
            "Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 5\r\n\r\nHello");
}

TEST_F(HttpResponseTest, StatusReasonAndBodyOverridenLowerWithBody) {
  HttpResponse resp(http::StatusCodeNotFound, "Not Found");
  resp.body("Hello");
  resp.status(http::StatusCodeOK).reason("OK");
  EXPECT_EQ(resp.reason(), "OK");
  auto full = concatenated(std::move(resp));

  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nDate: Thu, 01 "
            "Jan 1970 00:00:00 GMT\r\nContent-Length: 5\r\n\r\nHello");
}

TEST_F(HttpResponseTest, AllowsDuplicates) {
  HttpResponse resp(204, "No Content");
  resp.addHeader("X-Dup", "1").addHeader("X-Dup", "2").body("");
  auto full = concatenated(std::move(resp));
  auto first = full.find("X-Dup: 1\r\n");
  auto second = full.find("X-Dup: 2\r\n");
  ASSERT_NE(first, std::string_view::npos);
  ASSERT_NE(second, std::string_view::npos);
  EXPECT_LT(first, second);
}

TEST_F(HttpResponseTest, ProperTermination) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
}

TEST_F(HttpResponseTest, SendFileEmptyShouldReturnNullptr) {
  HttpResponse resp;
  EXPECT_EQ(resp.file(), nullptr);
}

TEST_F(HttpResponseTest, CannotSendFileAfterTrailers) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.body("some body");
  resp.addTrailer("X-trailer", "value");
  constexpr std::string_view kPayload = "static file payload";

  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());

  EXPECT_THROW(resp.file(std::move(file)), std::logic_error);
}

TEST_F(HttpResponseTest, SendFilePayload) {
  constexpr std::string_view kPayload = "static file payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();

  auto resp = HttpResponse(http::StatusCodeOK, "OK").file(std::move(file));

  EXPECT_THROW(resp.addTrailer("X-trailer", "value");, std::logic_error);

  auto prepared = finalizePrepared(std::move(resp));
  EXPECT_EQ(prepared.fileLength, sz);
  EXPECT_TRUE(prepared.file);
  EXPECT_EQ(prepared.file.size(), sz);

  std::string headers(prepared.data.firstBuffer());
  EXPECT_TRUE(headers.contains("Content-Length: " + std::to_string(sz)));
  EXPECT_FALSE(headers.contains("Transfer-Encoding: chunked"));
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
  EXPECT_EQ(prepared.fileLength, sz - 4);
  EXPECT_TRUE(prepared.file);
  EXPECT_EQ(prepared.file.size(), sz);

  std::string headers(prepared.data.firstBuffer());
  EXPECT_TRUE(headers.contains("Content-Length: " + std::to_string(sz - 4)));
  EXPECT_FALSE(headers.contains("Transfer-Encoding: chunked"));
}

TEST_F(HttpResponseTest, SendFilePayloadOffsetLengthRvalue) {
  constexpr std::string_view kPayload = "static file payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const auto sz = file.size();

  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.file(std::move(file), 3, sz - 6);

  auto prepared = finalizePrepared(std::move(resp));
  EXPECT_EQ(prepared.fileLength, sz - 6);
  EXPECT_TRUE(prepared.file);
  EXPECT_EQ(prepared.file.size(), sz);

  std::string headers(prepared.data.firstBuffer());
  EXPECT_TRUE(headers.contains("Content-Length: " + std::to_string(sz - 6)));
  EXPECT_FALSE(headers.contains("Transfer-Encoding: chunked"));
}

TEST_F(HttpResponseTest, SendFileHeadSuppressesPayload) {
  constexpr std::string_view kPayload = "head sendfile payload";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);
  File file(tmp.filePath().string());
  ASSERT_TRUE(file);
  const std::uint64_t sz = file.size();

  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.file(std::move(file));

  auto prepared = finalizePrepared(std::move(resp), true /*head*/);
  EXPECT_EQ(prepared.fileLength, 0U);
  EXPECT_FALSE(prepared.file);

  std::string headers(prepared.data.firstBuffer());
  EXPECT_TRUE(headers.contains("Content-Length: " + std::to_string(sz)));
  EXPECT_FALSE(headers.contains("Transfer-Encoding: chunked"));
}

TEST_F(HttpResponseTest, CapturedBodyWithTrailersAppendsFinalCRLF) {
  // Create a captured body larger than minCapturedBodySize (4096) so it remains external
  std::string bigBody(5000, 'x');
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::move(bigBody));
  resp.addTrailer("X-Custom-Trail", "trail-value");

  // Finalize and inspect the serialized response which concatenates head + external payload
  auto prepared = finalizePrepared(std::move(resp));
  std::string tail(prepared.data.secondBuffer());

  // The external payload (tail) should contain the body followed by the trailer line and a terminating CRLF
  EXPECT_TRUE(tail.contains("X-Custom-Trail: trail-value\r\n"));
  EXPECT_TRUE(tail.size() >= 2 && tail.substr(tail.size() - 2) == "\r\n");
}

TEST_F(HttpResponseTest, HeaderValueFindsLastHeader) {
  HttpResponse resp(http::StatusCodeOK);
  // Add multiple headers and ensure headerValue finds the last one when searching
  resp.addHeader("X-A", "one");
  resp.addHeader("X-B", "two");
  resp.addHeader("X-C", "three");

  EXPECT_EQ(resp.headerValue("X-C").value_or(""), "three");
  EXPECT_EQ(resp.headerValue("X-D"), std::nullopt);
}

TEST_F(HttpResponseTest, FileWithClosedFileThrows) {
  File file;  // default-constructed, closed
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.file(std::move(file)), std::invalid_argument);
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

TEST_F(HttpResponseTest, SingleTerminatingCRLF) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.addHeader("X-Header", "v1");
  auto full = concatenated(std::move(resp));
  ASSERT_TRUE(full.size() >= 4);
  EXPECT_EQ(full.substr(full.size() - 4), http::DoubleCRLF);
  EXPECT_TRUE(full.contains("X-Header: v1"));
}

TEST_F(HttpResponseTest, ReplaceDifferentSizes) {
  HttpResponse resp1(http::StatusCodeOK, "OK");
  resp1.addHeader("X-A", "1").body("Hello");
  HttpResponse resp2(http::StatusCodeOK, "OK");
  resp2.addHeader("X-A", "1").body("Hello");
  HttpResponse resp3(http::StatusCodeOK, "OK");
  resp3.addHeader("X-A", "1").body("Hello");
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
// Prior to the fix, this scenario could read from stale memory if ensureAvailableCapacityExponential
// triggered a reallocation.
TEST_F(HttpResponseTest, BodyAssignFromInternalReasonTriggersReallocSafe) {
  // Choose a non-empty reason so we have internal bytes to reference.
  HttpResponse resp(http::StatusCodeOK, "INTERNAL-REASON");
  std::string_view src = resp.reason();  // points into resp's internal buffer
  EXPECT_EQ(src, "INTERNAL-REASON");
  // Body currently empty -> diff = src.size() => ensureAvailableCapacityExponential likely reallocates
  resp.body(src);       // must be safe even if reallocation occurs
  src = resp.reason();  // reset reason after realloc
  EXPECT_EQ(src, "INTERNAL-REASON");
  EXPECT_EQ(resp.body(), src);
  auto full = concatenated(std::move(resp));
  resp = HttpResponse(http::StatusCodeOK, "INTERNAL-REASON");
  src = resp.reason();
  // Validate Content-Length header matches and body placed at tail.
  std::string clNeedle = std::string("Content-Length: ") + std::to_string(src.size()) + "\r\n";
  EXPECT_TRUE(full.contains(clNeedle)) << full;
  EXPECT_TRUE(full.ends_with(src)) << full;
}

// --- New tests for header(K,V) replacement logic ---

TEST_F(HttpResponseTest, HeaderNewViaSetter) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-First", "One");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-First: One\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceLargerValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Replace", "AA");
  // Replace with larger value
  resp.header("X-Replace", "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Replace: ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSmallerValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Replace", "LONG-LONG-VALUE");
  // Replace with smaller
  resp.header("X-Replace", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Replace: S\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceSameLengthValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Replace", "LEN10VALUE");  // length 10
  resp.header("X-Replace", "0123456789");  // also length 10
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nX-Replace: 0123456789\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

// Ensure replacement logic does not mistake key pattern inside a value as a header start.
TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternLarger) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Key", "before X-Key: should-not-trigger");
  // Replace header; algorithm must not treat the embedded "X-Key: " in the value as another header start
  resp.header("X-Key", "REPLACED-VALUE");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(
      full,
      "HTTP/1.1 200 OK\r\nX-Key: REPLACED-VALUE\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceIgnoresEmbeddedKeyPatternSmaller) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Key", "AAAA X-Key: B BBBBBB");
  resp.header("X-Key", "SMALL");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Key: SMALL\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

// --- New tests: header replacement while a body is present ---

TEST_F(HttpResponseTest, LocationHeader) {
  HttpResponse resp(http::StatusCodeFound, "Found");
  resp.location("http://example.com/new-location");
  resp.body("Redirecting...");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(
      full,
      "HTTP/1.1 302 Found\r\nLocation: http://example.com/new-location\r\nContent-Type: text/plain\r\nConnection: "
      "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 14\r\n\r\nRedirecting...");
}

TEST_F(HttpResponseTest, LocationHeaderRValue) {
  auto resp = HttpResponse(http::StatusCodeFound, "Found")
                  .location("https://another.example.com/redirect-here")
                  .body("Please wait...");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 302 Found\r\nLocation: https://another.example.com/redirect-here\r\nContent-Type: text/plain\r\n"
            "Connection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 14\r\n\r\nPlease wait...");
}

TEST_F(HttpResponseTest, ContentEncodingHeader) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.contentEncoding("gzip");
  resp.body("CompressedData");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Type: text/plain\r\nConnection: close\r\nDate: "
            "Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 14\r\n\r\nCompressedData");
}

TEST_F(HttpResponseTest, ContentEncodingHeaderRValue) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").contentEncoding("deflate").body("DeflatedData");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Type: text/plain\r\nConnection: close\r\nDate: "
            "Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 12\r\n\r\nDeflatedData");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodyLargerValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Val", "AA");
  resp.body("Hello");                  // body length 5
  resp.header("X-Val", "ABCDEFGHIJ");  // grow header value
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: ABCDEFGHIJ\r\nContent-Type: text/plain\r\nConnection: "
            "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 5\r\n\r\nHello");
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySmallerValue) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").header("X-Val", "SOME-LONG-VALUE");
  resp.body("WorldWide");     // length 9
  resp.header("X-Val", "S");  // shrink header value
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: S\r\nContent-Type: text/plain\r\nConnection: "
            "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 9\r\n\r\nWorldWide");
}

TEST_F(HttpResponseTest, AppendHeaderValueAppendsToExistingHeader) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").header("X-Custom", "value1");
  resp.appendHeaderValue("X-Custom", "value2");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Custom: value1, value2\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueCreatesHeaderWhenMissing) {
  auto resp = HttpResponse(http::StatusCodeOK, "OK").appendHeaderValue("X-Missing", "v1");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Missing: v1\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueHonorsCustomSeparator) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-List", "first");
  resp.appendHeaderValue("X-List", "second", "; ");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first; second\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptySeparator) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-List", "first");
  resp.appendHeaderValue("X-List", "second", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: firstsecond\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-List", "first");
  resp.appendHeaderValue("X-List", "", ", ");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first, \r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueEmptyValueAndSeparator) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-List", "first");
  resp.appendHeaderValue("X-List", "", "");

  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-List: first\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueSupportsNumericOverload) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Numeric", "1");
  resp.appendHeaderValue("X-Numeric", 42, "|");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Numeric: 1|42\r\n")) << full;
}

TEST_F(HttpResponseTest, AppendHeaderValueKeepsBodyIntact) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Trace", "alpha");
  resp.body("payload");
  resp.appendHeaderValue("X-Trace", "beta");
  auto full = concatenated(std::move(resp));
  EXPECT_TRUE(full.contains("X-Trace: alpha, beta\r\n")) << full;
  EXPECT_TRUE(full.ends_with("payload")) << full;
  EXPECT_TRUE(full.contains("Content-Length: 7\r\n")) << full;
}

TEST_F(HttpResponseTest, HeaderReplaceWithBodySameLengthValue) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("X-Val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Type: text/plain\r\nConnection: "
            "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 4\r\n\r\nData");
}

TEST_F(HttpResponseTest, GlobalHeadersShouldNotOverrideUserHeaders) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Global", "UserValue");
  ConcatenatedHeaders globalHeaders;
  globalHeaders.append("X-Global: GlobalValue");
  globalHeaders.append("X-Another: AnotherValue");
  resp.reason("Some Reason");
  auto full = concatenated(std::move(resp), globalHeaders);
  EXPECT_EQ(full,
            "HTTP/1.1 200 Some Reason\r\nX-Global: UserValue\r\nX-Another: AnotherValue\r\nConnection: close\r\nDate: "
            "Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, HeaderReplaceCaseInsensitive) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.header("X-Val", "LEN10VALUE");  // length 10
  resp.body("Data");                   // length 4
  resp.header("x-val", "0123456789");  // same length replacement
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nX-Val: 0123456789\r\nContent-Type: text/plain\r\nConnection: "
            "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 4\r\n\r\nData");
}

TEST_F(HttpResponseTest, HeaderGetterAfterSet) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  // Mix of headers to exercise several lookup cases:
  // - customHeader replaces case-insensitively
  // - addCustomHeader allows duplicates (first occurrence should be returned by headerValue)
  // - empty value is a present-but-empty header
  resp.header("X-Simple", "hello");
  resp.addHeader("X-Dup", "1");
  resp.addHeader("X-Dup", "2");
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

// Interleaved reason/header mutations stress test:
// 1. Start with empty reason
// 2. Append two headers
// 3. Add longer reason
// 4. Replace first header with larger value
// 5. Shrink reason to empty
// 6. Replace header with smaller value
// 7. Finalize and assert exact layout
TEST_F(HttpResponseTest, InterleavedReasonAndHeaderMutations) {
  HttpResponse resp(http::StatusCodeOK, "");
  resp.addHeader("X-A", "1");
  resp.addHeader("X-B", "2");
  resp.reason("LONGER-REASON");
  resp.header("X-a", "LARGER-VALUE-123");
  resp.reason("");
  resp.header("x-A", "S");
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200\r\nX-A: S\r\nX-B: 2\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeString) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.body("Non-empty body");
  EXPECT_EQ(resp.body(), "Non-empty body");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::string());  // set empty body
  EXPECT_EQ(resp.body(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeVectorBytes) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.body("Non-empty body");
  EXPECT_EQ(resp.body(), "Non-empty body");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::vector<std::byte>{});  // set empty body
  EXPECT_EQ(resp.body(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyShouldResetBodyAndRemoveContentTypeUniquePtrBytes) {
  HttpResponse resp(http::StatusCodeOK, "OK");
  resp.body("Non-empty body");
  EXPECT_EQ(resp.body(), "Non-empty body");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::unique_ptr<std::byte[]>(), 0);  // set empty body
  EXPECT_EQ(resp.body(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 OK\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

TEST_F(HttpResponseTest, SetCapturedBodyEmptyFromUniquePtrShouldResetBodyAndRemoveContentType) {
  HttpResponse resp(http::StatusCodeOK, "Longer Reason");
  static constexpr const char text[] = "UniquePtrBody";
  auto bodyPtr = std::make_unique<std::byte[]>(sizeof(text) - 1);
  for (size_t i = 0; i < sizeof(text) - 1; ++i) {
    bodyPtr[i] = static_cast<std::byte>(text[i]);
  }
  resp.body(std::move(bodyPtr), sizeof(text) - 1);
  EXPECT_EQ(resp.body(), "UniquePtrBody");
  EXPECT_TRUE(resp.headerValue(http::ContentType).has_value());
  resp.body(std::make_unique<char[]>(0), 0);  // set empty body
  EXPECT_EQ(resp.body(), "");
  EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
  auto full = concatenated(std::move(resp));
  EXPECT_EQ(full,
            "HTTP/1.1 200 Longer Reason\r\nConnection: close\r\nDate: Thu, 01 Jan 1970 "
            "00:00:00 GMT\r\n\r\n");
}

// ---------------- Additional Stress / Fuzz Tests ----------------

TEST_F(HttpResponseTest, RepeatedGrowShrinkCycles) {
  HttpResponse resp(http::StatusCodeOK, "");
  resp.addHeader("X-Static", "STATIC");
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
  std::string expected =
      "HTTP/1.1 200\r\nX-Static: STATIC\r\nX-Cycle: Z\r\nContent-Type: text/plain\r\nConnection: "
      "close\r\nDate: Thu, 01 Jan 1970 00:00:00 GMT\r\nContent-Length: 3\r\n\r\nEND";
  EXPECT_EQ(full, expected);
}

// --- Trailer-related tests (response-side) ---

TEST_F(HttpResponseTest, AddTrailerWithoutBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  // No body set at all -> adding trailer should throw
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, AddTrailerAfterEmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");
  // Explicitly-empty body should still be considered 'no body' for trailers
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

TEST_F(HttpResponseTest, SetBodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.addTrailer("X-Test", "val");
  // Once a trailer was inserted, setting body later must throw
  EXPECT_THROW(resp.body("later"), std::logic_error);
}

TEST_F(HttpResponseTest, LargeHeaderCountStress) {
  constexpr int kCount = 600;
  HttpResponse resp(http::StatusCodeOK, "OK");
  for (int i = 0; i < kCount; ++i) {
    resp.addHeader("X-" + std::to_string(i), std::to_string(i));
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
    pr.headers.emplace_back(std::string(line.substr(0, sep)), std::string(line.substr(sep + 2)));
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

  if (hasContentLen) {
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
    ParsedResponse parsed = parseResponse(serialized);

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
  resp.addHeader("X-Seed", "local-value");
  resp.body("payload");

  constexpr int kGlobalHeaders = HttpServerConfig::kMaxGlobalHeaders;
  // Build concatenated global headers but keep an indexed vector for targeted overrides below.
  std::vector<http::Header> headerVec;
  headerVec.reserve(kGlobalHeaders);
  aeronet::ConcatenatedHeaders globalHeaders;
  for (int headerIdx = 0; headerIdx < kGlobalHeaders; ++headerIdx) {
    std::string name = "X-Bulk-" + std::to_string(headerIdx);
    std::string value = "Value-" + std::to_string(headerIdx);
    headerVec.push_back({name, value});
    std::string header;
    header.reserve(name.size() + 2 + value.size());
    header.append(name);
    header.append(": ");
    header.append(value);
    globalHeaders.append(header);
  }
  // Force overlap with a couple of entries (exercise dynamic bitmap skip path)
  resp.header(headerVec[42].name, "UserOverride-42");
  resp.header(headerVec[199].name, "UserOverride-199");

  auto expected = ExpectedGlobalHeaderValues(resp, globalHeaders);
  auto serialized = concatenated(std::move(resp), globalHeaders);
  ParsedResponse parsed = parseResponse(serialized);

  ASSERT_GE(parsed.headers.size(), static_cast<std::size_t>(kGlobalHeaders));
  for (std::string_view gh : globalHeaders) {
    const auto sep = gh.find(": ");
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
  static constexpr int kNbHttpResponses = 60;
  static constexpr int kNbOperationsPerHttpResponse = 100;

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> opDist(0, 5);
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
    aeronet::ConcatenatedHeaders fuzzGlobalHeaders;
    std::vector<http::Header> fuzzHeaderVec;
    const int fuzzGlobalCount = globalHeaderCountDist(rng);
    fuzzHeaderVec.reserve(static_cast<std::size_t>(fuzzGlobalCount));
    for (int globalIdx = 0; globalIdx < fuzzGlobalCount; ++globalIdx) {
      std::string name = "X-Fuzz-Global-" + std::to_string(caseIndex) + "-" + std::to_string(globalIdx);
      std::string value = makeValue(globalValueLenDist(rng));
      fuzzHeaderVec.push_back({name, value});
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
    for (int step = 0; step < kNbOperationsPerHttpResponse; ++step) {
      switch (opDist(rng)) {
        case 0:
          lastHeaderKey = "X-" + std::to_string(step);
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<std::size_t>(rng() % fuzzHeaderVec.size())].name;
          }
          lastHeaderValue = makeValue(smallLen(rng));
          resp.addHeader(lastHeaderKey, lastHeaderValue);
          break;
        case 1:
          lastHeaderKey = "U-" + std::to_string(step % 5);
          if (!fuzzHeaderVec.empty() && reuseGlobalNameDist(rng) == 0) {
            lastHeaderKey = fuzzHeaderVec[static_cast<std::size_t>(rng() % fuzzHeaderVec.size())].name;
          }
          lastHeaderValue = makeValue(midLen(rng));
          resp.header(lastHeaderKey, lastHeaderValue);
          break;
        case 2:
          lastReason = makeReason(smallLen(rng));
          resp.reason(lastReason);
          break;
        case 3:
          if (lastTrailerKey.empty()) {
            if (lastBody.empty()) {
              lastBody = makeValue(smallLen(rng));
              resp.body(lastBody);
            } else {
              resp.body({});  // empty body
              lastBody.clear();
            }
          } else {
            // Once a trailer was set, body cannot be changed
            EXPECT_THROW(resp.body({}), std::logic_error);
          }
          break;
        case 4: {
          static constexpr http::StatusCode opts[] = {200, 204, 404};
          resp.status(opts[static_cast<std::size_t>(step) % std::size(opts)]);
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
          throw std::runtime_error("Invalid random value, update the test");
      }
    }

    // Pre-finalize state checks (reason/body accessible before finalize)
    EXPECT_EQ(resp.reason(), std::string_view(lastReason));
    EXPECT_EQ(resp.body(), std::string_view(lastBody));

    auto expectedGlobals = ExpectedGlobalHeaderValues(resp, fuzzGlobalHeaders);

    auto full = concatenated(std::move(resp), fuzzGlobalHeaders);
    ParsedResponse pr = parseResponse(full);

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
      ASSERT_GE(pr.headers.size(), 3U);
      EXPECT_EQ(pr.headers[pr.headers.size() - 3].first, http::Connection);
      EXPECT_EQ(pr.headers[pr.headers.size() - 2].first, http::Date);
      EXPECT_EQ(pr.headers[pr.headers.size() - 1].first, http::ContentLength);
    } else {
      EXPECT_EQ(clCount, 0);
      ASSERT_GE(pr.headers.size(), 2U);
      EXPECT_EQ(pr.headers[pr.headers.size() - 2].first, http::Connection);
      EXPECT_EQ(pr.headers[pr.headers.size() - 1].first, http::Date);
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

#ifdef NDEBUG
// In release builds assertions are disabled; just ensure we can set non-reserved but not crash when attempting what
// would be reserved (we avoid actually invoking UB). This block left empty intentionally.
#else
TEST(HttpHeadersCustom, SettingReservedHeaderTriggersAssert) {
  // We use EXPECT_DEATH to verify debug assertion fires when user attempts to set reserved headers.
  // Connection
  ASSERT_DEATH(
      {
        HttpResponse resp;
        resp.header("Connection", "keep-alive");
      },
      "");
  // Date
  ASSERT_DEATH(
      {
        HttpResponse resp;
        resp.header("Date", "Wed, 01 Jan 2020 00:00:00 GMT");
      },
      "");
  // Content-Length
  ASSERT_DEATH(
      {
        HttpResponse resp;
        resp.header("Content-Length", "10");
      },
      "");
  // Transfer-Encoding
  ASSERT_DEATH(
      {
        HttpResponse resp;
        resp.header("Transfer-Encoding", "chunked");
      },
      "");
}
#endif

// Basic trailer test - verify trailers are appended after body
TEST(HttpResponseTrailers, BasicTrailer) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test body");
  resp.addTrailer("X-Checksum", "abc123");

  // We can't easily test the serialized output without finalizing,
  // but we can verify no exception is thrown
  EXPECT_NO_THROW(resp.addTrailer("X-Signature", "sha256:..."));
}

// Test error when adding trailer before body
TEST(HttpResponseTrailers, ErrorBeforeBody) {
  HttpResponse resp(http::StatusCodeOK);
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

// Test error when adding trailer after an explicitly empty body
TEST(HttpResponseTrailers, EmptyBodyThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("");  // empty body set explicitly
  EXPECT_THROW(resp.addTrailer("X-Checksum", "abc123"), std::logic_error);
}

// Test trailer with captured body (std::string)
TEST(HttpResponseTrailers, CapturedBodyString) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured body content"));
  EXPECT_NO_THROW(resp.addTrailer("X-Custom", "value"));
}

// Test trailer with captured body (vector<char>)
TEST(HttpResponseTrailers, CapturedBodyVector) {
  HttpResponse resp(http::StatusCodeOK);
  std::vector<char> vec = {'h', 'e', 'l', 'l', 'o'};
  resp.body(std::move(vec));
  EXPECT_NO_THROW(resp.addTrailer("X-Data", "123"));
}

// Test multiple trailers
TEST(HttpResponseTrailers, MultipleTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("body");
  resp.addTrailer("X-Checksum", "abc");
  resp.addTrailer("X-Timestamp", "2025-10-20T12:00:00Z");
  resp.addTrailer("X-Custom", "val");
  // No assertion - just verify no crashes
}

// Test empty trailer value
TEST(HttpResponseTrailers, EmptyValue) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("test");
  EXPECT_NO_THROW(resp.addTrailer("X-Empty", ""));
}

// Test rvalue ref version
TEST(HttpResponseTrailers, RvalueRef) {
  EXPECT_NO_THROW(HttpResponse(http::StatusCodeOK).body("test").addTrailer("X-Check", "val"));
}

// Test that setting the body after inserting a trailer throws
TEST(HttpResponseTrailers, BodyAfterTrailerThrows) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("initial");
  resp.addTrailer("X-After", "v");
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
  resp.addHeader("X-Test", "val");
  // No trailers added -> empty view
  EXPECT_TRUE(resp.trailers().empty());
  // body remains accessible and unchanged
  EXPECT_EQ(resp.body(), std::string_view());
}

TEST_F(HttpResponseTest, TrailersInline_NoTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("inline-body");
  // No trailers added -> empty view
  auto tv = resp.trailers();
  EXPECT_TRUE(tv.empty());
  // body remains accessible and unchanged
  EXPECT_EQ(resp.body(), std::string_view("inline-body"));
}

TEST_F(HttpResponseTest, TrailersInline_WithTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body("inline-body");
  resp.addTrailer("X-First", "one");
  resp.addTrailer("X-Second", "two");
  auto tv = resp.trailers();
  EXPECT_FALSE(tv.empty());
  // trailers are stored as header lines terminated by CRLF
  EXPECT_TRUE(tv.contains("X-First: one\r\n"));
  EXPECT_TRUE(tv.contains("X-Second: two\r\n"));
  EXPECT_TRUE(tv.ends_with(http::CRLF));
  // body() should not include trailers
  EXPECT_EQ(resp.body(), std::string_view("inline-body"));
}

TEST_F(HttpResponseTest, TrailersCaptured_NoTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured-body-content"));
  auto tv = resp.trailers();
  EXPECT_TRUE(tv.empty());
  EXPECT_EQ(resp.body(), std::string_view("captured-body-content"));
}

TEST_F(HttpResponseTest, TrailersCaptured_WithTrailers) {
  HttpResponse resp(http::StatusCodeOK);
  resp.body(std::string("captured-body"));
  resp.addTrailer("X-Custom", "val");
  auto tv = resp.trailers();
  EXPECT_FALSE(tv.empty());
  EXPECT_TRUE(tv.contains("X-Custom: val\r\n"));
  EXPECT_TRUE(tv.ends_with(http::CRLF));
  // body() must remain the original captured body (trailers excluded)
  EXPECT_EQ(resp.body(), std::string_view("captured-body"));
}

TEST(HttpResponseAppendHeaderValue, AppendsToEmptyHeader) {
  HttpResponse resp;
  resp.appendHeaderValue("X-Test", "alpha");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "alpha");
}

TEST(HttpResponse, AppendReservedHeaderShouldDieInDebug) {
  HttpResponse resp;
  EXPECT_DEBUG_DEATH(resp.appendHeaderValue(http::Connection, "some value"), "");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithDefaultSeparator) {
  HttpResponse resp;
  resp.header("X-Test", "one");
  resp.appendHeaderValue("X-Test", "two");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "one, two");
}

TEST(HttpResponseAppendHeaderValue, AppendsWithCustomSeparator) {
  HttpResponse resp;
  resp.header("X-Test", "first");
  resp.appendHeaderValue("X-Test", "second", "; ");
  EXPECT_EQ(resp.headerValueOrEmpty("X-Test"), "first; second");
}

TEST(HttpResponseAppendHeaderValue, NumericOverloadAndSubsequentAppend) {
  HttpResponse resp;
  resp.appendHeaderValue("X-Num", 123);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "123");

  resp.appendHeaderValue("X-Num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "123, 456");

  resp = HttpResponse{}.appendHeaderValue("X-Num", 456);
  EXPECT_EQ(resp.headerValueOrEmpty("X-Num"), "456");
}

TEST(HttpResponseAppendHeaderValue, CaseInsensitiveKeyMatch) {
  HttpResponse resp;
  resp.header("x-TeSt", "lower");
  resp.appendHeaderValue("X-TEST", "upper");
  EXPECT_EQ(resp.headerValueOrEmpty("X-test"), "lower, upper");
}

TEST(HttpResponseAppendHeaderValue, VaryMergesAcceptEncoding) {
  HttpResponse resp;
  resp.header("Vary", "Origin");
  resp.appendHeaderValue("Vary", "Accept-Encoding");
  EXPECT_EQ(resp.headerValueOrEmpty("Vary"), "Origin, Accept-Encoding");
}

}  // namespace aeronet