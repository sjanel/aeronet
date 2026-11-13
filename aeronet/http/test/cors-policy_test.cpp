#include "aeronet/cors-policy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

class CorsPolicyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    globalHeaders.append("server: aeronet");
    request._pGlobalHeaders = &globalHeaders;
    request._pCompressionState = &compressionState;

    compressionState.pCompressionConfig = &compressionConfig;
  }

  http::StatusCode parse(RawChars raw) {
    inBuffer = std::move(raw);
    RawChars tmp;
    HttpRequestView::HeadParseState headParseState = HttpRequestView::HeadParseState::None;  // a new request
    return request.initTrySetHead(inBuffer, headParseState, tmp, 4096U, true, nullptr);
  }

  CompressionConfig compressionConfig;
  CompressionState compressionState;
  ConcatenatedHeaders globalHeaders;
  CorsPolicy policy;
  HttpResponse response;
  RawChars inBuffer;
  HttpRequestView request;
};

TEST_F(CorsPolicyTest, DefaultConstructedShouldNotBeActive) {
  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::NotCors);
  CorsPolicy::PreflightResult actual = policy.handlePreflight(request);

  EXPECT_EQ(actual.status, CorsPolicy::PreflightResult::Status::NotPreflight);
  EXPECT_TRUE(actual.response.empty());
}

TEST_F(CorsPolicyTest, ApplyAnyOriginSimpleRequest) {
  policy.allowAnyOrigin();

  const auto status = parse(BuildRawHttp11(http::GET, "/resource", "Origin: https://example.com\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), std::string_view{});
}

TEST_F(CorsPolicyTest, AllowEmptyOrigin) {
  policy.allowOrigin("");

  const auto status = parse(BuildRawHttp11(http::GET, "/resource", "Origin: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "");
}

TEST_F(CorsPolicyTest, AllowAlreadyExistingOrigin) {
  policy.allowOrigin("https://api.example");
  policy.allowOrigin("\thttps://api.example  ");  // should be no duplicate effect
  policy.allowOrigin("https://API.EXAMPLE");      // case-insensitive match, no duplicate

  const auto status = parse(BuildRawHttp11(http::GET, "/items", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://api.example");
}

TEST_F(CorsPolicyTest, ApplyAllowListMirrorsOriginAndAddsCredentials) {
  policy.allowOrigin("https://api.example").allowCredentials(true);

  const auto status = parse(BuildRawHttp11(http::GET, "/items", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  response.headerAddLine(http::Vary, http::AcceptEncoding);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://api.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
  std::string expectedVary(http::AcceptEncoding);
  expectedVary += ", ";
  expectedVary += http::Origin;
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), expectedVary);
}

TEST_F(CorsPolicyTest, PreflightWithEmptyAccessControlRequestHeaders) {
  const auto status = parse(
      BuildRawHttp11(http::OPTIONS, "/files",
                     "Access-Control-Request-Method: GET\r\nOrigin: test\r\nAccess-Control-Request-Headers:  \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowHeaders));
}

TEST_F(CorsPolicyTest, PreflightWithAccessControlHeaders) {
  const auto status = parse(BuildRawHttp11(
      http::OPTIONS, "/files",
      "Access-Control-Request-Method: GET\r\nOrigin: test\r\nAccess-Control-Request-Headers: X-Test\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");
  policy.allowMethods(http::Method::GET);
  policy.allowRequestHeader("X-Test");

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-Test");
}

TEST_F(CorsPolicyTest, PreflightWithAccessControlHeadersEmpty) {
  const auto status = parse(
      BuildRawHttp11(http::OPTIONS, "/files",
                     "Access-Control-Request-Method: GET\r\nOrigin: test\r\nAccess-Control-Request-Headers: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");
  policy.allowMethods(http::Method::GET);
  policy.allowRequestHeader("X-Test");

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowHeaders));
}

TEST_F(CorsPolicyTest, PreflightNoAccessControlRequestHeaders) {
  const auto status =
      parse(BuildRawHttp11(http::OPTIONS, "/files", "Access-Control-Request-Method: GET\r\nOrigin: test\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");
  policy.allowMethods(http::Method::GET);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowHeaders));
}

TEST_F(CorsPolicyTest, PreflightAllowed) {
  policy.allowOrigin("https://example.com")
      .allowMethods(static_cast<http::MethodBmp>(http::Method::GET) | static_cast<http::MethodBmp>(http::Method::POST))
      .allowRequestHeader("X-Trace")
      .maxAge(std::chrono::seconds{600});

  const auto status = parse(BuildRawHttp11(http::OPTIONS, "/files",
                                           "Origin: https://example.com\r\n"
                                           "Access-Control-Request-Method: POST\r\n"
                                           "Access-Control-Request-Headers: X-Trace\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  const HttpResponse& httpResponse = result.response;
  EXPECT_EQ(httpResponse.status(), http::StatusCodeNoContent);
  EXPECT_EQ(httpResponse.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://example.com");
  EXPECT_EQ(httpResponse.headerValueOrEmpty(http::AccessControlAllowMethods), "GET, POST");
  EXPECT_EQ(httpResponse.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-Trace");
  EXPECT_EQ(httpResponse.headerValueOrEmpty(http::AccessControlMaxAge), "600");
}

TEST_F(CorsPolicyTest, PreflightDeniedUnknownMethod) {
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(http::Method::GET));

  const auto status = parse(BuildRawHttp11(http::OPTIONS, "/files",
                                           "Origin: https://any\r\n"
                                           "Access-Control-Request-Method: POST\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyTest, PreflightDeniedForHeaders) {
  policy.allowOrigin("https://example.com").allowRequestHeader("X-One");

  const auto status = parse(BuildRawHttp11(http::OPTIONS, "/files",
                                           "Origin: https://example.com\r\n"
                                           "Access-Control-Request-Method: GET\r\n"
                                           "Access-Control-Request-Headers: X-One, X-Two\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::HeadersDenied);
}

TEST_F(CorsPolicyTest, MultipleAllowedOriginsAndMethods) {
  policy.allowOrigin("https://one.example");
  policy.allowOrigin("https://two.example");
  policy.allowMethods(http::Method::GET | http::Method::POST | http::Method::PUT);

  // Request from origin two + PUT should be allowed
  auto raw = BuildRawHttp11(http::OPTIONS, "/multi",
                            "Origin: https://two.example\r\n"
                            "Access-Control-Request-Method: PUT\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://two.example");
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowMethods), "GET, POST, PUT");
}

TEST_F(CorsPolicyTest, MultipleAllowedRequestHeaders) {
  policy.allowOrigin("https://headers.example");
  policy.allowRequestHeader("X-One");
  policy.allowRequestHeader("X-Two");
  policy.allowRequestHeader("  \t ");

  auto raw = BuildRawHttp11(http::OPTIONS, "/hdrs",
                            "Origin: https://headers.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers: X-One, X-Two\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-One, X-Two");
}

TEST_F(CorsPolicyTest, AllowAnyRequestHeadersAcceptsPreflight) {
  policy.allowAnyRequestHeaders().allowAnyOrigin();

  auto raw = BuildRawHttp11(http::OPTIONS, "/anyhdr",
                            "Origin: https://anyhdr.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers: X-Foo, X-Bar\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "*");
}

// Header names are case-insensitive (RFC 9110 §5.1), and browsers send Access-Control-Request-Headers byte-lowercased
// (Fetch: "convert header names to a sorted-lowercase set"), whatever the case used by the script or the policy.
TEST_F(CorsPolicyTest, PreflightRequestHeaderNamesAreCaseInsensitive) {
  policy.allowAnyOrigin()
      .allowMethods(http::Method::PUT)
      .allowRequestHeader("X-Trace")
      .allowRequestHeader("content-TYPE");

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files",
                                 "Origin: https://any\r\n"
                                 "Access-Control-Request-Method: PUT\r\n"
                                 "Access-Control-Request-Headers: content-type,x-trace\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::Allowed);

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files",
                                 "Origin: https://any\r\n"
                                 "Access-Control-Request-Method: PUT\r\n"
                                 "Access-Control-Request-Headers: X-TRACE, Content-Type\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::Allowed);

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files",
                                 "Origin: https://any\r\n"
                                 "Access-Control-Request-Method: PUT\r\n"
                                 "Access-Control-Request-Headers: x-other\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::HeadersDenied);
}

// Allowing / exposing the same header name twice, in different cases, does not duplicate it.
TEST_F(CorsPolicyTest, AllowedAndExposedHeaderNamesAreDeduplicatedCaseInsensitively) {
  policy.allowAnyOrigin().allowRequestHeader("X-Trace").allowRequestHeader("x-trace").exposeHeader("X-Id").exposeHeader(
      "x-id");

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files",
                                 "Origin: https://any\r\n"
                                 "Access-Control-Request-Method: GET\r\n"
                                 "Access-Control-Request-Headers: x-trace\r\n")),
            http::StatusCodeOK);
  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-Trace");

  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/files", "Origin: https://any\r\n")), http::StatusCodeOK);
  HttpResponse resp;
  ASSERT_EQ(policy.applyToResponse(request, resp), CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(resp.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Id");
}

TEST_F(CorsPolicyTest, ExposeHeadersAndVaryMerging) {
  policy.allowOrigin("https://expose.example");
  policy.exposeHeader("X-Exposed");
  policy.exposeHeader(" \t");  // should have no effect

  const auto status = parse(BuildRawHttp11(http::GET, "/expose", "Origin: https://expose.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  response.headerAddLine(http::Vary, http::AcceptEncoding);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Exposed");
  std::string expectedVary(http::AcceptEncoding);
  expectedVary += ", ";
  expectedVary += http::Origin;
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), expectedVary);
}

TEST_F(CorsPolicyTest, WildcardOriginWithCredentialsMirrorsOrigin) {
  policy.allowAnyOrigin().allowCredentials(true);

  const auto status = parse(BuildRawHttp11(http::GET, "/cred", "Origin: https://cred.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  // with credentials enabled, wildcard origin should not be used; origin must be mirrored
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://cred.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
}

// A received Origin may contain obs-text (RFC 9110 §5.5), unlike a serialized origin: it cannot be mirrored, as aeronet
// does not send obs-text, so it is denied with credentials instead of making the policy throw.
TEST_F(CorsPolicyTest, WildcardOriginWithCredentialsDeniesObsTextOrigin) {
  policy.allowAnyOrigin().allowCredentials(true);

  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/cred", "Origin: https://caf\xC3\xA9.example\r\n")), http::StatusCodeOK);

  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::OriginDenied);
  EXPECT_EQ(response.status(), http::StatusCodeForbidden);
  EXPECT_FALSE(response.headerValue(http::AccessControlAllowOrigin));
}

TEST_F(CorsPolicyTest, PreflightWildcardOriginWithCredentialsDeniesObsTextOrigin) {
  policy.allowAnyOrigin().allowCredentials(true);

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/cred",
                                 "Origin: https://caf\xC3\xA9.example\r\n"
                                 "Access-Control-Request-Method: GET\r\n")),
            http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::OriginDenied);
  EXPECT_TRUE(result.response.empty());
}

// Without credentials, the origin is not mirrored: any origin is answered '*'.
TEST_F(CorsPolicyTest, WildcardOriginWithoutCredentialsAllowsObsTextOrigin) {
  policy.allowAnyOrigin();

  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/res", "Origin: https://caf\xC3\xA9.example\r\n")), http::StatusCodeOK);

  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
}

TEST_F(CorsPolicyTest, PreflightPrivateNetworkHeaderEmitted) {
  policy.allowOrigin("https://pnet.example")
      .allowPrivateNetwork(true)
      .allowMethods(static_cast<http::MethodBmp>(http::Method::GET));

  auto raw = BuildRawHttp11(http::OPTIONS, "/pnet",
                            "Origin: https://pnet.example\r\n"
                            "Access-Control-Request-Method: GET\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowPrivateNetwork), "true");
}

TEST_F(CorsPolicyTest, NotCorsWhenNoOrigin) {
  policy.allowAnyOrigin();

  const auto status = parse(BuildRawHttp11(http::GET, "/noorigin"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
}

TEST_F(CorsPolicyTest, MaxAgeMustBeNonNegative) {
  EXPECT_THROW(policy.maxAge(std::chrono::seconds{-1}), std::invalid_argument);
}

TEST_F(CorsPolicyTest, ApplyOriginDeniedSetsForbidden) {
  policy.allowOrigin("https://allowed.example");

  const auto status = parse(BuildRawHttp11(http::GET, "/resource", "Origin: https://not.allowed\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::OriginDenied);
  EXPECT_EQ(response.status(), http::StatusCodeForbidden);
}

TEST_F(CorsPolicyTest, PreflightDeniedWhenPolicyAllowsNoMethods) {
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(0));

  auto raw = BuildRawHttp11(http::OPTIONS, "/files",
                            "Origin: https://any\r\n"
                            "Access-Control-Request-Method: GET\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyTest, PreflightWithRouteMethodsZeroIsDenied) {
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(http::Method::GET) |
                                       static_cast<http::MethodBmp>(http::Method::POST));

  auto raw = BuildRawHttp11(http::OPTIONS, "/files",
                            "Origin: https://any\r\n"
                            "Access-Control-Request-Method: GET\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request, static_cast<http::MethodBmp>(0));
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyTest, VaryWithEmptyValue) {
  policy.allowAnyOrigin();

  // existing Vary header with empty value
  response.headerAddLine(http::Vary, "");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
  EXPECT_TRUE(response.headerValue(http::Vary));
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "");
}

TEST_F(CorsPolicyTest, VaryAlreadyContainsOriginNotDuplicated) {
  policy.allowOrigin("https://api.example");

  const auto status = parse(BuildRawHttp11(http::GET, "/resource", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  // existing Vary already lists origin (lower-case), should not be duplicated
  response.headerAddLine(http::Vary, ",,Accept-Encoding, origin");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), ",,Accept-Encoding, origin");
}

TEST_F(CorsPolicyTest, ExposeHeaderDuplicatePrevention) {
  policy.allowOrigin("https://expose.example");
  policy.exposeHeader("X-Exposed");
  policy.exposeHeader(" X-Exposed ");

  const auto status = parse(BuildRawHttp11(http::GET, "/expose", "Origin: https://expose.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Exposed");
}

TEST_F(CorsPolicyTest, AllowRequestHeaderTrimmingAndDuplicates) {
  policy.allowOrigin("https://hdrs.example");
  policy.allowRequestHeader("  X-T  ");
  policy.allowRequestHeader("X-T");

  auto raw = BuildRawHttp11(http::OPTIONS, "/hdrs",
                            "Origin: https://hdrs.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers:  X-T  \r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-T");
}

TEST_F(CorsPolicyTest, PreflightOptionsWithoutOriginIsNotPreflight) {
  policy.allowAnyOrigin();

  // OPTIONS but no Origin header -> not a preflight request
  auto raw = BuildRawHttp11(http::OPTIONS, "/files");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::NotPreflight);
}

TEST_F(CorsPolicyTest, PreflightUnknownMethodTokenIsDenied) {
  policy.allowAnyOrigin().allowMethods(http::Method::GET);

  auto raw = BuildRawHttp11(http::OPTIONS, "/files",
                            "Origin: https://any\r\n"
                            "Access-Control-Request-Method: UNKNOWN_METHOD\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

// Methods are case-sensitive (RFC 9110 §9.1, and byte-case-sensitive in the Fetch CORS check): "delete" is not an
// allowed "DELETE".
TEST_F(CorsPolicyTest, PreflightMethodTokenIsCaseSensitive) {
  policy.allowAnyOrigin().allowMethods(http::Method::DELETE);

  for (std::string_view method : {"delete", "Delete", "DELETe"}) {
    std::string extraHeaders("Origin: https://any\r\nAccess-Control-Request-Method: ");
    extraHeaders.append(method).append("\r\n");
    ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files", extraHeaders)), http::StatusCodeOK);
    EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::MethodDenied) << method;
  }

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/files",
                                 "Origin: https://any\r\n"
                                 "Access-Control-Request-Method: DELETE\r\n")),
            http::StatusCodeOK);
  EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::Allowed);
}

TEST_F(CorsPolicyTest, RequestHeadersEmptyAfterTrimChecksEmptyAllowedList) {
  // leave allowed list empty
  policy.allowOrigin("https://hdrs.example");

  auto raw = BuildRawHttp11(http::OPTIONS, "/hdrs",
                            "Origin: https://hdrs.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers:   \r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  // presence of an explicitly empty Access-Control-Request-Headers should be treated as empty list => allow
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowHeaders));
}

TEST_F(CorsPolicyTest, RequestHeadersDoubleCommaSkipsEmptyToken) {
  policy.allowOrigin("https://hdrs.example");
  policy.allowRequestHeader("X-One");

  auto raw = BuildRawHttp11(http::OPTIONS, "/hdrs",
                            "Origin: https://hdrs.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers: X-One,,X-One\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-One");
}

TEST_F(CorsPolicyTest, RequestHeadersCanonicalizationProducesCanonicalList) {
  // Server allows no headers; client requests a messy list. The server should canonicalize
  // the requested list when echoing it back: trim tokens, skip empties, dedupe, and join with ", ".
  policy.allowOrigin("https://hdrs.example");

  auto raw = BuildRawHttp11(http::OPTIONS, "/hdrs",
                            "Origin: https://hdrs.example\r\n"
                            "Access-Control-Request-Method: GET\r\n"
                            "Access-Control-Request-Headers:  X-One, , X-Two, X-One , ,\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  // server allows no headers; requesting headers should be denied
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::HeadersDenied);
}

// allowAnyOrigin() with allowCredentials(true) mirrors the received origin. The opaque origin "null" (RFC 6454 §7.3),
// that any site can make a browser send (sandboxed iframe, data: URL), is not mirrored: granting it a credentialed
// response would let every site read it. It is answered like a denied origin (403), also for a preflight. An allow-list
// may still list it explicitly, and without credentials it gets the "*" of every origin.
TEST_F(CorsPolicyTest, AnyOriginWithCredentialsDoesNotGrantTheNullOrigin) {
  policy.allowAnyOrigin().allowCredentials(true);
  for (const std::string_view origin : {"null", "NULL"}) {
    ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/account", std::format("Origin: {}\r\n", origin))), http::StatusCodeOK);
    EXPECT_EQ(policy.wouldApply(request), CorsPolicy::ApplyStatus::OriginDenied) << origin;

    ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/account",
                                   std::format("Origin: {}\r\nAccess-Control-Request-Method: GET\r\n", origin))),
              http::StatusCodeOK);
    EXPECT_EQ(policy.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::OriginDenied) << origin;
  }

  // Other origins are still mirrored.
  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/account", "Origin: https://app.example\r\n")), http::StatusCodeOK);
  ASSERT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://app.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");

  CorsPolicy explicitNull;
  explicitNull.allowOrigin("null").allowCredentials(true);
  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/account", "Origin: null\r\n")), http::StatusCodeOK);
  EXPECT_EQ(explicitNull.wouldApply(request), CorsPolicy::ApplyStatus::Applied);

  CorsPolicy withoutCredentials;
  withoutCredentials.allowAnyOrigin();
  HttpResponse anyOriginResponse(http::StatusCodeOK);
  ASSERT_EQ(withoutCredentials.applyToResponse(request, anyOriginResponse), CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(anyOriginResponse.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
}

// Fetch, "CORS protocol and HTTP caches": a cache may serve the response to a request without Origin to a later CORS
// request, which the browser then blocks if it lacks Access-Control-Allow-Origin. When this header depends on the
// origin (an allow-list, or credentials, with which the origin is mirrored), the response to a request without Origin
// varies on Origin, an empty Origin counting as none. Origin is added once to an existing Vary, not at all to 'Vary:
// *'.
TEST_F(CorsPolicyTest, ResponseToRequestWithoutOriginVariesOnOrigin) {
  struct VaryCase {
    std::string_view handlerVary;  // null: none
    std::string_view expectedVary;
  };
  static constexpr VaryCase kVaryCases[]{
      {std::string_view(), "origin"},
      {"accept-encoding", "accept-encoding, origin"},
      {"Origin", "Origin"},
      {"accept-encoding, ORIGIN", "accept-encoding, ORIGIN"},
      {"*", "*"},
  };
  for (const bool credentials : {false, true}) {
    for (const std::string_view originHeader : {std::string_view{}, std::string_view{"Origin: \r\n"}}) {
      for (const VaryCase& varyCase : kVaryCases) {
        CorsPolicy allowList;
        allowList.allowOrigin("https://app.example").allowCredentials(credentials);
        CorsPolicy anyOriginWithCredentials;
        anyOriginWithCredentials.allowAnyOrigin().allowCredentials(true);
        for (const CorsPolicy* pPolicy : {&allowList, &anyOriginWithCredentials}) {
          ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/resource", originHeader)), http::StatusCodeOK);
          HttpResponse resp(http::StatusCodeOK);
          if (varyCase.handlerVary.data() != nullptr) {
            resp.headerAddLine(http::Vary, varyCase.handlerVary);
          }
          EXPECT_EQ(pPolicy->applyToResponse(request, resp), CorsPolicy::ApplyStatus::NotCors);
          EXPECT_EQ(resp.headerValueOrEmpty(http::Vary), varyCase.expectedVary);
          EXPECT_FALSE(resp.hasHeader(http::AccessControlAllowOrigin));
          EXPECT_FALSE(resp.hasHeader(http::AccessControlAllowCredentials));
        }
      }
    }
  }
}

// Without credentials, 'Access-Control-Allow-Origin: *' does not depend on the origin: it is sent to the requests
// without Origin too, with the exposed headers, so that the cached response is the one of a CORS request, without
// varying on Origin.
TEST_F(CorsPolicyTest, AnyOriginResponseToRequestWithoutOriginCarriesTheCorsHeaders) {
  policy.allowAnyOrigin().exposeHeader("x-total-count");

  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/resource", "")), http::StatusCodeOK);

  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::NotCors);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "x-total-count");
  EXPECT_FALSE(response.hasHeader(http::Vary));
  EXPECT_FALSE(response.hasHeader(http::AccessControlAllowCredentials));
}

// The 403 of a denied origin depends on the origin too. A preflight, answered by handlePreflight(), and a request to an
// inactive policy are left untouched.
TEST_F(CorsPolicyTest, OnlyResponsesDependingOnTheOriginVaryOnIt) {
  policy.allowOrigin("https://app.example");
  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/resource", "Origin: https://other.example\r\n")), http::StatusCodeOK);
  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::OriginDenied);
  EXPECT_EQ(response.status(), http::StatusCodeForbidden);
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), http::Origin);

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/resource",
                                 "Origin: https://app.example\r\nAccess-Control-Request-Method: GET\r\n")),
            http::StatusCodeOK);
  HttpResponse preflight(http::StatusCodeNoContent);
  EXPECT_EQ(policy.wouldApply(request), CorsPolicy::ApplyStatus::NotCors);
  EXPECT_EQ(policy.applyToResponse(request, preflight), CorsPolicy::ApplyStatus::NotCors);
  EXPECT_FALSE(preflight.hasHeader(http::Vary));

  const CorsPolicy inactive;
  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/resource", "Origin: https://other.example\r\n")), http::StatusCodeOK);
  EXPECT_EQ(inactive.wouldApply(request), CorsPolicy::ApplyStatus::NotCors);
  ASSERT_EQ(parse(BuildRawHttp11(http::GET, "/resource", "")), http::StatusCodeOK);
  EXPECT_EQ(inactive.wouldApply(request), CorsPolicy::ApplyStatus::NotCors);
  HttpResponse notCors(http::StatusCodeOK);
  EXPECT_EQ(inactive.applyToResponse(request, notCors), CorsPolicy::ApplyStatus::NotCors);
  EXPECT_FALSE(notCors.hasHeader(http::Vary));
  EXPECT_FALSE(notCors.hasHeader(http::AccessControlAllowOrigin));
}

// Fetch, CORS-preflight fetch, step 7: "authorization" is a CORS non-wildcard request-header name, which
// "Access-Control-Allow-Headers: *" never covers: it is listed next to the wildcard when requested, whatever its case.
TEST_F(CorsPolicyTest, AnyRequestHeadersListsAuthorization) {
  policy.allowAnyOrigin().allowAnyRequestHeaders();

  struct HeadersCase {
    std::string_view requested;
    std::string_view allowed;
  };
  static constexpr HeadersCase kCases[]{
      {"authorization", "*, authorization"},
      {"x-foo, Authorization", "*, authorization"},
      {"x-foo, x-bar", "*"},
      {"", "*"},
  };
  for (const HeadersCase& headersCase : kCases) {
    ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/api",
                                   std::format("Origin: https://app.example\r\nAccess-Control-Request-Method: GET\r\n"
                                               "Access-Control-Request-Headers: {}\r\n",
                                               headersCase.requested))),
              http::StatusCodeOK);

    const auto result = policy.handlePreflight(request);
    ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed) << headersCase.requested;
    EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), headersCase.allowed)
        << headersCase.requested;
  }
}

// Same step: for a request with credentials, "*" in Access-Control-Allow-Headers is not a wildcard but a header named
// "*". With credentials, the requested names are listed (they are allowed), none when none are requested.
TEST_F(CorsPolicyTest, AnyRequestHeadersWithCredentialsListsTheRequestedHeaders) {
  policy.allowOrigin("https://app.example").allowCredentials(true).allowAnyRequestHeaders();

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/api",
                                 "Origin: https://app.example\r\nAccess-Control-Request-Method: GET\r\n"
                                 "Access-Control-Request-Headers:  x-request-id,authorization \r\n")),
            http::StatusCodeOK);

  auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "x-request-id,authorization");

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/api",
                                 "Origin: https://app.example\r\nAccess-Control-Request-Method: PUT\r\n")),
            http::StatusCodeOK);
  policy.allowMethods(http::Method::PUT);
  result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowHeaders));
}

// Any request header is any header name: a requested element that is not a token is denied, so that the list can be
// sent back as is.
TEST_F(CorsPolicyTest, AnyRequestHeadersDeniesNamesThatAreNotTokens) {
  for (const bool credentials : {false, true}) {
    CorsPolicy anyHeaders;
    anyHeaders.allowOrigin("https://app.example").allowCredentials(credentials).allowAnyRequestHeaders();
    for (std::string_view requested : {"x-ok, x bad", "x-ok, (x)", "x-ok, x@y"}) {
      ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/api",
                                     std::format("Origin: https://app.example\r\nAccess-Control-Request-Method: GET\r\n"
                                                 "Access-Control-Request-Headers: {}\r\n",
                                                 requested))),
                http::StatusCodeOK);
      EXPECT_EQ(anyHeaders.handlePreflight(request).status, CorsPolicy::PreflightResult::Status::HeadersDenied)
          << requested;
    }
  }
}

// The requested header names are never reflected for a configured list (the list is sent), a received Origin is only
// mirrored when it is in the allow-list (or is a valid field value, with credentials), and a mirrored origin always
// comes with "Vary: Origin".
TEST_F(CorsPolicyTest, MirroredOriginAlwaysVariesAndIsNeverInjected) {
  policy.allowOrigin("https://app.example").allowRequestHeader("x-one");

  ASSERT_EQ(parse(BuildRawHttp11(http::OPTIONS, "/api",
                                 "Origin: https://APP.example\r\nAccess-Control-Request-Method: GET\r\n"
                                 "Access-Control-Request-Headers: X-One\r\n")),
            http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "x-one");
  EXPECT_EQ(result.response.headerValueOrEmpty(http::Vary), http::Origin);
  EXPECT_FALSE(result.response.hasHeader(http::AccessControlAllowCredentials));
}

}  // namespace aeronet
