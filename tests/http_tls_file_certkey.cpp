// Test that the TLS context loads certificate/key from filesystem paths (not in-memory PEM).
#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "http-constants.hpp"
#include "test_server_fixture.hpp"  // reuse generic TestServer with manual config
#include "test_temp_file.hpp"
#include "test_tls_client.hpp"
#include "test_tls_helper.hpp"  // for generating ephemeral cert/key

TEST(HttpTlsFileCertKey, HandshakeSucceedsUsingFileBasedCertAndKey) {
  auto pair = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());
  // Write both to temp files
  auto certFile = TempFile::createWithContent("aeronet_cert_", pair.first);
  auto keyFile = TempFile::createWithContent("aeronet_key_", pair.second);
  ASSERT_TRUE(certFile.valid());
  ASSERT_TRUE(keyFile.valid());

  aeronet::HttpServerConfig cfg;
  cfg.withTlsCertKey(certFile.path(), keyFile.path());  // file-based path (not memory)
  cfg.withTlsAlpnProtocols({"http/1.1"});
  // Use plain TestServer since we manually set config
  TestServer server(cfg, std::chrono::milliseconds{50});
  server.server.setHandler([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200)
        .reason("OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("FILETLS-") + std::string(req.alpnProtocol().empty() ? "-" : req.alpnProtocol()));
  });
  uint16_t port = server.port();

  TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk());
  auto resp = client.get("/file");
  server.stop();
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
  ASSERT_NE(resp.find("FILETLS-http/1.1"), std::string::npos);
}
