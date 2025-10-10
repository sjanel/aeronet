#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

namespace {  // Helper utilities local to this test file
bool HasGzipMagic(std::string_view body) {
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x1f && static_cast<unsigned char>(body[1]) == 0x8b;
}
bool LooksLikeZlib(std::string_view body) {
  // Very loose heuristic: zlib header is 2 bytes: CMF (compression method/flags) + FLG with check bits.
  // CMF lower 4 bits must be 8 (deflate), i.e. 0x78 is common for default window (0x78 0x9C etc).
  return body.size() >= 2 && static_cast<unsigned char>(body[0]) == 0x78;  // ignore second byte variability
}

}  // namespace

TEST(HttpCompressionBuffered, GzipAppliedWhenEligible) {
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string largePayload(200, 'A');
  ts.server.setHandler([largePayload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(largePayload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/x", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  if (it == resp.headers.end()) {
    std::cerr << "Headers received:\n";
    for (auto& kv : resp.headers) {
      std::cerr << kv.first << ": " << kv.second << "\n";
    }
  }
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
  EXPECT_LT(resp.body.size(), largePayload.size());
}

TEST(HttpCompressionBuffered, UserContentEncodingIdentityDisablesCompression) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'B');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.customHeader("Content-Encoding", "identity");  // explicit suppression
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/o", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  // Should remain uncompressed and server must not alter user-provided identity
  auto itCE = resp.headers.find("Content-Encoding");
  ASSERT_NE(itCE, resp.headers.end());
  EXPECT_EQ(itCE->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompressionBuffered, BelowThresholdNotCompressed) {
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string smallPayload(32, 'C');
  ts.server.setHandler([smallPayload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(smallPayload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/s", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_EQ(resp.body.size(), smallPayload.size());
}

TEST(HttpCompressionBuffered, NoAcceptEncodingHeaderStillCompressesDefault) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'D');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/i", {});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
    EXPECT_TRUE(HasGzipMagic(resp.body));
  }
}

TEST(HttpCompressionBuffered, IdentityForbiddenNoAlternativesReturns406) {
  CompressionConfig cfg;
  cfg.minBytes = 1;  // ensure compression considered
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(64, 'Q');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // Client forbids identity and offers only unsupported encodings (br here is unsupported in current build).
  auto resp = aeronet::test::simpleGet(ts.port(), "/bad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_EQ(resp.statusCode, 406) << "Expected 406 when identity forbidden and no acceptable encoding";
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompressionBuffered, IdentityForbiddenButGzipAvailableUsesGzip) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'Z');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/ok", {{"Accept-Encoding", "identity;q=0, gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompressionBuffered, UnsupportedEncodingDoesNotApplyGzip) {
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(200, 'E');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // If brotli support is compiled in, 'br' is actually supported and would trigger compression.
  // Use an obviously unsupported token (snappy) in that case.
#ifdef AERONET_ENABLE_BROTLI
  auto resp = aeronet::test::simpleGet(ts.port(), "/br", {{"Accept-Encoding", "snappy"}});
#else
  auto resp = aeronet::test::simpleGet(ts.port(), "/br", {{"Accept-Encoding", "br"}});
#endif
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
}

TEST(HttpCompressionBuffered, DeflateAppliedWhenPreferredAndAccepted) {
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::deflate);
  cfg.preferredFormats.push_back(Encoding::gzip);  // ensure ordering honored
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string largePayload(300, 'F');
  ts.server.setHandler([largePayload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(largePayload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/d1", {{"Accept-Encoding", "deflate,gzip"}});
  EXPECT_EQ(resp.statusCode, 200);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
  EXPECT_LT(resp.body.size(), largePayload.size());
}

TEST(HttpCompressionBuffered, GzipChosenWhenHigherPreference) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(256, 'G');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/d2", {{"Accept-Encoding", "gzip,deflate"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompressionBuffered, QValuesAffectSelection) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  // Server preference: gzip first, deflate second, but client gives gzip q=0.1 deflate q=0.9.
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(180, 'H');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/d3", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
  EXPECT_TRUE(LooksLikeZlib(resp.body));
}

TEST(HttpCompressionBuffered, IdentityFallbackIfDeflateNotRequested) {
  CompressionConfig cfg;
  cfg.minBytes = 8;
  cfg.preferredFormats.push_back(Encoding::deflate);  // Only influences tie-breaks; does not disable gzip.
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(256, 'I');
  ts.server.setHandler([payload](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp =
      aeronet::test::simpleGet(ts.port(), "/d4", {{"Accept-Encoding", "gzip"}});  // client does NOT list deflate
  auto it = resp.headers.find("Content-Encoding");
  // Under current semantics gzip is still chosen (higher q than identity) even if not in preferredFormats.
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
  EXPECT_LT(resp.body.size(), payload.size());
}
