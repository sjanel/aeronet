#include "aeronet/cors-policy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "connection-state.hpp"
#include "raw-chars.hpp"

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

class CorsPolicyHarness : public ::testing::Test {
 protected:
  http::StatusCode parse(RawChars raw) {
    connState.inBuffer = std::move(raw);
    RawChars tmp;
    return request.initTrySetHead(connState, tmp, 4096U, true, nullptr);
  }

  HttpRequest request;
  ConnectionState connState;
};

TEST_F(CorsPolicyHarness, ApplyAnyOriginSimpleRequest) {
  CorsPolicy policy;
  policy.allowAnyOrigin();

  const auto status = parse(BuildRaw(http::GET, "/resource", "Origin: https://example.com\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  HttpResponse response;
  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "*");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), std::string_view{});
}

TEST_F(CorsPolicyHarness, ApplyAllowListMirrorsOriginAndAddsCredentials) {
  CorsPolicy policy;
  policy.allowOrigin("https://api.example").allowCredentials(true);

  const auto status = parse(BuildRaw(http::GET, "/items", "Origin: https://api.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  HttpResponse response;
  response.addCustomHeader(http::Vary, "Accept-Encoding");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://api.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "Accept-Encoding, Origin");
}

TEST_F(CorsPolicyHarness, PreflightAllowed) {
  CorsPolicy policy;
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
  EXPECT_EQ(response.statusCode(), http::StatusCodeNoContent);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://example.com");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowMethods), "GET, POST");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowHeaders), "X-Trace");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlMaxAge), "600");
}

TEST_F(CorsPolicyHarness, PreflightDeniedUnknownMethod) {
  CorsPolicy policy;
  policy.allowAnyOrigin().allowMethods(static_cast<http::MethodBmp>(http::Method::GET));

  const auto status = parse(BuildRaw(http::OPTIONS, "/files",
                                     "Origin: https://any\r\n"
                                     "Access-Control-Request-Method: POST\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::MethodDenied);
}

TEST_F(CorsPolicyHarness, PreflightDeniedForHeaders) {
  CorsPolicy policy;
  policy.allowOrigin("https://example.com").allowRequestHeader("X-One");

  const auto status = parse(BuildRaw(http::OPTIONS, "/files",
                                     "Origin: https://example.com\r\n"
                                     "Access-Control-Request-Method: GET\r\n"
                                     "Access-Control-Request-Headers: X-One, X-Two\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  const auto result = policy.handlePreflight(request);
  EXPECT_EQ(result.status, CorsPolicy::PreflightResult::Status::HeadersDenied);
}

TEST_F(CorsPolicyHarness, MultipleAllowedOriginsAndMethods) {
  CorsPolicy policy;
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

TEST_F(CorsPolicyHarness, MultipleAllowedRequestHeaders) {
  CorsPolicy policy;
  policy.allowOrigin("https://headers.example");
  policy.allowRequestHeader("X-One");
  policy.allowRequestHeader("X-Two");

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

TEST_F(CorsPolicyHarness, AllowAnyRequestHeadersAcceptsPreflight) {
  CorsPolicy policy;
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

TEST_F(CorsPolicyHarness, ExposeHeadersAndVaryMerging) {
  CorsPolicy policy;
  policy.allowOrigin("https://expose.example");
  policy.exposeHeader("X-Exposed");

  const auto status = parse(BuildRaw(http::GET, "/expose", "Origin: https://expose.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  HttpResponse response;
  response.addCustomHeader(http::Vary, "Accept-Encoding");

  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlExposeHeaders), "X-Exposed");
  EXPECT_EQ(response.headerValueOrEmpty(http::Vary), "Accept-Encoding, Origin");
}

TEST_F(CorsPolicyHarness, WildcardOriginWithCredentialsMirrorsOrigin) {
  CorsPolicy policy;
  policy.allowAnyOrigin().allowCredentials(true);

  const auto status = parse(BuildRaw(http::GET, "/cred", "Origin: https://cred.example\r\n"));
  ASSERT_EQ(status, http::StatusCodeOK);

  HttpResponse response;
  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::Applied);
  // with credentials enabled, wildcard origin should not be used; origin must be mirrored
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowOrigin), "https://cred.example");
  EXPECT_EQ(response.headerValueOrEmpty(http::AccessControlAllowCredentials), "true");
}

TEST_F(CorsPolicyHarness, PreflightPrivateNetworkHeaderEmitted) {
  CorsPolicy policy;
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

TEST_F(CorsPolicyHarness, NotCorsWhenNoOrigin) {
  CorsPolicy policy;
  policy.allowAnyOrigin();

  const auto status = parse(BuildRaw(http::GET, "/noorigin"));
  ASSERT_EQ(status, http::StatusCodeOK);

  HttpResponse response;
  const auto applyStatus = policy.applyToResponse(request, response);
  EXPECT_EQ(applyStatus, CorsPolicy::ApplyStatus::NotCors);
}

}  // namespace aeronet
