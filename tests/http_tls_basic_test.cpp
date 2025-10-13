#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

TEST(HttpTlsBasic, HandshakeAndSimpleGet) {
  // Prepare config with in-memory self-signed cert/key
  aeronet::test::TlsTestServer ts;  // ephemeral TLS server
  ts.setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("TLS OK ") + std::string(req.path()));
  });
  aeronet::test::TlsClient client(ts.port());
  auto raw = client.get("/hello", {{"X-Test", "tls"}});
  ASSERT_FALSE(raw.empty());
  ASSERT_NE(std::string::npos, raw.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, raw.find("TLS OK /hello"));
}

TEST(HttpTlsBasic, LargePayload) {
  std::string largeBody(1 << 24, 'a');
  // Prepare config with in-memory self-signed cert/key
  aeronet::test::TlsTestServer ts({"http/1.1"}, [&](aeronet::HttpServerConfig& cfg) {
    cfg.maxOutboundBufferBytes = largeBody.size() + 512;  // +512 for headers
    cfg.keepAliveTimeout = std::chrono::years(1);
  });
  ts.setDefault([&largeBody]([[maybe_unused]] const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body(largeBody);
  });
  aeronet::test::TlsClient client(ts.port());
  auto raw = client.get("/hello", {{"X-Test", "tls"}});
  ASSERT_FALSE(raw.empty());
  EXPECT_TRUE(raw.contains("HTTP/1.1 200"));
  EXPECT_TRUE(raw.contains(largeBody));
}