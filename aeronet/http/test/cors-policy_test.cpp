#include "aeronet/cors-policy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {
namespace {

RawChars BuildRaw(std::string_view method, std::string_view target, std::string_view extraHeaders = {}) {
  RawChars raw;
  raw.append(method);
  raw.push_back(' ');
  raw.append(target);
  raw.append(" HTTP/1.1\r\n");
  raw.append("Host: example\r\n");
  raw.append(extraHeaders);
  raw.append(http::CRLF);
  return raw;
}

}  // namespace

class CorsPolicyTest : public ::testing::Test {
 protected:
  http::StatusCode parse(RawChars raw) {
    connState.inBuffer = std::move(raw);
    RawChars tmp;
    return request.initTrySetHead(connState, tmp, 4096U, true, nullptr);
  }

  HttpRequest request;
  ConnectionState connState;
  CorsPolicy policy;
  HttpResponse response;
};

TEST_F(CorsPolicyTest, DefaultConstructedShouldNotBeActive) {
  EXPECT_EQ(policy.applyToResponse(request, response), CorsPolicy::ApplyStatus::NotCors);

  CorsPolicy::PreflightResult expected;
  expected.status = CorsPolicy::PreflightResult::Status::NotPreflight;
  expected.response = HttpResponse{http::StatusCodeNoContent};

  CorsPolicy::PreflightResult actual = policy.handlePreflight(request);

  EXPECT_EQ(actual.status, expected.status);
  EXPECT_EQ(actual.response.status(), expected.response.status());
}

TEST_F(CorsPolicyTest, ApplyAnyOriginSimpleRequest) {
  policy.allowAnyOrigin();

  const auto status = parse(BuildRaw(http::GET, "/resource", "Origin: https://example.com\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), std::string_view{});
}

TEST_F(CorsPolicyTest, AllowEmptyOrigin) {
  policy.allowOrigin("");

  const auto status = parse(BuildRaw(http::GET, "/resource", "Origin: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "");
}

TEST_F(CorsPolicyTest, AllowAlreadyExistingOrigin) {
  policy.allowOrigin("https://api.example");
  policy.allowOrigin("\thttps://api.example  ");  // should be no duplicate effect
  policy.allowOrigin("https://API.EXAMPLE");      // case-insensitive match, no duplicate

  const auto status = parse(BuildRaw(http::GET, "/items", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://api.example");
}

TEST_F(CorsPolicyTest, ApplyAllowListMirrorsOriginAndAddsCredentials) {
  policy.allowOrigin("https://api.example").allowCredentials(true);

  const auto status = parse(BuildRaw(http::GET, "/items", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  response.addHeader(http::Vary, "Accept-Encoding");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://api.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "Accept-Encoding, Origin");
}

TEST_F(CorsPolicyTest, PreflightWithEmptyAccessControlRequestHeaders) {
  const auto status =
      parse(BuildRaw(http::OPTIONS, "/files",
                     "Access-Control-Request-Method: GET\r\nOrigin: test\r\nAccess-Control-Request-Headers:  \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.headerValue(http::AccessControlAllowHeaders).has_value());
}

TEST_F(CorsPolicyTest, PreflightWithAccessControlHeaders) {
  const auto status = parse(
      BuildRaw(http::OPTIONS, "/files",
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
  const auto status =
      parse(BuildRaw(http::OPTIONS, "/files",
                     "Access-Control-Request-Method: GET\r\nOrigin: test\r\nAccess-Control-Request-Headers: \r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");
  policy.allowMethods(http::Method::GET);
  policy.allowRequestHeader("X-Test");

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.headerValue(http::AccessControlAllowHeaders).has_value());
}

TEST_F(CorsPolicyTest, PreflightNoAccessControlRequestHeaders) {
  const auto status =
      parse(BuildRaw(http::OPTIONS, "/files", "Access-Control-Request-Method: GET\r\nOrigin: test\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  policy.allowOrigin("test");
  policy.allowMethods(http::Method::GET);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.headerValue(http::AccessControlAllowHeaders).has_value());
}

TEST_F(CorsPolicyTest, PreflightAllowed) {
  policy.allowOrigin("https://example.com")
      .allowMethods(static_cast<http::MethodBmp>(http::Method::GET) | static_cast<http::MethodBmp>(http::Method::POST))
      .allowRequestHeader("X-Trace")
      .maxAge(std::chrono::seconds{600});

  const auto status = parse(BuildRaw(http::OPTIONS, "/files",
                                     "Origin: https://example.com\r\n"
                                     "Access-Control-Request-Method: POST\r\n"
                                     "Access-Control-Request-Headers: X-Trace\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  const auto& response = result.response;
  EXPECT_EQ(response.status(), http::StatusCodeNoContent);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://example.com");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowMethods), "GET, POST");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-Trace");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlMaxAge), "600");
}

TEST_F(CorsPolicyTest, PreflightDeniedUnknownMethod) {
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(http::Method::GET));

  const auto status = parse(BuildRaw(http::OPTIONS, "/files",
                                     "Origin: https://any\r\n"
                                     "Access-Control-Request-Method: POST\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyTest, PreflightDeniedForHeaders) {
  policy.allowOrigin("https://example.com").allowRequestHeader("X-One");

  const auto status = parse(BuildRaw(http::OPTIONS, "/files",
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
  auto raw = BuildRaw(http::OPTIONS, "/multi",
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

  auto raw = BuildRaw(http::OPTIONS, "/hdrs",
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

  auto raw = BuildRaw(http::OPTIONS, "/anyhdr",
                      "Origin: https://anyhdr.example\r\n"
                      "Access-Control-Request-Method: GET\r\n"
                      "Access-Control-Request-Headers: X-Foo, X-Bar\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_EQ(result.response.headerValueOrEmpty(http::AccessControlAllowHeaders), "*");
}

TEST_F(CorsPolicyTest, ExposeHeadersAndVaryMerging) {
  policy.allowOrigin("https://expose.example");
  policy.exposeHeader("X-Exposed");
  policy.exposeHeader(" \t");  // should have no effect

  const auto status = parse(BuildRaw(http::GET, "/expose", "Origin: https://expose.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  response.addHeader(http::Vary, "Accept-Encoding");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Exposed");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "Accept-Encoding, Origin");
}

TEST_F(CorsPolicyTest, WildcardOriginWithCredentialsMirrorsOrigin) {
  policy.allowAnyOrigin().allowCredentials(true);

  const auto status = parse(BuildRaw(http::GET, "/cred", "Origin: https://cred.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  // with credentials enabled, wildcard origin should not be used; origin must be mirrored
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://cred.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
}

TEST_F(CorsPolicyTest, PreflightPrivateNetworkHeaderEmitted) {
  policy.allowOrigin("https://pnet.example")
      .allowPrivateNetwork(true)
      .allowMethods(static_cast<http::MethodBmp>(http::Method::GET));

  auto raw = BuildRaw(http::OPTIONS, "/pnet",
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

  const auto status = parse(BuildRaw(http::GET, "/noorigin"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
}

TEST_F(CorsPolicyTest, MaxAgeMustBeNonNegative) {
  EXPECT_THROW(policy.maxAge(std::chrono::seconds{-1}), std::invalid_argument);
}

TEST_F(CorsPolicyTest, ApplyOriginDeniedSetsForbidden) {
  policy.allowOrigin("https://allowed.example");

  const auto status = parse(BuildRaw(http::GET, "/resource", "Origin: https://not.allowed\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::OriginDenied);
  EXPECT_EQ(response.status(), http::StatusCodeForbidden);
  EXPECT_EQ(response.body(), http::ReasonForbidden);
}

TEST_F(CorsPolicyTest, PreflightDeniedWhenPolicyAllowsNoMethods) {
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(0));

  auto raw = BuildRaw(http::OPTIONS, "/files",
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

  auto raw = BuildRaw(http::OPTIONS, "/files",
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
  response.addHeader(http::Vary, "");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
  EXPECT_TRUE(response.headerValue(http::Vary));
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "");
}

TEST_F(CorsPolicyTest, VaryAlreadyContainsOriginNotDuplicated) {
  policy.allowOrigin("https://api.example");

  const auto status = parse(BuildRaw(http::GET, "/resource", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  // existing Vary already lists origin (lower-case), should not be duplicated
  response.addHeader(http::Vary, ",,Accept-Encoding, origin");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), ",,Accept-Encoding, origin");
}

TEST_F(CorsPolicyTest, ExposeHeaderDuplicatePrevention) {
  policy.allowOrigin("https://expose.example");
  policy.exposeHeader("X-Exposed");
  policy.exposeHeader(" X-Exposed ");

  const auto status = parse(BuildRaw(http::GET, "/expose", "Origin: https://expose.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Exposed");
}

TEST_F(CorsPolicyTest, AllowRequestHeaderTrimmingAndDuplicates) {
  policy.allowOrigin("https://hdrs.example");
  policy.allowRequestHeader("  X-T  ");
  policy.allowRequestHeader("X-T");

  auto raw = BuildRaw(http::OPTIONS, "/hdrs",
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
  auto raw = BuildRaw(http::OPTIONS, "/files");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::NotPreflight);
}

TEST_F(CorsPolicyTest, PreflightUnknownMethodTokenIsDenied) {
  policy.allowAnyOrigin().allowMethods(http::Method::GET);

  auto raw = BuildRaw(http::OPTIONS, "/files",
                      "Origin: https://any\r\n"
                      "Access-Control-Request-Method: UNKNOWN_METHOD\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyTest, RequestHeadersEmptyAfterTrimChecksEmptyAllowedList) {
  // leave allowed list empty
  policy.allowOrigin("https://hdrs.example");

  auto raw = BuildRaw(http::OPTIONS, "/hdrs",
                      "Origin: https://hdrs.example\r\n"
                      "Access-Control-Request-Method: GET\r\n"
                      "Access-Control-Request-Headers:   \r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  // presence of an explicitly empty Access-Control-Request-Headers should be treated as empty list => allow
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::Allowed);
  EXPECT_FALSE(result.response.headerValue(http::AccessControlAllowHeaders).has_value());
}

TEST_F(CorsPolicyTest, RequestHeadersDoubleCommaSkipsEmptyToken) {
  policy.allowOrigin("https://hdrs.example");
  policy.allowRequestHeader("X-One");

  auto raw = BuildRaw(http::OPTIONS, "/hdrs",
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

  auto raw = BuildRaw(http::OPTIONS, "/hdrs",
                      "Origin: https://hdrs.example\r\n"
                      "Access-Control-Request-Method: GET\r\n"
                      "Access-Control-Request-Headers:  X-One, , X-Two, X-One , ,\r\n");
  const auto status = parse(raw);
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  // server allows no headers; requesting headers should be denied
  ASSERT_EQ(result.status, CorsPolicy::PreflightResult::Status::HeadersDenied);
}

}  // namespace aeronet
