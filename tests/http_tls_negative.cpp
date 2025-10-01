#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/test_util.hpp"
#include "test_server_tls_fixture.hpp"
#include "test_tls_client.hpp"

namespace {
// Helper: perform a raw TCP connect and send cleartext HTTP to a TLS-only port -> should fail handshake quickly.
bool attemptPlainHttp(auto port) {
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string bogus = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // not TLS handshake
  (void)::send(fd, bogus.data(), bogus.size(), 0);
  // Expect server to close; read should return 0 or error
  char smallBuf[32];
  auto rr = ::read(fd, smallBuf, sizeof(smallBuf));
  return rr <= 0;  // success condition (auto close by RAII)
}

// Large response GET using TlsClient (simplified replacement).
std::string tlsGetLarge(auto port) {
  TlsClient client(port);
  if (!client.handshakeOk()) {
    return {};
  }
  return client.get("/large");
}
}  // namespace

TEST(HttpTlsNegative, PlainHttpToTlsPortRejected) {
  bool result = false;
  {
    TlsTestServer ts;  // default TLS (no ALPN needed here)
    result = attemptPlainHttp(ts.port());
    ts.stop();
  }
  // helper freed temporary key/cert
  ASSERT_TRUE(result);
}

TEST(HttpTlsNegative, LargeResponseFragmentation) {
  std::string resp;
  {
    TlsTestServer ts;  // basic TLS
    auto port = ts.port();
    ts.setHandler([](const aeronet::HttpRequest&) {
      return aeronet::HttpResponse(200)
          .reason("OK")
          .contentType(aeronet::http::ContentTypeTextPlain)
          .body(std::string(300000, 'A'));
    });
    resp = tlsGetLarge(port);
    ts.stop();
  }
  // helper freed temporary key/cert
  ASSERT_FALSE(resp.empty());
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
  ASSERT_NE(resp.find("AAAA"), std::string::npos);
}
