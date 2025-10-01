// Verifies that moving a TLS+ALPN configured HttpServer prior to running preserves
// a valid TLS context and ALPN callback pointer. This specifically guards against
// the prior design where TlsContext was stored by value (e.g. inside std::optional):
// a move of HttpServer could relocate the TlsContext object while the OpenSSL
// SSL_CTX ALPN selection callback still held the old address -> use-after-free /
// crash during handshake. The current design stores TlsContext behind a stable
// std::unique_ptr, so the address observed by OpenSSL remains valid after moves.
//
// This test would (non-deterministically) fail or ASan-crash under the old design
// when compiled with sanitizers and run enough times, especially under load, but
// here we simply assert successful handshake + ALPN negotiation after a move.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"
#include "test_tls_client.hpp"
#include "test_tls_helper.hpp"

TEST(HttpTlsMoveAlpn, MoveConstructBeforeRunMaintainsAlpnHandshake) {
  auto pair = aeronet::test::makeEphemeralCertKey();
  ASSERT_FALSE(pair.first.empty());
  ASSERT_FALSE(pair.second.empty());

  aeronet::HttpServerConfig cfg;
  cfg.withTlsCertKeyMemory(pair.first, pair.second);
  cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer both; client will request http/1.1 only

  aeronet::HttpServer original(cfg);
  original.setHandler([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200)
        .reason("OK")
        .contentType(aeronet::http::ContentTypeTextPlain)
        .body(std::string("MOVEALPN:") + (req.alpnProtocol().empty() ? "-" : std::string(req.alpnProtocol())));
  });

  auto port = original.port();
  aeronet::HttpServer moved(std::move(original));

  std::atomic_bool stop{false};
  std::jthread th([&] { moved.runUntil([&] { return stop.load(); }); });

  // Actively wait until the listening socket accepts a plain TCP connection to avoid race.
  // This replicates TestServer readiness logic without duplicating its wrapper.
  {
    aeronet::test::ClientConnection probe(port, std::chrono::milliseconds{500});
  }

  TlsClient::Options opts;
  opts.alpn = {"http/1.1"};
  TlsClient client(port, opts);
  ASSERT_TRUE(client.handshakeOk()) << "TLS handshake failed after move (potential stale TlsContext pointer)";
  auto raw = client.get("/moved");
  stop.store(true);

  ASSERT_NE(raw.find("HTTP/1.1 200"), std::string::npos);
  ASSERT_NE(raw.find("MOVEALPN:http/1.1"), std::string::npos) << raw;
}
