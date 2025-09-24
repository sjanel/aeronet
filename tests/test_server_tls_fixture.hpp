#pragma once
#include <chrono>
#include <functional>
#include <utility>
#include <vector>

#include "aeronet/server-config.hpp"
#include "test_server_fixture.hpp"
#include "test_tls_helper.hpp"

// TLS-enabled variant of TestServer that auto-generates an ephemeral certificate/key
// for each test instance and optionally configures ALPN protocols or applies additional
// user-supplied mutations to the ServerConfig before launch.
//
// Usage:
//   TlsTestServer ts; // basic TLS (no ALPN)
//   TlsTestServer ts({"http/1.1"}); // with ALPN preference list
//   TlsTestServer ts({}, [](ServerConfig& cfg){ cfg.withMaxRequestsPerConnection(5); });
//   ts.server.setHandler(...);
//
// Exposes the same interface expectations as TestServer (ts.server, ts.port(), ts.stop()).
struct TlsTestServer {
  TestServer server;  // underlying generic test server (already RAII-managed)

  using Mutator = std::function<void(aeronet::ServerConfig&)>;

  static aeronet::ServerConfig makeConfig(const std::vector<std::string>& alpn, const Mutator& mut) {
    aeronet::ServerConfig cfg;  // ephemeral port by default
    auto pair = aeronet::test::makeEphemeralCertKey();
    cfg.withTlsCertKeyMemory(pair.first, pair.second);
    if (!alpn.empty()) {
      cfg.withTlsAlpnProtocols(alpn);
    }
    if (mut) {
      mut(cfg);
    }
    return cfg;
  }

  explicit TlsTestServer(const std::vector<std::string>& alpn = {}, const Mutator& mut = nullptr,
                         std::chrono::milliseconds poll = std::chrono::milliseconds{50})
      : server(makeConfig(alpn, mut), poll) {}

  uint16_t port() const { return server.port(); }
  void stop() { server.stop(); }

  // Forward selected HttpServer APIs for convenience to reduce nested server.server noise.
  template <typename Handler>
  void setHandler(Handler&& handler) {
    server.server.setHandler(std::forward<Handler>(handler));
  }
  template <typename StreamHandler>
  void setStreamingHandler(StreamHandler&& handler) {
    server.server.setStreamingHandler(std::forward<StreamHandler>(handler));
  }
  template <typename ErrCb>
  void setParserErrorCallback(ErrCb&& cb) {
    server.server.setParserErrorCallback(std::forward<ErrCb>(cb));
  }
  auto stats() const { return server.server.stats(); }
  aeronet::HttpServer& http() { return server.server; }
};
