#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/test_server_tls_fixture.hpp"
#include "aeronet/test_tls_client.hpp"
#include "aeronet/test_util.hpp"

namespace {
// Helper: perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.
bool attemptPlainHttp(auto port) {
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  if (!aeronet::test::sendAll(fd, bogus)) {
    return false;
  }
  return aeronet::test::recvWithTimeout(fd).empty();  // success condition (auto close by RAII)
}

// Large response GET using TlsClient (simplified replacement).
std::string tlsGetLarge(auto port) {
  aeronet::test::TlsClient client(port);
  if (!client.handshakeOk()) {
    return {};
  }
  return client.get("/large");
}
}  // namespace

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  aeronet::test::TlsTestServer ts;  // default TLS (no ALPN needed here)
  ASSERT_TRUE(attemptPlainHttp(ts.port()));
}

TEST(HttpTlsNegative, LargeResponseFragmentation) {
  aeronet::test::TlsTestServer ts;  // basic TLS
  auto port = ts.port();
  ts.setDefault([](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(200, "OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string(300000, 'A'));
  });
  std::string resp = tlsGetLarge(port);
  // helper freed temporary key/cert
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("AAAA"));
}
