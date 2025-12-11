#pragma once

#include <chrono>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_tls_helper.hpp"

namespace aeronet::test {

// TLS-enabled variant of TestServer that auto-generates an ephemeral certificate/key
// for each test instance and optionally configures ALPN protocols or applies additional
// user-supplied mutations to the HttpServerConfig before launch.
//
// Usage:
//   TlsTestServer ts; // basic TLS (no ALPN)
//   TlsTestServer ts({"http/1.1"}); // with ALPN preference list
//   TlsTestServer ts({}, [](HttpServerConfig& cfg){ cfg.withMaxRequestsPerConnection(5); });
//   ts.router().setDefault(...);
//
// Exposes the same interface expectations as TestServer (ts.server, ts.port(), ts.stop()).
struct TlsTestServer {
  TestServer server;  // underlying generic test server (already RAII-managed)

  using Mutator = std::function<void(HttpServerConfig&)>;

  static HttpServerConfig makeConfig(std::initializer_list<std::string_view> alpn, const Mutator& mut) {
    HttpServerConfig cfg;  // ephemeral port by default
    auto pair = MakeEphemeralCertKey();
    cfg.withTlsCertKeyMemory(pair.first, pair.second);
    if (alpn.size() != 0) {
      cfg.withTlsAlpnProtocols(alpn);
    }
    if (mut) {
      mut(cfg);
    }
    return cfg;
  }

  explicit TlsTestServer(std::initializer_list<std::string_view> alpn = {}, const Mutator& mut = nullptr,
                         std::chrono::milliseconds poll = std::chrono::milliseconds{50})
      : server(makeConfig(alpn, mut), RouterConfig{}, poll) {}

  [[nodiscard]] uint16_t port() const { return server.port(); }
  void stop() { server.stop(); }

  // Forward selected SingleHttpServer APIs for convenience to reduce nested server.server noise.
  RouterUpdateProxy router() { return server.router(); }

  void setDefault(RequestHandler handler) { router().setDefault(std::move(handler)); }
  void setDefault(StreamingHandler handler) { router().setDefault(std::move(handler)); }

  template <typename ErrCb>
  void setParserErrorCallback(ErrCb&& cb) {
    server.server.setParserErrorCallback(std::forward<ErrCb>(cb));
  }
  [[nodiscard]] auto stats() const { return server.server.stats(); }
  TestHttpServer& http() { return server.server; }
};

}  // namespace aeronet::test