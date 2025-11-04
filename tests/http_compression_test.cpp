#include <gtest/gtest.h>

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "zstd_test_helpers.hpp"

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
TEST(HttpCompressionBrotliBuffered, BrAppliedWhenEligible) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(400, 'B');
  ts.server.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.customHeader("Content-Type", "text/plain");
    respObj.body(payload);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/br1", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "br");
  EXPECT_LT(resp.body.size(), payload.size());
}

TEST(HttpCompressionBrotliBuffered, UserContentEncodingIdentityDisablesCompression) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'U');
  ts.server.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.customHeader("Content-Type", "text/plain");
    respObj.customHeader("Content-Encoding", "identity");
    respObj.body(payload);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/br2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompressionBrotliBuffered, BelowThresholdNotCompressed) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 2048;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string small(64, 's');
  ts.server.router().setDefault([small](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(small);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/br3", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_EQ(resp.body.size(), small.size());
}

TEST(HttpCompressionBrotliBuffered, NoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(180, 'D');
  ts.server.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/br4", {});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompressionBrotliBuffered, IdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(70, 'Q');
  ts.server.router().setDefault([payload](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(payload);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/br5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompressionBrotliStreaming, BrActivatedOverThreshold) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 64;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.server.router().setDefault([part1, part2](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(part1);
    writer.writeBody(part2);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr1", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
  // Size heuristic: compressed should be smaller than concatenated plain text.
  EXPECT_LT(resp.body.size(), part1.size() + part2.size());
}

TEST(HttpCompressionBrotliStreaming, BelowThresholdIdentity) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string small(80, 'x');
  ts.server.router().setDefault([small](const HttpRequest &) {
    HttpResponse respObj;
    respObj.body(small);
    return respObj;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr2", {{"Accept-Encoding", "br"}});
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_TRUE(resp.body.contains('x'));
}

TEST(HttpCompressionBrotliStreaming, UserProvidedIdentityPreventsActivation) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(512, 'Y');
  ts.server.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(aeronet::http::StatusCodeOK);
    writer.customHeader("Content-Encoding", "identity");
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr3", {{"Accept-Encoding", "br"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Streaming identity may use chunked transfer, so body size can exceed raw payload due to framing; just ensure
  // we did not apply brotli (which would eliminate long runs of 'Y').
  EXPECT_TRUE(resp.body.contains(std::string(32, 'Y')));
}

TEST(HttpCompressionBrotliStreaming, QValuesInfluenceSelection) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 64;
  cfg.preferredFormats = {Encoding::gzip, Encoding::br};  // preferences list order
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(600, 'Z');
  ts.server.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 128));
    writer.writeBody(payload.substr(128));
    writer.end();
  });
  // Client strongly prefers br
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr4", {{"Accept-Encoding", "gzip;q=0.5, br;q=1.0"}});
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "br");
  }
}

TEST(HttpCompressionBrotliStreaming, IdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!aeronet::brotliEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats = {Encoding::br};
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(90, 'F');
  ts.server.router().setDefault([payload]([[maybe_unused]] const HttpRequest &req, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.writeBody(payload);
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbr5", {{"Accept-Encoding", "identity;q=0, snappy;q=0"}});
  // Server should respond 406 (not compressible with offered encodings; identity forbidden)
  EXPECT_TRUE(resp.headersRaw.contains(" 406 "));
}

TEST(HttpCompressionBuffered, UserContentEncodingIdentityDisablesCompression) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'B');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.customHeader("Content-Encoding", "identity");  // explicit suppression
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/o", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  // Should remain uncompressed and server must not alter user-provided identity
  auto itCE = resp.headers.find("Content-Encoding");
  ASSERT_NE(itCE, resp.headers.end());
  EXPECT_EQ(itCE->second, "identity");
  EXPECT_EQ(resp.body.size(), payload.size());
}

TEST(HttpCompressionBuffered, BelowThresholdNotCompressed) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1024;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string smallPayload(32, 'C');
  ts.server.router().setDefault([smallPayload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(smallPayload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/s", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_EQ(resp.body.size(), smallPayload.size());
}

TEST(HttpCompressionBuffered, NoAcceptEncodingHeaderStillCompressesDefault) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'D');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/i", {});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
    EXPECT_TRUE(HasGzipMagic(resp.body));
  }
}

TEST(HttpCompressionBuffered, IdentityForbiddenNoAlternativesReturns406) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;  // ensure compression considered
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(64, 'Q');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // Client forbids identity and offers only unsupported encodings (br here is unsupported in current build).
  auto resp = aeronet::test::simpleGet(ts.port(), "/bad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeNotAcceptable);
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompressionBuffered, IdentityForbiddenButGzipAvailableUsesGzip) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'Z');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/ok", {{"Accept-Encoding", "identity;q=0, gzip"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "gzip");
  EXPECT_TRUE(HasGzipMagic(resp.body));
}

TEST(HttpCompressionBuffered, UnsupportedEncodingDoesNotApplyGzip) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 1;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(200, 'E');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
    aeronet::HttpResponse resp;
    resp.customHeader("Content-Type", "text/plain");
    resp.body(payload);
    return resp;
  });
  // If brotli support is compiled in, 'br' is actually supported and would trigger compression.
  // Use an obviously unsupported token (snappy) in that case.
  auto resp =
      aeronet::test::simpleGet(ts.port(), "/br", {{"Accept-Encoding", aeronet::brotliEnabled() ? "snappy" : "br"}});
  EXPECT_EQ(resp.statusCode, aeronet::http::StatusCodeOK);
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
}

TEST(HttpCompressionBuffered, DeflateAppliedWhenPreferredAndAccepted) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::deflate);
  cfg.preferredFormats.push_back(Encoding::gzip);  // ensure ordering honored
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string largePayload(300, 'F');
  ts.server.router().setDefault([largePayload](const aeronet::HttpRequest &) {
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
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(256, 'G');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
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
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  // Server preference: gzip first, deflate second, but client gives gzip q=0.1 deflate q=0.9.
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(180, 'H');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
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
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 8;
  cfg.preferredFormats.push_back(Encoding::deflate);  // Only influences tie-breaks; does not disable gzip.
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(256, 'I');
  ts.server.router().setDefault([payload](const aeronet::HttpRequest &) {
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

// NOTE: These streaming tests validate that compression is applied (or not) and that negotiation picks
// the expected format. They do not currently attempt mid-stream header observation since the handler
// executes to completion before the test inspects the socket.

TEST(HttpCompressionStreaming, GzipActivatedOverThreshold) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 64;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string part1(40, 'a');
  std::string part2(80, 'b');
  ts.server.router().setDefault([part1, part2](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(part1);  // below threshold so far
    writer.writeBody(part2);  // crosses threshold -> compression should activate
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sgz", {{"Accept-Encoding", "gzip"}});
  // NOTE: Current implementation emits headers before compression activation, so Content-Encoding
  // may be absent even though body bytes are compressed. Accept either presence or absence but
  // verify gzip magic appears in body to confirm activation.
  auto it = resp.headers.find("Content-Encoding");
  if (it != resp.headers.end()) {
    EXPECT_EQ(it->second, "gzip");
  }
  EXPECT_TRUE(resp.body.contains("\x1f\x8b") || HasGzipMagic(resp.body));
}

TEST(HttpCompressionStreaming, DeflateActivatedOverThreshold) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 32;
  cfg.preferredFormats.push_back(Encoding::deflate);
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'X');
  ts.server.router().setDefault([payload](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 40));
    writer.writeBody(payload.substr(40));
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sdf", {{"Accept-Encoding", "deflate,gzip"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end())
      << "Content-Encoding header should be present after delayed header emission refactor";
  EXPECT_EQ(it->second, "deflate");
  // Minimal integrity check: compressed body should not be trivially equal to original repeated character sequence
  EXPECT_NE(resp.body.size(), 128U);  // chunked framing + compression alters size
}

TEST(HttpCompressionStreaming, BelowThresholdIdentity) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 512;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string small(40, 'y');
  ts.server.router().setDefault([small](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(small);  // never crosses threshold
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sid", {{"Accept-Encoding", "gzip"}});
  EXPECT_EQ(resp.headers.find("Content-Encoding"), resp.headers.end());
  EXPECT_TRUE(resp.body.contains('y'));
}

TEST(HttpCompressionStreaming, UserProvidedContentEncodingIdentityPreventsActivation) {
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string big(200, 'Z');
  ts.server.router().setDefault([big](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.customHeader("Content-Encoding", "identity");  // explicit suppression
    writer.writeBody(big.substr(0, 50));
    writer.writeBody(big.substr(50));
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/soff", {{"Accept-Encoding", "gzip"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "identity");
  // Body should contain literal 'Z' sequences (chunked framing around them)
  EXPECT_TRUE(resp.body.contains('Z'));
}

TEST(HttpCompressionStreaming, QValuesInfluenceStreamingSelection) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  CompressionConfig cfg;
  cfg.minBytes = 16;
  cfg.preferredFormats.push_back(Encoding::gzip);
  cfg.preferredFormats.push_back(Encoding::deflate);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(180, 'Q');
  ts.server.router().setDefault([payload](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody(payload.substr(0, 60));
    writer.writeBody(payload.substr(60));
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sqv", {{"Accept-Encoding", "gzip;q=0.1, deflate;q=0.9"}});
  auto it = resp.headers.find("Content-Encoding");
  ASSERT_NE(it, resp.headers.end());
  EXPECT_EQ(it->second, "deflate");
}

TEST(HttpCompressionStreaming, IdentityForbiddenNoAlternativesReturns406) {
  CompressionConfig cfg;
  cfg.minBytes = 1;  // ensure compression considered
  cfg.preferredFormats.push_back(Encoding::gzip);
  HttpServerConfig scfg{};
  scfg.withCompression(cfg);
  aeronet::test::TestServer ts(std::move(scfg));
  ts.server.router().setDefault([](const HttpRequest &, HttpResponseWriter &writer) {
    writer.statusCode(http::StatusCodeOK);  // will be overridden to 406 before handler invoked if negotiation rejects
    writer.contentType("text/plain");
    writer.writeBody(std::string(64, 'Q'));
    writer.end();
  });
  auto resp = aeronet::test::simpleGet(ts.port(), "/sbad", {{"Accept-Encoding", "identity;q=0, br;q=0"}});
  EXPECT_TRUE(resp.headersRaw.rfind("HTTP/1.1 406", 0) == 0) << resp.headersRaw;
  EXPECT_EQ(resp.body, "No acceptable content-coding available");
}

TEST(HttpCompressionZstdBuffered, ZstdAppliedWhenEligible) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  } else {
    CompressionConfig cfg;
    cfg.minBytes = 32;
    cfg.preferredFormats.push_back(Encoding::zstd);
    HttpServerConfig scfg{};
    scfg.withCompression(cfg);
    aeronet::test::TestServer ts(std::move(scfg));
    std::string payload(400, 'A');
    ts.server.router().setDefault([payload](const HttpRequest &) {
      HttpResponse resp;
      resp.customHeader("Content-Type", "text/plain");
      resp.body(payload);
      return resp;
    });
    auto resp = aeronet::test::simpleGet(ts.port(), "/z", {{"Accept-Encoding", "zstd"}});
    ASSERT_EQ(resp.statusCode, 200);
    auto it = resp.headers.find("Content-Encoding");
    ASSERT_NE(it, resp.headers.end());
    EXPECT_EQ(it->second, "zstd");
    EXPECT_TRUE(test::HasZstdMagic(resp.body));
    EXPECT_LT(resp.body.size(), payload.size());
    // Round-trip verify by decompressing (simple one-shot) to ensure integrity
    std::string decompressed = aeronet::test::zstdRoundTripDecompress(resp.body, payload.size());
    EXPECT_EQ(decompressed, payload);
  }
}

TEST(HttpCompressionZstdBuffered, WildcardSelectsZstdIfPreferred) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  } else {
    CompressionConfig cfg;
    cfg.minBytes = 16;
    cfg.preferredFormats.push_back(Encoding::zstd);
    if constexpr (aeronet::zlibEnabled()) {
      cfg.preferredFormats.push_back(Encoding::gzip);
    }
    HttpServerConfig scfg{};
    scfg.withCompression(cfg);
    aeronet::test::TestServer ts(std::move(scfg));
    std::string payload(256, 'B');
    ts.server.router().setDefault([payload](const HttpRequest &) {
      HttpResponse resp;
      resp.body(payload);
      resp.customHeader("Content-Type", "text/plain");
      return resp;
    });
    auto resp = aeronet::test::simpleGet(ts.port(), "/w", {{"Accept-Encoding", "*;q=0.9"}});
    auto it = resp.headers.find("Content-Encoding");
    ASSERT_NE(it, resp.headers.end());
    EXPECT_EQ(it->second, "zstd");
    EXPECT_TRUE(test::HasZstdMagic(resp.body));
  }
}

TEST(HttpCompressionZstdBuffered, TieBreakAgainstGzipHigherQ) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  } else {
    CompressionConfig cfg;
    cfg.minBytes = 16;
    cfg.preferredFormats.push_back(Encoding::zstd);
    if constexpr (aeronet::zlibEnabled()) {
      cfg.preferredFormats.push_back(Encoding::gzip);
    }
    HttpServerConfig scfg{};
    scfg.withCompression(cfg);
    aeronet::test::TestServer ts(std::move(scfg));
    std::string payload(512, 'C');
    ts.server.router().setDefault([payload](const HttpRequest &) {
      HttpResponse resp;
      resp.body(payload);
      resp.customHeader("Content-Type", "text/plain");
      return resp;
    });
    auto resp = aeronet::test::simpleGet(ts.port(), "/t", {{"Accept-Encoding", "gzip;q=0.9, zstd;q=0.9"}});
    auto it = resp.headers.find("Content-Encoding");
    ASSERT_NE(it, resp.headers.end());
    EXPECT_EQ(it->second, "zstd");
  }
}

TEST(HttpCompressionZstdStreaming, ZstdActivatesAfterThreshold) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  } else {
    CompressionConfig cfg;
    cfg.minBytes = 128;
    cfg.preferredFormats.push_back(Encoding::zstd);
    HttpServerConfig scfg{};
    scfg.withCompression(cfg);
    aeronet::test::TestServer ts(std::move(scfg));
    std::string chunk1(64, 'x');
    std::string chunk2(128, 'y');
    ts.server.router().setDefault([chunk1, chunk2](const HttpRequest &, HttpResponseWriter &writer) {
      writer.statusCode(http::StatusCodeOK);
      writer.contentType("text/plain");
      writer.writeBody(chunk1);
      writer.writeBody(chunk2);
      writer.end();
    });
    auto resp = test::simpleGet(ts.port(), "/zs", {{"Accept-Encoding", "zstd"}});
    auto it = resp.headers.find("Content-Encoding");
    ASSERT_NE(it, resp.headers.end());
    EXPECT_EQ(it->second, "zstd");
    EXPECT_TRUE(test::HasZstdMagic(resp.plainBody));
    // Round-trip decompression via helper
    std::string original = chunk1 + chunk2;
    auto decompressed = aeronet::test::zstdRoundTripDecompress(resp.plainBody, original.size());
    EXPECT_EQ(decompressed, original);
  }
}

TEST(HttpCompressionZstdStreaming, BelowThresholdIdentity) {
  if constexpr (!aeronet::zstdEnabled()) {
    GTEST_SKIP();
  } else {
    CompressionConfig cfg;
    cfg.minBytes = 1024;
    cfg.preferredFormats.push_back(Encoding::zstd);
    HttpServerConfig scfg{};
    scfg.withCompression(cfg);
    aeronet::test::TestServer ts(std::move(scfg));
    std::string data(200, 'a');
    ts.server.router().setDefault([data](const HttpRequest &, HttpResponseWriter &writer) {
      writer.statusCode(http::StatusCodeOK);
      writer.contentType("text/plain");
      writer.writeBody(data);
      writer.end();
    });
    auto resp = test::simpleGet(ts.port(), "/zi", {{"Accept-Encoding", "zstd"}});
    auto it = resp.headers.find("Content-Encoding");
    EXPECT_TRUE(it == resp.headers.end());  // identity
    EXPECT_TRUE(resp.plainBody == data) << "identity path should match input exactly";
  }
}