#pragma once

#ifdef AERONET_ENABLE_HTTP2

#include <chrono>
#include <functional>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/test_server_fixture.hpp"

namespace aeronet::test {

/// TLS-enabled HTTP/2 test server fixture.
///
/// Features:
///  * Automatic ephemeral TLS certificate generation
///  * HTTP/2 protocol support (ALPN "h2")
///  * Unified handler works for both HTTP/1.1 and HTTP/2
///
/// Usage:
///   TlsHttp2TestServer ts;
///   ts.setDefault([](const HttpRequest& req) {
///     return HttpResponse("Hello HTTP/2!");
///   });
struct TlsHttp2TestServer {
  using Mutator = std::function<void(HttpServerConfig&)>;
  using Http2Mutator = std::function<void(Http2Config&)>;
  using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

  TlsHttp2TestServer() noexcept {
    server.postConfigUpdate([](HttpServerConfig& cfg) {
      const auto pair = MakeEphemeralCertKey();
      cfg.withTlsCertKeyMemory(pair.first, pair.second);

      cfg.withTlsAlpnProtocols({"h2"});
      cfg.withHttp2(Http2Config{.enable = true});
    });
  }

  [[nodiscard]] uint16_t port() const { return server.port(); }
  void stop() { server.stop(); }

  /// Set the unified request handler (works for both HTTP/1.1 and HTTP/2)
  void setDefault(RequestHandler handler) { server.server.router().setDefault(std::move(handler)); }

  [[nodiscard]] auto stats() const { return server.server.stats(); }
  SingleHttpServer& http() { return server.server; }

  TestServer server;
};

}  // namespace aeronet::test

#endif
