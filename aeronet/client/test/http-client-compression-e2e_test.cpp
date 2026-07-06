// End-to-end tests for the client's automatic response decompression and request compression, driving a
// live aeronet server configured to compress responses and decompress request bodies. Both ends share the
// same codec bricks, so this exercises a full round-trip over real sockets.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {
namespace {

class HttpClientCompressionE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    Router router;
    // Echoes the (already server-decompressed) request body verbatim.
    router.setPath(http::Method::POST, "/echo",
                   [](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK, req.body(), "text/plain"); });
    // Returns the size of the (server-decompressed) request body, as a tiny uncompressed response.
    router.setPath(http::Method::POST, "/size", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::to_string(req.body().size()), "text/plain");
    });
    // Returns the Accept-Encoding header the server received (to observe what the client advertised).
    router.setPath(http::Method::GET, "/accept-encoding", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::string(req.headerValueOrEmpty("accept-encoding")), "text/plain");
    });
    // A large, highly compressible blob so the server compresses the response.
    router.setPath(http::Method::GET, "/blob",
                   [this](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, _blob, "text/plain"); });

    // Keep the server keep-alive idle timeout generous. A short timeout (e.g. 200ms) reaps a
    // freshly-accepted connection whose client, under CPU contention (parallel ctest on a loaded macOS
    // runner), is descheduled for >timeout between the TCP handshake completing and sending its request:
    // the server closes the never-used connection and the client's non-idempotent POST fails terminally
    // (default retry.maxAttempts == 1). Mirrors the TLS e2e test's choice for the same reason.
    HttpServerConfig cfg = HttpServerConfig{}
                               .withPort(0)
                               .withKeepAliveTimeout(std::chrono::seconds{5})
                               .withPollInterval(std::chrono::milliseconds{20});
    cfg.compression.minBytes = 16;  // compress even small response bodies
    _server = std::make_unique<SingleHttpServer>(std::move(cfg), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(_port) + std::string(path);
  }

  std::string _blob = test::MakePatternedPayload(64UL * 1024UL);
  std::unique_ptr<SingleHttpServer> _server;
  uint16_t _port{0};
};

}  // namespace

// --- Response decompression -------------------------------------------------

TEST_F(HttpClientCompressionE2E, AutoDecompressesResponseByDefault) {
  HttpClient client;  // decompression on by default; auto-advertises Accept-Encoding
  auto resp = client.get(url("/blob")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), _blob);                             // transparently decoded
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());  // header dropped after decode
}

TEST_F(HttpClientCompressionE2E, DisabledDecompressionLeavesBodyEncoded) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withDecompression(false).withDefaultAcceptEncoding("gzip, br, zstd, deflate");
  HttpClient client(cfg);
  auto resp = client.get(url("/blob")).value();
  EXPECT_EQ(resp.status(), 200);
  // Pass-through: the server compressed it, the client did not decode it.
  EXPECT_NE(resp.bodyInMemory(), _blob);
  EXPECT_FALSE(resp.headerValueOrEmpty("content-encoding").empty());
  EXPECT_LT(resp.bodyInMemory().size(), _blob.size());
}

// --- Accept-Encoding advertising -------------------------------------------

TEST_F(HttpClientCompressionE2E, AutoAdvertisesSupportedEncodings) {
  HttpClient client;
  auto resp = client.get(url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  if (test::SupportedEncodings().empty()) {
    EXPECT_TRUE(resp.bodyInMemory().empty());
  } else {
    EXPECT_FALSE(resp.bodyInMemory().empty());  // advertised the codecs we can decode
  }
}

TEST_F(HttpClientCompressionE2E, ExplicitAcceptEncodingOverridesAutoAdvertise) {
  HttpClientConfig cfg;
  cfg.withDefaultAcceptEncoding("identity");
  HttpClient client(cfg);
  auto resp = client.get(url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "identity");
}

TEST_F(HttpClientCompressionE2E, DisabledDecompressionDoesNotAdvertise) {
  HttpClientConfig cfg;
  cfg.withDecompression(false);  // and no explicit defaultAcceptEncoding
  HttpClient client(cfg);
  auto resp = client.get(url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());  // nothing advertised
}

// --- Request compression ----------------------------------------------------

TEST_F(HttpClientCompressionE2E, CompressesLargeRequestBody) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);

  const std::string payload = test::MakePatternedPayload(48UL * 1024UL);
  // The server decompresses the request body; /size echoes the decompressed length back.
  auto resp = client.post(url("/size"), payload, "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));

  // And a full content echo round-trips byte-for-byte.
  auto echo = client.post(url("/echo"), payload, "text/plain").value();
  EXPECT_EQ(echo.status(), 200);
  EXPECT_EQ(echo.bodyInMemory(), payload);
}

TEST_F(HttpClientCompressionE2E, IncompressibleRequestBodyFallsBackToPlain) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);

  const RawChars random = test::MakeRandomPayload(8192);
  const std::string payload(random.data(), random.size());
  // Compression is not beneficial -> sent uncompressed, server still receives it intact.
  auto resp = client.post(url("/size"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));
}

TEST_F(HttpClientCompressionE2E, SmallRequestBodyBelowThresholdIsNotCompressed) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);  // default minBytes (1024) keeps tiny bodies uncompressed
  HttpClient client(cfg);
  auto resp = client.post(url("/echo"), "tiny", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "tiny");
}

// A bodyless request (GET) with request compression enabled short-circuits the compression path on the
// empty-body guard -- no Content-Encoding is emitted and the request is sent as-is.
TEST_F(HttpClientCompressionE2E, RequestCompressionSkippedForBodylessGet) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 1;  // aggressive threshold: only the empty body prevents compression
  HttpClient client(cfg);
  auto resp = client.get(url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
}

// --- Captured vs scattered request write ------------------------------------

TEST_F(HttpClientCompressionE2E, CapturedAndScatteredBodiesBothRoundTrip) {
  HttpClientConfig cfg;
  cfg.withMaxCapturedRequestBodyBytes(1024);  // small bodies captured, large ones scattered
  HttpClient client(cfg);

  auto smallResp = client.post(url("/echo"), "captured-body", "text/plain").value();  // <= threshold => single write
  EXPECT_EQ(smallResp.bodyInMemory(), "captured-body");

  const std::string big(4096, 'x');  // > threshold => scatter write
  auto largeResp = client.post(url("/echo"), big, "text/plain").value();
  EXPECT_EQ(largeResp.bodyInMemory(), big);
}

// --- Per-encoding request compression --------------------------------------

TEST_F(HttpClientCompressionE2E, EachSupportedEncodingForRequestBody) {
  const std::string payload = test::MakePatternedPayload(32UL * 1024UL);
  for (const Encoding enc : test::SupportedEncodings()) {
    HttpClientConfig cfg;
    cfg.withRequestCompression(enc);
    cfg.requestCompression.codec.minBytes = 16;
    HttpClient client(cfg);
    auto resp = client.post(url("/echo"), payload, "text/plain").value();
    EXPECT_EQ(resp.status(), 200) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory(), payload) << GetEncodingStr(enc);
  }
}

}  // namespace aeronet
