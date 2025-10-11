#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"

TEST(HttpTlsBasic, HandshakeAndSimpleGet) {
  // Prepare config with in-memory self-signed cert/key
  std::string raw;
  {
    aeronet::test::TlsTestServer ts;  // ephemeral TLS server
    ts.setDefault([](const aeronet::HttpRequest& req) {
      return aeronet::HttpResponse(200, "OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body(std::string("TLS OK ") + std::string(req.path()));
    });
    aeronet::test::TlsClient client(ts.port());
    raw = client.get("/hello", {{"X-Test", "tls"}});
    ts.stop();
  }
  // No explicit cleanup required: helper frees all allocated OpenSSL objects.
  ASSERT_FALSE(raw.empty());
  ASSERT_NE(std::string::npos, raw.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, raw.find("TLS OK /hello"));
}
