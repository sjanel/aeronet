// End-to-end coverage of the native HTTP/2 client engine against a live aeronet server:
// prior-knowledge h2c over plain http (HttpVersionMode::Http2), the Auto cleartext fallback to
// HTTP/1.1, and -- with OpenSSL -- ALPN negotiation over https for every HttpVersionMode.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/client-protocol.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-version.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <csignal>

#include "aeronet/test-tls-helper.hpp"
#endif

namespace aeronet {
namespace {

constexpr std::size_t kLargeBodySize = 1UL << 20;  // 1 MiB: many DATA frames, several flow-control windows

std::string MakeLargeBody() {
  std::string body(kLargeBodySize, '\0');
  for (std::size_t charPos = 0; charPos < body.size(); ++charPos) {
    body[charPos] = static_cast<char>('A' + (charPos % 53));
  }
  return body;
}

// Spins up a plain-HTTP aeronet server (h2c prior knowledge is enabled by default) with routes that
// exercise both directions of the exchange, and records the HTTP version + client address the server
// observed for the last request.
class HttpClientHttp2E2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    Router router;
    router.setPath(http::Method::GET, "/hello", [this](const HttpRequestView& req) {
      observe(req);
      return HttpResponse(http::StatusCodeOK, "world", "text/plain");
    });
    router.setPath(http::Method::HEAD, "/hello", [this](const HttpRequestView& req) {
      observe(req);
      return HttpResponse(http::StatusCodeOK, "world", "text/plain");
    });
    router.setPath(http::Method::POST, "/echo", [this](const HttpRequestView& req) {
      observe(req);
      return HttpResponse(http::StatusCodeOK, req.body(), "application/test");
    });
    router.setPath(http::Method::GET, "/big", [this](const HttpRequestView& req) {
      observe(req);
      return HttpResponse(http::StatusCodeOK, MakeLargeBody(), "application/octet-stream");
    });
    router.setPath(http::Method::GET, "/redirect", [this](const HttpRequestView& req) {
      observe(req);
      HttpResponse resp(http::StatusCodeFound);
      resp.location("/hello");
      return resp;
    });
    router.setPath(http::Method::POST, "/see-other", [this](const HttpRequestView& req) {
      observe(req);
      HttpResponse resp(http::StatusCodeSeeOther);
      resp.location("/hello");
      return resp;
    });
    router.setPath(http::Method::GET, "/headers", [this](const HttpRequestView& req) {
      observe(req);
      HttpResponse resp(http::StatusCodeOK, "hdr", "text/plain");
      resp.header("x-echoed", req.headerValueOrEmpty("x-custom-token"));
      resp.header("x-server", "aeronet");
      return resp;
    });
    // 303 See Other -> rewrite to GET /headers with the request body dropped. A user header set on the
    // original POST must survive the drop-body rewrite and reach /headers (where it is reflected back).
    router.setPath(http::Method::POST, "/see-other-headers", [this](const HttpRequestView& req) {
      observe(req);
      HttpResponse resp(http::StatusCodeSeeOther);
      resp.location("/headers");
      return resp;
    });

    _server = std::make_unique<SingleHttpServer>(
        HttpServerConfig{}.withPort(0).withPollInterval(std::chrono::milliseconds{20}), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  // NOTE: HttpRequestView::clientAddress() is not usable here: requests dispatched through the HTTP/2
  // handler carry no owner state (pre-existing server-side gap), so only the version is recorded.
  void observe(const HttpRequestView& req) {
    _lastSeenHttp2.store(req.version() == http::HTTP_2_0, std::memory_order_relaxed);
  }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(_port) + std::string(path);
  }

  [[nodiscard]] static HttpClient MakeHttp2Client() {
    return HttpClient(HttpClientConfig{}.withHttpVersion(HttpVersionMode::Http2));
  }

  std::unique_ptr<SingleHttpServer> _server;
  std::atomic<bool> _lastSeenHttp2{false};
  uint16_t _port{0};
};

}  // namespace

TEST_F(HttpClientHttp2E2ETest, PriorKnowledgeSimpleGet) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
  EXPECT_TRUE(_lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST_F(HttpClientHttp2E2ETest, AutoModeStaysHttp11OnCleartext) {
  HttpClient client;  // default HttpVersionMode::Auto
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
  EXPECT_FALSE(_lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST_F(HttpClientHttp2E2ETest, Http11ModeStillWorks) {
  HttpClient client(HttpClientConfig{}.withHttpVersion(HttpVersionMode::Http1_1));
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_FALSE(_lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST_F(HttpClientHttp2E2ETest, NotFoundIsAResponseNotAnError) {
  HttpClient client = MakeHttp2Client();
  // No route matches, so observe() never runs: only the status is asserted.
  auto resp = client.get(url("/does-not-exist")).value();
  EXPECT_EQ(resp.status(), 404);
}

TEST_F(HttpClientHttp2E2ETest, PostEchoSmallBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(url("/echo"), "ping", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "ping");
}

TEST_F(HttpClientHttp2E2ETest, PostEchoLargeBodyExercisesSendFlowControl) {
  // 1 MiB upload: far beyond the default 65535-byte stream window, so the engine must repeatedly stall
  // on flow control and resume on the server's WINDOW_UPDATEs.
  const std::string payload = MakeLargeBody();
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(url("/echo"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), payload);
}

TEST_F(HttpClientHttp2E2ETest, LargeResponseBodyReassembledAcrossDataFrames) {
  // Decompression off => no Accept-Encoding => the server must ship the raw 1 MiB: it exceeds the
  // client's 65535-byte initial stream window, so the server defers behind flow control and resumes on
  // the client's automatic WINDOW_UPDATEs.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withDecompression(false);
  HttpClient client(cfg);
  auto resp = client.get(url("/big")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), MakeLargeBody());
}

TEST_F(HttpClientHttp2E2ETest, TransparentResponseDecompression) {
  // Default client: Accept-Encoding advertised, the (highly repetitive) 1 MiB body is compressed by the
  // server and transparently decoded by the client, dropping the Content-Encoding header.
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(url("/big")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), MakeLargeBody());
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());
}

TEST_F(HttpClientHttp2E2ETest, KeepAliveDisabledReconnectsPerRequest) {
  // Without keep-alive every exchange runs on a fresh connection: preface + SETTINGS each time, and the
  // engine (with its HPACK state) is dropped rather than pooled.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.keepAlive = false;
  HttpClient client(cfg);
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
  EXPECT_TRUE(_lastSeenHttp2.load(std::memory_order_relaxed));
}

TEST_F(HttpClientHttp2E2ETest, ManySequentialRequestsOnOneConnection) {
  // Drives stream ids well past the closed-stream retention window (pruning), past the server's
  // SETTINGS_MAX_CONCURRENT_STREAMS default of 100 (all streams are sequential, so per-connection
  // active-stream accounting must drop back to zero after each exchange), and reuses the HPACK dynamic
  // tables across exchanges.
  HttpClient client = MakeHttp2Client();
  for (int reqIdx = 0; reqIdx < 150; ++reqIdx) {
    auto resp = client.get(url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientHttp2E2ETest, RedirectFollowed) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.get(url("/redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientHttp2E2ETest, SeeOtherRewritesToGetAndDropsBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.post(url("/see-other"), "discard-me", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientHttp2E2ETest, HeadHasNoBody) {
  HttpClient client = MakeHttp2Client();
  auto resp = client.head(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientHttp2E2ETest, RequestBodyCompression) {
  // Opt-in outbound compression: the engine rewrites content-length to the compressed size and appends
  // content-encoding; the server transparently decompresses and echoes the original payload back.
  const std::string payload = MakeLargeBody();
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);
  auto resp = client.post(url("/echo"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), payload);
}

TEST_F(HttpClientHttp2E2ETest, UserFramingHeadersRespected) {
  // A user-supplied Host becomes the :authority value, explicit User-Agent / Accept-Encoding suppress
  // the injected ones, and connection-specific headers (forbidden in HTTP/2) are silently dropped.
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, url("/echo"));
  req.header("Host", "override-authority.test")
      .header("User-Agent", "custom-agent/1.0")
      .header("Accept-Encoding", "identity")
      .header("Connection", "keep-alive")
      .header("Keep-Alive", "timeout=5")
      .header("Upgrade", "h2c")
      .header("TE", "trailers")
      .header("Proxy-Connection", "keep-alive")
      .body("payload", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload");
}

TEST_F(HttpClientHttp2E2ETest, NonTrailersTeHeaderIsDropped) {
  // "TE: trailers" is the one TE value HTTP/2 permits; any other TE value is connection-specific and must
  // be dropped from the header block (unlike the kept "TE: trailers" case). The request still completes.
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, url("/echo"));
  req.header("TE", "gzip").body("payload", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload");
}

TEST_F(HttpClientHttp2E2ETest, DropBodyRedirectKeepsUserHeader) {
  // A 303 rewrites POST -> GET and drops the body: the body's Content-Type is dropped from the rewritten
  // header block, but an unrelated user header survives and reaches the redirect target.
  HttpClient client = MakeHttp2Client();
  auto req = client.makeRequest(http::Method::POST, url("/see-other-headers"));
  req.header("x-custom-token", "kept-across-redirect").body("discard-me", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-echoed"), "kept-across-redirect");
}

TEST_F(HttpClientHttp2E2ETest, TransferAndContentEncodingRequestHeadersDisableCompression) {
  // A request already carrying Transfer-Encoding / Content-Encoding is never auto-compressed; the
  // (forbidden) Transfer-Encoding header itself is dropped from the HTTP/2 header block.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 1;
  HttpClient client(cfg);
  auto req = client.makeRequest(http::Method::POST, url("/echo"));
  req.header("Transfer-Encoding", "identity").header("Content-Encoding", "identity").body("raw-body", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "raw-body");
}

TEST_F(HttpClientHttp2E2ETest, TransferEncodingAloneDisablesCompression) {
  // Only Transfer-Encoding present (no Content-Encoding): the compression gate still bails on the
  // pre-existing framing header, and TE is then dropped from the HTTP/2 block, so the server receives the
  // raw body unencoded. Distinct from the case above where Content-Encoding short-circuits the gate first.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 1;
  HttpClient client(cfg);
  auto req = client.makeRequest(http::Method::POST, url("/echo"));
  req.header("Transfer-Encoding", "chunked").body("raw-te-body", "text/plain");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "raw-te-body");
}

TEST_F(HttpClientHttp2E2ETest, ServerGoAwayAfterStreamBudgetReconnects) {
  // The server GOAWAYs the connection after one stream: the client observes it (during the first
  // exchange or on the pooled-connection vetting), drops the connection and reconnects transparently.
  _server.reset();
  Router router;
  router.setPath(http::Method::GET, "/hello",
                 [](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK, "world", "text/plain"); });
  HttpServerConfig serverCfg;
  serverCfg.withPort(0).withPollInterval(std::chrono::milliseconds{20});
  serverCfg.http2.maxStreamsPerConnection = 1;
  _server = std::make_unique<SingleHttpServer>(std::move(serverCfg), std::move(router));
  _port = _server->port();
  _server->start();

  HttpClient client = MakeHttp2Client();
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientHttp2E2ETest, ClientStreamBudgetLimitsConnectionReuse) {
  // With a 2-stream budget per connection the pooled connection is not reused for the 3rd request; the
  // client reconnects transparently and all requests succeed.
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.http2.maxStreamsPerConnection = 2;
  HttpClient client(cfg);
  for (int reqIdx = 0; reqIdx < 3; ++reqIdx) {
    auto resp = client.get(url("/hello")).value();
    ASSERT_EQ(resp.status(), 200);
    ASSERT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientHttp2E2ETest, CustomHeadersRoundTrip) {
  HttpClient client = MakeHttp2Client();
  // Uppercase name on purpose: HTTP/2 requires lowercase field names on the wire, the engine lowers it.
  auto req = client.makeRequest(http::Method::GET, url("/headers"));
  req.header("X-Custom-Token", "abc123");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-echoed"), "abc123");
  EXPECT_EQ(resp.headerValueOrEmpty("x-server"), "aeronet");
}

TEST_F(HttpClientHttp2E2ETest, MaxResponseBytesEnforced) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  // The cap applies to the received (wire) body: disable decompression so no Accept-Encoding is
  // advertised and the server cannot shrink the 1 MiB body below the cap by compressing it.
  cfg.withDecompression(false);
  cfg.maxResponseBytes = 1024;
  HttpClient client(cfg);
  auto res = client.get(url("/big"));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), HttpClientErrc::malformedResponse);
}

TEST_F(HttpClientHttp2E2ETest, InvalidHttp2ConfigRejectedAtConstruction) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  cfg.http2.maxFrameSize = 1;  // below the RFC 9113 minimum of 16384
  EXPECT_THROW(HttpClient{cfg}, std::invalid_argument);
}

#ifdef AERONET_ENABLE_OPENSSL

namespace {

// TLS server factory: h2 is offered through ALPN by default; `enableHttp2 = false` restricts the
// server to http/1.1 so the client's Http2 mode has nothing to negotiate.
std::unique_ptr<SingleHttpServer> MakeTlsServer(std::string_view cert, std::string_view key,
                                                std::atomic<bool>& sawHttp2, uint16_t& port, bool enableHttp2 = true) {
  // A client dropping its connection right after the handshake (the ALPN-mismatch test) can make the
  // in-process server's OpenSSL stack write to a closed fd, raising SIGPIPE on Linux -- same mitigation
  // as test_tls_client.cpp. Windows has no SIGPIPE concept.
#ifdef AERONET_POSIX
  std::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  Router router;
  router.setPath(http::Method::GET, "/hello", [&sawHttp2](const HttpRequestView& req) {
    sawHttp2.store(req.version() == http::HTTP_2_0, std::memory_order_relaxed);
    return HttpResponse(http::StatusCodeOK, "tls-world", "text/plain");
  });
  HttpServerConfig cfg;
  cfg.withPort(0).withPollInterval(std::chrono::milliseconds{20}).withTlsCertKeyMemory(cert, key);
  cfg.http2.enable = enableHttp2;
  if (enableHttp2) {
    cfg.withTlsAlpnProtocols({"h2", "http/1.1"});  // offer h2 via ALPN (not advertised by default)
  }
  auto server = std::make_unique<SingleHttpServer>(std::move(cfg), std::move(router));
  port = server->port();
  server->start();
  return server;
}

HttpClientConfig TlsClientConfig(HttpVersionMode mode) {
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // ephemeral self-signed test certificate
  cfg.withHttpVersion(mode);
  return cfg;
}

std::string TlsUrl(uint16_t port) { return "https://localhost:" + std::to_string(port) + "/hello"; }

}  // namespace

TEST(HttpClientHttp2TlsE2ETest, AutoModeNegotiatesH2ViaAlpn) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Auto));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "tls-world");
  EXPECT_TRUE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http2ModeNegotiatesH2ViaAlpn) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http2));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http11ModeNeverNegotiatesH2) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{true};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http1_1));
  auto resp = client.get(TlsUrl(port)).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_FALSE(sawHttp2.load(std::memory_order_relaxed));
}

TEST(HttpClientHttp2TlsE2ETest, Http2ModeFailsWhenOriginHasNoH2) {
  auto [cert, key] = test::MakeEphemeralCertKey("localhost");
  std::atomic<bool> sawHttp2{false};
  uint16_t port{0};
  auto server = MakeTlsServer(cert, key, sawHttp2, port, /*enableHttp2=*/false);
  HttpClient client(TlsClientConfig(HttpVersionMode::Http2));
  auto res = client.get(TlsUrl(port));
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), HttpClientErrc::protocolUnsupported);
}

#endif  // AERONET_ENABLE_OPENSSL

}  // namespace aeronet
